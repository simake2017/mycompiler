#pragma once
// =============================================================================
// 类型系统 (Type System)
// =============================================================================
// 类型是编译器理解程序语义的基石。
// 编译期看"符号"（类型名、字段名），运行期看"偏移量"（内存布局）。
// 类型系统在这里负责：
//   1. 表示所有可能的类型（基础类型、类类型、指针类型、模板参数类型）
//   2. 存储类的内存布局信息（字段偏移量、vtable 结构）
//   3. 支持类型比较和推导
// =============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace minicc {

// 前向声明
struct Type;
using TypePtr = std::shared_ptr<Type>;

// ─────────────────────────────────────────────────────────────────────────────
// TypeKind：类型的种类
// ─────────────────────────────────────────────────────────────────────────────
enum class TypeKind : uint8_t {
    Void,           // void
    Bool,           // bool
    Int,            // int
    Double,         // double
    Pointer,        // T*
    LValueReference, // T&（左值引用）
    RValueReference, // T&&（右值引用 / 万能引用，当 T 是模板参数时）
    Const,          // const T（常量限定）
    Class,          // 类类型（含内存布局信息）
    TemplateParam,  // 模板参数占位符（如 T）
    Auto,           // auto 占位符（等待推导）
};

// ─────────────────────────────────────────────────────────────────────────────
// AccessModifier：访问修饰符
// ─────────────────────────────────────────────────────────────────────────────
enum class AccessModifier : uint8_t {
    Public,
    Private,
    Protected,
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldInfo：类的字段信息（运行期看偏移量）
// ─────────────────────────────────────────────────────────────────────────────
struct FieldInfo {
    std::string name;
    TypePtr     type;
    uint32_t    offset  = 0;     // 字段在对象内存中的字节偏移量
    uint32_t    size    = 0;     // 字段占用的字节数
    AccessModifier access = AccessModifier::Public;
};

// ─────────────────────────────────────────────────────────────────────────────
// VTableEntry：虚函数表中的一个槽位
// ─────────────────────────────────────────────────────────────────────────────
struct VTableEntry {
    std::string mangledName;     // 经过 name mangling 的函数符号名
    uint32_t    index   = 0;     // 在 vtable 中的索引（0-based）
    bool        isOverridden = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// ClassLayout：类的完整内存布局
// ─────────────────────────────────────────────────────────────────────────────
// 这是"运行期看偏移量"的核心数据结构。
// 编译器在此计算好每个字段的绝对偏移量，后续代码生成直接使用这些数字。
// ─────────────────────────────────────────────────────────────────────────────
struct ClassLayout {
    std::string              className;
    uint32_t                 totalSize   = 0;    // 整个对象的字节大小
    bool                     hasVTable   = false; // 是否有虚函数表
    std::vector<FieldInfo>   fields;             // 所有字段（含继承的）
    std::vector<VTableEntry> vtableEntries;      // 虚函数表条目

    // vtable 槽位 -1：RTTI type_info 指针（位于 vtable 起始地址的前一个指针位置）
    // 在汇编层面：vtable[-1] = type_info_address
    std::string              rttiMangledName;    // RTTI 符号名

    // 查找字段偏移量
    const FieldInfo* findField(const std::string& name) const {
        for (auto& f : fields) {
            if (f.name == name) return &f;
        }
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Type：类型的统一表示
// ─────────────────────────────────────────────────────────────────────────────
struct Type {
    TypeKind kind;
    std::string name;           // 类型名称（如 "int", "MyClass", "T"）

    // ── 指针类型特有 ──
    TypePtr pointeeType;        // 指向的类型（仅 Pointer 类型使用）

    // ── 引用类型特有（T& 和 T&&）──
    TypePtr referencedType;     // 被引用的类型（仅 LValueReference / RValueReference 使用）

    // ── const 类型特有 ──
    TypePtr innerType;          // const 限定的内部类型（仅 Const 使用）

    // ── 类类型特有 ──
    ClassLayout classLayout;    // 类的内存布局（仅 Class 类型使用）

    // ── 模板参数类型特有 ──
    std::string templateParamName; // 模板参数名（如 "T"）

    // ── 工厂方法 ──
    static TypePtr makeVoid();
    static TypePtr makeBool();
    static TypePtr makeInt();
    static TypePtr makeDouble();
    static TypePtr makePointer(TypePtr pointee);
    static TypePtr makeLValueReference(TypePtr referenced);   // T&
    static TypePtr makeRValueReference(TypePtr referenced);   // T&&
    static TypePtr makeConst(TypePtr inner);                  // const T
    static TypePtr makeClass(const std::string& name);
    static TypePtr makeTemplateParam(const std::string& paramName);
    static TypePtr makeAuto();

    // ── 类型查询 ──
    bool isVoid()             const { return kind == TypeKind::Void; }
    bool isBool()             const { return kind == TypeKind::Bool; }
    bool isInt()              const { return kind == TypeKind::Int; }
    bool isDouble()           const { return kind == TypeKind::Double; }
    bool isPointer()          const { return kind == TypeKind::Pointer; }
    bool isLValueReference()  const { return kind == TypeKind::LValueReference; }
    bool isRValueReference()  const { return kind == TypeKind::RValueReference; }
    bool isReference()        const { return isLValueReference() || isRValueReference(); }
    bool isConst()            const { return kind == TypeKind::Const; }
    bool isClass()            const { return kind == TypeKind::Class; }
    bool isTemplateParam()    const { return kind == TypeKind::TemplateParam; }
    bool isAuto()             const { return kind == TypeKind::Auto; }
    bool isNumeric()          const { return isInt() || isDouble(); }

    // 去除引用和 const 的"裸类型"（用于类型比较和推导）
    TypePtr stripReferences() const;
    TypePtr stripConst() const;

    // 获取类型的大小（字节）
    uint32_t sizeInBytes() const;

    // 类型比较
    bool equals(const TypePtr& other) const;

    // 获取可读的类型名
    std::string toString() const;
};

} // namespace minicc
