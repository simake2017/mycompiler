// =============================================================================
// 阶段 3：语义分析器实现 —— 符号表构建、类型推导、内存布局
// （理论见 docs/learn/08、10、22、29）
// =============================================================================
// 输入 TranslationUnit（AST：类型只是语法标记、名字均未决议）→ 输出三样：
//   ① 标注 resolvedType 的 AST（auto 已抹去）  ② 符号表快照（Scope 链 + 栈偏移）
//   ③ 类布局表（字段偏移 / vtable / RTTI，CodeGen 直接消费）
// 管线：Preprocessor → Lexer → Parser → 【SemanticAnalyzer】 → 模板推导/实例化
//       → CodeGen(x86-64 .s)。"编译期看符号，运行期看偏移量"在此完成转换。
// 用例  struct Base { int b; virtual int kind(); };   Base* pb = &dr;   pb->kind();
//       ⇒ 符号 pb: stack@-8；Base 布局 {+0 _vptr, +8 b:int, total 16}；
//         inferCall 按 vtable 槽 0 命中 Base_kind（kind=Variable→运行期偏移的兑现）
//
// ── 理论背景（标准章节 → 本文件实现位置）────────────────────────────────────
//   | 章节 | 落点 |
//   |---|---|
//   | [basic.scope] | Scope::lookup / SymbolTable::enterScope/exitScope（逐层向外）|
//   | [expr] | inferType 自底向上给每个表达式定型；inferCall 记录 argIsLValue |
//   | [over.match] | resolveTemplateCall + isAtLeastAsSpecialized（候选→可行→最优）|
//   | [temp.names] | 两阶段查找：Pass 1 只注册蓝图；实例化后再 analyzeFunctionBody |
//   | [dcl.init.ref] | 绑定检查（非 const T& 拒右值、T&& 折叠）在 TemplateDeducer |
//   | [temp.deduct]/8 | SFINAE 的三收口点，见 include/sfinae.h |
//
// ── clang 对照 ──────────────────────────────────────────────────────────────
//   lib/Sema/SemaDecl.cpp     ≈ processClassDecl / registerFunction（ActOnDeclarableType 等）
//   lib/Sema/SemaExpr.cpp     ≈ inferType 系列（ActOnCallExpr / BuildMemberExpr / LookupName）
//   lib/Sema/SemaOverload.cpp ≈ resolveTemplateCall（AddTemplateOverloadCandidate）
//                               / isAtLeastAsSpecialized
// =============================================================================

// =============================================================================
// 读法：一条源码走完本文件的完整数据流（全线路标）
// =============================================================================
// 每个站点函数上方有一块 `┌─ DEMO`：该站点的真实输入 / 真实日志 / 输出（文末命令可复现）。
//
//   源码                            站点（本文件函数）           产出
//   ──────────────────────────────  ──────────────────────────  ─────────────────────
//   struct Base { ... };            processClassDecl        →  ClassType（布局 + vtable）
//                                   ├ computeClassLayout    →  字段偏移 / totalSize
//                                   └ injectVTableAndRTTI   →  vtable 条目 + _ZTI 符号
//   int add(int, int);              registerFunction        →  mangledName + 符号表三处登记
//   template<class T> T f(T);       processTemplateDecl     →  蓝图 / 候选集（不查体）
//   T x = expr;                     visit(VarDeclStmt)      →  auto 抹去 + 栈槽分配
//                                   └ foldStaticConst       →  Cls<A>::value → 字面量
//     └ expr 的每个子表达式          inferType（NodeKind 一次 switch，14 路）
//          ├ 字面量 / 名字 / 二元 / 一元 / 成员 / 下标 / new / this / dynamic_cast
//          ├ f(x)                   inferCall ─┬ ⓪ declval 内建
//          │                                   ├ ① 成员方法（class-scoped，BFS 基类）
//          │                                   ├ ② 普通函数 + ADL 候选合并
//          │                                   └ ③ 函数模板 resolveTemplateCall
//          └ decltype(e)            evaluateDecltype →  [dcl.type.decltype] 两规则
//   Box<int> b;                     resolveType ─→ getOrInstantiateClass
//                                   └ selectClassTemplate → 全特化 / 偏特化 / 主模板 三路择优
//
// 复现本文所有 DEMO（假设样例源码在 /tmp/sema_demo/）：
//   ./build.sh && ./minicc /tmp/sema_demo/demo.cpp -S > log.txt 2>&1 && grep -n "Pass 1" log.txt
// =============================================================================

#include "semantic_analyzer.h"
#include "template_deduction.h"
#include <format>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <functional>

namespace minicc {

// ─── SymbolKind 名称（仅供日志 / dump）──────────────────────────────────────
// demo: dump 符号表打印 `x  Variable  int  stack@-8` ⇒ "Variable" 由本函数产出
const char* symbolKindName(SymbolKind k) {
    switch (k) {
        case SymbolKind::Variable:    return "Variable";
        case SymbolKind::Parameter:   return "Parameter";
        case SymbolKind::Function:    return "Function";
        case SymbolKind::FunctionTemplate: return "FunctionTemplate";
        case SymbolKind::ClassField:  return "ClassField";
        case SymbolKind::ClassMethod: return "ClassMethod";
        case SymbolKind::Type:        return "Type";
    }
    return "Unknown";
}

// ═══ Scope 实现 ════════════════════════════════════════════════════════════
// 向本层符号表插入一条符号。emplace 失败（本层已有同名）返回 false —— 重声明检查
// [basic.scope.scope] 的落点。
// demo: 块内已有 `int x;` 再写 `int x;` ⇒ define 返回 false ⇒ processVarDecl 报
//       "Variable 'x' already declared in this scope"。
// ⚠ 外层同名不拦截（内层遮蔽外层合法）—— 遮蔽由 lookup 的顺序实现。
bool Scope::define(const std::string& name, Symbol sym) {
    auto [it, inserted] = m_symbols.emplace(name, std::move(sym));
    return inserted;
}

// 作用域链查找（[basic.scope.scope] 名字可见性的核心算法）：从本层开始，未命中则
// 递归父作用域，直到全局作用域为止。
// demo: block 内查 `x` ⇒ block ✗ → f ✓（内层遮蔽外层：先查到谁就用谁）；
//       若各层皆无 ⇒ nullptr ⇒ 上层转查类字段 / 函数名（见 inferVar）。
Symbol* Scope::lookup(const std::string& name) {
    auto it = m_symbols.find(name);
    if (it != m_symbols.end()) return &it->second;
    if (m_parent) return m_parent->lookup(name);
    return nullptr;
}

// 只查本层、不上链：用于需要"仅限同一作用域"语义的场合（对比 lookup）。
Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = m_symbols.find(name);
    return (it != m_symbols.end()) ? &it->second : nullptr;
}

// ─── Scope::dump：打印当前作用域中的所有符号 ────────────────────────────────
void Scope::dump(int indent) const {
    std::string pad(indent * 2, ' ');
    std::cout << std::format("{}┌─ Scope[{}] '{}' (depth={}) ─────────────────\n",
        pad, m_depth, m_name.empty() ? "anonymous" : m_name, m_depth);

    if (m_symbols.empty()) {
        std::cout << std::format("{}│  (empty)\n", pad);
    }

    for (auto& [name, sym] : m_symbols) {
        std::string typeStr = sym.type ? sym.type->toString() : "?";
        std::string offsetStr = sym.isLocal
            ? std::format("  stack@{}", sym.stackOffset) : "";

        std::cout << std::format("{}│  {:<12} {:<10} {:<12}{}{}\n",
            pad,
            name,
            symbolKindName(sym.kind),
            typeStr,
            offsetStr,
            sym.ownerClass.empty() ? "" : std::format(" [{}]", sym.ownerClass));
    }

    std::cout << std::format("{}└──────────────────────────────────────\n", pad);
}

// ═══ SymbolTable 实现 ══════════════════════════════════════════════════════
// 构造：创建全局作用域（链根，depth=0，名字 "global"），类名/函数名等顶层符号都注册在这里。
SymbolTable::SymbolTable()
    : m_globalScope(std::make_unique<Scope>(nullptr, 0, "global"))
    , m_currentScope(m_globalScope.get())
    , m_currentDepth(0) {}

// 压栈进入新作用域：新 Scope 的 parent 指向当前作用域。
// demo: analyzeFunctionBody 调 enterScope("f")，函数体再调 enterScope("block")
//       ⇒ 形成 global ← f ← block 链。
// 所有权：newScope 移交 m_allScopes 统一持有（退出作用域 ≠ 销毁对象，保留到编译结束供 dump）。
void SymbolTable::enterScope(const std::string& name) {
    m_currentDepth++;
    auto newScope = std::make_unique<Scope>(m_currentScope, m_currentDepth, name);
    m_currentScope = newScope.get();
    m_allScopes.push_back(std::move(newScope));
}

// 弹栈回到父作用域：子作用域中的名字从此不可见（对象仍保留在 m_allScopes）。
// 防御：global 的 parent 为 nullptr，多退一步也不会崩溃。
void SymbolTable::exitScope() {
    if (m_currentScope && m_currentScope->parent()) {
        m_currentScope = m_currentScope->parent();
        m_currentDepth--;
    }
}

// 委托当前作用域：符号插入当前层（跨层插入不存在——可见性规则使然）。
bool SymbolTable::define(const std::string& name, Symbol sym) {
    return m_currentScope->define(name, std::move(sym));
}

// 委托当前作用域：从当前层沿链向外查（实现见 Scope::lookup）。
Symbol* SymbolTable::lookup(const std::string& name) {
    return m_currentScope->lookup(name);
}

void SymbolTable::dump() const {
    std::cout << "\n  ╔══════════════════════════════════════════════╗\n";
    std::cout << "  ║         SYMBOL TABLE (符号表快照)            ║\n";
    std::cout << "  ╚══════════════════════════════════════════════╝\n";
    m_globalScope->dump(2);
    std::cout << "\n";
}

void SymbolTable::dumpCurrentScope() const {
    m_currentScope->dump(2);
}

// ═══ SemanticAnalyzer 实现 ═════════════════════════════════════════════════
// ─── typeCompatible：[conv.lval] 剥顶层引用 / const + [conv.promo] int→double ───
// demo: typeCompatible(int, int&) = true（[conv.lval]）｜(int, const int) = true
//       (double, int) = true（[conv.promo]）｜(int, bool) = false ⇒ 报错
//
// ─── classSpecAtLeastAsSpecialized：类模板偏序（理论见 docs/learn/21）─────────
// [temp.class.order]：A 至少和 B 一样特化 ⟺ 用 A 的模式（形参换成唯一合成类型）能
//   推出 B 的模式 —— 能推出 B ⇒ B 的约束更松、覆盖更广 ⇒ A 覆盖得少 ⇒ A 更特化。
// ★ 必须先重命名形参：两个偏特化各自写 `template<class T>`，名字都叫 T，但它们是
//   【不同的变量】。直接拿 A 的 T 去匹配 B 的 T，推导器会当成同一个变量而"匹配成功"
//   ⇒ 一切互相特化 ⇒ 全判成歧义。故先换成 `$ord_class_S0` 这样的合成名：它在 B 的
//   形参表里查无此名 ⇒ 推导器按【非依赖常量】处理 ⇒ 做结构比较。
//   这正是 clang 用 UniqueSynthesizedType 的目的。
// demo: S<T*> 与 S<T**>：用 T* 推 T** ✗（指针层数不符）；用 T** 推 T* ⇒ T := T*(合成) ✓
//   ⇒ T** 更特化，胜出；且结论与【声明顺序无关】。
bool SemanticAnalyzer::classSpecAtLeastAsSpecialized(const TemplateDeclPtr& a,
                                                     const TemplateDeclPtr& b) {
    // ① a 的模式 → 合成实参（形参名替换为唯一名）
    //   值位（NTTP）原样搬过去：偏序只需把"两边都是变量"的位置去相关，
    //   值位是常量，不与 b 的形参名同名，天然无关，无需重命名。
    std::vector<TemplateArg> synthArgs;
    const std::string prefix = "$ord_" + a->templateName() + "_";
    for (const auto& t : a->specPattern) {
        if (t.isType() && t.type) {
            synthArgs.push_back(TemplateArg::ofType(renameTemplateParams(t.type, prefix)));
        } else {
            synthArgs.push_back(t);
        }
    }

    // ② b 的形参名列表 —— 只有它们才是可绑定的"变量"
    std::vector<std::string> paramNames;
    for (const auto& p : b->templateParams) paramNames.push_back(p->name);

    // ★ SFINAE 吸收点 ③（全项目三处之一，见 include/sfinae.h 的收口点一览）
    //   这里"吸收"的语义有个特别之处：失败【不表示候选被淘汰】，而表示
    //   "a 不比 b 更特化" —— 即偏序关系里的一个方向不成立。
    //   同一份 matchPattern 在两个场景里承载两种结论，故两处都必须显式标注，
    //   否则读代码的人会误以为这里是普通的匹配失败。
    //   （吸收动作本身由 matchPattern 内部的 Sfinae::attempt 完成，本处只翻译结论。）
    std::unordered_map<std::string, TypePtr> subst;
    std::string reason;
    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    bool ok = deducer.matchPattern(b->specPattern, synthArgs, paramNames, subst, reason);

    std::cout << std::format(
        "  [order] '{}' 推 '{}' ⇒ {}    ({} 是否至少与 {} 同样特化)\n",
        patternToString(a->specPattern), patternToString(b->specPattern),
        ok ? "成功" : "失败（" + reason + "）",
        patternToString(a->specPattern), patternToString(b->specPattern));
    return ok;
}

// 深拷贝 + 把模板形参名换成 prefix+序号。只覆盖模式里可能出现的节点形态：
// TemplateParam / Class / Pointer / 引用 / Const。
// 对照 clang：TreeTransform 里 MakeUniqueSynthesizedType 的替换过程。
TypePtr SemanticAnalyzer::renameTemplateParams(const TypePtr& t,
                                               const std::string& prefix) {
    if (!t) return nullptr;
    if (t->isTemplateParam()) {
        auto r = Type::makeTemplateParam(prefix + t->templateParamName);
        return r;
    }
    if (t->isClass()) {
        // 模式里的裸形参名既可能是 TemplateParam 也可能是 Class（Parser 的产物），
        // 统一改名为合成名；带实参的类模板（如 std::void_t<...>）改内层实参。
        auto r = Type::makeClass(prefix + t->name);
        for (const auto& ta : t->templateArgs) {
            if (ta.isType() && ta.type) {
                r->templateArgs.push_back(
                    TemplateArg::ofType(renameTemplateParams(ta.type, prefix)));
            } else {
                r->templateArgs.push_back(ta);
            }
        }
        return r;
    }
    if (t->isPointer() && t->pointeeType) {
        return Type::makePointer(renameTemplateParams(t->pointeeType, prefix));
    }
    if (t->isLValueReference() && t->referencedType) {
        return Type::makeLValueReference(renameTemplateParams(t->referencedType, prefix));
    }
    if (t->isRValueReference() && t->referencedType) {
        return Type::makeRValueReference(renameTemplateParams(t->referencedType, prefix));
    }
    if (t->isConst() && t->innerType) {
        return Type::makeConst(renameTemplateParams(t->innerType, prefix));
    }
    return t;
}

std::string SemanticAnalyzer::patternToString(const std::vector<TemplateArg>& pattern) {
    std::string s;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (i > 0) s += ", ";
        s += pattern[i].toString();   // 类型位印类型名，值位印值（含形态）
    }
    return s;
}

std::string SemanticAnalyzer::typeListToString(const std::vector<TemplateArg>& types) {
    return patternToString(types);
}

// ─── decltype 求值（DecltypeEvaluator 接口的实现，理论见 docs/learn/20）───────
// 【[dcl.type.decltype] 两套规则】只看"操作数是否被括号包住"：
//   decltype(e)    e 是【未加括号】的 id-expression 或类成员访问 ⇒ 【声明类型】
//   decltype((e))  其它一切情况（含多加一层括号）⇒ 【表达式类型】，按值类别调整：
//                  左值 / xvalue → T&，prvalue → T
// 用例  int a = 1;      decltype(a) ⇒ int ｜ decltype((a)) ⇒ int&（(a) 是左值）
//       int x=1; int& r=x;  decltype(r) ⇒ int&（id-expression 取声明类型，不剥引用）
// 【为什么抛 SubstitutionFailure 而不是返回错误类型】求值失败属于 [temp.deduct]/8 的
//   immediate context 失败：在 void_t 探测里必须表现为"该偏特化不匹配"，异常即其载体。
// ┌─ DEMO（真实日志）──────────────────────────────────────────────────────────
// │ 源码  int x = 42;      decltype(x) w = x;
// │ 日志  [decltype] resolveType 遇到 decltype 节点，尝试立即求值
// │       [decltype] 求值 decltype(e) —— 未加括号 ⇒ 若为 id-expression 则取【声明类型】
// │       [resolve] 'x' → int    (kind=Variable, stack@-8)
// │       [decltype]   ⇒ 声明类型 = int
// │ 输出  int（未加括号的 id-expression）/ int&（加括号，左值）/ int（右值表达式）
// │ 依赖上下文  模板模式里 T 还没绑定时立即求值会失败 → 保持 Decltype 节点延迟，
// │       等 substituteType 阶段再算（见 resolveType 的 decltype 分支与 Case 1.5）。
// ─── findMemberType：typename T::type 的落点（理论见 docs/learn/25）───────────
// [temp.res]/5：模板里的限定名 `Q::m`，Q 依赖（含模板形参）时 m 是类型还是值定义期判
//   不出 ⇒ 要写 typename 消歧。本实现的 `typename` 只是语法前缀：去 Q 的成员别名表查，
//   查得到就是类型，当场解糖。
// 【同一份查表，两个入口，后果相反】[temp.deduct]/8：
//     resolveType 路径（解析实体声明的类型）不在直接上下文 ⇒ 硬错误
//     resolveMemberType 路径（替换模板实参）在直接上下文 ⇒ 软失败
//   用例  Get<Plain> ⇒ 字段 v : int；Get<int> ⇒ Sfinae::fail（候选被移出，不是硬错误）
// 对照 clang：Sema::getTypeName + LookupQualifiedName 负责"查"，失败后走
//   err_unknown_typename（硬）还是 SFINAE（软）。
// ┌─ DEMO（真实日志）──────────────────────────────────────────────────────────
// │ 源码  template<typename T> struct Get { typename T::type v; };
// │       struct Plain { using type = int; };   Get<Plain> g;
// │ 日志  [subst] 依赖限定名 T::type → 先替换限定者 ⇒ ★ Plain::type ⇒ int
// │ 输出  字段 v : int；若换成 Get<int> ⇒ Sfinae::fail ⇒ 该候选被移出，不是硬错误
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::findMemberType(const TypePtr& qual, const std::string& member) {
    if (!qual || !qual->isClass()) return nullptr;
    auto cit = m_classDecls.find(qual->name);
    if (cit == m_classDecls.end()) return nullptr;
    auto ait = cit->second->typeAliases.find(member);
    if (ait == cit->second->typeAliases.end() || !ait->second) return nullptr;
    return ait->second;
}

TypePtr SemanticAnalyzer::resolveMemberType(const TypePtr& qualifier,
                                            const std::string& member) {
    TypePtr q = resolveType(qualifier);
    TypePtr m = findMemberType(q, member);
    if (!m) {
        // 直接上下文内的失败 ⇒ 软失败信号（见 sfinae.h 的三方协议）
        Sfinae::fail(std::format("no type named '{}' in '{}'",
            member, q ? q->toString() : "?"));
    }
    return resolveType(m);
}

// ═══ 别名模板展开 [temp.alias]：template<class T> using Vec = MyPtr<T>; ════════
// 与类模板实例化的对照（理解别名模板最省事的一把尺子）：
//   | | 类模板 Box<T> | 别名模板 Vec<T> |
//   |---|---|---|
//   | 产物 | 新的类（Box_int）| 既有类型本身（MyPtr_int）|
//   | 做什么 | 深拷贝蓝图 + 替换 + 登记符号 | 只替换底层类型，解糖 |
//   | 有缓存吗 | 有（同实参复用同一实例）| 不需要（解糖是幂等纯函数）|
//   | 能特化吗 | 能（全/偏特化）| 本项目不做 |
//   | 占符号吗 | 占（_Z3BoxIiE）| 不占 |
// ★ [temp.alias]/1：别名模板的特化就是它所指代的类型，不引入新类型。
// 用例  template<class T> using Vec = MyPtr<T>;   Vec<int> v; ⇒ 类型是 MyPtr_int，
//       汇编符号 _Z5MyPtrIiE（与手写 MyPtr<int> v; 完全同一条路，别名痕迹为零）
// 对照 clang：Sema::CheckAliasTemplateId（校验实参个数与 kind）+ Sema::SubstType 对
//   底层类型做 TreeTransform，最后 getCanonicalType 解糖。（理论见 docs/learn/27）
bool SemanticAnalyzer::isAliasTemplate(const std::string& name) const {
    return m_aliasTemplates.count(name) != 0;
}

// ═══ 类模板实参推导 CTAD（[dcl.type.class.deduct]）+ 推导指引 ═════════════════
// 【一句话】`MyPtr m(7);` 没写 `<...>`，编译器拿构造实参的类型反推类模板形参。
// 它与【函数模板】实参推导是同一套合一算法的**反向使用**，直接复用 TemplateDeducer：
//     函数模板 f(T x)  ← 从"实参类型"推"函数模板形参"
//     CTAD  MyPtr(T)   ← 从"构造实参类型"推"类模板形参"（拿构造函数当那个 f）
// 用例  template<class T> struct MyPtr { MyPtr(T q); };   MyPtr m(7);
//       ⇒ P=T, A=int ⇒ T := int ⇒ 等价于 MyPtr<int> m(7);
// 【三条来源，按优先级】① 用户写的推导指引（显式优先，[temp.deduct.guide]/1）
//   ② 主模板的构造函数（隐式指引："拿构造函数当指引"）③ 都不成 ⇒ 报错
// 对照 clang：Sema::DeduceTemplateArguments 之前，DeclSpec 的 CTAD 分支先把候选指引
//   （含隐式合成的）收集成重载集，再走一遍重载决议。（理论见 docs/learn/28）
void SemanticAnalyzer::registerDeductionGuide(const DeductionGuideDeclPtr& g) {
    m_deductionGuides[g->guideName].push_back(g);
    std::cout << std::format(
        "  [register] deduction guide for '{}' (#{}) registered\n",
        g->guideName, m_deductionGuides[g->guideName].size());
}

TypePtr SemanticAnalyzer::deduceClassTemplateArgs(const VarDeclStmt& decl) {

    TypePtr t = decl.declaredType;
    // 适用条件（[dcl.type.class.deduct]/1）：裸的类模板名 + 括号直接初始化。
    // 已写实参（MyPtr<int> m(7)）、别名、指针等一律不走 CTAD。
    if (!t || !t->isClass() || !t->templateArgs.empty() || t->isNestedName()) return t;
    if (!m_classTemplates.count(t->name)) return t;
    if (decl.ctorArgs.empty()) return t;

    TemplateDeclPtr primary = m_classTemplates[t->name];

    // ── 实参类型 ──
    std::vector<TypePtr> argTypes;
    for (auto& a : decl.ctorArgs) {
        argTypes.push_back(inferType(a));
    }

    std::cout << std::format("  [ctad] ▶ {} {}(", decl.name, t->name);
    for (size_t i = 0; i < argTypes.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << (argTypes[i] ? argTypes[i]->toString() : "?");
    }
    std::cout << ") —— 未写模板实参，尝试类模板实参推导\n";

    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);

    auto tryGuide = [&](const std::vector<Parameter>& params,
                        const std::vector<std::string>& paramNames,
                        const std::string& via,
                        std::unordered_map<std::string, TypePtr>& outSubst) -> bool {
        if (params.size() != argTypes.size()) return false;
        DeductionResult out;
        outSubst.clear();
        for (size_t i = 0; i < params.size(); i++) {
            if (!deducer.deducePair(params[i].type, argTypes[i],
                                    /*argIsLValue=*/true, paramNames, outSubst, out)) {
                std::cout << std::format("  [ctad]   ✗ {} 不成立：{}\n", via, out.failureReason);
                return false;
            }
        }
        // 所有形参都必须推出来（[temp.deduct.type] 的收尾检查）
        for (const auto& p : paramNames) {
            if (!outSubst.count(p)) {
                std::cout << std::format("  [ctad]   ✗ {} 里有推不出的形参 '{}'\n", via, p);
                return false;
            }
        }
        return true;
    };

    // ── 来源①：用户写的推导指引（显式优先于隐式）──
    auto git = m_deductionGuides.find(t->name);
    if (git != m_deductionGuides.end()) {
        for (auto& g : git->second) {
            std::unordered_map<std::string, TypePtr> subst;
            std::vector<std::string> gParams;
            for (auto& p : g->templateParams) gParams.push_back(p->name);
            if (!tryGuide(g->parameters, gParams, "推导指引", subst)) continue;

            // 指引的 targetArgs 是"用推导结果参数化"的实参表
            TypePtr tid = Type::makeClass(t->name);
            for (auto& ta : g->targetArgs) {
                tid->templateArgs.push_back(
                    TemplateArg::ofType(resolveType(substituteInType(ta, subst))));
            }
            std::cout << std::format("  [ctad] ✔ 命中推导指引 ⇒ {}\n", tid->toString());
            return tid;
        }
    }

    // ── 来源②：构造函数的隐式指引 —— 拿每个构造函数的形参表当"指引形参表" ──
    // demo: template<class T> struct MyPtr { MyPtr(T q); };   实参 int
    //       ⇒ P = T, A = int ⇒ T := int ⇒ MyPtr<int>
    for (auto& method : primary->classTemplate->methods) {
        if (method->kind != NodeKind::Constructor) continue;
        auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
        std::vector<Parameter> params = ctor->parameters;
        if (params.size() != argTypes.size()) continue;

        std::unordered_map<std::string, TypePtr> subst;
        std::string via = std::format("构造函数 {}({})", t->name, params.size());
        if (!tryGuide(params, primary->typeParams, via, subst)) continue;

        TypePtr tid = Type::makeClass(t->name);
        for (auto& p : primary->typeParams) {
            tid->templateArgs.push_back(TemplateArg::ofType(subst[p]));
        }
        std::cout << std::format("  [ctad] ✔ 由构造函数推出 ⇒ {}\n", tid->toString());
        return tid;
    }

    std::cout << std::format(
        "  [ctad] ✗ 推导失败 —— 没有可用的推导指引，构造函数也推不出模板实参\n");
    return t;   // 交回原路径：由 resolveType 报"缺模板实参"
}

// 把替换表套用到类型上（指引 targetArgs 用）。
// 指引写的是 `-> MyPtr<T>`，T 是**指引自带的模板形参**，而要推的可能是外层类的模板
// 形参（两者名字空间不同）⇒ 借道一次完整替换（复用 substituteType）。
TypePtr SemanticAnalyzer::substituteInType(
    const TypePtr& type, const std::unordered_map<std::string, TypePtr>& subst) {
    TemplateInstantiator::TypeSubstitution tagged;
    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);
    return m_instantiator.substituteType(type, tagged);
}

TypePtr SemanticAnalyzer::expandAliasTemplate(
    const std::string& name, const std::vector<TemplateArg>& args) {

    auto it = m_aliasTemplates.find(name);
    if (it == m_aliasTemplates.end()) return nullptr;   // 不是别名模板，交给类模板路径

    TemplateDeclPtr decl = it->second;

    // ── 递归防护：`template<class T> using X = X<T>;` 展开 X 的过程中又遇到 X ──
    // 对照 clang: error: recursive alias template instantiation
    for (const auto& expanding : m_expandingAliases) {
        if (expanding == name) {
            error(std::format(
                "recursive alias template instantiation '{}' —— 别名模板的底层"
                "类型里又用到了它自己，展开会无限递归", name), SourceLocation{});
        }
    }

    // ── 实参个数校验（[temp.alias]/2）：必须与形参表一致 ──
    // 同样走软失败通道 —— `Vec<int,int>` 出现在重载候选 / 偏特化模式里时，
    // 应当表现为"该候选不成立"而不是硬错误。
    if (args.size() != decl->templateParams.size()) {
        Sfinae::fail(std::format(
            "[Alias Error] alias template '{}' expects {} argument(s), got {}",
            name, decl->templateParams.size(), args.size()));
    }

    // ── 逐位绑定：形参名 → 实参（tagged：类型与 NTTP 值混排）──
    TemplateInstantiator::TypeSubstitution subst;
    for (size_t i = 0; i < args.size(); i++) {
        const TemplateParam& param = *decl->templateParams[i];
        const auto& arg   = args[i];

        // kind 校验（[temp.arg]/1）：类型形参收类型实参，非类型形参收值。
        if (param.kind == TemplateParamKind::Type && !arg.isType()) {
            Sfinae::fail(std::format(
                "[Alias Error] alias template '{}' parameter '{}' expects a type",
                name, param.name));
        }
        if (param.kind != TemplateParamKind::Type && arg.isType()) {
            Sfinae::fail(std::format(
                "[Alias Error] alias template '{}' parameter '{}' expects a "
                "non-type value", name, param.name));
        }
        subst[param.name] = arg;
    }

    std::cout << std::format("  [alias] 展开别名模板 {} → {}，替换表 {{",
        name, decl->aliasTemplate->underlyingType
                  ? decl->aliasTemplate->underlyingType->toString() : "?");
    bool first = true;
    for (auto& [k, v] : subst) {
        std::cout << std::format("{}{} := {}", first ? "" : ", ", k, v.toString());
        first = false;
    }
    std::cout << "}\n";

    // ── 只替换、不实例化（[temp.alias]/1）──
    // 复用类模板那套结构化替换引擎：`MyPtr<T>` 里的 T 换成 int 后，会变成一个
    // Class(MyPtr) + templateArgs=[int] 的节点，再交回 resolveType 按普通类模板 id
    // 实例化 ⇒ 别名与手写 MyPtr<int> 走【完全同一条路】，实例化引擎无需第二套逻辑。
    m_expandingAliases.push_back(name);
    TypePtr expanded;
    try {
        expanded = m_instantiator.substituteType(
            decl->aliasTemplate->underlyingType, subst);
        expanded = resolveType(expanded);
    } catch (...) {
        m_expandingAliases.pop_back();
        throw;      // SubstitutionFailure 原样上抛给 Sfinae::attempt
    }
    m_expandingAliases.pop_back();

    std::cout << std::format("  [alias] ★ {}<...> 解糖 ⇒ {}\n",
        name, expanded ? expanded->toString() : "?");

    // ── 解糖后仍停留在形参上 ⇒ 外层模板还没绑，保持依赖 ──
    // demo: template<class T> struct W { Vec<T> v; }; 在 W 的蓝图里展开 Vec<T> 时 T 仍是
    //   TemplateParam，substituteType 原样返回；等 W<int> 实例化时对字段类型再做替换，
    //   才真正落到 MyPtr_int。
    if (expanded && expanded->isTemplateParam()) {
        std::cout << std::format(
            "  [alias] 结果仍是模板形参 '{}' ⇒ 保持依赖，等外层替换\n",
            expanded->templateParamName);
    }
    return expanded;
}

TypePtr SemanticAnalyzer::evaluateDecltype(const ExprPtr& expr, bool paren) {
    if (!expr) {
        Sfinae::fail("decltype: operand is empty");
    }

    std::cout << std::format("  [decltype] 求值 decltype({}) —— {}\n",
        paren ? "(e)" : "e",
        paren ? "加括号 ⇒ 取【表达式类型】(左值带 &)"
              : "未加括号 ⇒ 若为 id-expression 则取【声明类型】");

    // ── ★ SFINAE 的收口处：把"立即上下文里的硬错误"降级为软失败 ★ ──
    // 求值操作数时 inferMember 会直接调 error()，普通代码里那是真错误；但在 decltype
    //   探测里它是"该类型不支持这个成员"的信号，必须表现为"偏特化不匹配"而非编译失败
    //   （[temp.deduct]/8：失败发生在 immediate context 内，即表达式自身的类型检查）。
    // 用例  declval<int>().begin() ⇒ error("Cannot access member 'begin' on non-class
    //       type 'int&&'") ⇒ 本处 Sfinae::demote 降级 ⇒ 该偏特化被移出而非报错
    // ⚠ 捕获顺序：SubstitutionFailure 派生自 runtime_error，必须排在前面，
    //   否则会被下面的 catch 二次包装、丢掉原始语义。
    TypePtr t;
    m_inferDepth++;
    try {
        t = inferType(expr);
    } catch (const SubstitutionFailure&) {
        m_inferDepth--;
        throw;                       // 已经是软失败，原样上抛
    } catch (const std::runtime_error& e) {
        m_inferDepth--;
        // 全项目唯一一处"硬错误 → 软失败"的转换点，日志由 SFINAE 模块统一出口
        Sfinae::demote(e.what());
        Sfinae::fail(std::format(
            "decltype: operand is not valid in this context — {}", e.what()));
    }
    m_inferDepth--;

    if (!t) {
        Sfinae::fail("decltype: cannot deduce the type of the operand");
    }
    if (t->isAuto()) {
        // 推导不出来（如 declval<T>() 里 T 仍未确定）——按 immediate-context 失败处理
        Sfinae::fail(
            "decltype: operand type is still undeduced ('auto')");
    }

    // ── 未加括号：取声明类型（inferType 给的就是它）──
    if (!paren) {
        std::cout << std::format("  [decltype]   ⇒ 声明类型 = {}\n", t->toString());
        return t;
    }

    // ── 加括号：取表达式类型，左值要带上 & ──
    // 【本实现的边界】minicc 的 inferType 不携带值类别信息，故用 isLValueExpr 对操作数
    //   形态做一次结构判定来近似：id-expression / 成员访问 / 解引用 / 下标 / 字符串字面量
    //   判为左值，其余（算术、比较、函数调用返回值）判为 prvalue。
    //   真 C++ 是在每个表达式节点上带 ValueKind 的，见 [basic.lval]。
    TypePtr result = t;
    if (!t->isReference() && isLValueExpr(expr)) {
        result = Type::makeLValueReference(t);
        std::cout << std::format("  [decltype]   ⇒ 左值 ⇒ 表达式类型 = {}&\n", t->toString());
    } else {
        std::cout << std::format("  [decltype]   ⇒ 右值 ⇒ 表达式类型 = {}\n", t->toString());
    }
    return result;
}

// 值类别判定的简化版：哪些表达式形态是左值。
// 对照 clang：Expr::isLValue() / getValueKind()。
bool SemanticAnalyzer::isLValueExpr(const ExprPtr& expr) const {
    if (!expr) return false;
    // 值类别由节点种类直接决定，一次 switch 把分派表看全（零 RTTI 标签分派）。
    switch (expr->kind) {
        // 变量引用 [expr.prim.id]、成员访问 [expr.ref]、
        // 下标 [expr.sub]、字符串字面量 [lex.string] —— 都是左值
        case NodeKind::Var:
        case NodeKind::Member:
        case NodeKind::Index:
        case NodeKind::StringLiteral:
            return true;
        // 注：真 C++ 里解引用 *p 也是左值（[expr.unary.op]/1），但本项目的
        // UnaryOp 只有 Neg/Not 两种，构造不出解引用节点，故无此分支。
        default:
            return false;   // 其余（算术/比较/调用/new/字面量）按 prvalue
    }
}

// ─── 静态常量成员：查找 + 折叠（理论见 docs/learn/20）─────────────────────────
// 【理论】[class.member.lookup] 的名字查找沿【基类链】进行 —— 子类没有该名字就去基类
//   找，这正是 std::is_range 继承 std::false_type 之后还能访问 ::value 的原因。
// 对照 clang：LookupQualifiedName + LookupInBases。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  is_int<int>::value    // is_int<int> 实例化后继承 std::true_type
// │ 查找轨迹  第 0 层 is_int_int：无 value ⇒ 第 1 层 std::true_type：命中 value = 1
// │ 日志  [static] ★ 静态常量命中：std::true_type::value = 1 : bool（沿继承链第 1 层）
// │ 输出  通过 outValue 带出数值，通过 m_lastStaticConstType 带出类型
// │       （后者决定折叠成 BoolLiteral 还是 IntLiteral，见 foldStaticConst）
// │ 深度上限 64 —— 继承成环时不死循环（本项目不做环检测）
// └────────────────────────────────────────────────────────────────────────────
bool SemanticAnalyzer::lookupStaticConst(const std::string& className,
                                         const std::string& member,
                                         int64_t& outValue) {
    std::string cur = className;
    // 防御性深度上限：继承成环时不死循环（本项目不做环检测）
    for (int depth = 0; depth < 64 && !cur.empty(); depth++) {
        auto it = m_classDecls.find(cur);
        if (it == m_classDecls.end()) return false;
        ClassDeclPtr decl = it->second;

        auto sc = decl->staticConsts.find(member);
        if (sc != decl->staticConsts.end()) {
            outValue = sc->second.value;
            m_lastStaticConstType = sc->second.type;   // 供折叠时决定字面量种类
            std::cout << std::format(
                "  [static] ★ 静态常量命中：{}::{} = {} : {}（沿继承链第 {} 层）\n",
                cur, member, outValue,
                sc->second.type ? sc->second.type->toString() : "?",
                depth);
            return true;
        }
        cur = decl->firstBase();   // 继续往基类找
    }
    return false;
}

// 把类型限定访问 Cls<Args>::value 在编译期折叠成字面量。
// 【为什么必须折叠】静态成员不占对象内存、没有偏移量 —— 不折叠的话 CodeGen 会按
//   "字段偏移"去寻址，读到的是垃圾。值在编译期已完全确定，正确做法就是替换成常量。
// 对照 clang：常量求值上下文中 Expr::EvaluateAsInt 直接把节点求成 APValue。
// ┌─ DEMO（真实日志）──────────────────────────────────────────────────────────
// │ 源码  template <typename T> struct is_int : public std::false_type {};
// │       template <> struct is_int<int> : public std::true_type {};
// │       bool v = is_int<int>::value;
// │ 日志  [spec:select]   ├─ ① explicit specialization matched → USING IT
// │       [instantiate:class] ★ on-demand instantiation: is_int<int>
// │       [static] ★ 静态常量命中：std::true_type::value = 1 : bool（沿继承链第 1 层）
// │       [static] 折叠为字面量：is_int_int::value(is_int) → 1
// │ 输出  BoolLiteralExpr(true) —— 整个 Cls<Args>::value 节点被换掉
// │ 字面量种类  按常量自身的类型给：bool 给 BoolLiteralExpr，其余给 IntLiteralExpr。
// │       ⚠ 不能一律给 int —— typeCompatible(int, bool) 为 false，会误报类型不匹配。
// └────────────────────────────────────────────────────────────────────────────
ExprPtr SemanticAnalyzer::foldStaticConst(ExprPtr expr) {
    // 定向形态判定：只看是不是 `X::value` 这种静态成员访问（isTypeAccess）
    if (expr->kind != NodeKind::Member) return expr;
    auto me = std::static_pointer_cast<MemberExpr>(expr);
    if (!me->isTypeAccess) return expr;

    // object 是模板 id（VarExpr{name, explicitTemplateArgs}）
    if (me->object->kind != NodeKind::Var) return expr; //wangyang object 是var 表达式
    auto ve = std::static_pointer_cast<VarExpr>(me->object);

    // ── 解析出类实例名：带实参 Box<int> ⇒ 先实例化拿 Box_int；不带实参直接当类名 ──
    std::string clsName = ve->name; // 变量name
    if (!ve->explicitTemplateArgs.empty()) {
        TypePtr tid = Type::makeClass(ve->name);
        for (const auto& ta : ve->explicitTemplateArgs) {
            tid->templateArgs.push_back(
                ta.isType() ? TemplateArg::ofType(resolveType(ta.type)) : ta);
        }
        TypePtr inst = resolveType(tid); // wangyang**** 就是在这里解析相应的类型  tests/tmpl/test_tmpl_35_void_t_detect.cpp:74
        if (!inst) return expr;
        clsName = inst->name;
    }

    int64_t value = 0;
    m_lastStaticConstType = nullptr;
    if (!lookupStaticConst(clsName, me->memberName, value)) {
        // ── 诊断改进：类型限定访问 `Cls<Args>::member` 按语法只可能是静态成员，
        // 到这里找不到就说明【该成员在这个类里不存在】。若放任它走常规成员访问路径，
        // 会报成 "Undefined variable 'Cls'" —— 完全指错了方向。
        // 典型场景是 SFINAE 回退（test_tmpl_38）：偏特化因替换失败被移除 → 回退主模板
        //   → 用户真正需要知道的是"回退到的那个类没有这个成员"。
        // 例外：若同名成员是【方法】（Cls<Args>::make() 这种调用），它不是静态常量，
        //   应交回调用路径处理，不算错。
        bool isMethod = false;
        auto clsIt = m_classDecls.find(clsName);
        if (clsIt != m_classDecls.end()) {
            for (auto& m : clsIt->second->methods) {
                if (m->name == me->memberName) { isMethod = true; break; }
            }
        }
        if (!isMethod) {
            error(std::format(
                "no static member '{}' in class '{}' —— 类型限定访问 Cls<Args>::member "
                "只解析静态成员；若此处是 SFINAE 探测，说明偏特化被移除后回退到的"
                "主模板没有该成员（对照 test_tmpl_36 的兜底写法）",
                me->memberName, clsName), me->location);
        }
        return expr;   // 是方法 —— 交回常规调用路径
    }

    // 按常量自身的类型生成对应字面量：bool 常量给 BoolLiteralExpr，其余给 Int。
    // ⚠ 不能一律给 int —— 本项目 typeCompatible(int, bool) 为 false，
    // `bool v = Trait<T>::value;` 会因此误报类型不匹配。
    ExprPtr lit;
    if (m_lastStaticConstType && m_lastStaticConstType->isBool()) {
        lit = std::make_shared<BoolLiteralExpr>(value != 0);
    } else {
        lit = std::make_shared<IntLiteralExpr>(value);
    }
    lit->location = me->location;
    std::cout << std::format("  [static] 折叠为字面量：{}::{}({}) → {}\n",
        clsName, me->memberName, ve->name, value);
    return lit;
}

// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  Box<int> b;  Box<int*> c;  Undeclared q;  std::void_t<decltype(x)>
// │ 输入  Parser 建的"书写名类型"：Class("Box") + templateArgs=[int]（不查名，只按形式）
// │ 日志  [sema:targ] ✓ template arguments OK: Box<int>
// │       [spec:select] ★ selecting class template 'Box' for <int> ⇒ [instantiate:class]
// │ 输出  ① Box<int> → Box_int（布局齐全）② Box<int*> 同一路径，实参先递归 resolveType
// │       ③ Undeclared q; → error: unknown type name 'Undeclared'
// │       ④ void_t<...> → void；实参不合法则抛 SubstitutionFailure
// │ 五条分支（按序） decltype → void_t → 类模板 id → 普通类名查符号表 → 指针/引用/const
// │ ★ void_t 单独认：语义是"实参全合法 ⇒ void"，按普通类名解析会误报
// │   "'std::void_t' is not a class template"。
// └────────────────────────────────────────────────────────────────────────────
// 模板 id 的实参里是否还挂着【未绑定】的模板形参（区分"可实例化"与"等外层替换"：
//   Box<int> 可以，Box<T> 不可以 —— 后者硬实例化会造出假实例）。
static bool containsTemplateParam(const TypePtr& t) {
    if (!t) return false;
    for (const auto& arg : t->templateArgs) {
        if (!arg.isType() || !arg.type) continue;
        if (arg.type->isTemplateParam()) return true;
        if (containsTemplateParam(arg.type)) return true;
    }
    return false;
}

TypePtr SemanticAnalyzer::resolveType(TypePtr type) {
    if (!type) return nullptr;

    // ── decltype(expr)：非依赖上下文的立即求值（依赖上下文见 substituteType Case 1.5）──
    if (type->isDecltype()) {
        std::cout << "  [decltype] resolveType 遇到 decltype 节点，尝试立即求值\n";
        try {
            return evaluateDecltype(type->decltypeExpr, type->decltypeParen);
        } catch (const SubstitutionFailure& e) {
            // 求值不成（多半因为操作数里有还没绑定的模板参数）——
            // 保留 Decltype 节点继续传递，等替换阶段再算。
            std::cout << std::format(
                "  [decltype] 立即求值不成（{}）⇒ 保持延迟，等替换阶段\n", e.what());
            return type;
        }
    }

    // ── std::void_t<...>：归约为 void ──
    // 解析期若把 void_t 当成普通类名，会撞上"'std::void_t' is not a class template"
    // 的误报。它的语义是"实参全部合法则等价于 void"，故直接归约；实参不合法时抛
    // SubstitutionFailure（SFINAE）。
    if (type->isClass() &&
        (type->name == "std::void_t" || type->name == "void_t")) {
        std::cout << "  [void_t] 归约 std::void_t<...> → void\n";
        for (const auto& ta : type->templateArgs) {
            if (ta.isType() && ta.type) {
                // 递归解析各实参：内部含 decltype 时会在上面被求值，
                // 求值失败即抛 SubstitutionFailure（软失败）。
                resolveType(ta.type);
            }
        }
        return Type::makeVoid();
    }

    if (type->isClass()) {
        // ── 嵌套类型名 S::type / S<int>::type（类内类型别名，理论见 docs/learn/24）──
        // Parser 把 `Q::m` 记成 Class(m) + nestedQualifier=Q（见 parseType 的 `::` 循环）：
        //   ① 限定部分先当普通类型解析 —— Box<int> 在此被【实例化】成 Box_int，故下面查
        //      的就是实例化后的类声明（其 typeAliases 已由 Instantiator 替换过）；
        //   ② 在该类声明的 typeAliases 里查成员名，命中即递归解析其目标。
        // 用例  struct S { using type = int; };   S::type x; ⇒ x : int
        //       Box<int>::type（Box 内 `using type = T;`）⇒ Box<int> 先实例化 ⇒ int
        // 对照 clang：Sema::getTypeName → LookupQualifiedName + 别名解糖
        // （clang/lib/Sema/SemaType.cpp:getTypeName / Type::getUnqualifiedDesugaredType），
        // 本实现省掉了作用域链与 using 引入，只做「单级成员查表」。
        if (type->isNestedName()) {
            // ── 限定者还是模板形参 ⇒ 依赖上下文，不能在这里查 ──
            // `typename T::type` 要等实例化拿到实参才知道 T 是谁。
            // 原样返回（保持依赖），由 TemplateInstantiator::substituteType
            // 的 Case 5.5 在替换阶段查表 —— 那里才是 [temp.deduct]/8 的直接上下文。
            if (type->nestedQualifier && type->nestedQualifier->isTemplateParam()) {
                std::cout << std::format(
                    "  [sema:alias] {}::{} 限定者仍是模板形参 ⇒ 保持依赖，等替换阶段\n",
                    type->nestedQualifier->templateParamName, type->name);
                return type;
            }

            TypePtr qual = resolveType(type->nestedQualifier);
            std::string qualName = (qual && qual->isClass()) ? qual->name : "?";
            TypePtr member = findMemberType(qual, type->name);
            if (member) {
                TypePtr target = resolveType(member);
                std::cout << std::format("  [sema:alias] {}::{} ⇒ {}\n",
                    qualName, type->name, target ? target->toString() : "?");
                return target;
            }

            // 查不到：看当下是不是在直接上下文里 —— 是则降级为软失败
            // （SFINAE：该候选不成立，换下一个），不是才硬报错。
            std::string why = std::format("no type named '{}' in '{}'",
                                          type->name, qualName);
            if (SfinaeContext::inImmediateContext()) Sfinae::fail(why);
            error(why, SourceLocation{});
        }

        // ── P3：模板 id（Box<int>）→ 按需实例化，产出具体实例类型 ──
        // 判定条件：携带非空实参，且该名字是已登记的类模板蓝图（m_classTemplates，O(1)）。
        // （普通已实例化类如 Box_int 实参为空，走下面的符号表替换。）
        if (!type->templateArgs.empty()) {
            // ── 别名模板 id X<int>（[temp.alias]）──
            // ★ 必须排在类模板分支【前面】：别名与类模板在语法上长得一模一样
            // （都是 名字<实参>），只能靠注册表区分。先问别名表，命中即解糖。
            if (m_aliasTemplates.count(type->name)) {
                std::vector<TemplateArg> args;
                args.reserve(type->templateArgs.size());
                for (auto& arg : type->templateArgs) {
                    // 同一套按 kind 分派：类型实参递归解析（可为内层模板 id），
                    // NTTP 值实参原样带过。
                    if (arg.isType())
                        args.push_back(TemplateArg::ofType(resolveType(arg.type)));
                    else
                        args.push_back(arg);
                }
                std::cout << std::format(
                    "  [sema:alias] 遇到别名模板 id {}<...> ⇒ 解糖\n", type->name);
                return expandAliasTemplate(type->name, args);
            }
            // ── 实参里还有没绑定的模板形参 ⇒ 保持依赖，不实例化 ──
            // ★ 硬去实例化会拿 TemplateParam 当模板实参造出一个名叫 `Box_T` 的【假实例】
            //   （字段类型是 T、方法返回 T），后面全部对不上号，且不报错只是算错。
            // 对照 clang：Sema::CheckTemplateIdType 里若含依赖实参，建成
            //   TemplateSpecializationType（带 sugar 的依赖类型）而非实例化。
            if (containsTemplateParam(type)) {
                std::cout << std::format(
                    "  [sema] {} 的实参仍是模板形参 ⇒ 保持依赖，等外层替换\n",
                    type->toString());
                return type;
            }
            if (m_classTemplates.count(type->name)) {
                // 实参先递归解析（实参本身可能是模板 id：Box<Box<int>>）
                TypePtr tid = Type::makeClass(type->name);
                tid->templateArgs.reserve(type->templateArgs.size());

                // 形参表（逐位判"这一位期望什么"）—— 模板模板位要特殊对待：
                // Parser 把实参位的 `Box` 建成了 Class("Box") 类型节点，若直接送
                // resolveType，会撞上"裸类模板名不能当类型"的报错。故先看形参位。
                const std::vector<TemplateParamPtr>* fps = nullptr;
                if (auto ptIt = m_classTemplates.find(type->name);
                    ptIt != m_classTemplates.end()) {
                    fps = &ptIt->second->templateParams;
                }

                for (size_t i = 0; i < type->templateArgs.size(); i++) {
                    auto& arg = type->templateArgs[i];
                    // ★ 按 kind 分派（[temp.arg]）：类型实参 → 递归 resolveType
                    //   （内层模板 id 也要实例化，如 Box<Box<int>>）；
                    //   非类型实参（NTTP）→ 是常量值，没有类型可解析，原样带过。
                    if (arg.isType()) {
                        // ── ★ 该位期望一个【模板】时，实参是模板名而不是类型 ──
                        // `Wrap<Box, int>` 的 Box 不是"要被实例化的类型"，它本身就是
                        // 实参（[temp.arg.template]）。取出名字标成 Template 形态，
                        // 交给替换期去做 `C<T>` → `Box<T>` 的改名。
                        const TemplateParam* fp =
                            (fps && i < fps->size()) ? fps->at(i).get() : nullptr;
                        if (fp && fp->kind == TemplateParamKind::Template) {
                            // 这一位要的是【模板名】：`Wrap<int, ...>` 这种拿类型来填的
                            // 必须在此响亮报错，否则会造出一个模板名叫 "int" 的假实例，
                            // 一路算到汇编期才炸。对照 clang：err_template_template_parm_mismatch
                            // / "template template argument must be a class template"。
                            const std::string& tn = arg.type ? arg.type->name : std::string();
                            if (!arg.type || !arg.type->isClass() || tn.empty()
                                || (!m_classTemplates.count(tn)
                                    && !m_aliasTemplates.count(tn))) {
                                error(std::format(
                                    "template argument {} for '{}' ('{}') must be a class "
                                    "template, but '{}' is not a template",
                                    i + 1, type->name, fp->name,
                                    arg.type ? arg.type->toString() : "?"),
                                    SourceLocation{});
                            }
                            std::cout << std::format(
                                "  [sema:targ]   ⤷ 第 {} 位期望模板 ⇒ 实参 '{}' 按"
                                "【模板名】处理（[temp.arg.template]）\n",
                                i + 1, arg.type ? arg.type->name : "?");
                            tid->templateArgs.push_back(
                                TemplateArg::ofTemplate(arg.type ? arg.type->name : ""));
                            continue;
                        }
                        tid->templateArgs.push_back(
                            TemplateArg::ofType(resolveType(arg.type)));
                    }
                    else {
                        tid->templateArgs.push_back(arg);
                    }
                }
                // 模板 id 尚未实例化 ⇒ 当场实例化，拿到具体的类类型
                return getOrInstantiateClass(tid, SourceLocation{});
            }
        } else if (m_classTemplates.count(type->name) ||
                   m_aliasTemplates.count(type->name)) {
            // ── 裸类模板名 / 裸别名模板名（[temp.arg.explicit]）──
            // `Box b;` 不带实参：蓝图不是类型，不能当类名解析。此处早期报错，
            // 而不是放行未解析类型到使用期才报 'Class not declared'（诊断不指向根因）。
            // 对照 clang：use of class template 'Box' requires template arguments。
            // 别名模板同理：`Vec v;` 在 clang 里也是同一句诊断。
            bool isAlias = m_aliasTemplates.count(type->name) != 0;
            error(std::format("'{}' is {} {} template; provide template "
                              "arguments (e.g. {}<int>)",
                              type->name, isAlias ? "an" : "a",
                              isAlias ? "alias" : "class",
                              type->name), SourceLocation{});
        }
        Symbol* sym = m_symbolTable.lookup(type->name);
        // Parser 遇到 `Dog*` 这类书写名时会临时 new 一个 Class("Dog")，其 classLayout
        // 是空的；真正带字段/偏移/虚表布局的类型在 processClassDecl 阶段已登记进符号表
        // （kind=Type）。只要查到的注册类型与来者不是同一个对象，就用注册版替换 ——
        // 否则后面 inferMember 的 classLayout.findField 会在空布局上查无此字段。
        if (sym && sym->kind == SymbolKind::Type && sym->type &&
            sym->type != type) { // 指针不相同说明指向的不是同一个对象
            return sym->type;
        }

        // ── ★ 兜底：这个名字【从未被声明过】──
        // 不放行就原样返回那个空布局的 Class 节点，下面这些用例会全部 rc=0 悄悄通过：
        //     Undeclared q;   ｜   int a = 1; a q;（拿变量名当类型名）｜  a < b > z;
        // 空布局的后果不止"少报一个错"：后续 sizeof/字段访问拿到 0 或垃圾偏移，错误被
        //   推迟到运行期；`a < b > z;`（[stmt.ambig] 那套前瞻把比较式误判成声明）也因此
        //   能【静默编译】。对照 clang：error: unknown type name 'Undeclared'
        // 注：Parser 只为【书写名】建 Class 节点；模板形参走 TemplateParam 节点、模板 id
        //   走上面的实例化分支，都不会落到这里。
        if (!sym || sym->kind != SymbolKind::Type) {
            // ── 命名空间作用域回退：命名空间内用【非限定】名字 ──
            // `namespace N { struct S{}; int get(S s){...} }` 里那个 S：全局表只有
            // "N::S"（命名空间成员的登记方式就是名字前缀化），故这里按
            // "当前命名空间::名字" 再查一次。
            // 对照 clang：LookupName 沿 DeclContext 链上溯；本实现只有一层。
            if (!m_currentNamespace.empty()) {
                std::string scoped = m_currentNamespace + "::" + type->name;
                Symbol* nsSym = m_symbolTable.lookup(scoped);
                if (nsSym && nsSym->kind == SymbolKind::Type && nsSym->type) {
                    std::cout << std::format(
                        "  [resolve] '{}' 在命名空间 '{}' 内 ⇒ {}\n",
                        type->name, m_currentNamespace, scoped);
                    return nsSym->type;
                }
            }

            // ── 类作用域回退：类体内直接用别名（`type x;`）──
            // [basic.lookup.unqual] 在类体内先查类作用域再查外层。本实现没有作用域链，
            // 只有 m_currentClassName 这一个"当前类"，故用「查不到就回退到当前类的
            // typeAliases」近似 —— 足够覆盖类模板体内 `using type = T;` 后紧跟
            // `type x;` 这个最常见的写法。
            if (!m_currentClassName.empty()) {
                auto cit = m_classDecls.find(m_currentClassName);
                if (cit != m_classDecls.end()) {
                    auto ait = cit->second->typeAliases.find(type->name);
                    if (ait != cit->second->typeAliases.end() && ait->second) {
                        std::cout << std::format(
                            "  [sema:alias] {} 在类 '{}' 作用域内 ⇒ {}\n",
                            type->name, m_currentClassName,
                            ait->second->toString());
                        return resolveType(ait->second);
                    }
                }
            }
            error(std::format(
                "unknown type name '{}' —— 该名字没有作为类/类型被声明过。"
                "（若原文是比较表达式如 `a < b > c;`，本实现的前瞻会把它当成"
                "变量声明：C++ 的 [stmt.ambig] 规定「能当声明就当声明」，"
                "这需要解析期符号表；本实现用「跳过 <...> + 回滚」的试探法近似，"
                "详见 docs/learn/22 的已知边界）",
                type->name), SourceLocation{});
        }
    }
    if (type->isPointer()) {
        auto base = resolveType(type->pointeeType);
        if (base != type->pointeeType) return Type::makePointer(base);
    }
    if (type->isReference()) {
        auto base = resolveType(type->referencedType);
        if (base != type->referencedType) return Type::makeLValueReference(base);
    }
    if (type->isRValueReference()) {
        auto base = resolveType(type->referencedType);
        if (base != type->referencedType) return Type::makeRValueReference(base);
    }
    if (type->isConst()) {
        auto base = resolveType(type->innerType);
        if (base != type->innerType) return Type::makeConst(base);
    }
    return type;
}

static bool typeCompatible(const TypePtr& expected, const TypePtr& got) {
    if (!expected || !got) return false;
    TypePtr e = expected, g = got;
    while (e->isReference()) e = e->referencedType;
    while (g->isReference()) g = g->referencedType;
    if (e->isConst()) e = e->innerType;
    if (g->isConst()) g = g->innerType;
    if (e->equals(g)) return true;
    return e->isDouble() && g->isInt();
}

SemanticAnalyzer::SemanticAnalyzer() {
    // 把自己挂成实例化引擎的三个求值器（decltype / 成员类型 / 别名模板）。
    // 这是"依赖倒置"的落地点：TemplateInstantiator 需要求值能力，但求值住在 Sema 里，
    // 于是 instantiator 只持有抽象接口指针，由 Sema 在构造时注入自己。
    // 对照 clang：不需要这层 —— 它的 Instantiator 本身就是 Sema（TreeTransform 派生自
    // Sema），本项目为保住分层与可单测性而外提。
    m_instantiator.setDecltypeEvaluator(this);
    m_instantiator.setMemberTypeResolver(this);
    m_instantiator.setAliasTemplateResolver(this);
}

// ─── 推导缩进辅助 ───────────────────────────────────────────────────────────
std::string SemanticAnalyzer::inferIndent() const {
    return std::string(m_inferDepth * 2, ' ');
}

// ─── 分析入口：三遍扫描（理论见 docs/learn/22）────────────────────────────────
// Pass 1 收集类和模板声明 → 建立类型注册表；Pass 2 注册所有函数名（支持递归）；
// Pass 3 分析函数体 → 类型检查、auto 推导。
// 【为什么必须三遍】名字可以先使用后声明：Pass 1 先注册类型 ⇒ 类字段/继承引用不受声明
//   顺序影响；Pass 2 先注册函数名 ⇒
//   用例  int f() { return g(); } 写在 int g() 之前也能跑（Pass 3 在 m_functionMap 查到 g）
// 对照 clang：先把所有 Decl 挂进 DeclContext（建名字索引），函数体延迟到完整定义后分析。
// ⚠ 已知边界：Pass 1 单遍处理继承，基类必须先于派生类出现（不支持前向声明，教学级简化）。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Base {...};   Base b;   namespace N {}   using T = int;
// │ 输入  DeclPtr —— Parser 产出的顶层声明，是 6 种具体声明的共同基类
// │ 日志  （无）—— 本函数只做分发，活交给各自的 processXxx
// │ 输出  无返回值；就地改动声明节点，并写进 m_classTypes / m_symbolTable
// └────────────────────────────────────────────────────────────────────────────
// ─── forEachFunctionDecl：对每个函数声明执行 action ──────────────────────────
// 顶层函数、类内方法，并【递归进命名空间】—— 命名空间里的类与函数不在
//   unit.declarations 顶层，扁平遍历会整个漏掉它们的方法（症状：Sema 侧一切正常，
//   但 CodeGen 从没收到那些方法 ⇒ 链接期 undefined reference to 'N__S_N__S'）。
// 分派方式：handler 需要 shared_ptr（registerFunction / analyzeFunctionBody 都收
//   FuncDeclPtr）⇒ 用 NodeKind 标签分派，判据同 processDecl。
void SemanticAnalyzer::forEachFunctionDecl(
    const std::vector<DeclPtr>& decls,
    const std::function<void(FuncDeclPtr)>& action) {
    for (auto& decl : decls) {
        switch (decl->kind) {
            case NodeKind::Function:
                action(std::static_pointer_cast<FunctionDecl>(decl));
                break;
            case NodeKind::Class: {
                auto cls = std::static_pointer_cast<ClassDecl>(decl);
                for (auto& method : cls->methods) action(method);
                break;
            }
            case NodeKind::Namespace: {
                auto ns = std::static_pointer_cast<NamespaceDecl>(decl);
                std::string saved = m_currentNamespace;
                m_currentNamespace = saved.empty() ? ns->name : saved + "::" + ns->name;
                forEachFunctionDecl(ns->declarations, action);
                m_currentNamespace = saved;
                break;
            }
            default:
                break;   // 其余声明不产生独立函数体
        }
    }
}

// 声明分发器。
//
// ★ 判据（本项目唯一权威表述在 include/semantic_analyzer.h）：handler 只要引用 →
//   访问者（accept 虚分派）；还要 shared_ptr 所有权或返回值 → 标签分派（switch）。
// 用例  NodeKind::Class ⇒ processClassDecl(decl)（要把 shared_ptr 存进注册表）
//       NodeKind::Block ⇒ processStmt ⇒ accept 虚分派 ⇒ visit(Stmt&)（只要引用）
// 本例为何属后者：下面每个 handler 都要把 shared_ptr 交给注册表（m_classDecls /
//   m_classTypes / m_globalVars / m_enumDecls 存的就是它），而 AstVisitor::visit 只给
//   【引用】—— 从引用还原 shared_ptr 不安全（对象未必由 shared_ptr 持有，控制块是错的）。
// 对照  processStmt 走访问者（语句链 handler 不需要所有权）；clang 的 Decl 处理同样是
//   dyn_cast + switch，RecursiveASTVisitor 只服务遍历。
void SemanticAnalyzer::processDecl(DeclPtr decl) {
    if (!decl) return;

    // 一次 switch（跳表）分派。static_pointer_cast 安全的前提：节点 kind 由构造函数
    // 设定，恒等于自身类型（见 ast_visitor.h 的标签不变式）。
    switch (decl->kind) {
        case NodeKind::Class:
            processClassDecl(std::static_pointer_cast<ClassDecl>(decl)); break;
        case NodeKind::Template:
            processTemplateDecl(std::static_pointer_cast<TemplateDecl>(decl)); break;
        case NodeKind::GlobalVar:
            processGlobalVarDecl(std::static_pointer_cast<GlobalVarDecl>(decl)); break;
        case NodeKind::Enum:
            processEnumDecl(std::static_pointer_cast<EnumDecl>(decl)); break;
        case NodeKind::Namespace:
            processNamespaceDecl(std::static_pointer_cast<NamespaceDecl>(decl)); break;
        case NodeKind::TypeAlias:
            processTypeAliasDecl(std::static_pointer_cast<TypeAliasDecl>(decl)); break;
        // ── 非模板推导指引：`Box(int) -> Box<int>;` —— 顶层独立形态（不带 template<>
        //    外壳），单独接一支；带外壳的那种随 TemplateDecl 走 processTemplateDecl。
        case NodeKind::DeductionGuide:
            registerDeductionGuide(std::static_pointer_cast<DeductionGuideDecl>(decl)); break;
        default:
            break;   // 函数声明等由 analyze 的三趟流程各自处理，不经过这里
    }
}

// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Base { int b; virtual int kind(); };
// │       struct Derived : public Base { int d; int kind(); };
// │       template <typename T> T twice(T x) { return x + x; }
// │       int add(int a, int b) { return a + b; }
// │ 日志  Phase 3: Semantic Analysis (语义分析)
// │         [builtin] ✔ registered libc prototypes: malloc(int)→int*, free(int*), ...
// │         ┌──── Pass 1: 注册类、模板、全局变量与命名空间 ────
// │         ┌──── Pass 2: 注册所有函数（支持递归）──────────────
// │         [register] add(int a, int b) → int    [mangled: add]
// │         ┌──── Pass 3: 分析函数体（类型推导 + 符号决议）────
// │         ╔══ Function Body: Base::kind ══╗
// │ 输出  ① 标注 resolvedType 的 AST（auto 已抹去）② 符号表快照 ③ 类布局表（偏移/vtable/RTTI）
// │ 对照 clang：先把所有 Decl 挂进 DeclContext，函数体延迟到完整定义后再分析 ——
// │   三遍只是它的教学级直译（为什么必须三遍见上方 analyze 注释）。
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::analyze(TranslationUnit& unit) {

    // P4：libc 内建原型先登记——早于 Pass 1，全程可见
    registerBuiltins();

    std::cout << "\n  ┌──── Pass 1: 注册类、模板、全局变量与命名空间 ────\n";
    for (auto& decl : unit.declarations) {
        processDecl(decl);
    }

    // Pass 1 完成后所有类的继承关系已就绪 → 打印继承图（可观测性）
    printInheritanceGraph();

    std::cout << "\n  ┌──── Pass 2: 注册所有函数（支持递归）──────────────\n";
    // ★ 必须【递归进命名空间】：命名空间里的类和函数不在 unit.declarations 顶层，
    //   扁平遍历会整个漏掉它们的方法 —— 症状是 Sema 侧一切正常（类布局、符号表都有），
    //   但 CodeGen 从没收到那些方法 ⇒ 合成构造/析构不发射，链接期报
    //   undefined reference to 'N__S_N__S'。对照 clang：DeclContext 树本身就是递归的。
    forEachFunctionDecl(unit.declarations, [&](FuncDeclPtr f) { registerFunction(f); });

    // dump 全局符号表
    m_symbolTable.dump();

    std::cout << "  ┌──── Pass 3: 分析函数体（类型推导 + 符号决议）────\n";
    // 与 Pass 2 同理：命名空间内的函数体也要分析（形参/局部变量的类型解析、
    // 成员访问偏移，全都发生在这里）。
    forEachFunctionDecl(unit.declarations, [&](FuncDeclPtr f) { analyzeFunctionBody(f); });
}

// ─── 继承图打印（可观测性设施：把 Pass 1 收集到的继承关系渲染成 ASCII 树）────
// 以"无基类的类"为根，沿 baseClassNames 向下展开，树形打印全部类（继承关系是一片森林，
//   多继承下是 DAG）。真编译器在 Sema 内部维护 CXXRecordDecl 的 getBases() 边表，本函数用
//   ClassDecl.firstBase() 即时重建同样的图。
// 标注：[polymorphic] = 带虚函数表 ⇒ 对象含 _vptr，可参与虚调用与 dynamic_cast 的运行时
//   类型检查（typeinfo 继承链由 CodeGen 发射）。
// 输出示例：Animal [polymorphic] ├── Dog [polymorphic] └── Cat [polymorphic]
void SemanticAnalyzer::printInheritanceGraph() {
    // ① 邻接表：父类名 → 子类名列表（m_classDecls 是 Pass 1 的成果）
    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots; // 无基类的根类
    for (auto& [name, decl] : m_classDecls) {
        if (decl->baseClassNames.empty()) {
            roots.push_back(name);
        } else {
            for (auto& bn : decl->baseClassNames)
                children[bn].push_back(name);
        }
    }

    if (m_classDecls.empty()) return; // 无类可打印，静默跳过

    auto isPoly = [this](const std::string& cls) {
        auto it = m_classTypes.find(cls);
        return it != m_classTypes.end() && it->second->classLayout.hasVTable;
    };

    std::cout << "  │ ═══ 类型继承图（inheritance graph）═══\n";

    // ② 深度优先递归：prefix 记录竖线骨架，branch 决定本节点的分叉符号
    std::function<void(const std::string&, const std::string&, const std::string&)> dfs =
        [&](const std::string& cls, const std::string& prefix, const std::string& branch) {
            std::cout << std::format("  │ {}{}{}{}\n", prefix, branch, cls,
                isPoly(cls) ? " [polymorphic]" : "");
            auto it = children.find(cls);
            if (it == children.end()) return;
            auto& kids = it->second;
            std::sort(kids.begin(), kids.end()); // 排序保证输出可复现
            for (size_t i = 0; i < kids.size(); ++i) {
                bool last = (i + 1 == kids.size());
                // 本层分叉符号 + 下探一层的骨架（最后子节点以下空格延续）
                dfs(kids[i],
                    prefix + (branch.empty() ? "" : (last ? "    " : "│   ")),
                    last ? "└── " : "├── ");
            }
        };

    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        dfs(r, "", "");
    }
}

void SemanticAnalyzer::processGlobalVarDecl(GlobalVarDeclPtr decl) {
    decl->declaredType = resolveType(decl->declaredType);
    if (decl->initializer) {
        TypePtr initType = inferType(decl->initializer);
        if (decl->declaredType->isAuto()) {
            decl->declaredType = initType;
        } else if (!typeCompatible(decl->declaredType, initType)) {
            error(std::format("Cannot initialize global variable '{}' of type '{}' with value of type '{}'",
                decl->name, decl->declaredType->toString(), initType->toString()), decl->location);
        }
    }
    m_globalVars.push_back(decl);

    Symbol sym;
    sym.name = decl->name;
    sym.type = decl->declaredType;
    sym.kind = SymbolKind::Variable;
    sym.isLocal = false;
    sym.definedAt = decl->location;
    m_symbolTable.globalScope()->define(decl->name, sym);

    std::cout << std::format("  [register] global variable '{}' : {}\n",
        decl->name, decl->declaredType ? decl->declaredType->toString() : "auto");
}

void SemanticAnalyzer::processEnumDecl(EnumDeclPtr decl) {
    m_enumDecls[decl->name] = decl;
    TypePtr enumType = decl->underlyingType ? decl->underlyingType : Type::makeInt();

    for (auto& item : decl->items) {
        if (item.valueExpr) {
            inferType(item.valueExpr);
        }
        if (!decl->isScoped) {
            Symbol sym;
            sym.name = item.name;
            sym.type = enumType;
            sym.kind = SymbolKind::Variable;
            sym.isLocal = false;
            sym.definedAt = item.location;
            m_symbolTable.globalScope()->define(item.name, sym);
        }
        if (!decl->name.empty()) {
            Symbol scopedSym;
            scopedSym.name = decl->name + "::" + item.name;
            scopedSym.type = enumType;
            scopedSym.kind = SymbolKind::Variable;
            scopedSym.isLocal = false;
            scopedSym.definedAt = item.location;
            m_symbolTable.globalScope()->define(scopedSym.name, scopedSym);
        }
    }
    std::cout << std::format("  [register] enum '{}' ({} items)\n", decl->name, decl->items.size());
}

void SemanticAnalyzer::processNamespaceDecl(NamespaceDeclPtr decl) {
    std::cout << std::format("  [namespace] enter namespace '{}'\n", decl->name);

    // ── 进入命名空间作用域（[basic.namespace] / [basic.scope.namespace]）──
    // 命名空间内的函数/类可【非限定】引用同命名空间的成员：
    //   用例  namespace N { struct S{...}; int get(S s){...} } —— 那个 S 在全局符号表里
    //   根本不存在，只有 "N::S"。
    // 本实现没有作用域链，只用 m_currentNamespace 记住"现在身处哪个命名空间"，resolveType
    //   查不到名字时按 "当前命名空间::名字" 再查一次；嵌套 A::B 由 saved 拼接（与名字前缀化
    //   保持一致）。对照 clang：DeclContext 链天然是作用域链、LookupName 逐层上溯，本实现
    //   等价于"只看最近一层"的简化。
    std::string savedNs = m_currentNamespace;
    m_currentNamespace = savedNs.empty()
        ? decl->name
        : savedNs + "::" + decl->name;

    for (auto& innerDecl : decl->declarations) {
        // 命名空间内的声明一律【名字前缀化】成 N::X（mangling 与符号表都靠它），再走
        // 各自的处理入口。函数只改名不注册：注册是 Pass 2 的职责（forEachFunctionDecl
        // 会递归进命名空间）—— ⚠ 两处都注册会让同一个符号发射两次（汇编器报
        // symbol 'N__get' is already defined）。
        switch (innerDecl->kind) {
            case NodeKind::Function: {
                auto func = std::static_pointer_cast<FunctionDecl>(innerDecl);
                func->name = decl->name + "::" + func->name;
                break;
            }
            case NodeKind::Class: {
                auto cls = std::static_pointer_cast<ClassDecl>(innerDecl);
                cls->name = decl->name + "::" + cls->name;
                processClassDecl(cls);
                break;
            }
            case NodeKind::GlobalVar: {
                auto gvar = std::static_pointer_cast<GlobalVarDecl>(innerDecl);
                gvar->name = decl->name + "::" + gvar->name;
                processGlobalVarDecl(gvar);
                break;
            }
            case NodeKind::Enum: {
                auto enm = std::static_pointer_cast<EnumDecl>(innerDecl);
                enm->name = decl->name + "::" + enm->name;
                processEnumDecl(enm);
                break;
            }
            default:
                processDecl(innerDecl);   // 其余（别名、模板、推导指引…）走通用入口
                break;
        }
    }

    m_currentNamespace = savedNs;   // 退出命名空间作用域
}

void SemanticAnalyzer::processTypeAliasDecl(TypeAliasDeclPtr decl) {
    Symbol sym;
    sym.name = decl->aliasName;
    sym.type = decl->underlyingType;
    sym.kind = SymbolKind::Type;
    sym.isLocal = false;
    sym.definedAt = decl->location;
    m_symbolTable.globalScope()->define(decl->aliasName, sym);
    std::cout << std::format("  [register] type alias '{}' = {}\n",
        decl->aliasName, decl->underlyingType ? decl->underlyingType->toString() : "?");
}

// ═══ 类声明处理（理论见 docs/learn/17、19）════════════════════════════════════
// 做什么：建类类型 → 合并基类字段/vtable（继承）→ 收集自身字段/方法并建 vtable →
//   计算内存布局 → 注入 _vptr/RTTI → 注册进全局符号表。
// 理论依据：[class.derived] 单继承 public 布局 —— 基类子对象位于派生类成员之前；
//   [class.virtual] 虚函数"同槽位覆盖" —— override 替换基类同名虚函数的 vtable 槽位
//   而不是新增，这是动态分派的根基。
// demo: Animal{virtual speak} ⇒ vtable[0: Animal_speak]；Dog : Animal 覆写 speak ⇒
//   先拷贝基类表，再发现同名 override ⇒ 槽 0 改写为 Dog_speak（索引不变！）
//     Dog 对象内存            Dog 的 vtable
//       +0: _vptr ──────────→ [0] = &Dog_speak
//       +8: 自身字段 ……       [-1]= &type_info(_ZTI3Dog)（vtable 前一槽）
//   运行期 Animal* p = new Dog; p->speak() ⇒ callq *(%rdi) 按索引 0 间接跳转 ⇒ Dog_speak
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Base { int b; virtual int kind(); virtual ~Base(); };
// │       struct Derived : public Base { int d; int kind(); };
// │ 日志  [register] class 'Base'    field: b : int    method: kind() → int [virtual]
// │         ══ Memory Layout of 'Base' ══    Total size: 16 bytes
// │         +0: _vptr (8 bytes, hidden) → vtable     +8: b : int (4 bytes)
// │         vtable: 2 entries, RTTI: _ZTI4Base
// │       [register] class 'Derived' : public Base
// │         ↳ [primary] 'Base': 1 fields, 2 vtable entries      ← 主基类整体合并
// │         ══ Memory Layout of 'Derived' ══   Total size: 24 bytes  +0: _vptr +8: b +16: d
// │ 输出  ClassType（classLayout：字段偏移 / vtableEntries / rttiMangledName）
// │ 继承模型  [class.mi] Itanium：第一个【多态】基类当 primary（共享 _vptr、字段不加
// │       前缀），其余多态基类当 secondary（字段加 "Base." 前缀、独立次表、覆写时设
// │       thunkAdjust）。非多态基类字段也加前缀避免同名冲突。
// │ 子对象偏移不在这里算 —— 循环后由 [relocate] 阶段按 Itanium 规则统一摆放
// │       （因为"边扫边放"会让 A.x 与 _vptr 重叠）。
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::processClassDecl(ClassDeclPtr decl) {
    // ── 前置声明（`struct A;`）：只登记名字，不生成任何成员/布局/符号 ──
    // 用例：struct A;  struct B { A* pa; };  struct A { int x; };
    //   ① 前置声明让 `A*` 能被 resolveType 解析（名字进 m_classTypes）；
    //   ② 真定义到来时走下面的正常路径，覆盖同名登记；
    //   ③ 空壳【不生成默认构造/析构】—— 否则 CodeGen 会发两份 A_dtor，
    //      汇编期报 "symbol `A_dtor' is already defined"。
    // 简化点：不做"不完整类型"使用检查（clang 会对 `A a;` 这类值使用报错）。
    if (decl->isForwardDecl) {
        std::cout << std::format(
            "  [register] class '{}' (forward declaration ⇒ 只登记名字，不产符号)\n",
            decl->name);
        // 三处登记（与正常路径末尾同构）：类型表 + AST 声明 + ★符号表。
        // ★ 符号表这条不能省 —— resolveType 查的是 m_symbolTable.lookup()，
        //   少了它 `struct B { A* pa; };` 会误报 unknown type name 'A'。
        TypePtr fwdType = Type::makeClass(decl->name);
        decl->classType = fwdType;
        m_classTypes[decl->name] = fwdType;
        m_classDecls[decl->name] = decl;

        Symbol fwdSym;
        fwdSym.name = decl->name;
        fwdSym.type = fwdType;
        fwdSym.kind = SymbolKind::Type;
        fwdSym.isLocal = false;
        fwdSym.definedAt = decl->location;
        m_symbolTable.define(decl->name, fwdSym);
        return;
    }

    // 处理本类期间置"当前类"，供 resolveType 的类作用域回退用（类体内 `type x;` 要先在
    // 类作用域的 typeAliases 里找）。方法体的分析由 registerFunction 另行覆盖/还原这个值。
    std::string savedClassScope = m_currentClassName;
    m_currentClassName = decl->name;

    std::cout << std::format("  [register] class '{}' ", decl->name);
    if (!decl->baseClassNames.empty()) {
        std::cout << ": public ";
        for (size_t i = 0; i < decl->baseClassNames.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << decl->baseClassNames[i];
        }
    }
    std::cout << "\n";

    // 为本类新建类型对象：classLayout（字段/偏移/vtable）都挂在它上面，完成后注册进
    // m_classTypes（"编译期看符号"的那个符号）。
    // ★ 必须【立刻】挂到 decl->classType —— 后面的 computeClassLayout(decl) 从
    //   decl->classType 取类型计算偏移；若拖到函数末尾才赋值，布局会算在一个
    //   hasVTable=false 的临时孤儿类型上：字段偏移漏掉 _vptr、totalSize=0，
    //   CodeGen 按 0 字节 malloc，运行期写必然越界。
    TypePtr classType = Type::makeClass(decl->name);
    decl->classType = classType;
    // 立刻登记 AST 声明：下面的字段/方法类型解析要经 resolveType 的"类作用域回退"查
    // 本类的 typeAliases，而那条回退是经 m_classDecls 找类声明的 —— 晚到函数末尾才登记
    // 的话，类体内的别名一律查不到（`Int a;` 报 unknown type name）。
    m_classDecls[decl->name] = decl;

    // ── 处理继承（多继承：[class.mi] Itanium 主基类优化模型）──
    // 第一个多态基类 = 主基类（共享主表 + 字段无限定），其余多态基类 = 次基类（独立次表
    // entries，字段带 "BaseName." 前缀）。非多态基类的字段也带前缀（避免多基类同名冲突）。
    std::vector<std::string> allBaseFieldNames; // 所有基类字段名（用于自身字段限定）
    bool hasPrimaryVTable = false;
    // ★ 本循环【不计算】子对象偏移 —— offset 统一由循环后的 [relocate] 阶段按 Itanium
    //   规则摆放（primary 恒占 0，其余从 primary 尾部对齐累加）。
    // ⚠ 边扫边放会出两个 offset=0：非多态基类声明在多态基类之前时 A 先占 0、primary P
    //   又写死 0 ⇒ 子对象重叠（A.x 压 _vptr）；且 primary 分支的 currentOffset 是"重置"
    //   而非累加，会把先摆的进度悄悄丢弃。

    if (!decl->baseClassNames.empty()) {
        // 保存自身字段（Parser 已将它们填入 decl->fields），继承处理中重建顺序
        std::vector<FieldInfo> ownFields = std::move(decl->fields);
        decl->fields.clear();

        for (size_t baseIdx = 0; baseIdx < decl->baseClassNames.size(); ++baseIdx) {
            const std::string& baseName = decl->baseClassNames[baseIdx];
            auto baseIt = m_classTypes.find(baseName);
            if (baseIt == m_classTypes.end()) { // 这里就是要 必须先声明base 类才可以，必须要按照顺序去初始化才可以
                error(std::format("Base class '{}' not found", baseName), decl->location);
            }
            TypePtr baseType = baseIt->second;

            // 收集基类字段名（用于自身字段限定）
            for (auto& bf : baseType->classLayout.fields) {
                std::string rawName = bf.name;
                auto dot = rawName.find('.');
                if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                allBaseFieldNames.push_back(rawName);
            }

            if (!hasPrimaryVTable && baseType->classLayout.hasVTable) { // 第一个有虚函数的类才算是主基类
                // ═══ 主基类（primary base）═══
                // 合并字段到主字段列表 + 合并 vtable 到主表
                // 字段名不加限定前缀（保持单继承兼容）
                for (size_t k = 0; k < baseType->classLayout.fields.size(); ++k) {
                    FieldInfo fi = baseType->classLayout.fields[k];
                    if (fi.declaredName.empty()) fi.declaredName = fi.bareName();
                    if (fi.sourceClass.empty()) fi.sourceClass = baseName;
                    // ★ viaBase / baseFieldIndex 必须【覆盖】成本层的值：基类记录里那对值
                    //   指的是【它那一层】的子对象，照抄会让偏移算到错的子对象上
                    //   （BUGS.md B15：X : P, B 里从 P 带出的 "B.x"，其 viaBase 在 P 层是
                    //   "B"，到 X 层若不改成 "P" 就会被当成 X 的直接 B 子对象 ⇒ +8 变 +16）。
                    fi.viaBase = baseName;
                    fi.baseFieldIndex = static_cast<int>(k);
                    decl->fields.push_back(fi);
                }
                for (auto& baseEntry : baseType->classLayout.vtableEntries) {
                    classType->classLayout.vtableEntries.push_back(baseEntry);
                }
                classType->classLayout.hasVTable = true;
                hasPrimaryVTable = true;

                BaseSubobject primarySub;
                primarySub.baseClassName = baseName;
                primarySub.hasVTable = true;
                primarySub.isPrimary = true;
                primarySub.vtableSegmentOffset = 0;
                // offset 不在此设置：[relocate] 阶段统一置 0 并摆放其余子对象
                classType->classLayout.bases.push_back(primarySub);

                std::cout << std::format("    ↳ [primary] '{}': {} fields, {} vtable entries\n",
                    baseName, baseType->classLayout.fields.size(),
                    baseType->classLayout.vtableEntries.size());
            } else if (baseType->classLayout.hasVTable) {
                // ═══ 次基类（secondary base）═══
                // 字段带 "BaseName." 限定前缀；vtable 条目存入独立次表 entries
                for (size_t k = 0; k < baseType->classLayout.fields.size(); ++k) {
                    FieldInfo fi = baseType->classLayout.fields[k];
                    fi.declaredName = fi.bareName();          // 权威裸名先落袋
                    fi.name = baseName + "." + fi.declaredName;
                    fi.sourceClass = baseName;
                    fi.viaBase = baseName;
                    fi.baseFieldIndex = static_cast<int>(k);
                    decl->fields.push_back(fi);
                }

                BaseSubobject secSub;
                secSub.baseClassName = baseName;
                secSub.hasVTable = true;
                secSub.isPrimary = false;
                // offset 由 [relocate] 阶段统一摆放
                // 次表条目（独立于主表，覆写时设 thunkAdjust）
                for (auto& baseEntry : baseType->classLayout.vtableEntries) { // 次基类的vtable entry 不是放到 classLayout里面的
                    VTableEntry secEntry = baseEntry;
                    secEntry.mangledName = baseName + "_" + (baseEntry.baseFunctionName.empty()
                        ? std::string(baseEntry.mangledName.substr(baseEntry.mangledName.find('_') + 1))
                        : baseEntry.baseFunctionName);
                    secEntry.baseFunctionName = baseEntry.baseFunctionName.empty()
                        ? baseEntry.mangledName.substr(baseEntry.mangledName.find('_') + 1)
                        : baseEntry.baseFunctionName;
                    secSub.entries.push_back(secEntry);
                }
                classType->classLayout.bases.push_back(secSub);

                std::cout << std::format("    ↳ [secondary] '{}': {} fields, {} vtable entries\n",
                    baseName, baseType->classLayout.fields.size(),
                    secSub.entries.size());
            } else {
                // ═══ 非多态基类 ═══
                // 字段带限定前缀，无 vtable 贡献
                for (size_t k = 0; k < baseType->classLayout.fields.size(); ++k) {
                    FieldInfo fi = baseType->classLayout.fields[k];
                    fi.declaredName = fi.bareName();          // 权威裸名先落袋
                    fi.name = baseName + "." + fi.declaredName;
                    fi.sourceClass = baseName;
                    fi.viaBase = baseName;
                    fi.baseFieldIndex = static_cast<int>(k);
                    decl->fields.push_back(fi);
                }

                BaseSubobject nonPolySub;
                nonPolySub.baseClassName = baseName;
                nonPolySub.hasVTable = false;
                nonPolySub.isPrimary = false;
                // offset 由 [relocate] 阶段统一摆放
                classType->classLayout.bases.push_back(nonPolySub);

                std::cout << std::format("    ↳ [non-poly] '{}': {} fields\n",
                    baseName, baseType->classLayout.fields.size());
            }
        }

        // ── 统一摆放子对象偏移（Itanium [class.mi]：primary 恒占 offset 0）──
        // ★ 这是子对象偏移的【唯一】计算点 —— 上方循环只收集字段与 vtable 条目、不写
        //   offset，避免"边扫边放"产生两个 offset=0。
        // Itanium 规则：primary = 第一个【多态】基类（与声明顺序无关），恒放 0；其余子对象
        //   （含声明在 primary 之前的非多态基类）一律从 primary 子对象尾部依次对齐摆放。
        // 用例  struct A{int x;};  struct P{virtual void f();};  struct D : A, P {};
        //       ⇒ primary = P（不是首基类 A）⇒ [relocate] P@0、A@8（误用 bi==0 当 primary
        //         则 A@0 与 _vptr 重叠）
        // 简化声明：全部基类都非多态、且【本类自身也非多态】时，第一个基类视作 primary
        //   （占 0），与 computeClassLayout 的字段放置规则保持一致。
        // ★ 补上的第三种情形（BUGS.md B11）：本类【自身】多态、而所有基类都非多态 ⇒
        //   Itanium 下【没有主基类】—— primary base 只在动态基类里选，一个都没有时
        //   vptr 自己占 offset 0，全部基类子对象从 8 起摆。旧实现把首基类当 primary 摆到
        //   0，而本类自己的 _vptr 也要占 0 ⇒ 两者重叠（B.x 压 _vptr，写字段即写坏虚表
        //   指针，随后虚调用跳飞到 0x3）。
        if (!classType->classLayout.bases.empty()) {
            size_t primaryIdx = 0;
            bool anyPoly = false;
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i) {
                if (classType->classLayout.bases[i].hasVTable) {
                    primaryIdx = i;
                    anyPoly = true;
                    break;
                }
            }
            // 本类自身是否多态：无多态基类时 vtable 只可能来自本类，而"覆写基类虚函数"
            // 也要求基类先有多态 ⇒ 此刻只需看有没有带 virtual 关键字的方法。
            // ⚠ 判据必须与下方方法注册循环同源（那里才最终置 hasVTable）—— 位置却被夹在
            //   前面，因为子对象偏移随后就要被 thunkAdjust 用掉。
            bool selfPoly = false;
            if (!anyPoly) {
                for (auto& m : decl->methods) {
                    if (m->isVirtual) { selfPoly = true; break; }
                }
            }
            bool noPrimary = (!anyPoly && selfPoly);

            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i)
                classType->classLayout.bases[i].isPrimary = (!noPrimary && i == primaryIdx);

            // place = 已占用区间的右端：有 primary 时是 primary 子对象尾部；无 primary
            //   （本类自身多态）时是 vptr 之后的 8。
            // ★ 不能用"全非多态 ⇒ place=0"的写法：那会让第二个基类被 alignTo(0,8)=0 放到
            //   offset 0，与首个基类字段重叠（x@0 与 y@0 互踩）。
            uint32_t place = noPrimary ? 8u : 0u;
            if (!noPrimary) {
                auto pIt = m_classTypes.find(
                    classType->classLayout.bases[primaryIdx].baseClassName);
                if (pIt != m_classTypes.end())
                    place = pIt->second->classLayout.totalSize; // primary 尾部
            }
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i) {
                auto& sub = classType->classLayout.bases[i];
                if (sub.isPrimary) {
                    sub.offset = 0;
                    continue;   // primary 恒 0，无需打印（保持既有日志逐字节不变）
                }
                sub.offset = alignTo(place, 8); // 这里非常关键, 这里会对结束位置再做一次偏移，彻底锁死对应的位置
                auto bIt = m_classTypes.find(sub.baseClassName);
                place = sub.offset + (bIt != m_classTypes.end()
                    ? bIt->second->classLayout.totalSize : 0);
                std::string why = noPrimary
                    ? std::string("本类自身多态 ⇒ 无主基类，_vptr 占 0")
                    : std::format("primary='{}' 占 0",
                          classType->classLayout.bases[primaryIdx].baseClassName);
                std::cout << std::format("    ↳ [relocate] '{}' → offset {} ({})\n",
                    sub.baseClassName, sub.offset, why);
            }
        }

        // 追加自身字段到最后
        for (auto& f : ownFields) {
            // 自身字段：viaBase 留空（"住在本类里"）、下标 -1；权威裸名就是解析期那个名字
            if (f.declaredName.empty()) f.declaredName = f.name;
            f.viaBase.clear();
            f.baseFieldIndex = -1;
            decl->fields.push_back(f);
        }
    }

    // 自身字段与基类字段同名时加限定前缀（避免覆写基类字段名）
    size_t inheritedCount = 0;
    for (auto& baseName : decl->baseClassNames) {
        auto baseIt = m_classTypes.find(baseName);
        if (baseIt != m_classTypes.end())
            inheritedCount += baseIt->second->classLayout.fields.size();
    }
    for (size_t i = inheritedCount; i < decl->fields.size(); ++i) {
        std::string rawName = decl->fields[i].name;
        for (auto& bfn : allBaseFieldNames) {
            if (rawName == bfn) {
                decl->fields[i].name = decl->name + "." + rawName;
                decl->fields[i].sourceClass = decl->name;
                break;
            }
        }
    }

    // ── 注册字段到符号表：遍历的是"合并后"的字段列表（基类字段已在继承处理时插到
    //    头部），逐一转成 FieldInfo 挂进布局表；offset/size 稍后由 computeClassLayout 算。
    for (auto& field : decl->fields) {
        // 字段类型必须先 resolveType：Parser 造的 Class("Five") 是占位类型（classLayout
        // 全空，totalSize=0），不换成注册版本会导致
        //   ① sizeInBytes()=0 → 字段占 0 字节，后续字段重叠、malloc 分配不足；
        //   ② inferMember 拿空布局查 o->f.a → "No member 'a'"。
        // 对照 clang（RecordLayoutBuilder.cpp:1850 LayoutField）：成员布局信息经
        //   Context.getTypeInfoInChars(D->getType()) 一次取回 TI.Width/TI.Align，
        //   类型永远是 complete 的注册版（ASTContext::getASTRecordLayout 按需递归构建+缓存）。
        field.type = resolveType(field.type);

        if (field.declaredName.empty()) field.declaredName = field.bareName();
        FieldInfo fi;
        fi.name = field.name;
        fi.type = field.type;
        fi.access = field.access;
        fi.declaredName = field.declaredName;
        fi.viaBase = field.viaBase;
        fi.baseFieldIndex = field.baseFieldIndex;
        classType->classLayout.fields.push_back(fi);

        std::cout << std::format("    field: {} : {}\n",
            field.name, field.type ? field.type->toString() : "?");
    }

    // ── 合成默认构造与析构（若未显式提供）──
    bool hasExplicitCtor = false;
    bool hasExplicitDtor = false;
    for (auto& method : decl->methods) {
        if (method->kind == NodeKind::Constructor || method->name == decl->name) {
            hasExplicitCtor = true;
        }
        if (method->kind == NodeKind::Destructor || method->name == "~" + decl->name) {
            hasExplicitDtor = true;
        }
    }

    if (!hasExplicitCtor) {
        auto defaultCtor = std::make_shared<ConstructorDecl>();
        defaultCtor->name = decl->name;
        defaultCtor->ownerClassName = decl->name;
        defaultCtor->returnType = Type::makeVoid();
        defaultCtor->isDefaultCtor = true;
        defaultCtor->body = std::make_shared<BlockStmt>();
        decl->methods.push_back(defaultCtor);
    }
    if (!hasExplicitDtor) {
        auto defaultDtor = std::make_shared<DestructorDecl>();
        defaultDtor->name = "~" + decl->name;
        defaultDtor->ownerClassName = decl->name;
        defaultDtor->returnType = Type::makeVoid();
        defaultDtor->isDefaultDtor = true;
        defaultDtor->body = std::make_shared<BlockStmt>();
        decl->methods.push_back(defaultDtor);
    }

    // ── 校验构造函数初始化列表 ──
    for (auto& method : decl->methods) {
        if (method->kind == NodeKind::Constructor) {
            auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
            for (auto& init : ctor->initList) {
                bool found = false;
                // 检查是否匹配任一基类名
                for (auto& bn : decl->baseClassNames) {
                    if (init.memberName == bn) { found = true; break; }
                }
                // 检查是否匹配自身字段（含限定名）
                for (auto& f : decl->fields) {
                    if (f.name == init.memberName) {
                        // 去掉限定名再比
                        found = true; break;
                    }
                    // 匹配裸名
                    std::string rawName = f.name; // 有可能因为上面操作 成了 "a.name"这种结构
                    auto dot = rawName.find('.');
                    if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                    if (rawName == init.memberName) { found = true; break; }
                }
                if (!found) {
                    error(std::format("Member '{}' not found in class '{}' during initializer list",
                        init.memberName, decl->name), init.location);
                }
            }
        }
    }

    // ── 注册成员模板（[temp.mem]）──
    // 不进 methods：它不是可调用实体，只是一张"配方"（要用实参推导才产出函数）。
    // 单独存一张表，调用点在方法表查不到时来这里查。
    // ★ 蓝图里的形参名（T）必须保留原样直到实例化 —— 与自由函数模板同理。
    for (auto& mt : decl->memberTemplates) {
        mt->funcTemplate->ownerClassName = decl->name;
        m_classMemberTemplates[decl->name].push_back(mt);
        std::cout << std::format(
            "    ↳ member template '{}::{}' registered ({} template parameter(s))\n",
            decl->name, mt->templateName(), mt->templateParams.size());
    }

    // ── 注册方法，检查虚函数 ──
    for (auto& method : decl->methods) {
        method->ownerClassName = decl->name;

        std::string methodNameInVTable = method->name.starts_with("~") ? "dtor" : method->name;

        // Override 检测：即使没有 virtual 关键字，也检查是否覆写基类虚函数
        // （C++ 标准：覆写虚函数不需要重新声明 virtual）
        bool overriddenInPrimary = false;
        bool overriddenInSecondary = false;

        // 查主表
        for (auto& entry : classType->classLayout.vtableEntries) {
            std::string entryFuncName = entry.baseFunctionName.empty()
                ? entry.mangledName.substr(entry.mangledName.find('_') + 1)
                : entry.baseFunctionName;
            if (entryFuncName == methodNameInVTable) {
                entry.mangledName = decl->name + "_" + methodNameInVTable; // 这里会对继承的多态函数修饰
                entry.baseFunctionName = methodNameInVTable;
                entry.isOverridden = true;
                overriddenInPrimary = true;
                // 覆写基类虚函数 → 自身也变成虚函数
                method->isVirtual = true;
                break;
            }
        }

        // 查次表 entries（多继承时次基类的覆写）—— 注意：即使主表已命中也要查，
        // 同名函数可能同时存在于主表和次表（如 whoAmI 同时在 A 和 B 中），两处都要更新。
        for (auto& base : classType->classLayout.bases) {
            if (base.isPrimary || !base.hasVTable) continue;
            for (auto& entry : base.entries) {
                std::string entryFuncName = entry.baseFunctionName.empty()
                    ? entry.mangledName.substr(entry.mangledName.find('_') + 1)
                    : entry.baseFunctionName;
                if (entryFuncName == methodNameInVTable) {
                    entry.mangledName = decl->name + "_" + methodNameInVTable; // 这里会覆盖掉次基类 原先的名字
                    entry.baseFunctionName = methodNameInVTable;
                    entry.isOverridden = true;
                    overriddenInSecondary = true; // 覆写了，那么下面就不用在写了
                    // 覆写基类虚函数 → 自身也变成虚函数
                    method->isVirtual = true;
                    // thunk 调整量：-(次基类子对象偏移)
                    entry.thunkAdjust = -(int)base.offset;
                    std::cout << std::format("      ↳ override in secondary '{}': thunkAdjust={}\n",
                        base.baseClassName, entry.thunkAdjust);
                    break;
                }
            }
        }

        std::cout << std::format("    method: {}() → {}{}\n",
            method->name,
            method->returnType ? method->returnType->toString() : "?",
            method->isVirtual ? " [virtual]" :
            method->isOverride ? " [override]" : "");

        if (method->isVirtual) {
            classType->classLayout.hasVTable = true;

            if (!overriddenInPrimary && !overriddenInSecondary) { // 既没有覆盖主虚函数 又没有覆盖次虚函数
                VTableEntry entry;
                entry.mangledName = decl->name + "_" + methodNameInVTable; // 这里会修饰为当前 类名_方法名
                entry.baseFunctionName = methodNameInVTable;
                entry.index = static_cast<uint32_t>(
                    classType->classLayout.vtableEntries.size());
                classType->classLayout.vtableEntries.push_back(entry); // 也就是说classLayout 只会放 主基类和自己的虚函数
            }
        }
        std::cout << std::format("  ◀◀ END override check for '{}' (class '{}')\n",
            method->name, decl->name);
    }

    // ── 计算内存布局 ──
    computeClassLayout(decl);

    // ── 注入 vtable 和 RTTI ──
    if (classType->classLayout.hasVTable) {
        injectVTableAndRTTI(classType);
    }

    // 写回最终字段列表：decl->fields（基类+自身，且已被 computeClassLayout
    // 填好 offset/size）整体覆盖前面逐步 push 的版本，作为布局的权威结果。
    classType->classLayout.fields = decl->fields; // 这里时会做一个最终的回填

    // ── 注册到全局符号表：三处登记 —— m_classTypes（类型+布局）、m_classDecls
    //    （AST 声明）、符号表（kind=Type，供名字查找）。
    m_classTypes[decl->name] = classType;
    m_classDecls[decl->name] = decl;
    decl->classType = classType;

    Symbol classSym;
    classSym.name = decl->name;
    classSym.type = classType;
    classSym.kind = SymbolKind::Type;
    classSym.isLocal = false;
    classSym.definedAt = decl->location;
    m_symbolTable.define(decl->name, classSym);

    // ── 注册类内类型别名（using / typedef）──
    // [temp.alias]/[dcl.typedef]：别名不是新类型，只是一次名字替换。两处登记：
    //   ① 符号表里的 "Cls::alias"（供 `S::type x;` 这类限定名走普通名字查找）；
    //   ② 类声明的 typeAliases 表（供 resolveType 的嵌套名分支 / 类作用域回退查，
    //      也供实例化时替换）。
    // 放在类自身注册之后：目标里若引用本类（`using Self = S;`）也已可见。
    if (!decl->typeAliasOrder.empty()) {
        for (const auto& aliasName : decl->typeAliasOrder) {
            TypePtr target = resolveType(decl->typeAliases[aliasName]);
            decl->typeAliases[aliasName] = target; // 写回解析结果，实例化时直接用
            Symbol aliasSym;
            aliasSym.name = decl->name + "::" + aliasName;
            aliasSym.type = target;
            aliasSym.kind = SymbolKind::Type;
            aliasSym.isLocal = false;
            aliasSym.definedAt = decl->location;
            m_symbolTable.define(aliasSym.name, aliasSym);
            std::cout << std::format("  [sema:alias] {}::{} = {}\n",
                decl->name, aliasName, target ? target->toString() : "?");
        }
    }

    m_currentClassName = savedClassScope;

    // ── 打印内存布局 ──
    std::cout << std::format("    ══ Memory Layout of '{}' ══\n", decl->name);
    std::cout << std::format("    Total size: {} bytes\n", classType->classLayout.totalSize);
    if (classType->classLayout.hasVTable) {
        std::cout << "    +0: _vptr (8 bytes, hidden) → vtable\n";
    }
    for (auto& f : classType->classLayout.fields) {
        std::cout << std::format("    +{}: {} : {} ({} bytes)\n",
            f.offset, f.name, f.type ? f.type->toString() : "?", f.size);
    }
    if (classType->classLayout.hasVTable) {
        std::cout << std::format("    vtable: {} entries",
            classType->classLayout.vtableEntries.size());
        std::cout << std::format(", RTTI: {}\n", classType->classLayout.rttiMangledName);
    }
    std::cout << "\n";
}

// ─── 内存布局计算（[basic.align] 的对齐规则，教学简化版）──────────────────────
//   每个字段的对齐要求 = min(字段大小, 8)（8 字节封顶，无 packed/pragma）；
//   字段偏移 = 当前偏移向上取整到该对齐；整体大小 = 向上取整到最大对齐。
// demo（含虚函数的类，字段 age:int、weight:double）：
//     +0   _vptr   (8B，hasVTable 时恒在偏移 0，隐藏指针)
//     +8   age     (4B，alignTo(8,4)=8)      +16  weight (8B，alignTo(12,8)=16 —— 12~15 是填充)
// 简化：字段按声明顺序逐一排布，不做字段重排 / 空基类优化。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Base { int b; virtual int kind(); };   // hasVTable = true
// │ 日志  ══ Memory Layout of 'Base' ══    Total size: 16 bytes
// │         +0: _vptr (8 bytes, hidden) → vtable     +8: b : int (4 bytes)
// │ 手算  hasVTable ⇒ offset = 8, maxAlign = 8；字段 b（4B，对齐 4）→ alignTo(8,4)=8
// │       收尾 totalSize = alignTo(12,8) = 16    ← 末尾补 4 字节填充
// │ 无虚函数时：offset 从 0 起，totalSize 只到最后一个字段对齐后的位置
// │ 谁消费  CodeGen::emitNew 按 totalSize 调 malloc；inferMember 按 offset 发 mov
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::computeClassLayout(ClassDeclPtr decl) {
    TypePtr classType = decl->classType;
    if (!classType) {
        classType = Type::makeClass(decl->name);
        decl->classType = classType;
    }

    uint32_t offset = 0;
    uint32_t maxAlign = 1;
    bool hasVTable = classType->classLayout.hasVTable;

    // 如果有虚函数，先放 _vptr
    if (hasVTable) {
        offset = 8;
        maxAlign = 8;
    }

    // ── 多继承布局：子对象偏移已由 processClassDecl 的 [relocate] 阶段统一摆放 ──
    // 那里按 Itanium 规则完成：primary 占 offset 0，其余从 primary 尾部依次对齐。本函数
    // 只负责：① 从 bases 里读出 primary 的名字（决定字段走"直接复用偏移"分支）
    //   ② 逐字段放置（见下）。
    // ★ primary 的判据必须是"第一个多态基类"，不能用 bi==0 —— 两者打架时（非多态基类
    //   声明在前）会选出不同的 primary，A.offset 与 P.offset 同为 0，A.x 压在 _vptr 上。
    // ★ 旧实现还要在这里先挑出"主基类是谁"，再据此把字段分派到两条分支（主基类走
    //   "直接复用偏移"、次基类走"子对象偏移 + 内部偏移"）。现在两条分支合并成
    //   `sub->offset + src->offset`（见下）—— 主基类 offset 恒为 0，式子自动退化，
    //   不必再知道"谁是主基类"。挑主基类的那段随之删除。

    // ── 逐字段放置 ──
    // 字段顺序：[主基类字段...][次基类1字段...][次基类2字段...][自身字段...]
    // 次基类字段：复用字段在基类布局中的 offset + 该基类在 D 中的子对象起始偏移；
    // 主基类字段：offset=0（共享 _vptr），直接复用基类偏移；自身字段从最后一个基类
    // 子对象尾部继续累加。
    uint32_t currentFieldOffset = offset;  // 自身字段起始偏移（动态推进）
    std::string prevSrcClass;

    // 先计算自身字段的起始偏移（所有基类子对象之后）
    if (!decl->baseClassNames.empty()) {
        for (auto& base : classType->classLayout.bases) {
            auto baseIt = m_classTypes.find(base.baseClassName);
            if (baseIt != m_classTypes.end()) {
                uint32_t end = base.offset + baseIt->second->classLayout.totalSize;
                if (end > currentFieldOffset) currentFieldOffset = end; // 这里是一种覆盖设置 offset，前面 已经设置过每个 class 的base offset ,每个layout 已经自己对齐过了
            }
        }
        currentFieldOffset = alignTo(currentFieldOffset, 8);// 最后对齐到8 就可以了
    }

    for (auto& field : decl->fields) {
        // ── 自身字段（viaBase 为空）：从基类子对象尾部继续 ──
        // ── size 与 align 分离（对照 clang LayoutField 的 TI.Width / TI.Align）──
        // ★ 类类型字段的 align ≠ size：Five{bool×5} size=5 但 align=1，拿 size 当
        //   align 会推出 alignTo(4,5)=9 这种非 2 幂的错位布局。
        if (field.viaBase.empty()) {
            uint32_t fieldAlign = alignOf(field.type);   // 对齐要求（成员 align 递归 max）
            uint32_t fieldSize = field.type->sizeInBytes(); // 实际占用字节, 这里的字节数就是实际 resolveType 解析出来的字节数
            // 对齐的精髓就是 起始地址要能够 整除 filedAlign
            currentFieldOffset = alignTo(currentFieldOffset, fieldAlign);
            field.offset = currentFieldOffset;
            field.size = fieldSize;
            currentFieldOffset += fieldSize; // 加上这个字段的长度，等于当前位置
            maxAlign = std::max(maxAlign, fieldAlign);
            continue;
        }

        // ── 继承字段：① 找到装着它的那个【直接基类子对象】 ② 按 baseFieldIndex 取出
        //    该基类布局里那条记录的原始偏移 ⇒ offset = 子对象偏移 + 内部偏移（Itanium）
        // ★ 主基类子对象 offset 恒为 0（Itanium primary base optimization，D* → A* 是
        //   no-op 转换）⇒ 本式对它退化成"直接复用基类偏移"，与旧实现同值。
        // ★ 旧实现靠 name 字符串反查基类字段（剥第一个点再比裸名 + 找第一个命中），
        //   在"两条记录的显示名相同"时会认错人（BUGS.md B13/B15）—— 换成下标后，
        //   名字怎样都无所谓，与 clang 的 getFieldIndex() 一致。
        const BaseSubobject* sub = nullptr;
        for (auto& base : classType->classLayout.bases) {
            if (base.baseClassName == field.viaBase) { sub = &base; break; }
        }
        auto baseIt = m_classTypes.find(field.viaBase);
        const FieldInfo* src = nullptr;
        if (baseIt != m_classTypes.end()) {
            auto& bfields = baseIt->second->classLayout.fields;
            if (field.baseFieldIndex >= 0 &&
                static_cast<size_t>(field.baseFieldIndex) < bfields.size()) {
                src = &bfields[field.baseFieldIndex];
            }
        }
        if (sub && src) {
            field.offset = sub->offset + src->offset;
            field.size   = src->size;
            continue;
        }
        // 兜底：viaBase/下标缺失（不该发生）⇒ 保留扁平化时拷贝来的偏移
        std::cout << std::format(
            "    ⚠ [layout] 字段 '{}' 的 viaBase='{}' 无法定位（下标 {}），沿用扁平化偏移 {}\n",
            field.name, field.viaBase, field.baseFieldIndex, field.offset);
    }

    offset = currentFieldOffset;

    classType->classLayout.totalSize = alignTo(offset, maxAlign); // 最终长度也会进行一个对齐
    if (decl->fields.empty() && hasVTable) {
        classType->classLayout.totalSize = 8;
    }
}

// 注入 RTTI 符号并固化 vtable 索引（Itanium/GCC ABI 教学版）：
//   vtable[-1] = &type_info  ← RTTI，位于 vtable 起始地址的前一槽
//   vtable[0]  = &第一个虚函数，vtable[1] = &第二个虚函数 ……
//   RTTI 符号名按 GCC mangling：_ZTI + 类名长度 + 类名（Animal → _ZTI6Animal；
//   vtable 本身是 _ZTV 前缀）。
//   最后统一重排 index：继承+override 过程中槽位可能乱序，这里按最终顺序重新编号，
//   CodeGen 按 index 生成间接跳转。
void SemanticAnalyzer::injectVTableAndRTTI(TypePtr classType) {
    classType->classLayout.rttiMangledName =
        "_ZTI" + std::to_string(classType->name.size()) + classType->name;

    for (size_t i = 0; i < classType->classLayout.vtableEntries.size(); i++) {
        classType->classLayout.vtableEntries[i].index = static_cast<uint32_t>(i);
    }

    // ── 多继承：计算次表段在 _ZTV 符号内的字节偏移 ──
    // _ZTV 布局：[主表头 16B][主表槽位...][次表头 16B][次表槽位...]...；每段表头 16B
    //   = 8B offset-to-top + 8B RTTI ptr；主表段 vtableSegmentOffset = 0
    //   （vptr 直接指主表头后 16B 处）。
    uint32_t segmentOffset = 16 + static_cast<uint32_t>(
        classType->classLayout.vtableEntries.size()) * 8;
    for (auto& base : classType->classLayout.bases) {
        if (base.isPrimary || !base.hasVTable) continue;
        base.vtableSegmentOffset = segmentOffset;
        segmentOffset += 16 + static_cast<uint32_t>(base.entries.size()) * 8;
        std::cout << std::format("    [MI] secondary '{}' vtableSegmentOffset={}\n",
            base.baseClassName, base.vtableSegmentOffset);
    }
}

// 向上取整到 alignment 的倍数：alignTo(12,8)=16，alignTo(5,4)=8。等价于位运算
// (offset+align-1) & ~(align-1) 的算术写法；alignment==0 时原样返回，防御除零。
uint32_t SemanticAnalyzer::alignTo(uint32_t offset, uint32_t alignment) {
    if (alignment == 0) return offset;
    return (offset + alignment - 1) / alignment * alignment;
}

// 类型的对齐要求（字节）。对照 clang：ASTContext::getTypeInfoInChars 对 record 返回其
//   ASTRecordLayout 的 Alignment（= 成员 align 的递归最大值，见
//   RecordLayoutBuilder.cpp UpdateAlignment），对标量返回 TargetInfo ABI 表值。
// 教学简化：标量 = min(size,8)（int→4 double→8 bool→1 指针/引用→8，与 ABI 表一致）；
//   类类型 = 各成员 alignOf 递归 max（封顶 8）；无成员的空类 → align 1。
// ★ 绝不能拿 sizeInBytes 当 align：Five{bool×5} size=5 align=1，Five{int,int} size=8 align=4。
uint32_t SemanticAnalyzer::alignOf(const TypePtr& type) {
    if (!type) return 1;
    switch (type->kind) {
        case TypeKind::Const:
            return type->innerType ? alignOf(type->innerType) : 1;
        case TypeKind::Class: {
            uint32_t a = 1;
            for (auto& f : type->classLayout.fields)
                a = std::max(a, alignOf(f.type)); // 会找到 最大 的字段
            return std::min(a, 8u);
        }
        default: {
            uint32_t s = type->sizeInBytes();
            return s == 0 ? 1u : std::min(s, 8u); // Void/未知类型兜底 1
        }
    }
}

// ─── 注册函数（Pass 2，理论见 docs/learn/13）─────────────────────────────────
// 三处登记：① m_functionMap（名字 → 声明，调用点查这个，fullName 与裸名都登记）
//   ② m_functions（按声明顺序的列表，CodeGen 逐个输出汇编）③ 符号表（kind=Function）。
// mangling：普通函数 = 函数名本身；成员函数 = 类名_函数名；析构 = 类名_dtor；
//   同名多载或带参数 → 追加 _<参数个数>（CodeGen 的汇编标号、vtable 条目都用它）。
// 用例  int add(int,int) ⇒ "add" ｜ Base::kind() ⇒ "Base_kind"
//       Box_int::Box_int(int) ⇒ "Box_int_Box_int_1"（带参 ⇒ 后缀 _1）
// ⚠ 已知边界：符号表"一名一槽"，不支持普通函数重载（[over] 教学级简化），同名后注册者
//   覆盖前者；函数模板重载集另存 m_functionTemplateCandidates。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int add(int a, int b) {...}     struct Base { int kind(); };
// │ 日志  [register] add(int a, int b) → int        [mangled: add]
// │       [register] Base::kind() → int             [mangled: Base_kind]
// │       [register] Box_int::get() → int           [mangled: Box_int_get]
// │       [register] Box_int::Box_int() → void      [mangled: Box_int_Box_int]
// │ 输出  mangledName + 三处登记
// │ ★ 陷阱  m_functionMap 以【裸名】为键 ⇒ Box_int::get 与 Box_double::get 互相覆盖。
// │       这正是 inferCall 必须先用"对象类 → 方法表"查成员调用的原因。
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::registerFunction(FuncDeclPtr decl) {
    decl->returnType = resolveType(decl->returnType);
    for (auto& param : decl->parameters) {
        param.type = resolveType(param.type);
    }

    std::string fullName = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    m_functionMap[fullName] = decl;
    m_functionMap[decl->name] = decl;
    m_functions.push_back(decl);

    // 设置 mangled name
    if (decl->ownerClassName.empty()) {
        decl->mangledName = decl->name;
    } else {
        if (decl->name.starts_with("~")) {
            decl->mangledName = decl->ownerClassName + "_dtor";
        } else {
            decl->mangledName = decl->ownerClassName + "_" + decl->name;
            // 构造函数/方法重载：同名多个时追加参数个数区分（当前方法已 push_back 进
            // m_functions，所以计数要 -1）。
            size_t sameNameCount = 0;
            for (auto& f : m_functions) {
                if (f.get() != decl.get()
                    && f->ownerClassName == decl->ownerClassName
                    && f->name == decl->name) {
                    ++sameNameCount;
                }
            }
            if (sameNameCount > 0 || decl->parameters.size() > 0) {
                decl->mangledName += "_" + std::to_string(decl->parameters.size());
            }
        }
    }

    // 注册到符号表
    Symbol sym;
    sym.name = decl->name;
    sym.type = decl->returnType;
    sym.kind = SymbolKind::Function;
    sym.isLocal = false;
    sym.ownerClass = decl->ownerClassName;
    sym.definedAt = decl->location;
    m_symbolTable.define(decl->name, sym);

    std::string paramStr;
    for (size_t i = 0; i < decl->parameters.size(); i++) {
        if (i > 0) paramStr += ", ";
        paramStr += decl->parameters[i].type->toString()
                  + " " + decl->parameters[i].name;
    }
    std::cout << std::format("  [register] {}({}) → {}    [mangled: {}]\n",
        fullName, paramStr,
        decl->returnType ? decl->returnType->toString() : "void",
        decl->mangledName);
}

// ─── 内建外部函数原型注册（P4，理论见 docs/learn/08）──────────────────────────
// 给 libc 的四个内存管理函数登记"语义外壳"：名字、形参表、返回类型，body 一律 nullptr。
// 三处登记缺一不可：① m_functionMap（调用点按裸名命中，与自由函数同一条路径）
//   ② m_functions（CodeGen 的 generate() 遍历它，但 `if (func->body)` 守卫自动跳过发射
//   ⇒ .s 里不会出现这些函数的汇编体）③ 符号表（kind=Function，处处可见）。
// 用例  int* p = malloc(8); ⇒ 语义查名 + 实参个数匹配 ⇒ callq malloc ⇒ 链接期 libc 兑现
// （`.s` 里只有 callq，没有 malloc 的函数体）
// 签名取舍（教学级简化）：minicc 无 void/size_t 语义（只有 int 与指针），故一律 int*
//   表示"一块内存"、int 表示"字节数"：
//     malloc(int) → int* ｜ free(int*) → void
//     memcpy(int*,int*,int) → int* ｜ realloc(int*,int) → int*
// 对照 clang：内建也走"先有声明再使用"（Builtins::Info 类型串 → ASTContext::getBuiltinType）；
//   区别只在真 clang 多数内建会在 IR 层换成 llvm.memcpy 等 intrinsic，本项目直接调 libc 符号。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 日志  [builtin] ✔ registered libc prototypes: malloc(int)→int*, free(int*),
// │         memcpy(int*,int*,int)→int*, realloc(int*,int)→int*    [P4: no body]
// │       [builtin] ✔ registered std shims (主线 H): std::false_type{value=0},
// │         std::true_type{value=1}; std::void_t<...> → void
// │ 源码  int* p = malloc(8);            ← 语义认识名字与参数个数，CodeGen 发 callq
// │       bool v = is_int<int>::value;   ← 折叠时沿继承链查到 std::true_type::value
// │ 输出  原型进 m_functionMap / m_functions / 符号表，但 body = nullptr
// │ 为什么不写进头文件  真 libc 头（<stdlib.h>）需要完整的 C 语法（void/size_t/函数
// │       指针/typedef），本项目类型系统只有 int 与指针 ⇒ 把等价原型以"内建注入"的
// │       方式直接登记，绕开预处理与解析两道坎。std 垫片同理。
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::registerBuiltins() {
    // 登记一个原型：构造无 body 的 FunctionDecl 并写入三处
    auto declare = [this](const std::string& name, TypePtr ret,
                          std::vector<TypePtr> paramTypes) {
        auto decl = std::make_shared<FunctionDecl>();
        decl->name = name;
        decl->returnType = std::move(ret);
        decl->body = nullptr;  // 纯声明 → CodeGen 跳过发射，链接期由 libc 提供
        for (size_t i = 0; i < paramTypes.size(); i++) {
            Parameter p;
            p.name = "arg" + std::to_string(i);
            p.type = std::move(paramTypes[i]);
            decl->parameters.push_back(std::move(p));
        }
        decl->mangledName = name;          // 外部符号：不修饰，直接用原名
        m_functionMap[name] = decl;
        m_functions.push_back(decl);

        Symbol sym;
        sym.name = name;
        sym.type = decl->returnType;
        sym.kind = SymbolKind::Function;
        sym.isLocal = false;
        m_symbolTable.globalScope()->define(name, sym);  // 显式进全局作用域
    };

    TypePtr intTy  = Type::makeInt();
    TypePtr intPtr = Type::makePointer(intTy);
    TypePtr voidTy = Type::makeVoid();

    declare("malloc", intPtr, {intTy});               // malloc(int n)
    declare("free", voidTy, {intPtr});                // free(int* p)
    declare("memcpy", intPtr, {intPtr, intPtr, intTy}); // memcpy(dst, src, n)
    declare("realloc", intPtr, {intPtr, intTy});      // realloc(p, n)

    std::cout << "  [builtin] ✔ registered libc prototypes: "
                 "malloc(int)→int*, free(int*), "
                 "memcpy(int*,int*,int)→int*, realloc(int*,int)→int*    "
                 "[P4: no body — codegen skips, linker binds libc]\n";

    // ── 主线 H：std 垫片（编译器内建声明）──────────────────────────────────────
    // 真 C++ 里 std::false_type 依赖三样本项目当时不具备的能力：① 别名模板 `using X = Y;`
    //   ② 类内静态数据成员 `static const T value = v;` ③ NTTP 形参类型依赖前置形参
    //   `template<class T, T v>`。故由 Sema 直接注入等价类声明绕开 Parser ——【有意的
    //   简化】，代价是这些名字不能被用户重新定义。
    // 用例  std::false_type{value=0} / std::true_type{value=1}（独立类，真 C++ 里是
    //       integral_constant<bool,v> 的别名）；std::void_t<...> 不走这里 —— 别名模板
    //       无法表达，改在 resolveType / TemplateDeducer::reducePattern 里按名识别 ⇒ void。
    // 对照 clang：同样有内建（Sema::Initialize + ASTContext 里预置的 __builtin_* 声明）。
    // ─────────────────────────────────────────────────────────────────────────
    auto injectTraitClass = [this](const std::string& name, int64_t value,
                                   const std::string& base = "") {
        auto cls = std::make_shared<ClassDecl>();
        cls->name = name;
        if (!base.empty()) cls->baseClassNames.push_back(base);
        // value 的类型是 bool —— 与真 C++ 的 integral_constant<bool,v> 一致
        cls->staticConsts["value"] = StaticConstMember{value, Type::makeBool()};

        // 三处登记（与 processClassDecl 同构）：类表 + 符号表 + 布局
        m_classDecls[name] = cls;
        cls->classType = Type::makeClass(name);
        m_classTypes[name] = cls->classType;

        Symbol sym;
        sym.name = name;
        sym.type = cls->classType;
        sym.kind = SymbolKind::Type;
        sym.isLocal = false;
        m_symbolTable.globalScope()->define(name, sym);
    };

    // 真 C++ 里 false_type/true_type 是 integral_constant<bool,false/true> 的别名；
    // 这里退化成两个各带静态常量 value 的独立类，语义等价。
    injectTraitClass("std::false_type", 0);
    injectTraitClass("std::true_type",  1);

    std::cout << "  [builtin] ✔ registered std shims (主线 H): "
                 "std::false_type{value=0}, std::true_type{value=1}; "
                 "std::void_t<...> → void (按名识别，见 resolveType)    "
                 "[内建注入：绕开别名模板与类内 static 的语法缺口]\n";
}

// ═══ 分析函数体（Pass 3）—— 类型推导与符号决议的主战场 ═══════════════════════
// 函数作用域 [basic.scope.function] 的构建流程：
//   1. enterScope(函数名)   2. 参数入符号表 —— 每个参数占一个 8 字节栈槽（教学模型：
//      统一 8 字节槽，不区分 int/double/指针；栈向低地址生长，-8, -16 …）
//   3. 成员函数追加 this（[class.this]：隐式首参，类型 = 指向属主类的指针）
//   4. 逐条 processStmt(函数体) —— 体内局部变量继续 -8/槽
// demo: int add(int a, int b) { int s = a + b; return s; }
//   符号表 a:Parameter stack@-8 ｜ b:Parameter stack@-16 ｜ s:Variable stack@-24
// 本函数也被 S5 复用：模板实例化出的函数在此做"两阶段查找的第二阶段"（用具体类型查体）。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Vec { int n; int self() { return this->n; } };  int main() { int x = 42; }
// │ 日志  ╔══ Function Body: main ══╗
// │         [param] this : Vec*    stack@-8          ← 成员函数才有这一行
// │         [var decl] x : int =     [infer] IntLiteral(42) → int
// │         [symbol] ✚ x : int    stack@-8   ← 加入符号表
// │ 栈槽分配  从 -8 开始，每个形参 / 局部变量各占 8 字节（教学简化，不按实际大小）
// │ ★ 上下文保存/恢复：模板实例化（S5）会在分析外层函数的过程中【嵌套】分析实例的
// │   函数体，不保存/恢复会踩掉外层的 m_currentReturnType（main 中途实例化 void 函数
// │   后，main 的 return 被按 void 检查）。
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::analyzeFunctionBody(FuncDeclPtr decl) {
    if (!decl->body) return;

    std::string funcLabel = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    std::cout << std::format("\n  ╔══ Function Body: {} ══╗\n", funcLabel);

    m_symbolTable.enterScope(funcLabel);
    // ── 保存外层上下文：实例化（S5）会嵌套调用本函数分析实例体，不保存/恢复会把外层
    //    的 m_currentReturnType 等踩掉（详见 analyzeFunctionBody 的 DEMO 说明）。
    int         savedStackOffset = m_stackOffset;
    std::string savedClassName   = m_currentClassName;
    TypePtr     savedReturnType  = m_currentReturnType;
    std::string savedFuncName    = m_currentFuncName;
    m_stackOffset = 0;
    m_currentClassName = decl->ownerClassName;
    m_currentReturnType = decl->returnType;
    m_currentFuncName = decl->name;

    // ── 注册参数到符号表 ──
    for (auto& param : decl->parameters) {
        m_stackOffset -= 8;

        Symbol sym;
        sym.name = param.name;
        sym.type = param.type;
        sym.kind = SymbolKind::Parameter;
        sym.isLocal = true;
        sym.stackOffset = m_stackOffset;
        sym.definedAt = decl->location;
        m_symbolTable.define(param.name, sym);

        std::cout << std::format("  [param] {} : {}    stack@{}\n",
            param.name, param.type->toString(), m_stackOffset);
    }

    // ── 注册 this 指针（如果是成员函数）──
    if (!decl->ownerClassName.empty()) {
        auto classIt = m_classTypes.find(decl->ownerClassName);
        if (classIt != m_classTypes.end()) {
            m_stackOffset -= 8;

            Symbol thisSym;
            thisSym.name = "this";
            thisSym.type = Type::makePointer(classIt->second);
            thisSym.kind = SymbolKind::Parameter;
            thisSym.isLocal = true;
            thisSym.stackOffset = m_stackOffset;
            m_symbolTable.define("this", thisSym);

            std::cout << std::format("  [param] this : {}*    stack@{}\n",
                decl->ownerClassName, m_stackOffset);
        }
    }

    // ── 分析函数体（不创建新的 block scope，直接在函数 scope 中）──
    for (auto& stmt : decl->body->statements) {
        processStmt(stmt);
    }

    // ── dump 函数作用域符号表 ──
    std::cout << std::format("\n  ── Symbol Table after {} ──\n", funcLabel);
    m_symbolTable.dumpCurrentScope();

    m_symbolTable.exitScope();

    // ── 恢复外层上下文 ──
    m_stackOffset       = savedStackOffset;
    m_currentClassName  = savedClassName;
    m_currentReturnType = savedReturnType;
    m_currentFuncName   = savedFuncName;

    std::cout << std::format("  ╚══ End {} ══╝\n\n", funcLabel);
}

// ─── 模板声明处理（理论见 docs/learn/01、05、19）──────────────────────────────
// 注册模板蓝图（[temp] 教学模型：蓝图 = 等待实参的 AST 半成品）：
//   · 类模板：只存蓝图不展开；显式实参的收集与检查（[temp.arg.explicit] 简化：只支持显式
//     实参、个数按 typeParams 对齐）由 main.cpp 阶段 4 逐组调 instantiator 完成；
//   · 函数模板（S1）：注册进候选集 m_functionTemplateCandidates 等调用点推导，不查函数体。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  template <typename T> struct Box { T value; T get(); };        // 主模板
// │       template <typename T> struct Box<T*> { int tag(); };           // 偏特化
// │       template <> struct is_int<int> : public std::true_type {};     // 全特化
// │       template <typename T> T twice(T x) { return x + x; }           // 函数模板
// │ 日志  [register] template <typename T> Box (class blueprint stored, not analyzed)
// │         ↳ PARTIAL specialization #1 of 'Box' registered
// │         ↳ EXPLICIT (full) specialization #1 of 'is_int' registered
// │       [register] template <typename T> twice(T x) → T (function blueprint stored,
// │         awaiting call-site deduction)   ↳ overload candidate set 'twice' size = 1
// │ 输出  三张表分开存：m_classTemplates（主模板，名字 → 蓝图 O(1)）、m_partialSpecs /
// │       m_explicitSpecs（索引仍是模板名）—— 匹配算法不同，见 selectClassTemplate。
// │ 为什么不展开  模板体里的 T 是【依赖类型】，必须等实参到位才能做类型检查 —— 这就是
// │       两阶段名称查找的第一阶段：非依赖名现在查，依赖名推迟到实例化。
// │ ★ 全特化的额外把关  `template <>` 的形参表是空的 ⇒ 模式里每个名字都必须是【真实
// │       类型】。写成未声明名会被建成 Class 节点、比不中任何类型 ⇒ 静默永不匹配
// │       （用户以为在特化，实际一直走主模板，零报错）。
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::processTemplateDecl(TemplateDeclPtr decl) {
    std::cout << std::format("  [register] template <");
    for (size_t i = 0; i < decl->typeParams.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "typename " << decl->typeParams[i];
    }

    if (decl->isClassTemplate()) {
        std::cout << std::format("> {} (class blueprint stored, not analyzed)\n",
            decl->templateName());

        // ── 按 specKind 归档（[temp.class.spec] / [temp.expl.spec]）──
        // 主模板进 m_classTemplates（名字→蓝图 O(1)）；偏特化/全特化进各自的 vector，
        // 索引仍用模板名。三张表分开是因为匹配算法不同（见 selectClassTemplate）。
        if (decl->isPrimary()) {
            // ── 类模板注册表（与函数模板候选集对称）：名字→蓝图 O(1)；emplace 不覆盖，
            //    重名取先注册者。
            auto [it, inserted] =
                m_classTemplates.emplace(decl->templateName(), decl);
            std::cout << std::format("    ↳ class template '{}' registered{}\n",
                decl->templateName(),
                inserted ? "" : " (duplicate name, first registration wins)");
        }
        else if (decl->isPartialSpec()) {
            auto& list = m_partialSpecs[decl->templateName()];
            list.push_back(decl);
            std::cout << std::format(
                "    ↳ PARTIAL specialization #{} of '{}' registered\n",
                list.size(), decl->templateName());
        }
        else { // ExplicitSpec
            // ── ★ 全特化的模式必须是【完全具体】的类型 ──
            // 反例（本检查拦下的静默错误）  template <> struct Box<T*, T> { ... };  // T 未声明
            //   ⇒ Parser 把它建成 Class("T")（Parser 不查符号表）⇒ selectClassTemplate 逐位
            //   equals 比不中任何真实类型 ⇒【静默永不匹配】：用户以为在特化，实际一直走主
            //   模板，全程零报错。对照 clang：err_undeclared_identifier。
            // 为什么只查 ExplicitSpec：偏特化 `template<class T>` 里的裸 T 是【合法】形参，
            //   Parser 已建成 TemplateParam 节点（见 parseTemplateDecl 压入的
            //   m_templateParamScope），一并拦下会误伤。"T 到底存不存在"只有 Sema 判得了。
            for (const auto& arg : decl->specPattern) {
                // 值位（`template <> struct Flag<true>`）：值本就是具体量，
                //   不存在"名字未声明"的问题，跳过。
                if (!arg.isType()) continue;
                const TypePtr& pat = arg.type;
                if (!pat) continue;
                // 模式里挂着模板参数节点 = 形参表空却用了形参名，必是笔误
                if (pat->isTemplateParam()) {
                    error(std::format(
                        "use of undeclared template parameter '{}' in explicit "
                        "specialization of '{}' —— `template <>` 的形参表是空的，"
                        "模式里不能出现模板形参名",
                        pat->templateParamName, decl->templateName()), SourceLocation{});
                }
                // 裸标识符被建成 Class 节点：必须是已登记的类
                if (pat->isClass() && !pat->name.empty() &&
                    !m_classDecls.count(pat->name) &&
                    !m_classTemplates.count(pat->name) &&
                    pat->templateArgs.empty()) {
                    error(std::format(
                        "use of undeclared identifier '{}' in explicit "
                        "specialization pattern of '{}' —— `template <>` 的形参表是"
                        "空的，模式里每个名字都必须是【真实类型】（如 int / MyClass）；"
                        "若本意是偏特化，请把形参写进 template<...> 并改用 "
                        "`template <class {}> struct {}<...>`",
                        pat->name, decl->templateName(), pat->name,
                        decl->templateName()), SourceLocation{});
                }
            }

            auto& list = m_explicitSpecs[decl->templateName()];
            list.push_back(decl);
            std::cout << std::format(
                "    ↳ EXPLICIT (full) specialization #{} of '{}' registered\n",
                list.size(), decl->templateName());
        }
    } else if (decl->isDeductionGuide()) {
        // ── 推导指引（[temp.deduct.guide]）：只有 template<> 外壳的那种走这里；非模板
        //    形态（`Box(int) -> Box<int>;`）是顶层独立声明，由 processDecl 直接接住。
        // ★ 它不做任何"实例化"—— 指引没有实体、没有符号，只是 CTAD 的规则表。
        std::cout << std::format("> deduction guide for '{}' (rule stored, never instantiated)\n",
            decl->templateName());
        registerDeductionGuide(decl->guide);
    }
    else if (decl->isAliasTemplate()) {
        // ── 别名模板（[temp.alias]）：注册形态与另外两种模板都不同 ——
        //   类模板   → m_classTemplates，将来按需"实例化"出新的类；
        //   函数模板 → 候选集，将来在调用点做实参推导 + "实例化"；
        //   别名模板 → m_aliasTemplates，将来在【解析类型】时"解糖"。
        // 别名不需要候选集（不能重载）、不需要特化表（偏特化别名模板必须是另一个别名
        // 模板，本项目不做）、不产生符号。
        auto [it, inserted] =
            m_aliasTemplates.emplace(decl->templateName(), decl);
        std::cout << std::format(
            "> using {} = {} (alias blueprint stored, expands by substitution only)\n",
            decl->templateName(),
            decl->aliasTemplate->underlyingType
                ? decl->aliasTemplate->underlyingType->toString() : "?");
        std::cout << std::format("    ↳ alias template '{}' registered{}\n",
            decl->templateName(),
            inserted ? "" : " (duplicate name, first registration wins)");
    } else {
        // ── 函数模板（S1）：只注册蓝图，不分析函数体 —— 模板体中的 T 是"依赖类型"，
        //    要到实例化时（S5，由调用点推导驱动）才能做类型检查 = 两阶段查找的第一阶段。
        auto& func = decl->funcTemplate;
        std::string paramStr;
        for (size_t i = 0; i < func->parameters.size(); i++) {
            if (i > 0) paramStr += ", ";
            paramStr += func->parameters[i].type->toString() + " "
                      + func->parameters[i].name;
        }
        std::cout << std::format("> {}({}) → {} (function blueprint stored, awaiting call-site deduction)\n",
            func->name, paramStr,
            func->returnType ? func->returnType->toString() : "void");

        auto& candidates = m_functionTemplateCandidates[func->name];
        candidates.push_back(decl);
        std::cout << std::format("    ↳ overload candidate set '{}' size = {}\n",
            func->name, candidates.size());
    }

    m_templates.push_back(decl);
}

// ═══ 语句处理 ══════════════════════════════════════════════════════════════
// 语句分发器：AstVisitor 虚分派（handler 只收引用、不需要所有权，判据见 processDecl）。
void SemanticAnalyzer::processStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    // 一次虚表跳转落到对应 visit（零 RTTI：kind 恒等于自身类型）。
    stmt->accept(*this);
}

void SemanticAnalyzer::visit(DeleteStmt& stmt) {
    TypePtr ptrType = inferType(stmt.pointerExpr);
    if (!ptrType || !ptrType->isPointer()) {
        error("delete operand must be a pointer", stmt.location);
    }
    std::cout << std::format("  [delete] delete {}{}\n",
        stmt.isArray ? "[] " : "", ptrType->toString());
}

// 复合语句 `{ … }` → 新建块作用域（[basic.scope.block]）：块内声明的变量在 exitScope
// 后不再可见。demo: while 体内 `{ int t = 0; … }` —— t 只在块内可见，块外引用 t 会报
// Undefined variable。
void SemanticAnalyzer::visit(BlockStmt& block) {
    m_symbolTable.enterScope("block");
    for (auto& stmt : block.statements) {
        processStmt(stmt);
    }
    m_symbolTable.exitScope();
}

// ═══ 变量声明 —— auto 推导的核心战场 ═════════════════════════════════════════
// 遇到 `auto x = expr;`：declaredType 是 Auto 占位符 → 推导 initializer 的类型 → 把
// declaredType **永久替换**为真实类型 → auto 从此在 AST 中彻底消失（只是语法糖）。
// 非 auto 但有初值时做类型检查：`int x = y;`（y:int&）⇒ typeCompatible 剥引用放行
// （[conv.lval]）；`int x = 3.14;` ⇒ 无兼容规则 ⇒ 报 Type mismatch。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int x = 42;    auto d = 5;    bool v = is_int<int>::value;
// │       Vec& r = v;    Base* pb = &dr;    Undeclared q;
// │ 日志  [var decl] x : int =   [infer] IntLiteral(42) → int
// │         [symbol] ✚ x : int    stack@-8   ← 加入符号表
// │       [var decl] d : auto =   [infer] IntLiteral(5) → int
// │         [auto] ★ d : auto ⟹ int   ← auto 被永久替换!
// │       [static] ★ 静态常量命中：std::true_type::value = 1 : bool（沿继承链第 1 层）
// │       [static] 折叠为字面量：is_int_int::value(is_int) → 1
// │       [var decl] r : Vec& =   [conv] r : Vec ⟶ Vec& (implicit: ref-strip / promotion)
// │       [var decl] pb : Base* =   [infer] &Derived → Derived*
// │         [poly] pb : Derived* → Base* (polymorphic)     ← 派生类指针隐式转基类
// │ 五步  ① resolveType(声明类型)  ② foldStaticConst(初始化式)  ③ inferType(初始化式)
// │       ④ auto 替换真实类型 / 类型检查  ⑤ 分配栈槽 + 写符号表
// │ ★ 为什么②必须在③之前：折叠后得到的是普通字面量，后续类型检查与 CodeGen 都按常量走，
// │       不再涉及"字段偏移"那套运行期机制（静态成员不占对象内存）。
// │ 类型检查的三种放行  equals 全等 → 通过；typeCompatible（剥引用 + int→double）→ 打
// │       [conv] 日志；双指针 → 打 [poly] 日志；其余 → Type mismatch 报错。
// └────────────────────────────────────────────────────────────────────────────
// ── auto 占位符：去壳判据 ─────────────────────────────────────────────────
// 壳 ⇒ 下钻方向（每一层都只是"包着 auto"，不改变 auto 的身份）：
//   Const(Auto)        ⇒ innerType
//   Pointer(Auto)      ⇒ pointeeType
//   LValueRef(Auto)    ⇒ referencedType
//   RValueRef(Auto)    ⇒ referencedType
//   Pointer(Const(Auto)) ⇒ 两层依次下钻（const auto* p = &a;）
bool SemanticAnalyzer::containsAuto(const TypePtr& t) const {
    if (!t) return false;
    if (t->isAuto()) return true;
    if (t->isConst())     return containsAuto(t->innerType);
    if (t->isPointer())   return containsAuto(t->pointeeType);
    if (t->isReference()) return containsAuto(t->referencedType);
    return false;
}

// ── auto 占位符：反推 ────────────────────────────────────────────────────
// [dcl.spec.auto]/7：声明类型含占位符 ⇒ 类型由初始化式推导，规则等价于把 auto
// 当模板形参跑一次函数模板实参推导（[temp.deduct.call]）——即合一：pattern 是 P，
// 初始化式类型是 A，Auto 是待绑定的变量。
// 【为什么要递归而不是"剥到光杆 auto 再套回"】auto 外面有几层壳，结果就要套回几层；
//   且每遇一层 Pointer，A 也要同步剥掉一层指针才对齐（否则 auto** 会算成三层）。
// 用例（pattern + init ⇒ 结果）：
//   Auto                   + int                 ⇒ int
//   Const(Auto)            + int                 ⇒ Const(int)             const auto  v1 = a;
//   Pointer(Auto)          + Pointer(int)        ⇒ Pointer(int)           auto*       v3 = &a;
//   LValueRef(Auto)        + int                 ⇒ LValueRef(int)         auto&       v4 = a;
//   LValueRef(Const(Auto)) + int                 ⇒ LValueRef(Const(int))  const auto& v5 = a;
//   RValueRef(Auto)        + int                 ⇒ RValueRef(int)         auto&&      v6 = 7;
//   Pointer(Pointer(Auto)) + Pointer(Pointer(int)) ⇒ 同构                  auto**      pp = &p;
// 简化点：未做引用折叠（auto&& 绑左值按 [dcl.ref]/6 应折叠成 T&）——minicc 的
//   inferType 对标识符一律返回裸类型（表达式的类型从不含引用，[expr.type]/1），
//   该分支暂不可达；真要做时在此加 collapseRef 即可。
TypePtr SemanticAnalyzer::deduceAutoType(TypePtr pattern, TypePtr init,
                                         const std::string& varName, SourceLocation loc) {
    if (!pattern) return nullptr;
    if (pattern->isAuto()) return init;              // ★ 绑定点：auto := A
    if (pattern->isConst())
        return Type::makeConst(deduceAutoType(pattern->innerType, init, varName, loc));
    if (pattern->isPointer()) {
        // `auto*` 要求初始化式确实是指针，且 A 要剥掉一层才能与内层模式对齐
        if (!init || !init->isPointer())
            error(std::format(
                "cannot deduce 'auto*' for '{}': initializer is '{}', not a pointer",
                varName, init ? init->toString() : "?"), loc);
        return Type::makePointer(
            deduceAutoType(pattern->pointeeType, init->pointeeType, varName, loc));
    }
    if (pattern->isLValueReference())
        return Type::makeLValueReference(
            deduceAutoType(pattern->referencedType, init, varName, loc));
    if (pattern->isRValueReference())
        return Type::makeRValueReference(
            deduceAutoType(pattern->referencedType, init, varName, loc));
    return pattern;  // 不含 auto 的外壳（containsAuto 为真时不可达）
}

void SemanticAnalyzer::visit(VarDeclStmt& decl) {
    // ── CTAD：`MyPtr m(7);` → `MyPtr<int> m(7);`（[dcl.type.class.deduct]）──
    // ★ 必须在 resolveType【之前】：resolveType 见到裸类模板名会直接报 "requires
    //   template arguments"，而这里正是要把它补上。不适用 CTAD 时原样返回，后路不变。
    if (!decl.ctorArgs.empty()) {
        decl.declaredType = deduceClassTemplateArgs(decl);
    }

    decl.declaredType = resolveType(decl.declaredType);
    TypePtr type = decl.declaredType;

    // 初始化式先做静态常量折叠（Cls<Args>::value → 字面量），必须在 inferType 之前。
    if (decl.initializer) decl.initializer = foldStaticConst(decl.initializer);

    if (decl.initializer) {
        // ── 推导初始化表达式的类型 ──
        std::cout << std::format("  [var decl] {} : {} = ",
            decl.name, type->toString());

        m_inferDepth++;
        TypePtr initType = inferType(decl.initializer);
        m_inferDepth--;

        std::cout << std::format("{}    ⟹ inferred: {}\n",
            inferIndent(), initType ? initType->toString() : "?");

        // ─── auto 类型推导（[dcl.spec.auto]/7）───
        // ★ 判据是 containsAuto 而非 isAuto：const auto 的树是 Const(Auto)，
        //   光杆判据看不见占位符 ⇒ 落到 else 分支被当成"声明 const auto、实得 int"误报。
        if (containsAuto(type)) {
            if (!initType || initType->isAuto() || initType->isVoid()) {
                error(std::format("Cannot deduce auto type for '{}'", decl.name),
                      decl.location);
            }

            // ★★★ 核心：去壳反推 → 套回外壳 → 永久替换 ★★★
            // 日志打 pattern 原文：裸 auto 仍输出 "auto"（与旧日志一字不差），
            //   `const auto` / `auto*` / `auto&` 则原样可见。
            TypePtr declaredAuto = type;
            TypePtr deduced = deduceAutoType(type, initType, decl.name, decl.location);
            decl.declaredType = deduced;
            type = deduced;

            std::cout << std::format("  [auto] ★ {} : {} ⟹ {}   ← auto 被永久替换!\n",
                decl.name, declaredAuto->toString(), type->toString());
        }
        else {
            // ── 类型检查（含 [conv.lval] 引用剥除 / 数值提升）──
            if (!type->equals(initType)) {
                if (typeCompatible(type, initType)) {
                    std::cout << std::format("  [conv] {} : {} ⟶ {} (implicit: ref-strip / promotion)\n",
                        decl.name,
                        initType ? initType->toString() : "?", type->toString());
                }
                else if (type->isPointer() && initType && initType->isPointer()) {
                    std::cout << std::format("  [poly] {} : {} → {} (polymorphic)\n",
                        decl.name, initType->toString(), type->toString());
                }
                else {
                    error(std::format(
                        "Type mismatch in '{}': declared '{}', got '{}'",
                        decl.name, type->toString(), initType->toString()),
                        decl.location);
                }
            }
        }
    } else if (containsAuto(type)) {
        // 含占位符但无初始化式（`const auto x;` 同样要拦）
        error(std::format("auto variable '{}' must have an initializer", decl.name),
              decl.location);
    } else if (!decl.ctorArgs.empty()) {
        // ── 直接初始化 `Type name(args);`：选定构造函数 ──
        // mangling 是【有状态】的：同名方法/构造按参数个数追加后缀（MyPtr_int_MyPtr_int_1），
        //   而"该调哪个重载"是语义信息 ⇒ 由 Sema 选定并回填 decl.ctorSymbol。
        // 用例  MyPtr<int> m(7); ⇒ 日志 "[ctor] ★ m : MyPtr_int(1 个实参) ⇒ 选定构造函数符号
        //       MyPtr_int_MyPtr_int_1"；CodeGen 按 Name_Name 硬拼只会拼出个不存在的符号。
        // 简化点：按【参数个数】选（教学版不做完整的重载决议，与 Sema 既有口径一致）。
        // 对照 clang：Sema::BuildCXXConstructExpr 跑完整重载决议，CodeGen 只发它选中的符号。
        m_inferDepth++;
        for (auto& a : decl.ctorArgs) inferType(a);
        m_inferDepth--;

        if (type->isClass()) {
            auto cit = m_classDecls.find(type->name);
            if (cit != m_classDecls.end()) {
                for (auto& method : cit->second->methods) {
                    if (method->kind != NodeKind::Constructor) continue;
                    auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
                    if (ctor->parameters.size() != decl.ctorArgs.size()) continue;
                    decl.ctorSymbol = ctor->mangledName;
                    break;
                }
            }
        }
        if (decl.ctorSymbol.empty()) {
            error(std::format(
                "no matching constructor for '{}' with {} argument(s) —— "
                "'{}' 里找不到接受 {} 个实参的构造函数",
                decl.name, decl.ctorArgs.size(),
                type->toString(), decl.ctorArgs.size()),
                decl.location);
        }
        std::cout << std::format("  [ctor] ★ {} : {}({} 个实参) ⇒ 选定构造函数符号 {}\n",
            decl.name, type->toString(), decl.ctorArgs.size(), decl.ctorSymbol);
    } else {
        std::cout << std::format("  [var decl] {} : {} (no initializer)\n",
            decl.name, type->toString());
    }

    // ── 注册到符号表：分配栈槽，统一 8 字节/变量（教学简化），栈向低地址生长，
    //    该偏移与 CodeGen 的栈帧布局一一对应。
    m_stackOffset -= 8;

    Symbol sym;
    sym.name = decl.name;
    sym.type = type;
    sym.kind = SymbolKind::Variable;
    sym.isLocal = true;
    sym.stackOffset = m_stackOffset;
    sym.definedAt = decl.location;

    if (!m_symbolTable.define(decl.name, sym)) {
        error(std::format("Variable '{}' already declared in this scope", decl.name),
              decl.location);
    }

    std::cout << std::format("  [symbol] ✚ {} : {}    stack@{}   ← 加入符号表\n",
        decl.name, type->toString(), m_stackOffset);
}

// if 语句：条件必须可语境转换为 bool（[stmt.select]），教学级简化为只接受 bool | int。
// then / else 分支各自进入独立作用域（[basic.scope.block]），分支内声明的变量互不可见。
void SemanticAnalyzer::visit(IfStmt& stmt) {
    std::cout << std::format("  [if] condition:\n");
    m_inferDepth++;
    TypePtr condType = inferType(stmt.condition);
    m_inferDepth--;
    std::cout << std::format("  [if] condition type: {}\n",
        condType ? condType->toString() : "?");

    if (condType && !condType->isBool() && !condType->isInt()) {
        error("If condition must be bool or int", stmt.location);
    }

    m_symbolTable.enterScope("if-then");
    processStmt(stmt.thenBranch);
    m_symbolTable.exitScope();

    if (stmt.elseBranch) {
        m_symbolTable.enterScope("if-else");
        processStmt(stmt.elseBranch);
        m_symbolTable.exitScope();
    }
}

// while 语句：推导条件类型（教学级未强制 bool|int，比 if 宽松），循环体进入独立作用域。
// 循环体只静态分析一遍 —— 动态的反复执行是运行期的事，语义分析只保证"每一遍都类型合法"。
void SemanticAnalyzer::visit(WhileStmt& stmt) {
    std::cout << std::format("  [while] condition:\n");
    m_inferDepth++;
    TypePtr condType = inferType(stmt.condition);
    m_inferDepth--;
    std::cout << std::format("  [while] condition type: {}\n",
        condType ? condType->toString() : "?");

    m_symbolTable.enterScope("while-body");
    processStmt(stmt.body);
    m_symbolTable.exitScope();
}

// return 语句 [stmt.return]：返回值类型必须与函数声明的返回类型兼容（typeCompatible
// 负责剥引用/顶层 const、int→double 提升）。demo: int f() 中 `return x;`（x:int&）✓；
// `return 3.14;` ✗ 报 Return type mismatch；void 函数带返回值也被拦下。
// 不做流分析："非 void 函数漏写 return / 并非所有路径都有 return"不检查（clang 在
// CheckReturnVal 之外还有 CFG 流敏感检查，此处为教学级简化）。
void SemanticAnalyzer::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        m_inferDepth++;
        TypePtr retType = inferType(stmt.value);
        m_inferDepth--;

        std::cout << std::format("  [return] type: {} (expected: {})\n",
            retType ? retType->toString() : "?",
            m_currentReturnType ? m_currentReturnType->toString() : "?");

        // 类型检查（含 [conv.lval] 引用剥除 / 数值提升）
        TypePtr expType = resolveType(m_currentReturnType);
        if (retType && expType
            && !typeCompatible(expType, retType)) {
            error(std::format(
                "Return type mismatch: expected '{}', got '{}'",
                expType->toString(), retType->toString()),
                stmt.location);
        }
    } else {
        std::cout << "  [return] void\n";
    }
}

// 赋值语句：分别推导左值（target）与右值（value）的类型并打日志。
// 教学级简化：不检查"左侧必须是可修改左值"、不检查左右类型兼容性（clang 在
// SemaExpr.cpp 的 CheckAssignmentOperands 中完成这两件事）；左值性目前只在 inferCall
// 的模板推导路径中发挥作用。
void SemanticAnalyzer::visit(AssignStmt& stmt) {
    std::cout << "  [assign] lhs:\n";
    m_inferDepth++;
    TypePtr targetType = inferType(stmt.target);
    m_inferDepth--;

    std::cout << "  [assign] rhs:\n";
    m_inferDepth++;
    TypePtr valueType = inferType(stmt.value);
    m_inferDepth--;

    std::cout << std::format("  [assign] {} ⟵ {} \n",
        targetType ? targetType->toString() : "?",
        valueType ? valueType->toString() : "?");
}

// 表达式语句：只求类型不求值 —— 值被丢弃，语句的意义在副作用（典型如 `foo();`）。
// 推导本身会触发符号决议与类型检查。
void SemanticAnalyzer::visit(ExprStmt& stmt) {
    m_inferDepth++;
    inferType(stmt.expr);
    m_inferDepth--;
}

// ═══ 表达式类型推导 (inferType) —— 符号决议的核心 ═════════════════════════════
// 对每个表达式节点：递归推导子表达式 → 查符号表 → 检查合法性 → 结果写回
// expr->resolvedType（推导过程有缩进输出，可清晰看到每一步）。
// 理论依据：语法制导的类型推导（[expr] 类型规则即属性文法中的综合属性）—— 每个节点的
// 类型由其子节点自底向上合成；结果的 AST 注解供模板实例化与 CodeGen 直接消费
// （clang 对应物：Expr::setType / getType）。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int y = x + 1;
// │ 输入  BinaryExpr(+) { left = VarExpr(x), right = IntLiteral(1) }
// │ 日志  [infer] BinaryExpr(op) left:
// │         [resolve] 'x' → int    (kind=Variable, stack@-8)
// │       [infer] BinaryExpr(op) right:
// │         [infer] IntLiteral(1) → int
// │       [infer] int op int → int
// │ 输出  int —— 同时【写回】expr->resolvedType（CodeGen 靠它选指令宽度）
// │ 分派  按 NodeKind 一次 switch（14 路）。DeleteExpr 是唯一不返回自身类型的分支
// │       （推导完操作数返回 void）。
// │ 对照 clang：按 StmtClass 枚举/虚函数分派（BuildXXX），语义等价、效率更高。
// └────────────────────────────────────────────────────────────────────────────
// ★ 判据（本项目唯一权威表述在 include/semantic_analyzer.h）：handler 只要引用 →
//   访问者（accept 虚分派）；还要 shared_ptr 所有权或返回值 → 标签分派（switch）。
// 本例为何属后者：这是个【取值型】递归 —— 每个 handler 都要返回 TypePtr，而
//   AstVisitor::visit 返回 void。硬套访问者就得引入"结果槽 + 谁最后写槽"的隐式约定
//   （且 14 处都得遵守），得不偿失。
// 对照 clang：类型计算走 dyn_cast + switch；RecursiveASTVisitor 只服务遍历。
TypePtr SemanticAnalyzer::inferType(ExprPtr expr) {
    if (!expr) return nullptr;

    // 一次 switch（跳表）分派。static_cast 安全：节点 kind 由构造函数设定，恒等于自身类型。
    TypePtr type = nullptr;
    switch (expr->kind) {
        case NodeKind::IntLiteral:
            type = inferIntLiteral(static_cast<IntLiteralExpr&>(*expr)); break;
        case NodeKind::BoolLiteral:
            type = inferBoolLiteral(static_cast<BoolLiteralExpr&>(*expr)); break;
        case NodeKind::StringLiteral:
            type = inferStringLiteral(static_cast<StringLiteralExpr&>(*expr)); break;
        case NodeKind::NullptrLiteral:
            type = inferNullptrLiteral(static_cast<NullptrLiteralExpr&>(*expr)); break;
        case NodeKind::Var:
            type = inferVar(static_cast<VarExpr&>(*expr)); break;
        case NodeKind::Binary:
            type = inferBinary(static_cast<BinaryExpr&>(*expr)); break;
        case NodeKind::Unary:
            type = inferUnary(static_cast<UnaryExpr&>(*expr)); break;
        case NodeKind::Call:
            type = inferCall(static_cast<CallExpr&>(*expr)); break;
        case NodeKind::Member:
            type = inferMember(static_cast<MemberExpr&>(*expr)); break;
        case NodeKind::New:
            type = inferNew(static_cast<NewExpr&>(*expr)); break;
        case NodeKind::This:
            type = inferThis(static_cast<ThisExpr&>(*expr)); break;
        case NodeKind::DynamicCast:
            type = inferDynamicCast(static_cast<DynamicCastExpr&>(*expr)); break;
        case NodeKind::Index:
            type = inferIndex(static_cast<IndexExpr&>(*expr)); break;
        case NodeKind::Delete: {
            // delete 是 void 表达式：先看操作数成不成立，自身类型恒为 void
            auto& del = static_cast<DeleteExpr&>(*expr);
            inferType(del.pointerExpr);
            type = Type::makeVoid();
            break;
        }
        default:
            break;   // 不会到达：NodeKind 的表达式种类已全部覆盖
    }

    expr->resolvedType = type;
    return type;
}

// ─── 字面量推导（最简单：类型由字面量本身决定）───────────────────────────────
// 与标准的差异（教学简化）：
//   整数字面量   → int（标准有 int/long/long long 试配序列，[lex.icon]）
//   字符串字面量 → 指针类型（标准是 const char[N] 数组退化为 const char*，[lex.string]；
//                 本类型系统无 char 类型，以 int* 承载地址）
//   nullptr     → void*（标准有独立类型 std::nullptr_t，[lex.nullptr]）

// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int x = 42;   bool flag = true;   int* sp = "hi";   int* q = nullptr;
// │ 日志  [infer] IntLiteral(42) → int
// │       [infer] BoolLiteral(true) → bool
// │       [infer] StringLiteral → char*      ← 实为 int*（本项目无 char 类型）
// │       [infer] nullptr → void*
// │ 输出  类型完全由字面量本身决定，不看上下文 —— 这一点与标准一致：字面量先定自己的
// │       类型，再由 [conv] 做语境转换（见 visit(VarDeclStmt)）。
// │       `int* q = nullptr;` 于是走 [poly] 分支：void* → int*。
// │ 三处教学简化见上方注释（整数字面量恒 int；字符串字面量给指针；nullptr 给 void*）。
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferIntLiteral(IntLiteralExpr& expr) {
    std::cout << std::format("{}[infer] IntLiteral({}) → int\n",
        inferIndent(), expr.value);
    return Type::makeInt();
}

TypePtr SemanticAnalyzer::inferBoolLiteral(BoolLiteralExpr& expr) {
    std::cout << std::format("{}[infer] BoolLiteral({}) → bool\n",
        inferIndent(), expr.value ? "true" : "false");
    return Type::makeBool();
}

TypePtr SemanticAnalyzer::inferStringLiteral(StringLiteralExpr&) {
    std::cout << std::format("{}[infer] StringLiteral → char*\n", inferIndent());
    return Type::makePointer(Type::makeInt());
}

TypePtr SemanticAnalyzer::inferNullptrLiteral(NullptrLiteralExpr&) {
    std::cout << std::format("{}[infer] nullptr → void*\n", inferIndent());
    return Type::makePointer(Type::makeVoid());
}

// ─── 变量引用的符号决议 ═══════════════════════════════════════════════════════
// 查找顺序：① 符号表（作用域链由内向外）② 当前类的字段（类方法中，即隐式 this->x）
//   ③ m_functionMap（x 是函数名则返回其返回类型）④ 全失 → error("Undefined variable")
// demo（类 C 的成员函数内，块中引用名字 x）：block ✗ → 函数作用域 ✗ → global ✗
//   ⇒ C 的布局表 findField("x") ✓ ⇒ 返回字段类型（"编译期看符号"兑现为字段偏移的线索）
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  （在 main 中）int y = x + 1;     （在 Box_int::get 中）return value;
// │ 日志  [resolve] 'x' → int    (kind=Variable, stack@-8)        ← 局部变量
// │       [resolve] 'x' → Vec&    (kind=Parameter, stack@-8)      ← 引用形参
// │       [resolve] 'value' → int    (class field, offset=0)      ← 类字段（隐式 this->）
// │ 输出  名字对应的类型；三级都没命中 → error("Undefined variable 'x'")
// │ 细节  命中【类类型】时换成 m_classTypes 里那一份：符号表只记"名字 → 类型"，字段
// │       偏移 / vtable 这些布局细节挂在注册表那份上，不换就会在空布局上查无此字段。
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferVar(VarExpr& expr) {
    // 1. 查符号表（从当前作用域向外搜索）
    Symbol* sym = m_symbolTable.lookup(expr.name);
    if (sym) {
        TypePtr resolvedType = sym->type;

        // 如果是类类型，返回注册表中的完整类型（包含布局信息）
        if (resolvedType && resolvedType->isClass()) {
            auto classIt = m_classTypes.find(resolvedType->name);
            if (classIt != m_classTypes.end()) {
                resolvedType = classIt->second;
            }
        }

        std::cout << std::format(
            "{}[resolve] '{}' → {}    (kind={}, {})\n",
            inferIndent(),
            expr.name,
            resolvedType ? resolvedType->toString() : "?",
            symbolKindName(sym->kind),
            sym->isLocal ? std::format("stack@{}", sym->stackOffset) : "global");

        return resolvedType;
    }

    // 2. 如果在类方法中，查类的字段
    if (!m_currentClassName.empty()) {
        auto classIt = m_classTypes.find(m_currentClassName);
        if (classIt != m_classTypes.end()) {
            auto field = classIt->second->classLayout.findField(expr.name);
            if (field) {
                std::cout << std::format(
                    "{}[resolve] '{}' → {}    (class field, offset={})\n",
                    inferIndent(), expr.name,
                    field->type ? field->type->toString() : "?",
                    field->offset);
                return field->type;
            }
        }
    }

    // 3. 检查是否是函数名
    auto funcIt = m_functionMap.find(expr.name);
    if (funcIt != m_functionMap.end()) {
        std::cout << std::format("{}[resolve] '{}' → function\n",
            inferIndent(), expr.name);
        return funcIt->second->returnType;
    }

    error(std::format("Undefined variable '{}'", expr.name), expr.location);
}

// ─── 二元表达式推导（[expr.arith.conv] 的大幅简化）───────────────────────────
// 比较/逻辑（== != < > <= >= && ||）→ 结果恒为 bool（[expr.rel]/[expr.eq]/[expr.log.and]）
// 算术（+ - * /）→ 任一侧 double 则 double（int 提升）；否则 int × int → int
// demo: x:int + y:double → double ｜ a:int < b:int → bool
// 简化：不检查操作数类型组合的合法性（bool+bool 也放行），clang 在 SemaExpr.cpp 的
//   CheckBinOp 中逐组合校验。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int y = x + 1;      bool cmp = x < y;
// │ 日志  [infer] BinaryExpr(op) left:   [resolve] 'x' → int (kind=Variable, stack@-8)
// │       [infer] BinaryExpr(op) right:  [resolve] 'y' → int (kind=Variable, stack@-40)
// │       [infer] int op int → bool    (comparison)
// │ 输出  bool（比较 / 逻辑）/ int 或 double（算术）
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferBinary(BinaryExpr& expr) {
    std::cout << std::format("{}[infer] BinaryExpr(op) left:\n", inferIndent());

    m_inferDepth++;
    TypePtr leftType = inferType(expr.left);
    std::cout << std::format("{}[infer] BinaryExpr(op) right:\n", inferIndent());
    TypePtr rightType = inferType(expr.right);
    m_inferDepth--;

    // 比较运算符返回 bool
    if (expr.op == BinaryOp::Eq || expr.op == BinaryOp::Neq
        || expr.op == BinaryOp::Lt || expr.op == BinaryOp::Gt
        || expr.op == BinaryOp::Le || expr.op == BinaryOp::Ge
        || expr.op == BinaryOp::And || expr.op == BinaryOp::Or) {

        std::cout << std::format(
            "{}[infer] {} {} {} → bool    (comparison)\n",
            inferIndent(),
            leftType ? leftType->toString() : "?",
            "op",
            rightType ? rightType->toString() : "?");
        return Type::makeBool();
    }

    // 算术运算符
    if (leftType && rightType) {
        TypePtr resultType;
        if (leftType->isDouble() || rightType->isDouble()) {
            resultType = Type::makeDouble();
        } else if (leftType->isInt() && rightType->isInt()) {
            resultType = Type::makeInt();
        } else {
            resultType = Type::makeInt(); // 默认
        }

        std::cout << std::format(
            "{}[infer] {} op {} → {}\n",
            inferIndent(),
            leftType->toString(),
            rightType->toString(),
            resultType->toString());
        return resultType;
    }

    error("Invalid binary operation types", expr.location);
}

// 一元表达式 [expr.unary.op]：! → 结果恒为 bool（操作数被语境转换为 bool）；
//   - → 保持操作数类型（简化：不校验操作数是否为数值类型）；& → Pointer(操作数类型)。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  bool nf = !flag;   int neg = -x;   int* p = &x;
// │ 日志  [infer] !bool → bool ｜ [infer] -int → int
// │       [infer] &int → int*    (取地址 [expr.unary.op]/3)
// │ & 的意义  Box<decltype(&a)> 的实参类型就是它：decltype(&a) = int*，拿去匹配偏特化
// │       Box<T*, T> 时 T := int —— 见 docs/learn/21 的偏序。
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferUnary(UnaryExpr& expr) {
    m_inferDepth++;
    TypePtr operandType = inferType(expr.operand);
    m_inferDepth--;

    if (expr.op == UnaryOp::Not) {
        std::cout << std::format("{}[infer] !{} → bool\n",
            inferIndent(), operandType ? operandType->toString() : "?");
        return Type::makeBool();
    }

    // ── 解引用 *p（[expr.unary.op]/1）──
    // 结果类型 = 指针的 pointee（被指类型），且结果是【左值】—— 故 `*p = v` 合法。
    // 操作数必须是指针：C++ 里没有指针不做隐式转换（C 也没有），非指针即错误。
    // demo: int a; int* p = &a;  *p → int（左值）
    // 对照 clang：Sema::CheckIndirectionOperand（SemaExpr.cpp）
    if (expr.op == UnaryOp::Deref) {
        if (!operandType) {
            error("Cannot dereference a null-typed operand", expr.location);
        }
        if (!operandType->isPointer()) {
            // 文案对齐 clang：indirection requires pointer operand ('X' invalid)
            error(std::format(
                "Indirection requires pointer operand ('{}' invalid)",
                operandType->toString()), expr.location);
        }
        TypePtr result = operandType->pointeeType;
        std::cout << std::format(
            "{}[infer] *{} → {}    (解引用 [expr.unary.op]/1)\n",
            inferIndent(), operandType->toString(),
            result ? result->toString() : "?");
        return result;
    }

    // ── 取地址 &x（[expr.unary.op]/3）──
    // 结果类型 = Pointer(操作数类型)，操作数必须是左值。
    // demo: int a;  &a → int*  —— 也就是 Box<decltype(&a)> 里的实参类型。
    if (expr.op == UnaryOp::Addr) {
        if (!operandType) {
            error("Cannot take the address of a null-typed operand", expr.location);
        }
        if (operandType->isVoid()) {
            error("Cannot take the address of a void expression", expr.location);
        }
        TypePtr result = Type::makePointer(operandType);
        std::cout << std::format("{}[infer] &{} → {}    (取地址 [expr.unary.op]/3)\n",
            inferIndent(), operandType->toString(), result->toString());
        return result;
    }

    std::cout << std::format("{}[infer] -{} → {}\n",
        inferIndent(),
        operandType ? operandType->toString() : "?",
        operandType ? operandType->toString() : "?");
    return operandType;
}

// ─── 函数调用推导（重载决议 [over.match] 三阶段，理论见 docs/learn/06）────────
//   ① 候选：同名普通函数（m_functionMap）+ 同名函数模板候选集（m_functionTemplateCandidates）
//   ② 可行：参数个数匹配；模板须推导成功（resolveTemplateCall）
//   ③ 最优：非模板优先于模板（[over.match.best] 规则简化）；模板之间按偏序取最特化（S6）
// demo: `twice(42)` 同时存在 twice(int) 与 template<T> T twice(T) ⇒ 命中 twice(int) 直接
//   返回 int（非模板胜出，模板候选根本不参与）；若只有模板 ⇒ 推导 T:=int → 实例化
//   _Z5twiceIiE → 返回 int。
// 方法调用（callee 是 MemberExpr）先走【类作用域】查找（分支 ①），查不到才落回普通函数
//   路径；并置 isMethodCall 标记供 CodeGen 处理隐式 this。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int sum = add(x, y);   int v = bi.get();   int t = twice(x);   int e = probe(r);
// │ 日志  → add   ：[call] add(2 args) → int    [symbol resolved, non-template preferred]
// │       → bi.get：[member] Box_int.get() → int    (method)
// │                [call] Box_int.get(0 args) → int    [class-scoped member call]
// │       → twice ：[call] twice — 1 function template candidate(s), overload resolution begins
// │                [deduction]   P=T    A=int    ⇒ T := int
// │                [call] twice → _Z5twiceIiE (template resolved) → int
// │       → probe ：[deduction]   P=T    A=Vec&   ⇒ 值传递调整 A'=Vec ⇒ T := Vec
// │ 输出  被调函数的返回类型
// │ 四级瀑布（先命中先返回 —— 不是"查到就返回 / 查不到就报错"）：
// │   ⓪ std::declval<T>() 内建 → T&&，完全不查表
// │   ① 成员方法：按"对象类 → 方法表"查，BFS 含所有基类
// │   ② 普通函数 + ADL 候选合并：名字 + 参数个数命中 ∧ 精确匹配者优先
// │   ③ 函数模板：resolveTemplateCall 推导 + 偏序 + 实例化
// │   ④ 兜底：inferType(callee) → 名字根本不存在时在这里报 Undefined variable
// │ ★ 为什么①必须先于②：m_functionMap 以裸名作键，Box_int::get 与 Box_double::get 会
// │   互相覆盖，直接查名字表会随机命中别的类的方法。
// │ 报错只有两处：③ 的 "no matching function"（所有候选都推导失败）与 ④ 的
// │   Undefined variable；若调用点处在 Sfinae::attempt 内，异常被吸收成"该候选不可行"。
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferCall(CallExpr& expr) {
    // ── 确定 callee 名字与形态 ──
    std::string funcName;
    std::shared_ptr<VarExpr> calleeVar;
    if (expr.callee->kind == NodeKind::Var) {
        calleeVar = std::static_pointer_cast<VarExpr>(expr.callee);
        funcName = calleeVar->name;
    } else if (expr.callee->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr.callee);
        funcName = mem->memberName;
        mem->isMethodCall = true;
        // 先推导 callee 成员表达式，给 mem->object 标注 resolvedType（如 shape → Shape*）。
        // CodeGen::emitCall 依赖该类型解引用出类名、查 vtable 条目，从而把 shape->area()
        // 降级为经 _vptr 的间接调用；否则下方 m_functionMap 命中方法名后提前返回，对象
        // 类型永远不会被推导，虚调用退化为直接 callq。
        // ⚠ 只对 MemberExpr 补推导 —— 普通函数/模板的 VarExpr callee 若在此推导，会因
        //   符号表查无此"变量"而误报 Undefined variable。
        m_inferDepth++;
        inferType(mem);
        m_inferDepth--;
    }

    // ── 主线 H 内建：std::declval<T>() → T&& ──────────────────────────────
    // 真 C++ 里 declval 声明在 <utility>（[declval]/1）：template<class T>
    //   add_rvalue_reference_t<T> declval() noexcept; 且【故意只声明不定义】—— 只准出现在
    //   decltype/sizeof 这类【未求值上下文】。本项目没有 <utility>、也没有别名模板
    //   add_rvalue_reference_t，故与 std::void_t 同策：语义阶段按名识别，直接给出结果类型。
    // 用例  declval<Vec>().begin() ⇒ Vec&&（"假设有 Vec 对象可被引用"）⇒ 才能在 decltype 里
    //       查 Vec 的成员；换成 declval<int>() ⇒ int&& ⇒ 查 begin 失败 ⇒ SFINAE 落选
    // 对照 clang：走正常的函数模板实例化（Sema::BuildDeclRefExpr + 推导），只是函数体永远
    //   为空；本项目跳过实例化直接给类型 —— 要的只是类型，符号永不落地。
    // ⚠ 已知边界：不检查"只在未求值上下文出现"，`int x = declval<int>();` 也接受。
    if (calleeVar && (funcName == "std::declval" || funcName == "declval")) {
        if (!calleeVar->explicitTemplateArgs.empty() &&
            calleeVar->explicitTemplateArgs[0].type) {
            TypePtr t = calleeVar->explicitTemplateArgs[0].type;
            TypePtr r = Type::makeRValueReference(t);
            std::cout << std::format(
                "{}[declval] std::declval<{}>() → {}    [内建：按名识别，[declval]/1]\n",
                inferIndent(), t->toString(), r->toString());
            return r;
        }
    }

    // ── 推导实参类型（并记录左值性：变量/成员访问是左值）──
    // 左值性是推导的输入：T& 绑定检查、万能引用折叠（S2）
    std::vector<TypePtr> argTypes;
    std::vector<bool>    argIsLValue;
    for (auto& arg : expr.arguments) {
        m_inferDepth++;
        TypePtr t = inferType(arg);
        m_inferDepth--;
        argTypes.push_back(t);
        // ★ 值类别（lvalue/rvalue）判定——简化模型：变量引用 / 成员访问 → 有确定内存
        //   地址 → 左值；字面量 / 算术结果 / 调用返回值 → 右值。该标记随 argTypes 传给
        //   TemplateDeducer 执行引用绑定检查（[dcl.init.ref]）：非 const T& 拒绝绑定右值
        //   实参；T&& 为万能引用，经引用折叠后左右值皆可绑定（S2 实现）。
        bool isLValue = arg->kind == NodeKind::Var || arg->kind == NodeKind::Member;
        argIsLValue.push_back(isLValue);
        std::cout << std::format("{}  arg: {}{}\n", inferIndent(),
            t ? t->toString() : "?", isLValue ? " (lvalue)" : " (rvalue)");
    }

    // ── 成员方法调用：按"对象类 → 方法表"查，不走全局名字表 ──
    // Box_int::get 与 Box_double::get 这类同名方法（模板实例或多类同名成员）在全局
    // m_functionMap 里互相覆盖（registerFunction 以裸名作键），直接查名字表会随机命中
    // 别的类的方法，返回类型/参数全错。对照 clang：成员调用走 UnqualifiedIdExpr 的
    // 类作用域限定查找（BuildMemberCallExpr → LookupMember），普通名字查找只是兜底。
    // 前提：先补推导 callee 成员表达式，拿到 mem->object 的 resolvedType。
    if (expr.callee->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr.callee);
        if (!mem->object->resolvedType) {
            m_inferDepth++;
            inferType(mem->object);
            m_inferDepth--;
        }
        TypePtr objType = mem->object->resolvedType;
        if (objType && objType->isPointer()) objType = objType->pointeeType;
        if (objType && objType->isClass()) {
            // 搜索对象类及其所有基类（BFS）的方法
            std::vector<std::string> searchQueue = {objType->name};
            std::set<std::string> searched;
            while (!searchQueue.empty()) {
                std::string clsName = searchQueue.back(); searchQueue.pop_back();
                if (!searched.insert(clsName).second) continue;
                auto classIt = m_classDecls.find(clsName);
                if (classIt != m_classDecls.end()) {
                    if (auto method = findMethodInClass(clsName, funcName,
                                                        static_cast<int>(argTypes.size()))) {
                        // ★ 回填符号：CodeGen 硬拼 `类名_方法名` 拼不出带参方法的
                        //   真符号 —— Sema 给带参成员方法名加了"参数个数"后缀
                        //   （IntVec_at_1 / IntVec_set_2），硬拼得到的是 IntVec_at。
                        //   虚调用不受影响：CodeGen 在前面的 vtable 分支就 return 了。
                        mem->resolvedCalleeSymbol = method->mangledName;
                        std::cout << std::format(
                            "{}[call] {}.{}({} args) → {}    [class-scoped member call{}]\n",
                            inferIndent(), objType->name, funcName, argTypes.size(),
                            method->returnType ? method->returnType->toString() : "?",
                            clsName != objType->name ? std::format(" via '{}'", clsName) : "");
                        return method->returnType;
                    }
                    // ── 成员模板（[temp.mem]）：普通方法表里没有，查成员的模板表 ──
                    // ★ 位置刻意放在"方法表查完之后"：普通成员函数优先（重载决议里
                    //   非模板候选本来就优先于模板候选，[over.match.best]）。
                    if (auto mtIt = m_classMemberTemplates.find(clsName);
                        mtIt != m_classMemberTemplates.end()) {
                        for (auto& mt : mtIt->second) {
                            if (mt->templateName() != funcName) continue;
                            FuncDeclPtr inst = getOrInstantiateMemberFunction(
                                mt, argTypes, argIsLValue, clsName, expr.location);
                            if (!inst) continue;   // 推导失败 ⇒ 试下一个同名成员模板
                            // 回填符号：CodeGen 的成员调用硬拼 `类名_方法名`，
                            // 而成员模板的多个实例各有符号，必须由这里指定。
                            mem->resolvedCalleeSymbol = inst->mangledName;
                            std::cout << std::format(
                                "{}[call] {}.{}({} args) → {}    "
                                "[member template instantiated]\n",
                                inferIndent(), objType->name, funcName, argTypes.size(),
                                inst->returnType ? inst->returnType->toString() : "?");
                            return inst->returnType;
                        }
                    }
                    // 搜基类
                    for (auto& bn : classIt->second->baseClassNames)
                        searchQueue.insert(searchQueue.begin(), bn);
                }
            }
        }
        // 类里没查到 → 落入下方既有路径（兼容既有行为与报错）
    }

    // ── 普通函数查找 + ADL 候选合并（[basic.lookup.unqual] + [basic.lookup.argdep]）──
    //    （理论见 docs/learn/26）
    // ★ ADL 不是"普通查找失败才启用的兜底"，而是【把候选补进同一个候选集】：
    //       namespace N { struct S{}; int get(S); }   int get(int);   N::S s;  get(s);
    //   两个 get【同场竞争】，由实参类型决定胜负 ⇒ 精确匹配的 get(N::S) 胜出。
    //   若实现成"先到先得"（普通查找命中就返回），上例会静默调用 get(int)：能编译、能
    //   链接、结果错 —— 最坏的一类错误。
    // 裁决规则（[over.match.viable] + [over.match.best] 教学精简版）：① 形参个数相同
    //   ② 形参类型与实参类型【逐位精确相等】者优先（剥引用/顶层 const 后比）③ 无精确匹配
    //   时退回"按个数命中"。真重载决议还要算隐式转换序列的 rank。
    // 对照 clang：LookupResult 收集普通查找与 ADL 两批候选后交 OverloadCandidateSet 排序。
    auto it = m_functionMap.find(funcName);

    // ── ① 关联命名空间：从实参类型反推（[basic.lookup.argdep]/2）──
    // 本实现把命名空间成员的名字前缀化成 "N::S"，故从类名反推即可：
    //   N::S ⇒ {N}；A::B::S ⇒ {A, A::B}
    // 标准还包含基类与模板实参的关联命名空间，本实现只做"类名自身的前缀"。
    std::vector<std::string> assocNs;
    for (const auto& t : argTypes) {
        TypePtr core = t;
        while (core && (core->isPointer() || core->isLValueReference() ||
                        core->isRValueReference() || core->isConst())) {
            core = core->isPointer()  ? core->pointeeType
                 : core->isConst()    ? core->innerType
                                      : core->referencedType;
        }
        if (!core || !core->isClass()) continue;
        const std::string& n = core->name;
        for (size_t pos = n.find("::"); pos != std::string::npos; pos = n.find("::", pos + 2)) {
            assocNs.push_back(n.substr(0, pos));
        }
    }

    // ── ② 合并候选：普通查找（裸名）+ ADL（限定名）──
    // 剥引用与顶层 const：调用 `get(s)` 时形参写 S、实参类型是 S 的左值，两者在精确匹配
    // 判据里应当看作同一个类型（[dcl.init.ref] 的绑定规则不改变"是不是同一个类型"）。
    auto stripRefConst = [](TypePtr t) {
        while (t && (t->isLValueReference() || t->isRValueReference() || t->isConst())) {
            t = t->isConst() ? t->innerType : t->referencedType;
        }
        return t;
    };
    auto arityOk = [&](const FuncDeclPtr& f) {
        return f->parameters.size() == argTypes.size();
    };
    auto exactMatch = [&](const FuncDeclPtr& f) {
        if (!arityOk(f)) return false;
        for (size_t i = 0; i < argTypes.size(); i++) {
            TypePtr pt = stripRefConst(f->parameters[i].type);
            TypePtr at = stripRefConst(argTypes[i]);
            if (!pt || !at || !pt->equals(at)) return false;
        }
        return true;
    };

    struct Candidate { FuncDeclPtr decl; std::string via; };  // via: "普通查找" / "ADL(N)"
    // 【判据：只收"普通自由函数" —— 即 mangledName == name】m_functions 里混着两类不该
    //   在此参与裁决的东西：
    //   · 类成员函数：name 是裸方法名（Box_int::get 的 name 就是 "get"，mangled 带类名前缀）
    //     —— 成员调用走类作用域查找，不该被裸名匹配到；
    //   · 函数模板实例（mix<int,int>）：name 保留模板名 "mix"，符号名 _Z 开头。它们必须由
    //     模板路径（S5）解析，否则 `mix<int,int>(3,4)` 会被这个"精确匹配"抢先命中，调用的
    //     却是一个尚未按调用点重写名字的实例 ⇒ 链接期 undefined 'mix'（test_tmpl_17）。
    auto isPlainFreeFunction = [](const FuncDeclPtr& f) {
        return f->ownerClassName.empty() && f->mangledName == f->name;
    };
    std::vector<Candidate> pool;
    for (auto& f : m_functions) {
        if (!isPlainFreeFunction(f)) continue;
        if (f->name == funcName) {
            pool.push_back({f, "普通查找"});
        }
    }
    for (const auto& ns : assocNs) {
        std::string qualified = ns + "::" + funcName;
        for (auto& f : m_functions) {
            if (!isPlainFreeFunction(f)) continue;
            if (f->name == qualified) pool.push_back({f, std::format("ADL({})", ns)});
        }
    }

    // ── ③ 裁决：精确匹配优先；无精确匹配时退回既有行为 ──
    Candidate* winner = nullptr;
    for (auto& c : pool) {
        if (!arityOk(c.decl)) continue;
        if (exactMatch(c.decl)) { winner = &c; break; }   // 平手取注册顺序靠前者
    }
    if (winner) {
        bool isAdl = winner->via.rfind("ADL", 0) == 0;
        std::cout << std::format(
            "{}[call] {}({} args) → {}    [{}{}：形参类型精确匹配]\n",
            inferIndent(), funcName, argTypes.size(),
            winner->decl->returnType ? winner->decl->returnType->toString() : "?",
            winner->via, isAdl ? "，" + funcName + " 走 ADL 限定查找" : "");
        // ★ 名字原地改写：CodeGen 按名字发射 callq，ADL 命中的是
        //   "N::get" 而不是 "get"（再由 asmSymbol 净化成 N__get）。
        //   与 resolveTemplateCall 把名字改成 mangled 符号是同一手法。
        if (isAdl && calleeVar) calleeVar->name = winner->decl->ownerClassName.empty()
            ? winner->decl->name : winner->decl->ownerClassName + "::" + winner->decl->name;
        return winner->decl->returnType;
    }

    // ── ④ 退回既有路径：精确匹配一个都没有 ──
    if (it != m_functionMap.end()) {
        if (it->second->parameters.size() == argTypes.size()) {
            // ★ 限定名调用（`C::f()`）必须把 callee 名改写成【发射符号】。
            //   定义端成员函数用 mangledName（C_f），调用端若留着源码名 "C::f"，
            //   asmSymbol 会把 "::" 净化成 "__" 得到 C__f ⇒ 链接期 undefined reference。
            //   与模板实例（calleeVar->name = instance->mangledName）是同一手法：
            //   Sema 定死符号名，CodeGen 照着发 —— 名字的"真身"在语义阶段兑现。
            //   判据用 mangledName != funcName：命名空间自由函数两边同为 "N::get"
            //   （registerFunction 对 ownerClassName 为空者不修饰），天然不触发。
            if (calleeVar && !it->second->mangledName.empty()
                && it->second->mangledName != funcName) {
                std::cout << std::format(
                    "{}[call] 限定名 '{}' → 发射符号 '{}'（成员函数 mangled）\n",
                    inferIndent(), funcName, it->second->mangledName);
                calleeVar->name = it->second->mangledName;
            }
            std::cout << std::format(
                "{}[call] {}({} args) → {}    [symbol resolved, non-template preferred]\n",
                inferIndent(), funcName, argTypes.size(),
                it->second->returnType ? it->second->returnType->toString() : "?");
            return it->second->returnType;
        }
        std::cout << std::format(
            "{}[call] non-template '{}' arg count mismatch ({} vs {}), keep searching\n",
            inferIndent(), funcName, it->second->parameters.size(), argTypes.size());
    }

    // ── ⑤ ADL 单独命中（普通查找完全没有这个裸名）──
    // 这是最常见的情形：`get(s)` 里 get 在全局根本不存在，只在 N 里。
    if (it == m_functionMap.end()) {
        for (const auto& ns : assocNs) {
            std::string qualified = ns + "::" + funcName;
            for (auto& f : m_functions) {
                if (!isPlainFreeFunction(f)) continue;
                if (f->name != qualified || !arityOk(f)) continue;
                std::cout << std::format(
                    "{}[call] {}({} args) → {}    [ADL: 实参关联命名空间 '{}' 命中]\n",
                    inferIndent(), funcName, argTypes.size(),
                    f->returnType ? f->returnType->toString() : "?", ns);
                if (calleeVar) calleeVar->name = qualified;
                return f->returnType;
            }
        }
    }

    // ── 函数模板路径（S2~S6）：仅对非成员调用 ──
    if (calleeVar) {
        // 显式模板实参的类型/值分流：推导引擎（S3）目前只吃类型实参表，故此处把
        // TemplateArg 拆回 TypePtr 列表；值为空的 Integral 实参说明用户写了 foo<4>(x)
        // —— 函数模板的 NTTP 尚未实现，明确报错而不是把空 TypePtr 塞进推导引擎引发崩溃。
        // （类模板的 NTTP 已实现，见 checkTemplateArguments。）
        std::vector<TypePtr> explicitTypeArgs;
        explicitTypeArgs.reserve(calleeVar->explicitTemplateArgs.size());
        for (const auto& a : calleeVar->explicitTemplateArgs) {
            if (a.isValue()) {
                error(std::format(
                    "explicit non-type template argument '{}' for function "
                    "template '{}' is not supported yet (only class templates "
                    "support non-type parameters)", a.toString(), funcName),
                    expr.location);
            }
            explicitTypeArgs.push_back(a.type);
        }

        TypePtr resolved = resolveTemplateCall(
            funcName, calleeVar, argTypes, argIsLValue,
            explicitTypeArgs, expr.location);
        if (resolved) return resolved;
    }

    // ── 兜底：原有 callee 推导路径（可能报 Undefined variable）──
    m_inferDepth++;
    TypePtr calleeType = inferType(expr.callee);
    m_inferDepth--;
    std::cout << std::format("{}[call] {}() → {} (callee type)\n",
        inferIndent(), funcName,
        calleeType ? calleeType->toString() : "?");
    return calleeType;
}

// ─── 函数模板调用解析（S2~S6 全链路，理论见 docs/learn/02、05、06）────────────
// 流程（对照 clang: AddTemplateOverloadCandidate → 推导 → 排序）：
//   ① 取同名候选集  ② 逐候选推导（S2 核心，含 S3 显式前缀 / S4 不可推导上下文检查）
//   ③ 可行候选按偏序排序选最特化（S6）  ④ 实例化赢家（S5，带缓存），把 callee 重写为
//      mangled 符号（CodeGen 直接 callq）
// demo: twice(42) 唯一候选 ⇒ P=T, A=int ⇒ T := int ✓ → 实例化 twice<int> →
//   mangled _Z5twiceIiE → AST 中 callee 名字被原地改写 → 返回实例的返回类型 int
// 全部候选推导失败 → 按 [over.match.viable] 报 "no matching function"。
// 返回 nullptr 表示"没有模板候选"，交回 inferCall 的兜底路径 —— 这不是错误。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  template <typename T> T twice(T x) { return x + x; }      int t = twice(x);
// │ 日志  [call] twice — 1 function template candidate(s), overload resolution begins
// │         candidate: template <T> twice
// │       [deduction] ▶ twice — 模板参数 <T>，实参 1 个
// │       [deduction]   P=T            A=int          ⇒ T := int
// │       [deduction] ◀ 推导成功: <T=int>
// │       ╔══ Function Template Instantiation (S5) ═══════╗
// │       ║ Blueprint: twice <T>     Substitution: { T → int, }
// │       ║ Symbol: twice → _Z5twiceIiE
// │       ╚═══════════════════════════════════════════════╝
// │       [call] twice → _Z5twiceIiE (template resolved) → int
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::resolveTemplateCall(
    const std::string& funcName,
    std::shared_ptr<VarExpr> calleeVar,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::vector<TypePtr>& explicitArgs,
    SourceLocation loc) {

    auto candIt = m_functionTemplateCandidates.find(funcName);
    if (candIt == m_functionTemplateCandidates.end() || candIt->second.empty())
        return nullptr;

    auto& candidates = candIt->second;
    std::cout << std::format(
        "{}[call] {} — {} function template candidate(s), overload resolution begins\n",
        inferIndent(), funcName, candidates.size());

    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    struct Viable { TemplateDeclPtr decl; DeductionResult result; };
    std::vector<Viable> viables;

    for (auto& cand : candidates) {
        std::string plist;
        for (size_t i = 0; i < cand->typeParams.size(); i++) {
            if (i > 0) plist += ", ";
            plist += cand->typeParams[i];
        }
        std::cout << std::format("{}  candidate: template <{}> {}\n",
            inferIndent(), plist, funcName);
        auto r = deducer.deduce(cand, argTypes, argIsLValue, explicitArgs);
        if (r.success) {
            viables.push_back({cand, r});
        } else {
            // ★ SFINAE 吸收点 ②（全项目三处之一，见 include/sfinae.h 的收口点一览）
            //   本处失败以【推导返回值】表达（deduce 返回 success=false），不像 ① 那样
            //   靠异常 —— 两种写法并存，但"候选被移除"的语义与日志出口已统一到
            //   Sfinae::rejected。
            Sfinae::rejected(std::format("函数模板重载 '{}'", funcName),
                             r.failureReason, inferIndent());
        }
    }

    if (viables.empty()) {
        error(std::format(
            "no matching function for call to '{}' (no template candidate deduces successfully)",
            funcName), loc);
    }

    // ── S6：偏序 —— 选最特化的可行候选 ──
    size_t winnerIdx = 0;
    for (size_t i = 1; i < viables.size(); i++) {
        if (isAtLeastAsSpecialized(viables[i].decl, viables[winnerIdx].decl)) {
            winnerIdx = i;
        }
    }
    if (viables.size() > 1) {
        std::cout << std::format(
            "{}  [overload] {} viable candidate(s), partial ordering picks #{}\n",
            inferIndent(), viables.size(), winnerIdx);
    }

    auto& winner = viables[winnerIdx];

    // ── S5：实例化（带缓存）──
    FuncDeclPtr instance =
        getOrInstantiateFunction(winner.decl, winner.result.deducedArgs);

    // 把 callee 重写为 mangled 符号，codegen 直接 callq
    calleeVar->name = instance->mangledName;
    std::cout << std::format("{}[call] {} → {} (template resolved) → {}\n",
        inferIndent(), funcName, instance->mangledName,
        instance->returnType ? instance->returnType->toString() : "void");

    return instance->returnType;
}

// ─── 实例化函数模板（S5）：带缓存，避免同一 <实参> 重复实例化 ─────────────────
// [temp.inst]：同一模板 + 同一实参列表，全局只实例化一次。缓存键 = mangled 名。
// 用例  twice<int> ⇒ _Z5twiceIiE（_Z + 名字长度 5 + twice + I + i(int 的编码) + E）；
//       再调同一 <int> ⇒ 日志 "[instantiate] cache hit: _Z5twiceIiE (skip re-instantiation)"
// 新实例诞生后的三步：① m_functionMap[mangled] = 实例（后续调用直接命中）
//   ② push 进 m_functions（CodeGen 按序输出汇编）③ analyzeFunctionBody(实例)：用具体
//   类型检查函数体 = 两阶段查找 [temp.names] 的第二阶段（第一阶段 = processTemplateDecl
//   只注册蓝图不查体）。
// ─── P3：类模板按需实例化（[temp.inst] 隐式实例化点）──────────────────────────
// 触发时机：类型位置（变量声明/形参/返回类型）出现 Box<int> 这类模板 id，resolveType
//   发现"带实参的类名命中类模板蓝图"即调入本函数。三步走（对照 clang：Sema::ActOnTag
//   → InstantiateClass → 成员延迟分析）：
//   ① instantiate() 深拷贝蓝图 + 结构化替换 → 实例 ClassDecl（如 Box_int）
//   ② processClassDecl 实例类按普通类走完整注册（构造/析构合成、布局、vtable/RTTI、符号表）
//   ③ 实例方法注册 + 函数体分析 —— 即两阶段查找的第二阶段
// ★ 缓存先行：先写缓存再析方法体，方法体若再引用同一实例（递归/互用）直接命中。
// ─── 模板实参校验（[temp.arg]）：类型形参与非类型形参（NTTP）的形态把关 ────────
// 逐位比对「结构化形参表 templateParams（带 kind）」与「实际实参表」，三道检查：
//   ① 个数 [temp.arg.explicit]/1：多于形参数 ⇒ 报错；少于则差额必须都有默认实参
//   ② 形态：类型形参（typename/class T）收【类型】实参，非类型形参（int N）收【值】实参
//      —— NTTP 与普通模板参数的分水岭（本条主线的核心）③ 值类型：NTTP 形参声明的类型
//      必须受支持（本项目只支持 int）
// ★ 必须用 templateParams 而不是 typeParams：后者只有名字、kind 已丢，
//   `template<class T, int N>` 会数出 2 个"类型"形参，对 Buf<int,4> 这种"1 类型 + 1 值"
//   的实参表必然错位。
// 对照 clang：Sema::CheckTemplateArgumentList（clang/Sema/SemaTemplate.cpp）—— 真实现还会
//   做隐式转换（Buf<4> 的 4 → unsigned/枚举）、默认模板实参填充、参数包匹配；本项目只留
//   ①②③三条主干，够讲清"类型 vs 值"的判定。逐位判定结果见下方 DEMO 的真实日志。
// ┌─ DEMO（真实日志）──────────────────────────────────────────────────────────
// │ 源码  Box<int> bi;      Box<int*> bp;      Buf<4> b;
// │ 日志  [sema:targ]   ✓ param 1: 'T' (type) ← int
// │       [sema:targ] ✓ template arguments OK: Box<int>
// │ 反例  Buf<int> → ②-b 命中：
// │       [ERROR] template argument 1 for 'Buf' ('N') must be a non-type argument
// │               of type 'int', but 'int' is a type
// └────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::checkTemplateArguments(
    const TemplateDeclPtr& blueprint, const TypePtr& templateIdType,
    SourceLocation loc) {

    const auto& params = blueprint->templateParams;
    const auto& args   = templateIdType->templateArgs;
    const std::string& tname = templateIdType->name;

    // ── ① 个数（[temp.arg.explicit]/1 + [temp.param]/12）──
    // 实参可以【少于】形参，差额由默认实参补齐；但不能多于形参，且每位缺失的形参都必须
    // 带默认值，否则无法补全。
    // demo：template<class T, class U = void> + Box<int> → 少 1 位，U 有默认 ✓
    //       template<class T, class U = void> + Box<int,double,char> → 多了 ✗
    if (args.size() > params.size()) {
        error(std::format(
            "class template '{}' expects at most {} argument(s), but {} given",
            tname, params.size(), args.size()), loc);
    }
    if (args.size() < params.size()) {
        for (size_t i = args.size(); i < params.size(); i++) {
            if (!params[i]->hasDefault) {
                error(std::format(
                    "class template '{}' expects at least {} argument(s) "
                    "(parameter '{}' has no default), but {} given",
                    tname, i + 1, params[i]->name, args.size()), loc);
            }
        }
    }

    // ── ②③ 逐位形态与值类型（只校验【用户实际给出】的那些位）──
    // 第 i >= args.size() 位留空是合法的，只要它有默认实参（由上面的个数检查把关）。
    for (size_t i = 0; i < std::min(params.size(), args.size()); i++) {
        const TemplateParam& p = *params[i];
        const TemplateArg&   a = args[i];

        // 该位"期望什么"的可读描述（供两向报错复用）
        const std::string want =
            (p.kind == TemplateParamKind::Type)
                ? "a type argument"
                : std::format("a non-type argument of type '{}'",
                              p.nonType ? p.nonType->toString() : "?");

        // ②-a 类型形参 收到 值实参：Box<4> 而 T 是类型形参
        if (p.kind == TemplateParamKind::Type && a.isValue()) {
            error(std::format(
                "template argument {} for '{}' ('{}') must be {}, but '{}' is a value",
                i + 1, tname, p.name, want, a.toString()), loc);
        }
        // ②-b 非类型形参 收到 类型实参：Buf<int> 而 N 是 NTTP
        if (p.kind == TemplateParamKind::NonType && a.isType()) {
            error(std::format(
                "template argument {} for '{}' ('{}') must be {}, but '{}' is a type",
                i + 1, tname, p.name, want, a.toString()), loc);
        }
        // ③ NTTP 的值类型必须受支持（本项目支持 int / bool）
        //    对照 clang：[temp.param]/6 允许整型（含 bool）/枚举/指针/左值引用/字面量类类型等
        if (p.kind == TemplateParamKind::NonType
            && p.nonType && !p.nonType->isInt() && !p.nonType->isBool()) {
            error(std::format(
                "non-type template parameter '{}' of '{}' has unsupported type '{}' "
                "(only 'int' and 'bool' are supported)",
                p.name, tname, p.nonType->toString()), loc);
        }
        // ③-b 值实参的【形态】须与形参类型相容（[temp.arg.nontype]/1：实参应是
        //     形参类型的【转换后常量表达式】）。`Flag<1>`（bool 形参收 int 字面量）
        //     与 `Buf<true>`（int 形参收 bool）都不合法。
        //     本项目做"形态精确匹配"，不做隐式转换 —— 与实参推导的严格性一致。
        if (p.kind == TemplateParamKind::NonType && a.isValue()
            && a.valueType && p.nonType && !a.valueType->equals(p.nonType)) {
            error(std::format(
                "non-type template argument for '{}' ('{}') has type '{}', "
                "but the parameter type is '{}'",
                tname, p.name, a.valueType->toString(), p.nonType->toString()), loc);
        }

        std::cout << std::format(
            "  [sema:targ]   ✓ param {}: '{}' ({}) ← {}\n",
            i + 1, p.name,
            p.kind == TemplateParamKind::Type ? "type" : "non-type",
            a.toString());
    }

    std::cout << std::format(
        "  [sema:targ] ✓ template arguments OK: {}<{}>\n", tname, [&] {
            std::string s;
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) s += ", ";
                s += args[i].toString();
            }
            return s;
        }());
}

// ─── 类模板特化择优（[temp.class.spec.match] + [temp.expl.spec]/6）────────────
// 【择优顺序】显式（全）特化优先于偏特化，偏特化优先于主模板（逐条的用例见下方 DEMO）：
//   ① 全特化：specPattern 与实参【逐位类型相等】→ 命中即用（最高优先）
//      template<> struct Box<int*, int>;   对 Box<int*, int> ⇒ int*==int* ✓ int==int ✓
//   ② 偏特化：用实参去【推导】specPattern 中的模板参数，全位成功即匹配
//      template<class T> struct Box<T*, T>;   对 Box<double*, double> ⇒ T:=double（一致 ✓）
//                                             对 Box<int, int> ⇒ 指针结构失配 ✗ 继续下一个
//   ③ 都不中 → 主模板（替换表留空，由 instantiate 按 templateParams 逐位分派）
// 【多候选】先【全部收集】匹配的偏特化，再用 dominance 循环逐对比较
//   （classSpecAtLeastAsSpecialized，合成 $ord_ 类型做偏序），无人支配者胜出；
//   仍多于一个则按标准报 ambiguous。
// 【对照 clang】Sema::CheckClassTemplatePartialSpecializationArgs /
//   Sema::InstantiateClassTemplateSpecialization（真实现还会检测特化是否比主模板更特化、
//   有无重复定义 —— 本项目不做）。
// 【返回】选中的声明 + 特化路径的替换表（主模板路径为空表）
std::tuple<TemplateDeclPtr, TemplateInstantiator::TypeSubstitution, bool>
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  template <typename T> struct Box {...};       // 主模板
// │       template <typename T> struct Box<T*> {...};   // 偏特化
// │       template <> struct is_int<int> : public std::true_type {};   // 全特化
// │ 日志  Box<int>    → [spec:select]   ✗ parameter pattern 'T*' expects pointer
// │                                       argument, got 'int'
// │                     [spec:select]   ├─ ② 候选不匹配：parameter pattern 'T*' ...
// │                     [spec:select]   └─ ③ falling back to PRIMARY template
// │       Box<int*>   → [spec:select]   P=T   A=int   ⇒ T := int
// │                     [spec:select]   │  唯一候选 → 直接选中 'Box<T*>'
// │       is_int<int> → [spec:select]   ├─ ① explicit specialization matched
// │                                       (exact argument equality) → USING IT
// │ 输出  (选中的声明, 替换表, 是否走特化)；主模板路径的替换表为空
// │ 偏特化的失败原因会原样打进日志（"reference structure mismatch" 这类）
// └────────────────────────────────────────────────────────────────────────────
SemanticAnalyzer::selectClassTemplate(
    const TemplateDeclPtr& primary,
    const std::vector<TemplateArg>& args,
    SourceLocation loc) {

    const std::string& name = primary->templateName();

    // ★ 实参表原样参与匹配（含 NTTP 值位）。
    //   此前这里有个"含值实参就跳过特化路径"的守卫，前提是"specPattern 只含类型"
    //   —— 而 [temp.class.spec] 并不禁止模式位写成值，`enable_if<true, T>` 这种
    //   "用 NTTP 选中特化"的惯用法正需要它。守卫拆掉后，值位由 matchPattern 逐位比。

    std::cout << std::format("  [spec:select] ★ selecting class template '{}' for <{}>\n",
        name, [&] { std::string s;
                   for (size_t i = 0; i < args.size(); i++) {
                       if (i > 0) s += ", "; s += args[i].toString();
                   } return s; }());

    {
        // ── ① 全特化：逐位实参相等 ──
        // 对照 clang：Sema::CheckTemplateArgumentList 对已知特化的精确匹配路径
        if (auto it = m_explicitSpecs.find(name); it != m_explicitSpecs.end()) {
            for (const auto& spec : it->second) {
                if (spec->specPattern.size() != args.size()) continue;
                bool same = true;
                for (size_t i = 0; i < args.size(); i++) {
                    if (!spec->specPattern[i].equals(args[i])) { same = false; break; }
                }
                if (same) {
                    std::cout << std::format(
                        "  [spec:select]   ├─ ① explicit specialization matched "
                        "(exact argument equality) → USING IT\n");
                    return {spec, {}, true};
                }
            }
            std::cout << "  [spec:select]   ├─ ① no explicit specialization matched\n";
        }

        // ── ② 偏特化：用实参推导模式 ──
        // 对照 clang：Sema::CheckClassTemplatePartialSpecializationArgs
        //              + DeduceTemplateArguments（与函数模板同一套合一算法）
        if (auto it = m_partialSpecs.find(name); it != m_partialSpecs.end()) {
            // ── 第一轮：收集【全部】匹配的候选（不再一匹配就返回）——
            // 收集是偏序裁决的前提：只有一个候选时无从比较，两个以上才谈得上"谁更特化"。
            struct Candidate {
                TemplateDeclPtr spec;
                TemplateInstantiator::TypeSubstitution taggedSubst;
            };
            std::vector<Candidate> matched;

            for (const auto& spec : it->second) {
                // 偏特化的形参名列表（用于识别模式里的可绑定未知量）
                std::vector<std::string> paramNames;
                for (const auto& p : spec->templateParams) paramNames.push_back(p->name);

                std::unordered_map<std::string, TypePtr> subst;
                std::string reason;
                // ★ 必须把求值器挂上：偏特化模式里的 void_t<decltype(...)>
                //   要靠它做"替换 + 求值"探测。缺了这步，void_t 不会被归约成
                //   void，模式第 2 位就永远匹配不上 —— SFINAE 探测整条链断掉。
                TemplateDeducer deducer; // 模板推导
                deducer.setDecltypeEvaluator(this);
                deducer.setMemberTypeResolver(this);   // void_t<typename T::type> 探测要用
                deducer.setAliasTemplateResolver(this);
                if (deducer.matchPattern(spec->specPattern, args,
                                         paramNames, subst, reason)) {
                    std::cout << std::format(
                        "  [spec:select]   ├─ ② 候选：'{}<{}>' 匹配成功\n",
                        name, patternToString(spec->specPattern));

                    TemplateInstantiator::TypeSubstitution tagged;
                    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);
                    matched.push_back({spec, tagged});
                }
                else {
                    std::cout << std::format(
                        "  [spec:select]   ├─ ② 候选不匹配：{}\n", reason);
                }
            }

            // ── 第二轮：偏序裁决（[temp.class.order]）──
            // 只有一个候选 → 直接胜出，无需比较。
            if (matched.size() == 1) {
                std::cout << std::format(
                    "  [spec:select]   │  唯一候选 → 直接选中 '{}<{}>'\n",
                    name, patternToString(matched[0].spec->specPattern));
                return {matched[0].spec, matched[0].taggedSubst, true};
            }

            if (matched.size() > 1) {
                std::cout << std::format(
                    "  [spec:select]   │  ★ {} 个偏特化同时匹配 → 进入 [temp.class.order] 偏序裁决\n",
                    matched.size());
                int winner = -1;
                {
                    std::vector<int> best;
                    for (size_t i = 0; i < matched.size(); i++) {
                        bool dominated = false;
                        for (size_t j = 0; j < matched.size() && !dominated; j++) {
                            if (i == j) continue;
                            // j 严格比 i 更特化（j 至少和 i 一样特化，且 i 不比 j 更特化）
                            if (classSpecAtLeastAsSpecialized(matched[j].spec, matched[i].spec) &&
                                !classSpecAtLeastAsSpecialized(matched[i].spec, matched[j].spec)) {
                                dominated = true;
                            }
                        }
                        if (!dominated) best.push_back(static_cast<int>(i));
                    }
                    if (best.size() == 1) {
                        winner = best[0];
                    } else {
                        // 多个候选互不支配 → 歧义，标准要求报错
                        // 对照 clang：err_ambiguous_partial_specialization
                        std::string cands;
                        for (int b : best) {
                            if (!cands.empty()) cands += " / ";
                            cands += patternToString(matched[b].spec->specPattern);
                        }
                        error(std::format(
                            "ambiguous partial specializations of '{}' for <{}>: "
                            "{} are equally specialized, none is more specialized "
                            "than the others",
                            name, typeListToString(args), cands), loc);
                    }
                }
                std::cout << std::format(
                    "  [spec:select]   │  ⇒ 最特化者：'{}<{}>' → USING IT\n",
                    name, patternToString(matched[winner].spec->specPattern));
                return {matched[winner].spec, matched[winner].taggedSubst, true};
            }
        }
    }

    // ── ③ 主模板 ──
    std::cout << "  [spec:select]   └─ ③ falling back to PRIMARY template\n";
    (void)loc;
    return {primary, {}, false};
}

// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  Box<int> bi;      // resolveType 把模板 id Box<int> 交到这里
// │ 日志  [instantiate:class] ★ on-demand instantiation: Box<int>
// │       [subst:map] 'T' (type) := int    [register] class 'Box_int'  field: value : int
// │       ╔══ Template Instantiation ═════════════════════╗
// │       ║ Blueprint: Box <typename T> [primary]   Instance: Box_int
// │       ║ Substitution map: { 'T' → 'int', }     Mangled: Box_int → _Z3BoxIiE
// │       ║ field 'value' : T → int      method 'get' :  → T
// │       ╚═══════════════════════════════════════════════╝
// │ 输出  ClassType（已注册进 m_classTypes，布局/vtable/mangled 全齐，后续 findField、
// │       new、成员调用都按普通类走 —— 模板痕迹到此抹平）。
// │ 缓存键  模板名 + 实参可读串（"Box<int>" / "Buf<4>"），TemplateArg::toString 按 kind
// │       分派 ⇒ Box<4> 与 Box<int> 天然不同键；时序：实例类【当场】走完
// │       processClassDecl → registerFunction → analyzeFunctionBody。
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::getOrInstantiateClass(
    TypePtr templateIdType, SourceLocation loc) {

    // ── 缓存键：模板名 + 实参可读串，如 "Box<int>" / "Buf<4>" —— TemplateArg::toString
    //    按 kind 分派（类型实参给类型名、值实参给数字），故 Box<4> 与 Box<int> 天然是
    //    不同键，不会互相顶掉缓存。
    std::string key = templateIdType->name + "<";
    for (size_t i = 0; i < templateIdType->templateArgs.size(); i++) {
        if (i > 0) key += ",";
        key += templateIdType->templateArgs[i].toString();
    }
    key += ">";

    auto cached = m_classInstanceCache.find(key);
    if (cached != m_classInstanceCache.end()) {
        std::cout << std::format(
            "  [instantiate:class] cache hit: {} (skip re-instantiation)\n", key);
        return cached->second;
    }

    // ── 查主模板（类模板注册表 O(1)；重名取先注册者，emplace 不覆盖）──
    TemplateDeclPtr primary;
    if (auto it = m_classTemplates.find(templateIdType->name);
        it != m_classTemplates.end()) {
        primary = it->second;
    }
    if (!primary) {
        error(std::format("'{}' is not a class template", templateIdType->name), loc);
    }

    // ── 实参校验（[temp.arg]）：个数 + 每位形态（类型 vs 值）都要对上 ──
    // ★ 这里是 NTTP 的关键校验点：template<class T> 收到值实参、template<int N> 收到类型
    //   实参，都必须在此拦下。注意用 templateParams（带 kind 的结构化形参表）而不是
    //   typeParams（退化的名字列表）—— 后者根本不知道哪位是 NTTP。
    checkTemplateArguments(primary, templateIdType, loc);

    // ── 用默认实参把实参表补全（[temp.param]/12）──
    // demo：template<class T, class U = void> + 使用点 Box<int>
    //         ⇒ 补成 [Type:int, Type:void]，此后一切按"实参已完整"处理。
    // ★ 补全必须在【选择特化之前】做：偏特化/全特化的匹配都是对完整实参表做的
    //   （Box<int*, int> 的全特化要有 2 位才能匹配上）。
    std::vector<TemplateArg> fullArgs = templateIdType->templateArgs;
    if (fullArgs.size() < primary->templateParams.size()) { //wangyang 这里属于将模板参数缺失的参数也带进来
        for (size_t i = fullArgs.size(); i < primary->templateParams.size(); i++) {
            const TemplateParam& p = *primary->templateParams[i];

            // ── ★ 默认实参可能是【依赖】的：先用已绑定的前 i 位替换一遍 ──
            // 用例  template<class T, class U = T> struct Box { T a; U b; };
            //       Box<int> ⇒ U 的默认值 T 必须当场变成 int ⇒ 实例名 Box_int_int、U b : int；
            //       不替换直接塞进 fullArgs ⇒ 实例名成 Box_int_T、字段 b 停在裸的 T 上
            //       （大小 0 字节），下游连锁答错且一声不吭 —— 这就是"替换 ≠ 实例化"。
            // 对照 clang：[temp.param]/12 的默认实参在【使用点】才实例化
            //   （Sema::SubstDefaultTemplateArgument），蓝图里存依赖形式是对的。
            // 前 i 位（fullArgs 里已有的：显式给的 + 前面刚补的）都能用。
            TemplateArg filled = p.defaultArg;
            if (i > 0 && filled.isType() && filled.type) {
                TemplateInstantiator::TypeSubstitution prior;
                for (size_t j = 0; j < i; j++) {
                    prior[primary->templateParams[j]->name] = fullArgs[j];
                }
                TypePtr resolved = m_instantiator.substituteType(filled.type, prior);
                if (resolved != filled.type) {
                    std::cout << std::format(
                        "  [sema:targ] ⤷ default argument '{}' is dependent: {} → {}\n",
                        p.name, filled.type->toString(), resolved->toString());
                    filled = TemplateArg::ofType(resolved);
                }
            }

            fullArgs.push_back(filled);
            std::cout << std::format(
                "  [sema:targ] ⤷ default argument filled: '{}' := {}\n",
                p.name, filled.toString());
        }
    }

    // ── 择优：全特化 → 偏特化 → 主模板（[temp.class.spec.match]）──
    TemplateDeclPtr blueprint = primary;
    TemplateInstantiator::TypeSubstitution specSubst; // 仅特化路径非空
    bool pickedSpec = false;
    std::tie(blueprint, specSubst, pickedSpec) =
        // 三路择优（全特化 / 偏特化 + 偏序裁决 / 主模板兜底），偏特化路径内部用 SFINAE 剔除候选
        selectClassTemplate(primary, fullArgs, loc);

    std::cout << std::format(
        "\n  [instantiate:class] ★ on-demand instantiation: {}\n", key);

    // ── ① 深拷贝蓝图 + 结构化替换 ──
    // 特化路径传 &specSubst（由 selectClassTemplate 的模式匹配推导）；主模板路径传
    // nullptr ⇒ instantiate 内部按 templateParams 逐位分派。两条路径的 args 都只用于
    // 实例名与 mangling。
    ClassDeclPtr instance = m_instantiator.instantiate(
        blueprint, fullArgs, pickedSpec ? &specSubst : nullptr);

    // ── ② 实例类完整注册（与源码中手写的类一视同仁）──
    processClassDecl(instance);

    TypePtr instanceType = m_classTypes[instance->name];

    // ── 记下实例"出身"（哪个模板 + 哪些实参）──
    // 实例类型名叫 MyPtr_int，改名的瞬间模板 id 的信息就没了；
    // 而函数模板实参推导要拿 `MyPtr_int` 去匹配形参模式 `MyPtr<T>`，
    // 必须能回答"你由哪个模板、用哪些实参实例化而来"。
    // 见 include/type.h 的 templateOriginName 注释。
    if (instanceType) {
        instanceType->templateOriginName = templateIdType->name;
        instanceType->templateOriginArgs = fullArgs;
    }

    // 实例类符号同时登记进全局作用域：实例化可能在某函数体分析中途触发，
    // processClassDecl 会把符号 define 进该函数作用域 —— 兄弟函数不可见。
    // 全局作用域冗余登记一份，保证任何位置 lookup("Box_int") 都命中。
    Symbol globalSym;
    globalSym.name = instance->name;
    globalSym.type = instanceType;
    globalSym.kind = SymbolKind::Type;
    globalSym.isLocal = false;
    globalSym.definedAt = instance->location;
    m_symbolTable.globalScope()->define(instance->name, globalSym);

    // 缓存先行写入（方法体分析可能再次引用本实例——递归/互用场景）
    m_classInstanceCache[key] = instanceType;

    // ── ③ 实例方法：注册（等价 Pass 2）+ 函数体分析（等价 Pass 3）──
    for (auto& method : instance->methods) {
        registerFunction(method);
    }
    for (auto& method : instance->methods) {
        analyzeFunctionBody(method);
    }

    std::cout << std::format(
        "  [instantiate:class] ✔ {} ready ({} bytes, {} method(s))\n",
        instance->name, instanceType->classLayout.totalSize,
        instance->methods.size());
    return instanceType;
}

FuncDeclPtr SemanticAnalyzer::getOrInstantiateFunction(
    TemplateDeclPtr tmpl, const std::vector<TypePtr>& args) {
    // 函数模板的实参由推导引擎产出，全是类型（函数模板 NTTP 未实现），
    // 故包成 TemplateArg::ofType 送进通用的 mangler。
    std::vector<TemplateArg> targs;
    targs.reserve(args.size());
    for (auto& a : args) targs.push_back(TemplateArg::ofType(a));

    // ★ 缓存键必须与实例化端（TemplateInstantiator）编出【同一串】—— 它用的是
    //   完整签名（返回类型 + 参数表）。此前这里只编到模板实参，于是
    //   `f(1)` 与 `f(1,2)`（两个不同重载、T 同为 int）编出同一个键，
    //   后者撞上前者的缓存项被当成"已实例化" ⇒ 静默调用错误的实例（docs/BUGS.md B2）。
    std::string mangled = NameMangler::mangleFunctionTemplateInstance(
        tmpl->funcTemplate->name, targs,
        tmpl->funcTemplate->returnType, tmpl->funcTemplate->parameters,
        tmpl->typeParams);

    auto it = m_templateInstanceCache.find(mangled);
    if (it != m_templateInstanceCache.end()) {
        std::cout << std::format("  [instantiate] cache hit: {} (skip re-instantiation)\n",
            mangled);
        return it->second;
    }

    FuncDeclPtr instance = m_instantiator.instantiateFunction(tmpl, args);
    m_templateInstanceCache[mangled] = instance;

    // ── 实例签名的"落地解析"：把残留的模板 id 换成具体实例类型 ──
    // 结构化替换（substituteType）只做【替换】不做【实例化】：`MyPtr<T>` 配 {T:=int} 出来的
    //   是 `MyPtr<int>` 这个**半成品类型节点**（名字是模板名 + 实参表），而不是 `MyPtr_int`。
    //   不补这一步，函数体里 `p.value` 在 m_classTypes 查不到 "MyPtr" ⇒ 报 No member
    //   （类型其实是对的，只是"还没被兑现"）。交给 resolveType 按需实例化，与变量/字段声明
    //   走同一条兑现路径；别名模板的形参位不需要 —— 解糖内部已调过一次 resolveType。
    // 对照 clang：TreeTransform 之后 Sema 仍会用具体类型重新 CheckFunctionDeclaration
    //   （两阶段查找的第二阶段）。
    if (instance->returnType) instance->returnType = resolveType(instance->returnType);
    for (auto& param : instance->parameters) {
        param.type = resolveType(param.type);
    }

    // 注册进 codegen 函数列表；函数体在具体类型下做两阶段查找的第二阶段
    m_functionMap[mangled] = instance;
    m_functions.push_back(instance);
    analyzeFunctionBody(instance);
    return instance;
}

// ─── 成员模板实例化（[temp.mem]）────────────────────────────────────────────
// 与自由函数模板【逐字同构】：推导 → 实例化（带缓存）→ 落地解析 → 注册进 CodeGen
//   的函数列表。标准里 [temp.mem]/1 明说成员模板的推导规则与 [temp.deduct] 一致 ——
//   形参表就是形参表，不为"成员"另立一套合一算法。
// 只多两件事：
//   ① ownerClassName（隐式 this）：CodeGen 靠它决定参数寄存器要不要偏一位；
//   ② 符号是 `类名_方法名_实参`（不是 Itanium 模板实例名）—— 同一个方法名的多个
//      实例（S_id_int / S_id_double）必须各有符号，CodeGen 的成员调用才能各调各的。
FuncDeclPtr SemanticAnalyzer::getOrInstantiateMemberFunction(
    const TemplateDeclPtr& tmpl,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::string& ownerClassName,
    SourceLocation loc) {

    std::cout << std::format(
        "{}[call] {}::{} — 成员模板候选，开始推导（[temp.mem]）\n",
        inferIndent(), ownerClassName, tmpl->templateName());

    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    auto r = deducer.deduce(tmpl, argTypes, argIsLValue);
    if (!r.success) {
        // SFINAE 性质：这位成员模板推不出来【不一定是错误】—— 调用方可能还有别的
        // 重载可选。故只报推导失败原因，由调用方决定是继续找还是报 no matching。
        std::cout << std::format("{}[call] {}::{} 推导失败：{}\n",
            inferIndent(), ownerClassName, tmpl->templateName(), r.failureReason);
        return nullptr;
    }

    // 缓存键 = 实例的符号名。与 Instantiator 用同一个 sanitizeSymbolChars ——
    // 两处各写一份的话，键与符号名会悄悄对不上（命中失败或误命中，都不报错）。
    std::string mangled = sanitizeSymbolChars(
        ownerClassName + "_" + tmpl->funcTemplate->name);
    for (auto& a : r.deducedArgs) {
        mangled += "_" + sanitizeSymbolChars(a ? a->toString() : "?");
    }

    auto it = m_templateInstanceCache.find(mangled);
    if (it != m_templateInstanceCache.end()) {
        std::cout << std::format("  [instantiate] cache hit: {} (skip re-instantiation)\n",
            mangled);
        return it->second;
    }

    FuncDeclPtr instance =
        m_instantiator.instantiateFunction(tmpl, r.deducedArgs, ownerClassName);
    m_templateInstanceCache[mangled] = instance;

    // 落地解析（同自由函数模板）：替换只做【替换】，残留的半成品模板 id 要靠
    // resolveType 兑现成具体实例类型，否则函数体里 p.value 查不到类。
    if (instance->returnType) instance->returnType = resolveType(instance->returnType);
    for (auto& param : instance->parameters) {
        param.type = resolveType(param.type);
    }

    std::cout << std::format("{}[call] {}::{} → {} (member template instantiated) → {}\n",
        inferIndent(), ownerClassName, tmpl->templateName(), mangled,
        instance->returnType ? instance->returnType->toString() : "void");

    m_functionMap[mangled] = instance;
    m_functions.push_back(instance);
    analyzeFunctionBody(instance);
    (void)loc;
    return instance;
}

// ─── S6 偏序：a 是否"至少与 b 同样特化"（[temp.func.order] 简化，docs/learn/21）──
// ⚠ 方向易错：用 a 自己的参数类型当合成实参，去推导 b。b 能推导成功 ⇒ b 覆盖 a 的全部
//   输入域 ⇒ a 接受的类型更少 ⇒ a 更特化。
// demo: po(T) vs po(T*)：用 T* 当实参推导 po(T) ⇒ T := T* 成功 ⇒ po(T*) 更特化 ✓
// 对照 clang：lib/Sema/SemaOverload.cpp → IsAtLeastAsSpecialized
bool SemanticAnalyzer::isAtLeastAsSpecialized(TemplateDeclPtr a, TemplateDeclPtr b) {
    TemplateDeducer deducer;
    deducer.setDecltypeEvaluator(this);
    deducer.setMemberTypeResolver(this);
    deducer.setAliasTemplateResolver(this);
    std::vector<TypePtr> synthArgs;
    std::vector<bool>    synthLValue;
    for (auto& p : a->funcTemplate->parameters) {
        synthArgs.push_back(p.type);   // a 的模板参数名在此扮演"唯一合成类型"
        synthLValue.push_back(true);
    }

    std::cout << std::format("  [overload] partial ordering check (deduction-based)...\n");
    auto r = deducer.deduce(b, synthArgs, synthLValue);
    std::cout << std::format("  [overload]   ⇒ {}\n",
        r.success ? "前者至少与后者同样特化（前者胜）"
                  : "无特化关系（保留原候选）");
    return r.success;
}

// ─── 成员访问推导 —— "编译期看符号 → 运行期看偏移量"的转换点 ────────────────
// 成员访问 [expr.ref] 的两条形态：p->x（isArrow）p 是指针，先取 pointeeType 解引用再
//   查成员；obj.x 直接查成员。
// 成员解析两步走：① 字段：查布局表 findField（线性扫描，含继承字段）→ 返回字段类型，
//   日志同时给出 offset/size —— CodeGen 拿 offset 直接寻址，这就是"符号 → 偏移量"的
//   兑现时刻；② 方法：查 m_classDecls 的方法表 → 返回方法返回类型。
// demo: p:Animal*，p->age ⇒ findField("age") → int (offset=8, size=4)
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  int k = pb->kind();     // pb : Base*，kind 是虚函数
// │       int v = bi.get();       // bi : Box_int
// │ 日志  [resolve] 'pb' → Base*    (kind=Variable, stack@-128)
// │       [member] Base.kind() → int    (method, virtual)      ← 方法：标了 virtual
// │       [member] Vec.n → int    (offset=0, size=4)           ← 字段：带出偏移
// │ 输出  字段 / 方法的类型（字段还带出 offset 与 size）
// │ ★ 去引用  [expr.type] 引用要"看穿"：declval<Vec>().begin() 里 actualType 是 Vec&&，
// │       不剥掉就会误报 "Cannot access member on non-class type 'Vec&&'"
// │ 找不到  error("No member 'x' in class 'C'")
// └────────────────────────────────────────────────────────────────────────────
// ── 类成员查找的两个原语（[class.member.lookup]）────────────────────────────
// ★ 为什么抽出来：成员访问（inferMember）与成员调用（inferCall）是两条通路，但"一个类
//   有没有叫这个名字的方法"必须是【同一个判据】—— 写成两份就会各自演化（BUGS.md B10
//   的根因正是同一规则两处实现不一致；B12 则是其中一处漏了基类链）。此处收口：
//   谓词只有 findMethodInClass 一处，层次遍历只有 findMethodInHierarchy 一处。
// 对照 clang：两条通路最终都落到 LookupResult（DeclContext::lookup 沿 base 链）+ 重载决议。
FuncDeclPtr SemanticAnalyzer::findMethodInClass(const std::string& className,
                                                const std::string& methodName,
                                                int arity) const {
    auto it = m_classDecls.find(className);
    if (it == m_classDecls.end()) return nullptr;
    for (auto& method : it->second->methods) {
        if (method->name != methodName) continue;
        // arity < 0 ⇒ 不看参数个数（`d.g` 这种没有实参可数，无法据此筛选）
        if (arity >= 0 && method->parameters.size() != static_cast<size_t>(arity)) continue;
        return method;
    }
    return nullptr;
}

FuncDeclPtr SemanticAnalyzer::findMethodInHierarchy(const std::string& className,
                                                    const std::string& methodName,
                                                    std::string* declaringClass) const {
    // 遍历顺序刻意与 inferCall 的成员方法搜索保持逐字一致（基类名 insert 到队首、
    // 从队尾取）—— 否则"两个基类都有同名方法"时两条通路会选中不同的那一个。
    std::vector<std::string> searchQueue = {className};
    std::set<std::string> searched;
    while (!searchQueue.empty()) {
        std::string clsName = searchQueue.back();
        searchQueue.pop_back();
        if (!searched.insert(clsName).second) continue;
        if (auto method = findMethodInClass(clsName, methodName, -1)) {
            if (declaringClass) *declaringClass = clsName;
            return method;
        }
        auto it = m_classDecls.find(clsName);
        if (it == m_classDecls.end()) continue;
        for (auto& bn : it->second->baseClassNames)
            searchQueue.insert(searchQueue.begin(), bn);
    }
    return nullptr;
}

TypePtr SemanticAnalyzer::inferMember(MemberExpr& expr) {
    m_inferDepth++;
    TypePtr objType = inferType(expr.object);
    m_inferDepth--;

    if (!objType) {
        error("Cannot access member of null type", expr.location);
    }

    // 解引用指针
    TypePtr actualType = objType;
    if (expr.isArrow && objType->isPointer()) {
        actualType = objType->pointeeType;
    }

    // ── 去引用：引用类型要"看穿" ──
    // 【理论】[expr.ref]/[expr.type]：成员访问的对象表达式先做左值化调整，引用被剥掉后
    //   再查成员 —— `Vec& r; r.begin()` 查的是 Vec 的 begin，而不是"引用类型的 begin"
    //   （引用根本没有成员）。
    // ⚠ 不剥掉的后果：decltype/declval 打通后，表达式里会冒出裸的 `T&&`（declval<T>()
    //   的返回类型），`declval<Vec>().begin()` 于是撞上 "Cannot access member on
    //   non-class type 'Vec&&'" —— 明明 Vec 有这个成员。
    if (actualType && actualType->isReference()) {
        actualType = actualType->referencedType;
    }

    if (!actualType || !actualType->isClass()) {
        error(std::format("Cannot access member '{}' on non-class type '{}'",
            expr.memberName, actualType ? actualType->toString() : "?"),
            expr.location);
    }

    // 查找字段
    // ★ 歧义必须先于"取第一条"（[class.member.lookup]/8）：两个不同基类子对象各有一个
    //   同名成员时，真 C++ 是 ill-formed，而"先到先得"会静默绑到其中一个 —— 最坏的一类
    //   错误（能编译、能跑、结果错）。BUGS.md B13 的现场就是布局打印出两条同名
    //   "A.name"、第二条偏移还算成 0。
    auto fieldMatches = actualType->classLayout.findFields(expr.memberName);
    if (fieldMatches.size() > 1) {
        std::string where;   // 每条命中都点出"经哪个子对象、显示名是什么"
        for (auto* f : fieldMatches) {
            if (!where.empty()) where += "、";
            where += std::format("经 '{}' 的 '{}'",
                f->viaBase.empty() ? std::string("(本类)") : f->viaBase, f->name);
        }
        error(std::format(
            "Member '{}' is ambiguous in class '{}': found in {} base-class subobjects ({})",
            expr.memberName, actualType->name, fieldMatches.size(), where), expr.location);
    }
    auto fieldInfo = fieldMatches.empty() ? nullptr : fieldMatches.front();
    if (fieldInfo) {
        std::cout << std::format(
            "{}[member] {}.{} → {}    (offset={}, size={})\n",
            inferIndent(),
            actualType->name, expr.memberName,
            fieldInfo->type ? fieldInfo->type->toString() : "?",
            fieldInfo->offset, fieldInfo->size);
        return fieldInfo->type;
    }

    // 查找方法 —— 走【类作用域 + 基类链】（[class.member.lookup]）
    // ★ 修复 BUGS.md B12：这里此前只扫本类的方法表，不沿 baseClassNames 走。字段能查到
    //   是因为字段已被扁平化进 classLayout.fields，而方法没有扁平化 ⇒ `D d; d.g()`
    //   （g 在基类 A）报 "No member 'g' in class 'D'"。更具讽刺的是 inferCall 另有一条
    //   搜基类的路径，但它在更早的 inferType(callee) 就被这里的 error() 挡死了 ——
    //   同一条查找规则写在两处、只有一处带基类链，正是 B10 那类"双份判据"的翻版。
    std::string declCls;
    if (auto method = findMethodInHierarchy(actualType->name, expr.memberName, &declCls)) {
        std::cout << std::format(
            "{}[member] {}.{}() → {}    (method{}{})\n",
            inferIndent(),
            actualType->name, expr.memberName,
            method->returnType ? method->returnType->toString() : "?",
            method->isVirtual ? ", virtual" : "",
            declCls != actualType->name ? std::format(" via '{}'", declCls) : "");
        return method->returnType;
    }

    // ── 成员模板（[temp.mem]）：名字存在，但"是哪一个函数"要等实参 ──
    // ★ 这里【不能】报 No member：inferCall 对成员调用会先补推导一次 callee 表达式
    //   （见 inferCall 里 `inferType(mem)` 那段），走到这里时手上【没有实参】，
    //   推不出 T 也就选不出实例。报错会把整条成员模板调用路径掐死在推导之前。
    //   故只回答"名字确实存在"，返回 void 占位；真正的选择由调用点带实参完成。
    //   限定 isMethodCall：`auto x = s.id;` 这种"取成员模板本身"的写法仍然报错
    //   （clang 里那是未决名字，同样不合法）。
    if (expr.isMethodCall) {
        if (auto mtIt = m_classMemberTemplates.find(actualType->name);
            mtIt != m_classMemberTemplates.end()) {
            for (auto& mt : mtIt->second) {
                if (mt->templateName() == expr.memberName) {
                    std::cout << std::format(
                        "{}[member] {}.{} —— 成员模板（待实参推导，[temp.mem]）\n",
                        inferIndent(), actualType->name, expr.memberName);
                    return Type::makeVoid();
                }
            }
        }
    }

    error(std::format("No member '{}' in class '{}'",
        expr.memberName, actualType->name), expr.location);
}

// 下标表达式 v[i]（读值形态）—— [expr.sub] 的糖化：
//   minicc 没有运算符重载，IndexExpr 在此被"语义化"为成员调用约定：容器类必须提供
//   at() 方法，v[i] 读值 ≡ v.at(i)（CodeGen 按此发射）。校验两件事：① object 是类类型；
//   ② 该类存在形参合法的 at()（下标类型还要与 at() 形参兼容）。结果类型 = at() 的返回
//   类型（Vector::at → int，Map::at → value 类型）。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Vec { int n; int at(int i) { return n; } };    int a = v[0];
// │ 日志  [resolve] 'v' → Vec    (kind=Variable, stack@-8)
// │       [infer] IntLiteral(0) → int ｜ [index] Vec[int] → int  (sugar for Vec.at(i))
// │ 输出  at() 的返回类型
// │ 报错  Class 'X' has no at() method —— subscript requires the at()/set() convention
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferIndex(IndexExpr& expr) {
    m_inferDepth++;
    TypePtr objType = inferType(expr.object);
    m_inferDepth--;

    if (!objType) {
        error("Cannot subscript expression of null type", expr.location);
    }

    // 支持容器指针：p[i] ≡ (*p)[i]（与 MemberExpr 的 -> 解引用同一约定）
    TypePtr actualType = objType;
    if (objType->isPointer()) {
        actualType = objType->pointeeType;
    }

    if (!actualType || !actualType->isClass()) {
        error(std::format("Cannot apply subscript to non-class type '{}'",
            actualType ? actualType->toString() : "?"), expr.location);
    }

    // 查约定方法 at()：必须存在且恰好接收 1 个形参
    auto classIt = m_classDecls.find(actualType->name);
    if (classIt == m_classDecls.end()) {
        error(std::format("Class '{}' not declared", actualType->name),
              expr.location);
    }

    std::shared_ptr<FunctionDecl> atMethod;  // 形参校验与结果类型共用
    for (auto& method : classIt->second->methods) {
        if (method->name == "at" && method->parameters.size() == 1) {
            atMethod = method;
            break;
        }
    }
    if (!atMethod) {
        error(std::format(
            "Class '{}' has no at() method —— subscript requires the "
            "at()/set() convention (see docs/learn/12)", actualType->name),
            expr.location);
    }

    // ★ 回填两个约定方法的【符号】：CodeGen 不能硬拼 `类名_at` / `类名_set`
    //   —— Sema 给带参成员方法名加了"参数个数"后缀（IntVec_at_1 / IntVec_set_2），
    //   硬拼得到的是 IntVec_at，链接期 undefined reference。
    //   set 只回填不强制：类里没有 set 时留空串，CodeGen 退回硬拼，
    //   报错仍推迟到链接期（与既有行为一致）。
    expr.atSymbol = atMethod->mangledName;
    for (auto& method : classIt->second->methods) {
        if (method->name == "set" && method->parameters.size() == 2) {
            expr.setSymbol = method->mangledName;
            break;
        }
    }

    // 下标表达式推导 + 与 at() 形参类型校验
    TypePtr idxType = inferType(expr.index);
    if (idxType && !typeCompatible(atMethod->parameters[0].type, idxType)) {
        error(std::format(
            "Subscript type '{}' does not match {}.at() parameter '{}'",
            idxType->toString(), actualType->name,
            atMethod->parameters[0].type->toString()), expr.location);
    }

    std::cout << std::format("{}[index] {}[{}] → {}    (sugar for {}.at(i))\n",
        inferIndent(), actualType->name,
        idxType ? idxType->toString() : "?",
        atMethod->returnType ? atMethod->returnType->toString() : "?",
        actualType->name);

    return atMethod->returnType;
}

// new 表达式 [expr.new]（大幅简化）：只检查类名是否已注册；返回"指向该类的指针"；分配
//   字节数 = computeClassLayout 算出的 totalSize（CodeGen 据此调 malloc）；不调用构造
//   函数（构造/析构特性见 ROADMAP 主线 A）。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  Base* nw = new Base;      Box<int>* b = new Box<int>();
// │ 日志  [new] Base → Base*    (size=16 bytes, args=0)
// │       [new] template-id new Box → new Box_int (instantiated) → Box_int*
// │ 模板实参  new Box<int>()：先按需实例化，把 expr->className【原地改写】为 Box_int 并
// │       清空 templateArgs —— 模板痕迹在语义阶段一次性抹平，之后全管线只认实例名。
// │ 简化  只接受类名（`new int` 不解析）；构造实参需匹配已有构造函数（个数 + 类型兼容）。
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferNew(NewExpr& expr) {
    // P3 —— new Box<int>()：先按需实例化，className 原地改写为实例名（如 Box_int）。
    // 之后全管线（本函数查表、CodeGen::emitNew）只认实例名，模板痕迹在语义阶段一次性抹平。
    if (!expr.templateArgs.empty()) {
        TypePtr tid = Type::makeClass(expr.className);
        for (auto& arg : expr.templateArgs) {
            // 与 resolveType 的模板 id 分支同一套分派：
            // 类型实参递归 resolveType，非类型实参（NTTP 值）原样带过。
            tid->templateArgs.push_back(
                arg.isType() ? TemplateArg::ofType(resolveType(arg.type)) : arg);
        }
        TypePtr instance = getOrInstantiateClass(tid, expr.location);
        std::cout << std::format("  [new] template-id new {} → new {} (instantiated)\n",
            expr.className, instance->name);
        expr.className = instance->name;
        expr.templateArgs.clear();
    }

    auto it = m_classTypes.find(expr.className);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", expr.className), expr.location);
    }

    // 推导构造函数实参类型
    std::vector<TypePtr> argTypes;
    for (auto& arg : expr.constructorArgs) {
        argTypes.push_back(inferType(arg));
    }

    // 查找匹配的构造函数
    auto classIt = m_classDecls.find(expr.className);
    if (classIt != m_classDecls.end()) {
        bool foundMatch = false;
        for (auto& method : classIt->second->methods) {
            // 构造函数按 kind 认；非构造的同名方法（罕见）也一并纳入候选
            auto ctor = method->kind == NodeKind::Constructor
                      ? std::static_pointer_cast<ConstructorDecl>(method) : nullptr;
            if (!ctor && method->name != expr.className) continue;
            if (method->parameters.size() == argTypes.size()) {
                bool match = true;
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    if (!typeCompatible(method->parameters[i].type, argTypes[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    foundMatch = true;
                    break;
                }
            }
        }
        if (!foundMatch && (!argTypes.empty() || !classIt->second->methods.empty())) {
            if (!argTypes.empty()) {
                error(std::format("No matching constructor for class '{}' with {} arguments",
                    expr.className, argTypes.size()), expr.location);
            }
        }
    }

    std::cout << std::format("{}[new] {} → {}*    (size={} bytes, args={})\n",
        inferIndent(), expr.className, expr.className,
        it->second->classLayout.totalSize, argTypes.size());

    return Type::makePointer(it->second);
}

// this 表达式 [class.this]：只能出现在成员函数内（否则报错）；类型为"指向属主类的指针"
//   （标准中 this 是 prvalue）。对应的 this 符号已由 analyzeFunctionBody 注册为隐式参数；
//   此处只负责给 ThisExpr 节点定型。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  struct Vec { int n; int self() { return this->n; } };
// │ 日志  （analyzeFunctionBody 先注册隐式形参）
// │       [param] this : Vec*    stack@-8
// │       然后分析函数体：[this] → Vec* ｜ [member] Vec.n → int    (offset=0, size=4)
// │ 输出  Vec* —— 本项目把 this 实现成"隐式形参 + 符号表条目"
// │ 报错  'this' used outside of class method
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferThis(ThisExpr&) {
    if (m_currentClassName.empty()) {
        error("'this' used outside of class method", SourceLocation{});
    }

    auto it = m_classTypes.find(m_currentClassName);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", m_currentClassName), SourceLocation{});
    }

    std::cout << std::format("{}[this] → {}*\n",
        inferIndent(), m_currentClassName);
    return Type::makePointer(it->second);
}

// ─── dynamic_cast<T*>(expr)：运行时类型检查转型 [expr.dynamic.cast] ────────────
// 检查规则（真 C++ 的子集 —— 只支持"类指针 → 类指针"）：
//   ① 目标类型必须是已声明的类名  ② 源表达式必须是"指向类的指针"（如 Base*）
//   ③ 编译期静态检查：源与目标必须同属一个继承体系，判据 = 存在公共祖先（含 a == b）；
//      不满足即报 "Cannot dynamic_cast 'A*' to 'B*': unrelated class types"
//   ④ 成败取决于运行时实际类型，编译期不裁决 —— 这正是 dynamic_cast 与 static_cast 的
//      本质区别（static_cast 完全由静态类型推导）
// 用例  Dog dr; Animal* pa = &dr; Dog* pd = dynamic_cast<Dog*>(pa);   // Dog : Animal
//       ⇒ 有公共祖先 ⇒ 通过；结果类型 Dog*，真值留到运行期查 RTTI（_ZTI 符号）
// 结果类型：T*（指针类型）。
// ┌─ DEMO ─────────────────────────────────────────────────────────────────────
// │ 源码  Base* pb = &dr;      Derived* pd = dynamic_cast<Derived*>(pb);
// │ 日志  [resolve] 'pb' → Base*    (kind=Variable, stack@-128)
// │       [dynamic_cast] Base* → Derived*    (runtime RTTI check)
// │ 输出  Derived*（编译期只给出静态类型，真值留到运行期查 RTTI（_ZTI 符号））
// └────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferDynamicCast(DynamicCastExpr& expr) {
    // 1. 目标类必须已声明
    auto targetIt = m_classTypes.find(expr.targetClassName);
    if (targetIt == m_classTypes.end()) {
        error(std::format("Unknown class '{}' in dynamic_cast", expr.targetClassName),
            expr.location);
    }

    // 2. 源表达式必须是"指向类的指针"
    TypePtr srcType = inferType(expr.operand);
    if (!srcType || !srcType->isPointer()
        || !srcType->pointeeType || !srcType->pointeeType->isClass()) {
        error("dynamic_cast operand must be a pointer to a class",
            expr.location);
    }
    std::string srcClass = srcType->pointeeType->name;

    // 3. 静态可达性检查：真 C++ 只要求两类"同属一个继承体系"（[expr.dynamic.cast]：源与
    //    目标须关联，兄弟类互转也合法，成败交由运行时的实际类型裁决）⇒ 编译期判据是
    //    "存在公共祖先"（含自身等同：a == b）。
    //    hasCommonAncestor(a, b)：收集 a 的祖先集，沿 b 链上溯查找交集。
    auto hasCommonAncestor = [this](const std::string& a, const std::string& b) {
        // BFS 收集 a 的全部祖先（遍历所有 baseClassNames，不再只走 firstBase）
        std::set<std::string> anc;
        std::vector<std::string> work = {a};
        while (!work.empty()) {
            std::string cur = work.back(); work.pop_back();
            if (!anc.insert(cur).second) continue;  // 已访问
            auto declIt = m_classDecls.find(cur);
            if (declIt != m_classDecls.end()) {
                for (auto& bn : declIt->second->baseClassNames)
                    work.push_back(bn);
            }
        }
        // BFS 检查 b 的祖先是否有交集
        work.push_back(b);
        std::set<std::string> visited;
        while (!work.empty()) {
            std::string cur = work.back(); work.pop_back();
            if (!visited.insert(cur).second) continue;
            if (anc.count(cur)) return true;
            auto declIt = m_classDecls.find(cur);
            if (declIt != m_classDecls.end()) {
                for (auto& bn : declIt->second->baseClassNames)
                    work.push_back(bn);
            }
        }
        return false;
    };
    if (!hasCommonAncestor(expr.targetClassName, srcClass)) {
        error(std::format("Cannot dynamic_cast '{}*' to '{}*': unrelated class types",
            srcClass, expr.targetClassName), expr.location);
    }

    std::cout << std::format("{}[dynamic_cast] {}* → {}*    (runtime RTTI check)\n",
        inferIndent(), srcClass, expr.targetClassName);
    return Type::makePointer(targetIt->second);
}

// ─── 错误处理 ────────────────────────────────────────────────────────────────
// 统一错误出口：携带源码位置（行/列）抛出异常，由 main() 捕获打印。
// 教学级采用 fail-fast 策略：遇到第一个语义错误即终止，不做错误恢复/继续收集
// （clang 的 DiagnosticsEngine 支持跳过错误继续）。
[[noreturn]] void SemanticAnalyzer::error(const std::string& msg, SourceLocation loc) {
    throw std::runtime_error(
        std::format("[Semantic Error] {}: {}", loc.toString(), msg));
}

// ═══ 诊断可视化：--dump-hierarchy / --dump-layout ═══════════════════════════
// 由 main.cpp 根据命令行标志调用，正常编译路径不受影响。
// typeinfo 形态判断规则（Itanium ABI 三种形态）：
//   bases.empty()     → 'C' (__class_type_info,     无基类, 链终点)
//   bases.size() == 1 → 'S' (__si_class_type_info,  单继承, base 指针)
//   bases.size() > 1  → 'V' (__vmi_class_type_info, 多继承, base 数组)

static char typeinfoForm(const ClassLayout& layout) {
    if (layout.bases.empty()) return 'C';
    if (layout.bases.size() == 1) return 'S';
    return 'V';
}

static const char* typeinfoName(char form) {
    switch (form) {
        case 'C': return "__class_type_info";
        case 'S': return "__si_class_type_info";
        case 'V': return "__vmi_class_type_info";
        default:  return "?";
    }
}

// 递归打印 typeinfo 链：当前类 → base → base.base → ...
static void printRTTIChain(
    const std::string& className,
    const std::unordered_map<std::string, TypePtr>& classTypes,
    const std::string& indent,
    std::set<std::string>& visited)
{
    std::string mangled = std::format("_ZTI{}{}", className.length(), className);

    if (visited.count(className)) {
        std::cout << std::format("{}{} → (已访问，跳过)\n", indent, mangled);
        return;
    }
    visited.insert(className);

    auto it = classTypes.find(className);
    if (it == classTypes.end()) {
        std::cout << std::format("{}{} → (未知类)\n", indent, mangled);
        return;
    }

    auto& layout = it->second->classLayout;
    char form = typeinfoForm(layout);

    std::cout << std::format("{}{} ['{}' {}]", indent, mangled, form, typeinfoName(form));

    if (layout.bases.empty()) {
        std::cout << " → 终止\n";
        return;
    }

    if (layout.bases.size() == 1) {
        auto& base = layout.bases[0];
        std::cout << std::format(" ──base──→\n");
        printRTTIChain(base.baseClassName, classTypes, indent, visited);
    } else {
        std::cout << std::format(" (base_count={})\n", layout.bases.size());
        for (size_t i = 0; i < layout.bases.size(); ++i) {
            auto& base = layout.bases[i];
            bool last = (i + 1 == layout.bases.size());
            std::string branch = last ? "└── " : "├── ";
            std::string childIndent = indent + (last ? "    " : "│   ");
            std::cout << std::format("{}{}bases[{}] @offset={}: ",
                indent, branch, i, base.offset);
            printRTTIChain(base.baseClassName, classTypes, childIndent, visited);
        }
    }
}

void SemanticAnalyzer::dumpHierarchy(
    const std::unordered_map<std::string, TypePtr>& classTypes)
{
    if (classTypes.empty()) return;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  类层次结构图 (Class Hierarchy Diagram)                         ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // ── ① 继承树 ──
    std::cout << "\n  ┌─ 继承树 (Inheritance Tree) ──────────────────────────────────\n";

    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots;
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.bases.empty()) {
            roots.push_back(name);
        } else {
            for (auto& base : type->classLayout.bases)
                children[base.baseClassName].push_back(name);
        }
    }

    auto isPoly = [](const TypePtr& t) { return t->classLayout.hasVTable; };
    auto getSize = [](const TypePtr& t) { return t->classLayout.totalSize; };

    std::set<std::string> treeVisited;
    std::function<void(const std::string&, const std::string&, const std::string&)> dfs =
        [&](const std::string& cls, const std::string& prefix, const std::string& branch) {
            auto it = classTypes.find(cls);
            if (it == classTypes.end()) return;
            auto& t = it->second;
            bool dup = !treeVisited.insert(cls).second;
            if (dup) {
                std::cout << std::format("  │ {}{}{}  [{}{}B]  ↑ (已展开)\n",
                    prefix, branch, cls,
                    isPoly(t) ? "polymorphic, " : "",
                    getSize(t));
                return;
            }
            std::cout << std::format("  │ {}{}{}  [{}{}B]\n",
                prefix, branch, cls,
                isPoly(t) ? "polymorphic, " : "",
                getSize(t));
            auto ci = children.find(cls);
            if (ci == children.end()) return;
            auto& kids = ci->second;
            std::sort(kids.begin(), kids.end());
            for (size_t i = 0; i < kids.size(); ++i) {
                bool last = (i + 1 == kids.size());
                dfs(kids[i],
                    prefix + (branch.empty() ? "" : (last ? "    " : "│   ")),
                    last ? "└── " : "├── ");
            }
        };

    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        dfs(r, "", "");
    }
    std::cout << "  └───────────────────────────────────────────────────────────────\n";

    // ── ② 每个类的详情框：按类名字母顺序打印（确保每个类只打印一次）──
    std::vector<std::string> ordered;
    for (auto& [name, type] : classTypes) {
        ordered.push_back(name);
    }
    std::sort(ordered.begin(), ordered.end());

    for (auto& name : ordered) {
        auto it = classTypes.find(name);
        if (it == classTypes.end()) continue;
        auto& layout = it->second->classLayout;
        char form = typeinfoForm(layout);

        std::cout << std::format("\n  ┌─ {} ─────────────────────────────────────────────────\n", name);

        // typeinfo 形态
        std::cout << std::format("  │  typeinfo : {} ['{}'", typeinfoName(form), form);
        if (form == 'C') std::cout << " 无基类";
        else if (form == 'S') std::cout << " 单继承";
        else if (form == 'V') std::cout << " 多继承";
        std::cout << "]\n";

        // size
        std::cout << std::format("  │  size     : {} bytes\n", layout.totalSize);

        // 基类
        if (!layout.bases.empty()) {
            for (auto& base : layout.bases) {
                std::cout << std::format("  │  基类     : {} ({}{}, offset={})\n",
                    base.baseClassName,
                    base.isPrimary ? "主基类" : "次基类",
                    base.hasVTable ? ", 多态" : "",
                    base.offset);
            }
        }

        // 字段（区分继承 vs 自有）
        if (!layout.fields.empty()) {
            bool hasInherited = false, hasOwn = false;
            for (auto& f : layout.fields) {
                if (f.sourceClass.empty() || f.sourceClass == name) hasOwn = true;
                else hasInherited = true;
            }
            if (hasInherited) {
                std::cout << "  │  继承字段 :\n";
                for (auto& f : layout.fields) {
                    if (!f.sourceClass.empty() && f.sourceClass != name) {
                        std::cout << std::format("  │    +{:<4} {:<12} : {} ({})  ← {}\n",
                            f.offset, f.name,
                            f.type ? f.type->toString() : "?",
                            f.size, f.sourceClass);
                    }
                }
            }
            if (hasOwn) {
                std::cout << "  │  自有字段 :\n";
                for (auto& f : layout.fields) {
                    if (f.sourceClass.empty() || f.sourceClass == name) {
                        std::cout << std::format("  │    +{:<4} {:<12} : {} ({})\n",
                            f.offset, f.name,
                            f.type ? f.type->toString() : "?",
                            f.size);
                    }
                }
            }
        }

        // 虚函数
        if (layout.hasVTable && !layout.vtableEntries.empty()) {
            std::cout << "  │  虚函数   :\n";
            for (auto& entry : layout.vtableEntries) {
                std::cout << std::format("  │    [{}] {} {}",
                    entry.index, entry.mangledName,
                    entry.isOverridden ? "(override)" : "");
                if (entry.thunkAdjust != 0)
                    std::cout << std::format("  thunk={}", entry.thunkAdjust);
                std::cout << "\n";
            }
            std::cout << std::format("  │  RTTI     : {}\n", layout.rttiMangledName);
        }

        // RTTI 链
        std::cout << "  │  RTTI 链  :\n";
        std::set<std::string> visited;
        printRTTIChain(name, classTypes, "  │    ", visited);

        std::cout << "  └───────────────────────────────────────────────────────────────\n";
    }
}

void SemanticAnalyzer::dumpLayout(
    const std::unordered_map<std::string, TypePtr>& classTypes)
{
    if (classTypes.empty()) return;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  类内存布局详图 (Memory Layout Detail)                          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // 按继承顺序：先打印无基类的，再打印有基类的
    std::vector<std::string> ordered;
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.bases.empty())
            ordered.push_back(name);
    }
    for (auto& [name, type] : classTypes) {
        if (!type->classLayout.bases.empty())
            ordered.push_back(name);
    }
    std::sort(ordered.begin(), ordered.end());

    for (auto& name : ordered) {
        auto it = classTypes.find(name);
        if (it == classTypes.end()) continue;
        auto& layout = it->second->classLayout;

        std::cout << std::format("\n  ━━ {} ({} bytes) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n",
            name, layout.totalSize);

        // ── 第一层：对象内存 ──
        std::cout << std::format("  ┌─ {} 对象 ({}B) ─────────────────────────────────┐\n",
            name, layout.totalSize);
        if (layout.hasVTable) {
            std::cout << std::format("  │  +{:<3} _vptr ────────────────────────┐\n", 0);
        }
        for (auto& f : layout.fields) {
            std::string src = "";
            if (!f.sourceClass.empty() && f.sourceClass != name)
                src = std::format("  ← {}", f.sourceClass);
            std::cout << std::format("  │  +{:<3} {:<10} : {} ({}B){}\n",
                f.offset, f.name,
                f.type ? f.type->toString() : "?",
                f.size, src);
        }
        std::cout << "  └──────────────────────────────────────────────────────┘\n";

        if (!layout.hasVTable) continue;

        // ── 第二层：vtable ──
        std::string vtblName = std::format("_ZTV{}{}", name.length(), name);
        std::string rttiName = std::format("_ZTI{}{}", name.length(), name);
        std::cout << std::format("                                        │\n");
        std::cout << std::format("                                        ▼\n");
        std::cout << std::format("  ┌─ {} ──────────────────────────────────────────┐\n", vtblName);
        std::cout << std::format("  │  [-2]  offset-to-top = 0    ─→ 归顶: top = obj + 0\n");
        std::cout << std::format("  │  [-1]  typeinfo ptr ──────────┐  ─→ {} (RTTI)\n", rttiName);
        std::cout << std::format("  │  ── ↑ vptr 指向此处 ───────── │ ──\n");
        for (auto& entry : layout.vtableEntries) {
            std::cout << std::format("  │  [{:<2}]  {}{}\n",
                entry.index, entry.mangledName,
                entry.isOverridden ? "  (override)" : "");
        }
        std::cout << std::format("  └────────────────────────────── │ ──────────────────────────┘\n");

        // ── 第三层：typeinfo 链 ──
        std::cout << std::format("                                  │\n");
        std::cout << std::format("                                  ▼\n");

        char form = typeinfoForm(layout);
        std::string rttiBoxName = std::format("_ZTI{}{}", name.length(), name);
        std::cout << std::format("  ┌─ {} ({}) ────────────────────────────────────┐\n",
            rttiBoxName, form);
        std::cout << std::format("  │  +0   vptr  → 形态标记 '{}'\n", form);
        std::cout << std::format("  │  +8   name  → \"{}\" (mangled: {}{})\n",
            name, name.length(), name);

        if (form == 'V') {
            // VMI 类型：只列出 bases 数组，不递归展开（避免深层嵌套太复杂）
            std::cout << std::format("  │  +16  base_count = {}\n", layout.bases.size());
            for (size_t i = 0; i < layout.bases.size(); ++i) {
                auto& base = layout.bases[i];
                std::cout << std::format("  │  bases[{}]: {} @offset={} [{}]\n",
                    i, base.baseClassName, base.offset,
                    base.isPrimary ? "primary" : "secondary");
            }
            std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
        } else {
            // 'S'（单继承）或 'C'（无基类）：沿主基类链递归展开
            std::set<std::string> layoutVisited;
            layoutVisited.insert(name);
            std::string currentName = name;
            const ClassLayout* currentLayout = &layout;

            while (true) {
                if (currentLayout->bases.empty()) {
                    // 'C' 类型：链终止
                    std::cout << std::format("  │  (无 +16 字段 — 链终止)\n");
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }
                // 'S' 类型：有 +16 base 指针
                std::cout << std::format("  │  +16  base  ──────────────────────┐\n");
                std::cout << std::format("  └───────────────────────────────────── │ ─────┘\n");
                std::cout << std::format("                                        │\n");
                std::cout << std::format("                                        ▼\n");

                auto& base = currentLayout->bases[0];
                auto baseIt = classTypes.find(base.baseClassName);
                if (baseIt == classTypes.end()) {
                    std::cout << std::format("  ┌─ _ZTI{}{} (?) ──────────────────────────────────┐\n",
                        base.baseClassName.length(), base.baseClassName);
                    std::cout << std::format("  │  (基类不在当前翻译单元中)\n");
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }
                if (!layoutVisited.insert(base.baseClassName).second) {
                    std::cout << std::format("  ┌─ _ZTI{}{} → (已访问，跳过) ──────────────────────┐\n",
                        base.baseClassName.length(), base.baseClassName);
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }

                auto& baseLayout = baseIt->second->classLayout;
                char baseForm = typeinfoForm(baseLayout);
                std::string baseRttiName = std::format("_ZTI{}{}",
                    base.baseClassName.length(), base.baseClassName);
                std::cout << std::format("  ┌─ {} ({}) ────────────────────────────────────┐\n",
                    baseRttiName, baseForm);
                std::cout << std::format("  │  +0   vptr  → 形态标记 '{}'\n", baseForm);
                std::cout << std::format("  │  +8   name  → \"{}\"\n", base.baseClassName);

                currentName = base.baseClassName;
                currentLayout = &baseLayout;
            }
        }
    }
}

} // namespace minicc
