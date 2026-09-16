// =============================================================================
// 类型系统实现
// =============================================================================
// 实现 include/type.h 声明的 Type 成员：工厂方法、sizeInBytes、equals、
// toString、stripReferences/stripConst。
// 注意分工：
//   · 引用折叠与类型替换（substituteType）在 src/template_instantiation.cpp；
//   · 类型 → Itanium mangling 编码（encodeType）在 src/template_instantiation.cpp
//     的 NameMangler 中。本文件的 toString 面向"人读"，encodeType 面向"链接器"。
// =============================================================================

#include "type.h"
#include <format>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// TemplateArg：模板实参的人读形态
// ─────────────────────────────────────────────────────────────────────────────
// 按 kind 分派：类型实参取类型名，非类型实参（NTTP）取十进制数字。
// 这一步是"值 vs 类型"在字符串层面的体现——Buf<4> 打印成 "Buf<4>"，
// 而若把 4 当成类型打印会得到无意义的 "?"。
// demo：ofType(makeInt()).toString() → "int"；ofValue(4).toString() → "4"
std::string TemplateArg::toString() const {
    if (kind == TemplateArgKind::Type) return type ? type->toString() : "?";
    return std::to_string(value);
}

// ─────────────────────────────────────────────────────────────────────────────
// 工厂方法：创建各种类型实例
// ─────────────────────────────────────────────────────────────────────────────

// void 类型。无入参。demo：void f(); 的返回类型 → Type{kind=Void, name="void"}
TypePtr Type::makeVoid() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Void;
    t->name = "void";
    return t;
}

// bool 类型。无入参。demo：bool b; 的声明类型 → Type{kind=Bool, name="bool"}，encodeType→"b"
TypePtr Type::makeBool() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Bool;
    t->name = "bool";
    return t;
}

// int 类型。无入参。demo：int x; 的声明类型 → Type{kind=Int, name="int"}，encodeType→"i"
TypePtr Type::makeInt() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    t->name = "int";
    return t;
}

// double 类型。无入参。demo：double d; → Type{kind=Double, name="double"}，encodeType→"d"
TypePtr Type::makeDouble() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Double;
    t->name = "double";
    return t;
}

// 指针类型 T*。入参 pointee = 被指向的内层类型。
// demo：makePointer(Int) → Pointer(Int)，toString="int*"，encodeType→"Pi"
//       makePointer(Pointer(Int)) → int**，encodeType→"PPi"
TypePtr Type::makePointer(TypePtr pointee) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Pointer;
    t->name = pointee->toString() + "*";
    t->pointeeType = std::move(pointee);
    return t;
}

// 左值引用 T&。入参 referenced = 被引用的内层类型。
// demo：makeLValueReference(Int) → LValueReference(Int)，toString="int&"，encodeType→"Ri"
// 注意：若 referenced 本身已是引用，则构成嵌套引用，实例化时由引用折叠处理。
TypePtr Type::makeLValueReference(TypePtr referenced) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::LValueReference;
    t->name = referenced->toString() + "&";
    t->referencedType = std::move(referenced);
    return t;
}

// 右值引用 T&&。入参 referenced = 被引用的内层类型。
// demo：makeRValueReference(Int) → RValueReference(Int)，toString="int&&"，encodeType→"Oi"
// 当 T 是模板参数时 T&& 是"万能引用"（[temp.deduct.call]）；折叠规则见 type.h。
TypePtr Type::makeRValueReference(TypePtr referenced) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::RValueReference;
    t->name = referenced->toString() + "&&";
    t->referencedType = std::move(referenced);
    return t;
}

// const 限定 const T。入参 inner = 被限定的内层类型。
// demo：makeConst(Int) → Const(Int)，toString="const int"，encodeType→"Ki"
//       makeConst(LValueReference(Int)) → const int&，encodeType→"KRi"
// ★ const 落在哪一层由【源码位置】决定（[dcl.type.cv]，见 type.h"组合规则"）：
//   makeConst(Int)               → Const(Int)            "const int"
//   makeConst(Pointer(Int))      → Const(Pointer(Int))   "int* const"   （声明符侧）
//   makePointer(makeConst(Int))  → Pointer(Const(Int))   "const int*"   （说明符侧）
//   两种结构打印不同、mangling 不同、偏特化匹配结果也不同 —— 不可互换。
// ※ 旧注释写"本项目 const 总包在最外层"，那是 ⑨ 修复前的实情，2026-09-14 更正。
TypePtr Type::makeConst(TypePtr inner) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Const;
    t->name = "const " + inner->toString();
    t->innerType = std::move(inner);
    return t;
}

// 类类型。入参 name = 类名；同时初始化 classLayout.className。
// demo：makeClass("Point") → Class{ name="Point", classLayout.className="Point" }
// encodeType→"5Point"（长度5 + 名字）。字段偏移/vtable 由语义阶段填充。
TypePtr Type::makeClass(const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Class;
    t->name = name;
    t->classLayout.className = name;
    return t;
}

// 模板参数占位类型。入参 paramName = 模板参数名。
// demo：makeTemplateParam("T") → TemplateParam{ templateParamName="T" }
// 出现在模板蓝图中；实例化时被 substituteType 替换为实际类型。
TypePtr Type::makeTemplateParam(const std::string& paramName) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::TemplateParam;
    t->name = paramName;
    t->templateParamName = paramName;
    return t;
}

// auto 占位类型。无入参。demo：auto x = ...; → Type{kind=Auto, name="auto"}
// 语义阶段用初始化表达式类型回填后即被消除（阶段3之后不应再出现 auto）。
TypePtr Type::makeAuto() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Auto;
    t->name = "auto";
    return t;
}

// decltype(expr) 半成品类型。入参 expr = 操作数；paren = 原文是否写成 decltype((e))。
// demo：decltype(x) → Type{kind=Decltype, decltypeExpr=VarExpr{x}, decltypeParen=false}
// 【不立即求值】：表达式原样留存，等 substituteType 阶段再求
// （模板模式里 T 未知，此刻无法求）。对照 clang：DecltypeType。
TypePtr Type::makeDecltype(DecltypeExprPtr expr, bool paren) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Decltype;
    t->name = "decltype";          // 占位名；求值后整个节点被具体类型取代
    t->decltypeExpr = std::move(expr);
    t->decltypeParen = paren;
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
        case TypeKind::Decltype: return 0; // decltype 未求值，大小未知
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// equals：类型比较
// ─────────────────────────────────────────────────────────────────────────────
// 结构化递归比较：kind 相同才继续，复合类型递归比较内层。
// 用于语义检查（实参/形参匹配）与函数模板推导中"多处推导结果须一致"的判断
// （合一算法 unify 成功后的类型等价检查，见 [temp.deduct]）。
// demo：int  equals int  → true；int equals double → false（kind 不同）
//       Pi   equals Pi   → true（递归比较 pointee）
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
            // ★ 嵌套类型名（S<int>::type / T::type）不能只比 name ——
            //   两个不同限定者的 "type" 会撞成相等（Box<int>::type 与
            //   Box<double>::type 是不同类型）。必须连限定者一起递归比。
            if (isNestedName() != other->isNestedName()) return false;
            if (isNestedName()) {
                return name == other->name
                    && nestedQualifier->equals(other->nestedQualifier);
            }
            return name == other->name; // 类类型按名字比较

        case TypeKind::TemplateParam:
            return templateParamName == other->templateParamName;

        case TypeKind::Auto:
            return true; // auto 与 auto 相等

        case TypeKind::Decltype:
            // 未求值的 decltype 之间无从比较（操作数是表达式，不是类型）。
            // 正常流程下不该走到这里：substituteType / resolveType 之后
            // Decltype 节点应已被求值成具体类型并整体替换掉。
            return false;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// toString：类型的可读字符串表示
// ─────────────────────────────────────────────────────────────────────────────
// 递归拼接，用于中文日志与报错信息。与 NameMangler::encodeType 的区别：
// toString 面向"人读"，encodeType 面向"链接器"。
// demo：Const(LValueReference(Int)).toString() → "const int&"
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
        case TypeKind::Const: {
            if (!innerType) return "const ?";
            // ★ 'const' 印在【哪一侧】由它限定的东西决定（[dcl.type.cv]）：
            //     Const(Int)          → "const int"     限定基类型
            //     Pointer(Const(Int)) → "const int*"    指针【指向】const
            //     Const(Pointer(Int)) → "int* const"    const 的【指针】
            //   此前一律输出 "const " + inner->toString()，于是
            //   Pointer(Const(Int)) 与 Const(Pointer(Int)) 都印成 "const int*"
            //   —— 两种不同结构拿到同一个字符串。危害不止是日志难读：
            //   ① getOrInstantiateClass 的【缓存键】以 toString 为输入
            //      ⇒ C<const int*>（指针指向 const）与 C<int* const>（const 指针）
            //        共用一条缓存，后者的实例被前者的实例顶掉；
            //   ② 实例名清洗（template_instantiation.cpp Step 2）同样吃 toString。
            //   即"类型树的形状"与"人读串"必须是单射 —— 这是 ⑩ 那条教训的
            //   同族问题：名字是另一层的键。
            //   对照 clang：TypePrinter::printQualifiedType 按 TypeClass 分派，
            //   指针的 const 走 printPointer 输出 "int *const"。
            if (innerType->isPointer()) {
                return (innerType->pointeeType ? innerType->pointeeType->toString() : "?")
                       + "* const";
            }
            if (innerType->isLValueReference()) {
                return (innerType->referencedType ? innerType->referencedType->toString() : "?")
                       + "& const";
            }
            if (innerType->isRValueReference()) {
                return (innerType->referencedType ? innerType->referencedType->toString() : "?")
                       + "&& const";
            }
            return "const " + innerType->toString();
        }
        case TypeKind::Class: {
            // 嵌套类型名：印成限定者::成员（S<int>::type），与源码书写一致。
            // 必须与普通类名区分开 —— 否则 S<int>::type 与类 "type" 同名，
            // 又要重演 docs/learn/23 里"人读串不单射"的老问题。
            if (isNestedName()) {
                return (nestedQualifier ? nestedQualifier->toString() : "?")
                       + "::" + name;
            }
            // P3 / NTTP：模板 id（Box<int>、Buf<4>）带上实参打印——人读形态对齐源码书写
            // TemplateArg::toString 已按 kind 分派：类型实参取类型名，值实参取数字。
            if (templateArgs.empty()) return name;
            std::string s = name + "<";
            for (size_t i = 0; i < templateArgs.size(); i++) {
                if (i > 0) s += ", ";
                s += templateArgs[i].toString();
            }
            return s + ">";
        }
        case TypeKind::TemplateParam: return templateParamName;
        case TypeKind::Auto:   return "auto";
        case TypeKind::Decltype:
            // 未求值形态：打印成 decltype(...)，括号数按原文还原，便于日志观察
            return decltypeParen ? "decltype((...))" : "decltype(...)";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// stripReferences / stripConst：去除修饰，获取"裸类型"
// ─────────────────────────────────────────────────────────────────────────────
// 推导时常用：先剥掉实参/形参的引用与 const，再比较"裸类型"
// （对应 [temp.deduct.call] 中对实参类型做的退化调整）。
// demo：stripReferences(RValueReference(Int)) → Int；stripReferences(Int) → nullptr
//       stripConst(Const(Int)) → Int；stripConst(Int) → nullptr
// （返回 nullptr 表示"无可剥的修饰"，调用方据此回退到原类型。）
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
