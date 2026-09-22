#pragma once
// =============================================================================
// 阶段 4：模板实例化引擎 (Template Instantiation Engine)（理论见 docs/learn/05、13）
// =============================================================================
// 实例化 = AST 结构化克隆 + 类型替换 [temp.subst]，不是文本替换；产物带全局唯一符号名。
// 蓝图写法 ⇒ 实例写法（MyPtr<int>，即 {T→int}）：
//   class MyPtr<T>   ⇒ 类改名 ⇒ MyPtr_int，符号 _Z5MyPtrIiE（Itanium mangling，见 docs/learn/13）
//   T* data          ⇒ int* data
//   T& get()         ⇒ int& get()
//   return N;（NTTP）⇒ return 4;（表达式位置也替换，见 cloneExpr）
//   Vec<T>（别名）    ⇒ 只有解糖，零新类、零新符号（[temp.alias]/1）
//
// ── 本文件的组成 ───────────────────────────────────────────────────────
//   DecltypeEvaluator     │ decltype 求值回调（[dcl.type.decltype]）
//   MemberTypeResolver    │ `typename T::type` 成员查表回调（[temp.res]/5）
//   AliasTemplateResolver │ 别名模板 id `X<...>` 展开回调（[temp.alias]）
//   NameMangler           │ 符号修饰（Itanium ABI 子集）
//   TemplateInstantiator  │ 引擎本体：[temp.inst] 实例化时机 │ [temp.subst] 实参替换 │
//                           [dcl.ref]/6 引用折叠（万能引用实例化的关键）
//   三个回调接口同构（第三次出现同一形状）：蓝图与查表能力住在 Sema，调用它们的是替换
//   引擎 —— 抽成接口即避免 Sema ⇄ Instantiator 双向依赖，也让单元测试不必拖进整个 Sema。
//
// 对照 clang：SemaTemplateInstantiate.cpp（声明级实例化 ≈ instantiate*）│
//   TreeTransform.h（AST 递归重建 ≈ cloneExpr/cloneStmt）；区别：clang 用
//   SubstTemplateTypeParmType 类型节点记录替换，minicc 在克隆时直接换类型
//
// 管线：Parser(蓝图) → Sema(发现实例化需求) → TemplateDeducer(推导实参)
//   → 【本文件：按实参克隆 + 替换，产出具体类/函数】→ Sema(对实例做类型检查) → CodeGen
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

// SubstitutionFailure 已迁出本文件 → include/sfinae.h（它是"替换失败信号"，被推导、
//   语义、实例化三方共用，挂靠在"实例化"名下会让人找不到 SFINAE 的入口）。
// 本头文件仍 include sfinae.h —— 因为 DecltypeEvaluator 的契约里
//   "求值失败抛 SubstitutionFailure" 是它的一部分。

// ─────────────────────────────────────────────────────────────────────────────
// DecltypeEvaluator：decltype 求值的回调接口
// ─────────────────────────────────────────────────────────────────────────────
// 【分层问题】求值能力（"表达式 → 类型"）住在 SemanticAnalyzer（inferType），触发方是
//   TemplateInstantiator::substituteType 的 Case 1.5。直接依赖 = ① Sema ⇄ Instantiator
//   双向依赖；② tests/unit/ 里【裸构造】TemplateDeducer / TemplateInstantiator 的单元测试
//   被迫拖进整个 Sema。故抽成接口：Sema 实现它，instantiator 只持一个可选指针。
// 【可选语义】指针为 nullptr（单元测试路径）⇒ decltype 节点**不求值**、原样保留：
//   substituteType(makeDecltype(e), {}) ⇒ 仍是 Decltype 节点（"延迟求值"本身可观察）
// 对照 clang：Sema::SubstType 内直接调 BuildDecltypeType（clang 的 Instantiator = Sema 的一部分）
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
// 【分层问题】与 DecltypeEvaluator 同构：查成员别名住在 Sema（才有 m_classDecls /
//   typeAliases / 按需实例化），需要它的是替换引擎 substituteType 的 Case 5.5（以及
//   推导器 reducePattern 里临时建的 instantiator）。
// 写法 ⇒ 替换期当场的结果（[temp.res]/5）：
//   typename Plain::type          ⇒ 替换成 Plain 里的别名目标
//   typename T::type，T := Plain  ⇒ 查 Plain.typeAliases ⇒ 命中即解糖
//   typename T::type，T := int    ⇒ 查不到 ⇒ Sfinae::fail（软失败，该候选出局）
// ★ 必须在这里（替换当场）失败：这正是 [temp.deduct]/8 的**直接上下文** ——
//     template<class T> using has_type = void_t<typename T::type>;
//   拖到 Sema 解析期就成硬错误，SFINAE 探测惯例整个失效。
// 对照 clang：Sema::SubstType 对 DependentNameType 走 getTypeName + LookupQualifiedName
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
// 【分层问题】与上面两个接口同构（第三次出现同一形状）：别名蓝图住在 Sema（它才有模板
//   注册表），用它的地方是替换引擎的 Case 5.8 与推导器；故仍"接口在低层、实现由 Sema
//   注入"，否则 Sema ⇄ Instantiator 立刻变成双向依赖。
// 写法 ⇒ 替换期当场的结果（[temp.alias]/1 与 [temp.deduct]/8 的交汇）：
//   enable_if_t<true, T>   ⇒ 解糖 ⇒ T（候选保留）
//   enable_if_t<false, T>  ⇒ 展开失败 ⇒ Sfinae::fail ⇒ 该候选被剔除（不是硬错误）
//   Vec<int>               ⇒ 解糖 ⇒ MyPtr<int>（同一个类，零新符号）
// 定义期根本查不动它（T 未知，两阶段查找的第一阶段），必须等调用点推出 T 后在替换的
//   当口展开 —— 拖到 Sema 解析期就晚了：那时已不在直接上下文里，失败变硬错误。
// 对照 clang：Sema::SubstType 判 isTypeAlias() 后走 CheckAliasTemplateId + getCanonicalType
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
// NameMangler：符号修饰器（理论见 docs/learn/13）
// ─────────────────────────────────────────────────────────────────────────────
// 汇编器/链接器只认唯一名字 ⇒ mangling 把完整签名编码成唯一串。
// 源码写法 ⇒ 符号（Itanium C++ ABI，GCC/Clang 所用）：
//   MyPtr<int>        ⇒ _Z5MyPtrIiE        （_Z + <长度><名字> 的 source-name + I…E 模板实参表）
//   MyClass::foo(int) ⇒ _ZN7MyClass3fooEi  （N…E 嵌套限定名）
//   Buf<4>            ⇒ _Z3BufILi4EE       （NTTP 走 <expr-primary> L<类型编码><值>E）
//   Buf<-3>           ⇒ _Z3BufILin3EE      （负数编 n<绝对值>：'-' 不是合法 mangling 字符）
// =============================================================================
class NameMangler {
public:
    // 生成符号名：MyPtr<int> → _Z5MyPtrIiE；函数模板实例同走这里：
    //   twice<int> → _Z5twiceIiE（与源码名区分，避免链接冲突）。
    // NTTP（[temp.arg.nontype]）走 Itanium 的 <expr-primary> 分支：
    //   Buf<4> → _Z3BufILi4EE
    //            └┬┘└┬┘│└┬┘│└┘
    //             3Buf I  L i 4 E E
    //             └ 名字 │ └ L…E = <expr-primary>（非类型实参专用：i=int 类型编码，
    //                    └ I…E = 模板实参表        4=值，负数编 n4，即 -4 → Lin4E）
    // 参照：Itanium C++ ABI §5.1.8 <template-arg> → <expr-primary> → L <type> <value> E
    static std::string mangleTemplateInstance(
        const std::string& templateName,
        const std::vector<TemplateArg>& args);

    // 函数模板实例的符号名（★ 比类模板实例多末尾一段 <bare-function-type>）
    // 格式: _Z + 名 + I<模板实参>E + <返回类型> + <各参数类型>
    // demo: template<class T> T twice(T x) 以 T=int 实例化 ⇒ _Z5twiceIiET_T_
    //                                                        └┬┘ └┬┘
    //                                                    返回 T_  参数 T_
    //       template<class T> T pick(T a, int n)  ⇒ _Z4pickIiET_i
    // 少了这段，同名模板的多个重载会撞成同一个符号（docs/BUGS.md B2）。
    // 模板形参在签名里编成 Itanium 的 <template-param>（T_ / T0_…）—— 它引用
    // 模板实参表里的第 n 项，故不同实例仍靠前缀 I…E 区分，签名只如实反映形状。
    // ★ 与 clang 的逐字符差异：clang 会在参数表里用替换表压缩重复类型
    //   （第二个 T 编成 S0_），本实现一律展开 ⇒ _Z4pickIiET_i vs clang 的
    //   _Z4pickIiET_S0_i。唯一性不受影响，差异记于 docs/BUGS.md B7。
    static std::string mangleFunctionTemplateInstance(
        const std::string& funcName,
        const std::vector<TemplateArg>& args,
        const TypePtr& returnType,
        const std::vector<Parameter>& params,
        const std::vector<std::string>& typeParams);

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
    // ★ typeParams 非空时，TypeKind::TemplateParam 编成 Itanium 的 <template-param>
    //   （T_ / T0_ / T1_…）；为空表时保持原行为（原样输出形参名）—— 故类模板
    //   路径（恒传 1 参）的符号逐字节不变。
    static std::string encodeType(TypePtr type,
        const std::vector<std::string>& typeParams = {});
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

    // 实例化一个模板类 [temp.inst]：深拷贝蓝图 + 结构化替换 [temp.subst]。
    // 实参形态 ⇒ 替换表 ⇒ 产物：
    //   MyPtr<int> ⇒ {T→TemplateArg{Type,int}} ⇒ 字段 T* data → int* data，
    //                类名 MyPtr_int，符号 _Z5MyPtrIiE
    //   Buf<4>     ⇒ {N→TemplateArg{Integral,4}} ⇒ 方法体 `return N;` 的 VarExpr{N} → IntLiteral{4}
    // templateDecl: 蓝图 │ args: 实参（类型与值混排，如 [Type:int] / [Integral:4]），
    //   顺序与 templateDecl->templateParams 一一对应 │ 返回实例化后的 ClassDecl。
    // substOverride：nullptr（默认）= 主模板路径：按 templateParams 的 kind 逐位分派并校验；
    //   非 nullptr = 特化路径：直接采用该表，跳过逐位校验与个数校验。
    // ★ 用指针而不是"空表即主模板"：全特化（template<> struct Box<int*,int>）的形参表为空，
    //   匹配推导出的替换表**本来就是空的** —— 用 empty() 当判别条件会把全特化误判成主模板，
    //   进而撞上"expects 0 argument(s), got 2"的个数错位。路径归属是调用方的知识
    //   （Sema::selectClassTemplate 已经判定过），必须显式传入。
    ClassDeclPtr instantiate(
        TemplateDeclPtr templateDecl,
        const std::vector<TemplateArg>& args,
        const TypeSubstitution* substOverride = nullptr);

    // 实例化一个函数模板（S5）：typeArgs 由推导引擎（S2~S4）产出。
    // 蓝图写法 ⇒ 实例（{T := int} 应用到每个类型位置，不是文本替换）：
    //   void twice(T x)   ⇒ void twice(int x)      符号 _Z5twiceIiE
    //   T max(T a, T b)   ⇒ int max(int a, int b)
    //   T tmp = a;（局部）⇒ int tmp = a;           （cloneStmt 的 VarDecl 分支也替换）
    // 注：形参仍是裸 TypePtr 列表 —— 函数模板的非类型形参（NTTP）尚未实现（推导引擎
    //   只产出类型），需要时在函数体内包成 TemplateArg::ofType；类模板走上面的
    //   instantiate()，形参已可类型/值混排。
    // 实例名清洗：把实参的【人读串】洗成合法的汇编符号字符。
    // ★ 由 Instantiator（造类/函数实例名）与 Sema（拼成员模板实例的缓存键）
    //   【共用同一份】—— 这条规则一旦两处各写一份，改一处必漏另一处，
    //   缓存的键就会与实际符号名对不上（命中失败或误命中，且都不报错）。
    // 定义在 src/template_instantiation.cpp，紧挨 instantiate()。
    // 自由函数，故意放在类外：它是纯粹的字符串工具，不属于实例化器的状态。

    // ownerClassName 非空 ⇒ 按【成员模板】实例化（[temp.mem]）：实例带隐式 this，
    // 符号用 `类名_方法名_实参后缀` 而非 Itanium 模板实例名。
    FuncDeclPtr instantiateFunction(
        TemplateDeclPtr templateDecl,
        const std::vector<TypePtr>& typeArgs,
        const std::string& ownerClassName = "");

    // 获取所有已实例化的类
    const std::vector<ClassDeclPtr>& getInstantiatedClasses() const {
        return m_instantiatedClasses;
    }

    // 获取所有已实例化的函数（S5）
    const std::vector<FuncDeclPtr>& getInstantiatedFunctions() const {
        return m_instantiatedFunctions;
    }

    // 类型替换（公开：推导引擎替换返回类型时复用同一套规则）。
    // 理论：[temp.subst] 的核心操作 —— 遍历类型树，模板参数叶节点换成实参类型，
    //   复合节点（指针/引用/const）递归重建；重建引用节点时执行引用折叠 [dcl.ref]。
    // demo: substituteType(T, {T := int}) → int │ (T&, {T := int}) → int&
    //       (T&&, {T := int&}) → int& && 折叠为 int&（万能引用落地）
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

// 实例名清洗（见类内注释；定义在 src/template_instantiation.cpp）
std::string sanitizeSymbolChars(const std::string& raw);

} // namespace minicc
