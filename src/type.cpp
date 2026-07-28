// =============================================================================
// 类型系统实现
// =============================================================================

#include "type.h"
#include <format>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// 工厂方法：创建各种类型实例
// ─────────────────────────────────────────────────────────────────────────────

TypePtr Type::makeVoid() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Void;
    t->name = "void";
    return t;
}

TypePtr Type::makeBool() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Bool;
    t->name = "bool";
    return t;
}

TypePtr Type::makeInt() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    t->name = "int";
    return t;
}

TypePtr Type::makeDouble() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Double;
    t->name = "double";
    return t;
}

TypePtr Type::makePointer(TypePtr pointee) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Pointer;
    t->name = pointee->toString() + "*";
    t->pointeeType = std::move(pointee);
    return t;
}

TypePtr Type::makeLValueReference(TypePtr referenced) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::LValueReference;
    t->name = referenced->toString() + "&";
    t->referencedType = std::move(referenced);
    return t;
}

TypePtr Type::makeRValueReference(TypePtr referenced) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::RValueReference;
    t->name = referenced->toString() + "&&";
    t->referencedType = std::move(referenced);
    return t;
}

TypePtr Type::makeConst(TypePtr inner) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Const;
    t->name = "const " + inner->toString();
    t->innerType = std::move(inner);
    return t;
}

TypePtr Type::makeClass(const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Class;
    t->name = name;
    t->classLayout.className = name;
    return t;
}

TypePtr Type::makeTemplateParam(const std::string& paramName) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::TemplateParam;
    t->name = paramName;
    t->templateParamName = paramName;
    return t;
}

TypePtr Type::makeAuto() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Auto;
    t->name = "auto";
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// sizeInBytes：获取类型占用的字节数
// ─────────────────────────────────────────────────────────────────────────────
// 这是"运行期看偏移量"的基础——编译器必须知道每种类型占多少内存。
// 在我们的简化模型中，假设 64 位系统：
//   - void:  0 字节
//   - bool:  1 字节（但为了对齐通常占 4 或 8 字节）
//   - int:   4 字节
//   - double: 8 字节
//   - pointer: 8 字节
//   - class: 由 classLayout.totalSize 决定
// ─────────────────────────────────────────────────────────────────────────────
uint32_t Type::sizeInBytes() const {
    switch (kind) {
        case TypeKind::Void:   return 0;
        case TypeKind::Bool:   return 1;
        case TypeKind::Int:    return 4;
        case TypeKind::Double: return 8;
        case TypeKind::Pointer: return 8;
        case TypeKind::LValueReference: return 8;  // 引用本质是指针，占 8 字节
        case TypeKind::RValueReference: return 8;  // 同上
        case TypeKind::Const:  return innerType ? innerType->sizeInBytes() : 0;
        case TypeKind::Class:  return classLayout.totalSize;
        case TypeKind::TemplateParam: return 0; // 模板参数大小未知
        case TypeKind::Auto:   return 0; // auto 大小未知，等待推导
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// equals：类型比较
// ─────────────────────────────────────────────────────────────────────────────
bool Type::equals(const TypePtr& other) const {
    if (!other) return false;
    if (kind != other->kind) return false;

    switch (kind) {
        case TypeKind::Void:
        case TypeKind::Bool:
        case TypeKind::Int:
        case TypeKind::Double:
            return true; // 基础类型只比较 kind

        case TypeKind::Pointer:
            return pointeeType && other->pointeeType
                && pointeeType->equals(other->pointeeType);

        case TypeKind::LValueReference:
        case TypeKind::RValueReference:
            return referencedType && other->referencedType
                && referencedType->equals(other->referencedType);

        case TypeKind::Const:
            return innerType && other->innerType
                && innerType->equals(other->innerType);

        case TypeKind::Class:
            return name == other->name; // 类类型按名字比较

        case TypeKind::TemplateParam:
            return templateParamName == other->templateParamName;

        case TypeKind::Auto:
            return true; // auto 与 auto 相等
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// toString：类型的可读字符串表示
// ─────────────────────────────────────────────────────────────────────────────
std::string Type::toString() const {
    switch (kind) {
        case TypeKind::Void:   return "void";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Int:    return "int";
        case TypeKind::Double: return "double";
        case TypeKind::Pointer:
            return pointeeType ? pointeeType->toString() + "*" : "?*";
        case TypeKind::LValueReference:
            return referencedType ? referencedType->toString() + "&" : "?&";
        case TypeKind::RValueReference:
            return referencedType ? referencedType->toString() + "&&" : "?&&";
        case TypeKind::Const:
            return innerType ? "const " + innerType->toString() : "const ?";
        case TypeKind::Class:  return name;
        case TypeKind::TemplateParam: return templateParamName;
        case TypeKind::Auto:   return "auto";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// stripReferences / stripConst：去除修饰，获取"裸类型"
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Type::stripReferences() const {
    if (isLValueReference() || isRValueReference()) {
        return referencedType ? referencedType : nullptr;
    }
    // 如果不是引用类型，返回自身的 shared_ptr（简化处理）
    return nullptr;
}

TypePtr Type::stripConst() const {
    if (isConst()) {
        return innerType ? innerType : nullptr;
    }
    return nullptr;
}

} // namespace minicc
