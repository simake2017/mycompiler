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
// decltype 的操作数（见 TypeKind::Decltype）。
// ★ 此处只前置声明、不 include ast.h：ast.h 反过来依赖 type.h，
//   互相 include 会成环。只存指针，不需要完整类型。
struct Expression;
using DecltypeExprPtr = std::shared_ptr<Expression>;

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
    Decltype,       // decltype(expr) —— 半成品类型，替换后才求值（见下方"两段式"说明）
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

// =============================================================================
// TemplateArg：模板实参的 tagged 值（[temp.arg]）
// =============================================================================
// 【为什么需要它】模板实参不只有"类型"一种形态。C++20 允许四类：
//     template<class T>  → 类型实参        Box<int>
//     template<int N>    → 非类型实参 NTTP  Buf<4>       ★ 本结构新增的形态
//     template<template<class> class TT> → 模板模板实参（本项目未实现）
//     包展开 ...                                                        （未实现）
//
// 旧实现把实参一律存成 TypePtr，于是 template<int N> 的实参 4 **无处安放**
// ——4 是一个值，不是类型，TypePtr 结构上就表达不了。这里用一个 tagged union
// 把"类型 or 值"这一对形态装进同一个槽位。
//
// 【对照 clang】clang::TemplateArgument（clang/AST/TemplateBase.h）
//   是一个真正四形态的 tagged union：
//     struct TemplateArgument {
//       enum ArgKind { Null, Type, Declaration, NullPtr, Integral,
//                      Template, TemplateExpansion, Expression, Pack };
//       union { TypeSourceInfo *TypeInfo; ValueDecl *Decl; ... };
//       llvm::APSInt Integer;   // ← Integral 形态用
//     };
//   本实现只取其中两形态（Type / Integral），够讲清"值 vs 类型"这条主线。
//   形参侧对应 clang 的 TemplateTypeParmDecl / NonTypeTemplateParmDecl
//   （clang/AST/DeclTemplate.h），本项目对应 ast.h 的 TemplateParamKind。
//
// 【demo】template<int N> class Buf → Buf<4>
//     TemplateParam{kind=NonType, name="N", nonType=int}
//     TemplateArg  {kind=Integral, value=4}
//     ⇒ 替换表 { "N" → TemplateArg{Integral, 4} }
// =============================================================================
enum class TemplateArgKind : uint8_t {
    Type,     // 类型实参：Box<int>         →  payload 在 type 字段
    Integral, // 非类型实参：Buf<4>（NTTP） →  payload 在 value 字段
};

struct TemplateArg {
    TemplateArgKind kind = TemplateArgKind::Type;
    TypePtr         type  = nullptr; // kind == Type     时有效
    int64_t         value = 0;       // kind == Integral 时有效

    TemplateArg() = default;

    // 隐式转换：TypePtr → 类型实参。
    // 对照 clang：clang::TemplateArgument 同样有非 explicit 的转换构造
    //（TemplateArgument(QualType, TypeSourceInfo*) / (ValueDecl*) 等），
    // 因此 clang 代码里处处能写 `TemplateArgument(Ty)`。
    // 本实现只对【类型】开这个口子，【值】实参必须显式写 ofValue(4) ——
    // 于是 `{{"T", Type::makeInt()}}` 读起来自然，而写 NTTP 实参时
    // 必须显式表态，刻意保留"这是值不是类型"的书写摩擦。
    TemplateArg(TypePtr t)  // NOLINT(*-explicit-constructor)
        : kind(TemplateArgKind::Type), type(std::move(t)) {}

    static TemplateArg ofType(TypePtr t) {
        TemplateArg a; a.kind = TemplateArgKind::Type; a.type = std::move(t); return a;
    }
    static TemplateArg ofValue(int64_t v) {
        TemplateArg a; a.kind = TemplateArgKind::Integral; a.value = v; return a;
    }

    bool isType()  const { return kind == TemplateArgKind::Type; }
    bool isValue() const { return kind == TemplateArgKind::Integral; }

    // 人读形态：类型实参取类型名，值实参取十进制数字
    // demo：ofType(makeInt()) → "int"；ofValue(4) → "4"
    // 定义放 src/type.cpp —— 此处置于 Type 定义之前，Type 尚不完整，
    // 无法内联调用 Type::toString()。
    std::string toString() const;
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
//   const int&     LValueReference(Const(Int))          KRi   ← const 在【内层】
//   int* const     Const(Pointer(Int))                  KPi   ← const 在【外层】
//
//   ASCII：const int& 的嵌套（外 → 内）——说明符侧的 const 修饰的是【基类型】
//        LValueReference
//         └─ referencedType ──► Const
//                                 └─ innerType ──► Int
//
//   ASCII：int* const 的嵌套（外 → 内）——声明符侧的 const 修饰的是【指针本身】
//        Const
//         └─ innerType ──► Pointer
//                            └─ pointeeType ──► Int
//
//   ★ 两者不可混淆：const 写在哪一侧，就修饰谁（[dcl.type.cv]）。
//     详见 docs/learn/23 与 tests/tmpl/test_tmpl_47_cv_position.cpp。
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
//   ★ 折叠是【引用类型的不变量】，在构造点完成 —— 见 src/type.cpp 的
//     Type::makeLValueReference / makeRValueReference（[dcl.ref]/6 四行合一）。
//     早先只在 TemplateInstantiator::substituteType 里折叠，等于把规范化绑死在
//     一条路径上：推导万能引用时 bind 出的 `T := A&`（A 本身已是引用）没人收拾，
//     于是系统里出现非法的嵌套引用节点 `int& &`（伪造符号 _Z2idIRRiE）。
//     集中到工厂函数后，"不存在嵌套引用节点"成为类型系统的不变量。
//     对照 clang：Sema::BuildReferenceType（clang/lib/Sema/SemaType.cpp:1887）
//     是折叠的唯一实现点，canonical type 在构造时即算好
//     （ASTContext::getLValueReferenceType，clang/lib/AST/ASTContext.cpp:4163）。
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

    // ── 类模板实参（P3 / NTTP）──
    // Box<int> 解析为 Class(name="Box", templateArgs=[Type:int])；
    // Buf<4>   解析为 Class(name="Buf", templateArgs=[Integral:4])。
    // ★ 用 TemplateArg（tagged 值）而非 TypePtr —— 见上方 TemplateArg 注释：
    //    类型实参与非类型实参（NTTP 的值）必须共存于同一槽位，
    //    否则 template<int N> 的 4 无处安放。
    // 语义阶段 resolveType 见到非空实参 → 触发按需实例化（见
    // SemanticAnalyzer::getOrInstantiateClass），产出具体实例类型
    // （如 Box_int）后整体替换本节点——即"模板 id 是半成品类型，
    // 实例化后才成为完整类型"（[temp.inst] 的落地形态）。
    std::vector<TemplateArg> templateArgs;

    // ── 嵌套/依赖类型名（`S<int>::type`、`T::type`）──
    // 【表示】qualifier 非空 ⇒ 本节点表示"qualifier 所指数类型里的成员类型别名"：
    //     nestedQualifier = S<int>（半成品，交给 resolveType 按需实例化）
    //     name            = "type"（成员名）
    //   `T::type` 里 qualifier 就是 TemplateParam 节点（依赖情形，见 docs/learn/24）。
    // 【为什么单列一组字段】它既不是"类名"（不能拿去查 m_classTypes），
    //   也不是"模板 id"（没有实参可实例化）—— 而是一个【待解析的路径】：
    //   先把 qualifier 解析成具体类，再去那个类的 typeAliases 里取成员。
    //   对照 clang：DependentNameType / ElaboratedType 的 qualifier + NamedDecl。
    TypePtr     nestedQualifier;    // 限定部分（如 S<int> 或 T）
    bool isNestedName() const { return nestedQualifier != nullptr; }

    // ── 实例"出身"（模板实例类型专有）──
    // 【要解决什么】实例化后的类型叫 `MyPtr_int`，模板 id 时代的信息
    //   （哪个模板、哪些实参）在改名的那一刻就丢了。而函数模板实参推导
    //   必须回答："`MyPtr_int` 是 `MyPtr<T>` 对 T 的一次成功绑定吗？"
    //   —— 只靠名字 `MyPtr_int` 反推是不可靠的（名字是清洗过的可读串，
    //   不是单射）。故在实例化时把出身显式记下来。
    // 【demo】MyPtr<int> 实例化后：
    //     name = "MyPtr_int"（实例类型名，参与布局与 mangling）
    //     templateOriginName = "MyPtr"        ← 出身模板
    //     templateOriginArgs = [Type:int]     ← 实例化用的实参
    //   推导时对 P=MyPtr<T> 与 A=MyPtr_int：出身同名 + 实参个数相等
    //   ⇒ 逐位合一 ⇒ T := int。
    // 【为什么不复用上面的 templateArgs】那个字段的语义是"待实例化的半成品"
    //   ——resolveType 见到非空 templateArgs 就会去触发实例化。实例类型若也
    //   填这个字段，每次解析它都会重走一遍实例化分支。出身字段是【只读记录】，
    //   不参与任何解析决策，故必须分开。
    // 对照 clang：ClassTemplateSpecializationDecl 自身就带着 TemplateArgumentList
    //   （clang 不存在"改名后丢实参"的问题，因为实例类型仍是一个 Decl）。
    std::string              templateOriginName;
    std::vector<TemplateArg> templateOriginArgs;
    bool isTemplateInstance() const { return !templateOriginName.empty(); }

    // ── 模板参数类型特有 ──
    std::string templateParamName; // 模板参数名（如 "T"）

    // ── decltype 类型特有（TypeKind::Decltype）──
    // ★ 两段式：decltype 出现时不立刻求值，先原样留存表达式，
    //   等【替换】（substituteType）阶段再求。
    //   原因：is_range<T, void_t<decltype(declval<T>().begin())>> 这种写法里
    //   decltype 位于【模板模式】中，此刻 T 未知 —— 立即求值无从下手。
    //   对照 clang：DecltypeType 在依赖上下文中就是依赖类型，
    //   直到 Sema::SubstType 才被 Instantiator 求值成具体类型。
    DecltypeExprPtr decltypeExpr;              // 操作数表达式（半成品，替换后求值）
    bool            decltypeParen = false;     // 是否多套了一层括号，见 [dcl.type.decltype]

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
    // decltype(expr)：paren 表示原文是否写成 decltype((e))，见 [dcl.type.decltype]
    static TypePtr makeDecltype(DecltypeExprPtr expr, bool paren);

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
    bool isDecltype()         const { return kind == TypeKind::Decltype; }
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
