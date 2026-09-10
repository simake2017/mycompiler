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
//
// 【在管线中的位置】
//   Preprocessor → Lexer → Parser → SemanticAnalyzer → TemplateDeduction/
//   Instantiation → CodeGen
//   type.h 横跨所有阶段，是语义信息的"通货"：
//     · Parser              parseType() 构造 Type（见 src/parser.cpp:168）
//     · SemanticAnalyzer    填充 Expression::resolvedType、计算 ClassLayout 偏移
//     · TemplateInstantiator::substituteType 对 Type 做 T→实际类型 的结构化替换
//                           （含引用折叠，见 src/template_instantiation.cpp:280）
//     · NameMangler::encodeType 把 Type 编码为 Itanium mangling 字符
//
// 【对应 C++ 标准章节】
//   [basic.type]          类型总览
//   [basic.fundamental]   基础类型 void/bool/int/double
//   [dcl.ptr]             指针 T*
//   [dcl.ref]             引用 T& / T&&（引用折叠 [dcl.ref]/6）
//   [dcl.type.cv]         const 限定
//   [dcl.spec.auto]       auto 占位符
//   [class] / [class.mem] / [class.virtual]   类、成员布局、虚函数/vtable
//   [temp.param] / [temp.deduct]              模板参数类型与实参推导
//
// 【对应 clang 模块】
//   include/clang/AST/Type.h        Type / BuiltinType / PointerType /
//                                   LValueReferenceType / RValueReferenceType /
//                                   RecordType / TemplateTypeParmType / AutoType
//   include/clang/AST/Decl.h        FieldDecl（对应本文件 FieldInfo）
//   include/clang/AST/RecordLayout.h ASTRecordLayout（对应本文件 ClassLayout）
//   差异说明：clang 用 QualType + Qualifiers 承载 const（不单独建节点）；
//             教学实现把 const 建成独立的 TypeKind::Const 节点，便于观察与讲解。
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
// TypeKind ↔ clang 类型类 ↔ Itanium mangling 编码 对照表
// ─────────────────────────────────────────────────────────────────────────────
//   TypeKind          clang 对应（include/clang/AST/Type.h）   encodeType 编码
//   Void              BuiltinType::Void                        v
//   Bool              BuiltinType::Bool                        b
//   Int               BuiltinType::Int                         i
//   Double            BuiltinType::Double                      d
//   Pointer           PointerType                              P + 内层
//   LValueReference   LValueReferenceType                      R + 内层
//   RValueReference   RValueReferenceType                      O + 内层
//   Const             （clang 用 Qualifier，非独立 Type 节点）  K + 内层
//   Class             RecordType / CXXRecordDecl               <名字长度><名字>
//   TemplateParam     TemplateTypeParmType                     参数名原样输出
//   Auto              AutoType                                 （阶段3后应已消除）
//   encodeType 的实现见 src/template_instantiation.cpp 的 NameMangler::encodeType。
// ─────────────────────────────────────────────────────────────────────────────

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
// 对应 clang::FieldDecl（include/clang/AST/Decl.h）+ ASTRecordLayout 中的字段偏移。
// demo：class Point { int x; int y; };
//   fields = [ FieldInfo{name=x, type=int, offset=0, size=4},
//              FieldInfo{name=y, type=int, offset=4, size=4} ]
//   （int 按 4 字节对齐，x 在偏移 0，y 紧随其后在偏移 4）
struct FieldInfo {
    std::string name;
    TypePtr     type;
    uint32_t    offset  = 0;     // 字段在对象内存中的字节偏移量
    uint32_t    size    = 0;     // 字段占用的字节数
    AccessModifier access = AccessModifier::Public;
    std::string sourceClass;     // 字段来源的类名（空 = 自身字段；非空 = 继承自该类）
};

// ─────────────────────────────────────────────────────────────────────────────
// VTableEntry：虚函数表中的一个槽位
// ─────────────────────────────────────────────────────────────────────────────
// 对应 clang 的 vtable 布局计算（clang/lib/CodeGen/VTables.cpp）。
// 含虚函数的类，对象头部藏一个 vptr 指向 vtable；vtable 本质是函数指针数组。
// demo：class Base { virtual void foo(); };
//   vtableEntries = [ VTableEntry{mangledName="_ZN4Base3fooEv", index=0} ]
struct VTableEntry {
    std::string mangledName;     // 经过 name mangling 的函数符号名
    uint32_t    index   = 0;     // 在 vtable 中的索引（0-based）
    bool        isOverridden = false;
    std::string baseFunctionName; // 原始方法名（override 检测用，不受 mangledName 更新影响）
    // 多继承 thunk 调整量（[class.mi] + Itanium ABI 2.4）：
    // 非 0 时，本槽位不能直接填函数地址，而要填一个跳板（thunk）——
    // 跳板先把 this 加上 vptr[-2]（offset-to-top）归顶，再跳真实函数。
    // 仅出现在【次表的覆写槽】：基类自己的函数期待基类 this（调用方
    // 本来就传基类指针），无需调整；覆写函数期待最派生类 this，才要调整。
    int         thunkAdjust = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// BaseSubobject：多继承下的基类子对象信息（[class.mi]）
// ─────────────────────────────────────────────────────────────────────────────
// class D : A, B 的对象 = A 子对象 + B 子对象 + D 自身字段，按声明顺序摆放。
// 每个多态基类子对象自带一个 _vptr；第一个多态基类（主基类）与派生类共享
// 主虚表（Itanium 主基类优化），其余基类各占一张次表。
struct BaseSubobject {
    std::string baseClassName;        // 基类名
    uint32_t    offset = 0;           // 子对象在完整对象中的起始偏移
    bool        hasVTable = false;    // 该子对象是否带 _vptr（基类是否多态）
    bool        isPrimary = false;    // 是否主基类（与派生类共享主表）
    uint32_t    vtableSegmentOffset = 0;  // 本表段在 _ZTV 符号内的字节偏移
                                          //（vptr = &_ZTV + 段偏移 + 16）
    std::vector<VTableEntry> entries;     // 次表槽位（仅非主多态基类有意义）
};

// ─────────────────────────────────────────────────────────────────────────────
// ClassLayout：类的完整内存布局
// ─────────────────────────────────────────────────────────────────────────────
// 这是"运行期看偏移量"的核心数据结构。
// 编译器在此计算好每个字段的绝对偏移量，后续代码生成直接使用这些数字。
// 布局计算见 src/semantic_analyzer.cpp 的类注册路径（约 :226~:373）：
//   先并入基类的 fields/vtableEntries，再累加本类字段偏移，最后分配 vtable 槽位。
//
// 对象内存示意（class Derived : Base，Base 有虚函数，Derived 新增 int d;）：
//   偏移 0   ┌─────────────────────────┐
//            │ vptr (8B) ─────────────►│ vtable 符号 _ZTV7Derived
//   偏移 8   ├─────────────────────────┤   [-1] _ZTI7Derived（RTTI type_info）
//            │ Base 继承来的字段 ...    │   [0]  Derived::foo（虚函数槽位 0）
//   偏移 k   ├─────────────────────────┤
//            │ int d (4B)              │
//            └─────────────────────────┘
//   totalSize = 按最大对齐数对齐后的对象总字节数
struct ClassLayout {
    std::string              className;
    uint32_t                 totalSize   = 0;    // 整个对象的字节大小
    bool                     hasVTable   = false; // 是否有虚函数表
    std::vector<FieldInfo>   fields;             // 所有字段（含继承的，名字已限定）
    std::vector<VTableEntry> vtableEntries;      // 主虚表条目（主基类槽位合并结果）

    // 多继承扩展（[class.mi]）：全部基类子对象（含单继承的 0/1 个）。
    // 单继承时 = {一个元素, offset=0, isPrimary=true}，行为退化为原实现。
    // CodeGen 依据它：① 摆放对象内各 _vptr；② 发射次表与 thunk；
    // ③ 上/下转型的指针调整量；④ RTTI 的基类数组（typeinfo + 偏移）。
    std::vector<BaseSubobject> bases;

    // vtable 槽位 -1：RTTI type_info 指针（位于 vtable 起始地址的前一个指针位置）
    // 在汇编层面：vtable[-1] = type_info_address
    std::string              rttiMangledName;    // RTTI 符号名

    // 查找字段偏移量
    // 精确匹配 + 按 sourceClass 的限定名匹配（MI 下继承字段名带 "BaseName." 前缀）
    const FieldInfo* findField(const std::string& name) const {
        // ① 精确匹配（自身字段 或 全限定名 "A.a"）
        for (auto& f : fields) {
            if (f.name == name) return &f;
        }
        // ② 限定名匹配：name="a" 时，试 "A.a" / "B.a"（按 sourceClass 分组）
        for (auto& f : fields) {
            if (!f.sourceClass.empty() && f.name == f.sourceClass + "." + name)
                return &f;
        }
        return nullptr;
    }
};

// =============================================================================
// 【指针 / 引用 / const 的组合规则】
// =============================================================================
// 本项目的 Type 是"洋葱式"嵌套结构：每个修饰符都是独立的 Type 节点，
// 通过 pointeeType / referencedType / innerType 三条链指向内层被修饰类型。
//
// 重要：本项目 Parser 的组合顺序（见 src/parser.cpp parseType :168）——
//   1) const 作为"前缀"先被记下；2) 解析基础类型；3) 依次叠加后缀 * / & / &&；
//   4) 最后才把 const 包在最外层。因此 const 总是最外层节点（教学简化）。
//   注意这与真实 C++ 不同：真实 C++ 里 `const int*`（指向 const int 的指针）
//   与 `int* const`（const 的、指向 int 的指针）是两种不同类型；本项目语法
//   只允许 const 前缀，统一按"最外层 const"处理。
//
//   源码           Type 结构（外 → 内）                 encodeType
//   int            Int                                  i
//   int*           Pointer(Int)                         Pi
//   int&           LValueReference(Int)                 Ri
//   int&&          RValueReference(Int)                 Oi
//   const int      Const(Int)                           Ki
//   int**          Pointer(Pointer(Int))                PPi
//   const int*     Const(Pointer(Int))                  KPi   ← const 在最外层
//   const int&     Const(LValueReference(Int))          KRi   ← const 在最外层
//
//   ASCII：const int& 的嵌套（外 → 内）
//        Const
//         └─ innerType ──► LValueReference
//                             └─ referencedType ──► Int
//
// 【引用折叠（Reference Collapsing）】 标准依据：[dcl.ref]/6；
//   模板实参推导产生嵌套引用时见 [temp.deduct.call] / [temp.deduct.type]。
//   普通 C++ 不允许写"引用的引用"，但 T&& 的模板替换会产生它。折叠规则：
//        T&  &  → T&     T&  && → T&     T&& &  → T&     T&& && → T&&
//   一句话：只要有一层是左值引用，结果就是左值引用（& 永远赢）。
//   这是"万能引用/转发引用"的原理：
//        template<typename T> void foo(T&& x);
//        foo(42);   ⇒ T=int,   T&& = int&&           （右值引用）
//        foo(var);  ⇒ T=int&,  T&& = int& && → int&   （折叠为左值引用）
//   折叠不在本文件实现，由 TemplateInstantiator::substituteType 完成
//   （src/template_instantiation.cpp:280），但被折叠的对象正是这里的 Type 节点。
// =============================================================================

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

    // ── 类模板实参（P3）──
    // Box<int> 解析为 Class(name="Box", templateArgs=[int])。
    // 语义阶段 resolveType 见到非空实参 → 触发按需实例化（见
    // SemanticAnalyzer::getOrInstantiateClass），产出具体实例类型
    // （如 Box_int）后整体替换本节点——即"模板 id 是半成品类型，
    // 实例化后才成为完整类型"（[temp.inst] 的落地形态）。
    std::vector<TypePtr> templateArgs;

    // ── 模板参数类型特有 ──
    std::string templateParamName; // 模板参数名（如 "T"）

    // ── 工厂方法 ──
    // 每个 make* 返回一个新构造的 Type 节点；组合规则见上方"洋葱式"说明。
    // 各工厂函数的入参含义、产物形状与 encodeType 结果见 src/type.cpp 对应实现。
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
