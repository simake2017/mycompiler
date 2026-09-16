#pragma once
// =============================================================================
// 阶段 4：模板实例化引擎 (Template Instantiation Engine)
// =============================================================================
// 核心职责：
//   1. 识别代码中对模板的使用（如 MyPtr<int>）
//   2. 深拷贝模板 AST 蓝图
//   3. 将所有模板参数占位符（T）替换为真实类型（int）
//   4. 生成全局唯一的符号名（Name Mangling）
//   5. 将实例化后的类注册到全局类型表中
//
// 模板的本质：编译期的"复制粘贴"——但不是简单的文本替换，
// 而是 AST 层面的结构化克隆与类型替换。
//
// 管线位置（阶段 4，推导 S5）：
//   Parser(蓝图) → SemanticAnalyzer(发现实例化需求) → TemplateDeducer(推导实参)
//   → 【本文件：TemplateInstantiator 按实参克隆 + 替换，产出具体类/函数】
//   → SemanticAnalyzer(对实例做类型检查) → CodeGen
//
// 对应 C++ 标准章节：
//   [temp.inst]   模板实例化的时机与规则（隐式/按需实例化）
//   [temp.subst]  模板实参替换：把模板参数替换为实参类型
//   [dcl.ref]     引用折叠规则（万能引用实例化的关键）
// 对照 clang：
//   lib/Sema/SemaTemplateInstantiate.cpp —— 声明级实例化（本文件 instantiate*）
//   lib/Sema/TreeTransform.h             —— AST 递归重建（本文件 cloneExpr/cloneStmt）
//   区别：clang 用 SubstTemplateTypeParmType 类型节点记录替换，minicc 在克隆时直接换类型
//
// 实例化 = 结构化替换（MyPtr<int>）：
//   蓝图 template<typename T> class MyPtr          实例 MyPtr_int
//   ┌──────────────────────────────┐  {T→int}  ┌──────────────────────────────┐
//   │ field data : T*              │ ────────▶ │ field data : int*            │
//   │ method get() : T&            │           │ method get() : int&          │
//   └──────────────────────────────┘           └──────────────────────────────┘
//   符号名：_Z5MyPtrIiE（Itanium ABI mangling）
// =============================================================================

#include "ast.h"
#include "type.h"
#include "sfinae.h"      // SubstitutionFailure（替换失败信号）+ SFINAE 协议
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <stdexcept>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// SubstitutionFailure 已迁出本文件 → include/sfinae.h
// ─────────────────────────────────────────────────────────────────────────────
// 【为什么搬走】它是"替换失败信号"，被推导（TemplateDeducer）、语义
//   （SemanticAnalyzer）、实例化（TemplateInstantiator）三方共用，
//   挂靠在"实例化"名下会让读代码的人找不到 SFINAE 的入口。
//   现在它连同整套 SFINAE 协议一起住在 include/sfinae.h。
//
// 本头文件仍 include sfinae.h —— 因为 DecltypeEvaluator 的契约里
//   "求值失败抛 SubstitutionFailure" 是它的一部分。

// ─────────────────────────────────────────────────────────────────────────────
// DecltypeEvaluator：decltype 求值的回调接口
// ─────────────────────────────────────────────────────────────────────────────
// 【要解决的分层问题】
//   decltype 的求值能力（"表达式 → 类型"）住在 SemanticAnalyzer 里（inferType）；
//   而需要触发求值的地方是 TemplateInstantiator::substituteType。
//   若让 instantiator 直接依赖 SemanticAnalyzer，会：
//     ① 形成 Sema ⇄ Instantiator 双向依赖；
//     ② 让 tests/unit/ 里那些【裸构造】TemplateDeducer / TemplateInstantiator
//        的单元测试被迫拖进整个 Sema。
//   故抽成抽象接口：Sema 实现它，instantiator 只持一个可选指针。
//
// 【可选语义】指针为 nullptr 时（单元测试路径），decltype 节点**不求值**，
//   原样保留。调用方据此可观察到"延迟求值"这一事实本身。
//
// 对照 clang：Sema::SubstType 内部直接调 BuildDecltypeType，
//   因为 clang 的 Instantiator 本身就是 Sema 的一部分（TreeTransform 派生自 Sema）。
//   本实现把这一步外提成接口，换取分层清晰与可单测。
class DecltypeEvaluator {
public:
    virtual ~DecltypeEvaluator() = default;

    // 求 decltype(expr) 的类型。
    //   expr  —— 已完成替换的操作数表达式
    //   paren —— 原文是否为 decltype((e)) 形态（[dcl.type.decltype] 两套规则）
    // 返回：求得的类型。
    // 抛：SubstitutionFailure —— 表达式在该上下文中不合法（SFINAE 软失败）。
    virtual TypePtr evaluateDecltype(const ExprPtr& expr, bool paren) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// MemberTypeResolver：依赖类型名 `typename T::type` 的成员查表回调
// ─────────────────────────────────────────────────────────────────────────────
// 【要解决的分层问题】与 DecltypeEvaluator 完全同构：
//   "去某个类里查成员类型别名"这件事住在 SemanticAnalyzer 里
//   （它才有 m_classDecls / typeAliases / 按需实例化能力），
//   而需要它的地方是替换引擎 TemplateInstantiator::substituteType
//   （以及推导器 reducePattern 里临时建的那个 instantiator）。
//   直接依赖 Sema 会把 Sema ⇄ Instantiator 变成双向依赖。
//
// 【为什么必须在【替换】阶段就查、不能拖到 Sema 解析期】
//   因为这里是 [temp.deduct]/8 的**直接上下文**：
//     template<class T> using has_type = void_t<typename T::type>;
//   对 T := int 替换时 `int::type` 不存在 ⇒ 必须在替换的当场失败，
//   由 Sfinae::attempt 吸收成"该候选不成立"；拖到后面就变成硬错误，
//   SFINAE 探测惯例整个失效。
//
// 对照 clang：Sema::SubstType 里对 DependentNameType 直接调
//   Sema::getTypeName + LookupQualifiedName，失败即
//   Sema::SubstitutionFailure（因为 TreeTransform 本身就是 Sema 的一部分）。
class MemberTypeResolver {
public:
    virtual ~MemberTypeResolver() = default;

    // 在 qualifier 所指的类里查成员类型别名 member。
    //   qualifier —— 已完成替换的限定者（如 int / Plain / Box_int）
    // 返回：查到的成员类型（已解糖）。
    // 抛：SubstitutionFailure —— 查不到，或限定者根本不是类（软失败）。
    virtual TypePtr resolveMemberType(const TypePtr& qualifier,
                                      const std::string& member) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// AliasTemplateResolver：别名模板 id `X<...>` 的展开回调（[temp.alias]）
// ─────────────────────────────────────────────────────────────────────────────
// 【要解决的分层问题】与上面两个接口同构，第三次出现同一个形状 ——
//   "别名模板蓝图住在 Sema（它才有模板注册表），而需要用它的地方是替换引擎"。
//   若让 Instantiator 直接持有 Sema，Sema ⇄ Instantiator 立刻变成双向依赖；
//   故仍旧是"接口在低层、实现由 Sema 注入"。
//
// 【为什么替换阶段也必须能展开】这是 [temp.alias]/1 与 [temp.deduct]/8 的交汇：
//     template<class T> std::enable_if_t<sizeof(T) >= 4, T> f(T x);
//   返回类型里的 enable_if_t<...> 是【依赖】的 —— 定义期 T 未知，函数体
//   两阶段查找的第一阶段根本查不动它；必须等调用点推导出 T := int 后，
//   在替换的当口展开成 int（或展开失败 ⇒ 该候选被 SFINAE 剔除）。
//   拖到 Sema 解析期再展开就晚了：那时已经不在直接上下文里，失败会变成硬错误。
//
// 对照 clang：Sema::SubstType 里对 TemplateSpecializationType 判
//   isTypeAlias() 后直接走 Sema::CheckAliasTemplateId +
//   getCanonicalType 解糖（clang/lib/Sema/SemaTemplateInstantiate.cpp）。
class AliasTemplateResolver {
public:
    virtual ~AliasTemplateResolver() = default;

    // name 是否是已登记的别名模板。
    // 【为什么要单独一个查询】推导器每匹配一个模式位都要问一次"这名字要不要
    //   先解糖"；若靠 expandAliasTemplate 返回 nullptr 来判，就得以"试展开"
    //   的方式提问 —— 而试展开会走完整的替换流程，实参个数不符时还会发
    //   Sfinae::fail。判断句不该有副作用，故单列一个纯查询。
    virtual bool isAliasTemplate(const std::string& name) const = 0;

    // 把 `name<args>` 展开成别名所指向类型的替换结果。
    // 返回 nullptr ⇒ name 不是已登记的别名模板（调用方按普通类模板继续处理）。
    // 抛：SubstitutionFailure —— 实参个数不符、或替换途中失败（软失败）。
    virtual TypePtr expandAliasTemplate(const std::string& name,
                                        const std::vector<TemplateArg>& args) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// NameMangler：符号修饰器
// ─────────────────────────────────────────────────────────────────────────────
// 为什么需要 Name Mangling？
//   C++ 支持函数重载和模板，但汇编器/链接器只认唯一的名字。
//   Name Mangling 将函数的完整签名编码为一个全球唯一的字符串。
//
// 例如（简化版 GCC ABI）：
//   MyPtr<int>        → _Z5MyPtrIiE
//   MyClass::foo(int) → _ZN7MyClass3fooEi
//
// 编码规则对齐 Itanium C++ ABI（GCC/Clang 所用）：
//   _Z 前缀、<长度><名字> 的 source-name、I…E 模板实参表、N…E 嵌套限定名
// =============================================================================
class NameMangler {
public:
    // 对模板类实例化生成符号名
    // 例: MyPtr<int> → _Z5MyPtrIiE
    // 函数模板实例同样走这里：twice<int> → _Z5twiceIiE（与源码名区分，避免链接冲突）
    // NTTP（[temp.arg.nontype]）走 Itanium 的 <expr-primary> 分支：
    //   Buf<4>  → _Z3BufILi4EE
    //             └┬┘└┬┘│└┬┘│└┘
    //              3Buf I  L i 4 E E
    //              └ 名字 └ 模板实参表开始
    //                      └ L…E = <expr-primary>（表达式主形，非类型实参专用）
    //                          i  = int 类型编码
    //                          4  = 值（负数编码为 n4，即 -4 → Lin4E）
    //                             末尾 E 收 expr-primary，再末尾 E 收实参表
    // 参照：Itanium C++ ABI §5.1.8 <template-arg> → <expr-primary> → L <type> <value> E
    static std::string mangleTemplateInstance(
        const std::string& templateName,
        const std::vector<TemplateArg>& args);

    // 对函数生成符号名
    // 例: foo(int, double) → _Z3foo id
    static std::string mangleFunction(
        const std::string& funcName,
        const std::string& className,
        const std::vector<Parameter>& params);

    // 生成 RTTI type_info 的符号名
    // 例: MyClass → _ZTI7MyClass
    static std::string mangleRTTI(const std::string& className);

    // 生成 vtable 的符号名
    // 例: MyClass → _ZTV7MyClass
    static std::string mangleVTable(const std::string& className);

private:
    // 将类型编码为 mangling 字符串
    static std::string encodeType(TypePtr type);
};

// ─────────────────────────────────────────────────────────────────────────────
// TemplateInstantiator：模板实例化引擎
// ─────────────────────────────────────────────────────────────────────────────
class TemplateInstantiator {
public:
    // 模板参数名 → 实际实参的映射（公开：推导引擎构造后传入）
    // ★ value 是 TemplateArg（tagged）而非裸 TypePtr：模板形参可以是类型
    //   （typename T）也可以是非类型的值（int N，NTTP），两种形态必须能共存于
    //   同一张表。若 value 仍是 TypePtr，{N → 4} 这条映射根本无处安放——
    //   这正是 NTTP 在同一张表里"活不下来"的根因。
    // 对照 clang：Sema 的 MultiLevelTemplateArgumentList
    //（DeclTemplate.h），每层是 TemplateArgument 列表；单层对应本表。
    using TypeSubstitution = std::unordered_map<std::string, TemplateArg>;

    // 【做什么】类模板实例化 [temp.inst]：深拷贝蓝图 + 结构化替换 [temp.subst]
    // 【demo】MyPtr<int>：替换表 {T→int}，字段 T* data → int* data，
    //         方法中每个 T 换成 int，新类名 MyPtr_int，符号 _Z5MyPtrIiE
    //         Buf<4>（NTTP）：替换表 {N→TemplateArg{Integral,4}}，
    //         方法体 `return N;` 的 VarExpr{N} → IntLiteral{4}
    //
    // 实例化一个模板类
    // templateDecl: 模板蓝图
    // args: 实际的模板实参（类型与值混排，如 [Type:int] 或 [Integral:4]），
    //       顺序与 templateDecl->templateParams 一一对应
    // 返回：实例化后的 ClassDecl
    // substOverride：特化路径的替换表覆盖（见下方 instantiate 实现处的说明）。
    //   nullptr（默认）→ 主模板路径：按 templateParams 的 kind 逐位分派并校验；
    //   非 nullptr    → 特化路径：直接采用该表，跳过逐位校验与个数校验。
    //
    // ★ 为什么用指针而不是"空表即主模板"：全特化（template<> struct Box<int*,int>）
    //   的形参表为空，匹配推导出的替换表**本来就是空的** —— 用 empty() 当判别条件
    //   会把全特化误判成主模板，进而撞上"expects 0 argument(s), got 2"的个数错位。
    //   路径归属是调用方的知识（Sema 的 selectClassTemplate 已经判定过），
    //   必须显式传入，不能从数据里猜。
    ClassDeclPtr instantiate(
        TemplateDeclPtr templateDecl,
        const std::vector<TemplateArg>& args,
        const TypeSubstitution* substOverride = nullptr);

    // 实例化一个函数模板（S5）
    // typeArgs 由推导引擎（S2~S4）产出；克隆蓝图函数并替换所有 T，
    // 生成 mangled 符号名（_Z5twiceIiE 风格）。
    // 【理论】实例化 = 结构化替换 substitution：把推导得到的 {T := int} 应用到
    //         蓝图的每个类型位置（返回类型/形参/函数体局部变量），而非文本替换
    // 【demo】twice(3) 推导出 T := int ⇒ 实例化 twice<int>：
    //         void twice(T x) → void twice(int x)，函数体中的 T 同步替换，符号 _Z5twiceIiE
    // 注：形参仍是裸 TypePtr 列表——函数模板的非类型形参（NTTP）尚未实现
    //（推导引擎只会产出类型），需要时在函数体内包成 TemplateArg::ofType。
    // 类模板走上面的 instantiate()，形参已可类型/值混排。
    FuncDeclPtr instantiateFunction(
        TemplateDeclPtr templateDecl,
        const std::vector<TypePtr>& typeArgs);

    // 获取所有已实例化的类
    const std::vector<ClassDeclPtr>& getInstantiatedClasses() const {
        return m_instantiatedClasses;
    }

    // 获取所有已实例化的函数（S5）
    const std::vector<FuncDeclPtr>& getInstantiatedFunctions() const {
        return m_instantiatedFunctions;
    }

    // 类型替换（公开：推导引擎替换返回类型时复用同一套规则，含引用折叠）
    // 【理论】[temp.subst] 的核心操作：遍历类型树，每个模板参数叶节点换成实参类型，
    //         复合节点（指针/引用/const）递归重建；引用节点重建时执行引用折叠 [dcl.ref]
    // 【demo】substituteType(T,  {T := int})  → int
    //         substituteType(T&, {T := int})  → int&
    //         substituteType(T&&,{T := int&}) → int& && 折叠为 int&（万能引用落地）
    TypePtr substituteType(TypePtr type, const TypeSubstitution& subst);

    // decltype 求值回调（见 DecltypeEvaluator 注释）。
    // nullptr（默认，单元测试路径）→ 替换时不求值，Decltype 节点原样保留。
    // Sema 构造时会把自己挂上（setDecltypeEvaluator）。
    void setDecltypeEvaluator(DecltypeEvaluator* ev) { m_decltypeEval = ev; }
    DecltypeEvaluator* decltypeEvaluator() const { return m_decltypeEval; }

    // 成员类型查表回调（见 MemberTypeResolver 注释）。
    // nullptr（默认，单元测试路径）→ 依赖类型名原样保留，不查表。
    void setMemberTypeResolver(MemberTypeResolver* r) { m_memberResolver = r; }
    MemberTypeResolver* memberTypeResolver() const { return m_memberResolver; }

    // 别名模板展开回调（见 AliasTemplateResolver 注释）。
    // nullptr（默认，单元测试路径）→ 别名 id 原样保留，不解糖。
    void setAliasTemplateResolver(AliasTemplateResolver* r) { m_aliasResolver = r; }
    AliasTemplateResolver* aliasTemplateResolver() const { return m_aliasResolver; }

private:
    DecltypeEvaluator*        m_decltypeEval = nullptr;
    MemberTypeResolver*       m_memberResolver = nullptr;
    AliasTemplateResolver*    m_aliasResolver = nullptr;

    // 实例名占用表：清洗后的汇编符号前缀 → 无损键（"Box<int*>" 这种）。
    // 【为什么要有】实例名是人读的折中产物，清洗规则不是单射 —— 见
    //   template_instantiation.cpp Step 2a 的撞名守卫。这张表让"两条不同实例
    //   抢同一个符号前缀"从【汇编期 duplicate symbol】提前成【语义期可读报错】。
    std::unordered_map<std::string, std::string> m_instanceNameOwner;

    std::vector<ClassDeclPtr> m_instantiatedClasses;
    std::vector<FuncDeclPtr>  m_instantiatedFunctions;

    // ── AST 深拷贝与类型替换 ──
    // 对应 clang TreeTransform 的递归重建思想：逐节点克隆，类型位置套用 substituteType；
    // 克隆产物不携带旧的语义分析结果，交回语义分析器重新检查（两阶段查找的第二阶段）
    ExprPtr cloneExpr(ExprPtr expr, const TypeSubstitution& subst);
    StmtPtr cloneStmt(StmtPtr stmt, const TypeSubstitution& subst);
    FuncDeclPtr cloneMethod(FuncDeclPtr method, const TypeSubstitution& subst,
                            const std::string& newClassName);
    FieldInfo cloneField(const FieldInfo& field, const TypeSubstitution& subst);
};

} // namespace minicc
