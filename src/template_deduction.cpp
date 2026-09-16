// =============================================================================
// 模板实参推导引擎实现 —— S2 逐对推导 / S3 显式实参 / S4 不可推导上下文
// =============================================================================
// 算法骨架（对应 [temp.deduct.call]）：
//   1. 显式实参占前缀，直接进替换表（S3）
//   2. 逐 (P, A) 配对：
//        P = const X      → 剥 P 顶层 const，继续
//        P = T&           → A 必须左值，对 P 内层继续
//        P = T&&          → 万能引用：A 左值 ⇒ T := A&；A 右值 ⇒ 对内层继续
//        P = T*           → A 必须指针，对 pointee 继续
//        P = T（裸参数）  → 值传递调整（剥 A 顶层引用/const）后绑定
//        P 非依赖         → 要求 P == A（恒等）
//   3. 绑定一致性：同一 T 两次绑定类型必须相等，否则冲突（S2）
//   4. 收尾：仍有未绑定参数 → 不可推导上下文错误（S4）
// =============================================================================
//
// 对应 C++ 标准章节：
//   [temp.deduct]        推导总则             [temp.deduct.call]  逐对 P/A 的调整规则
//   [temp.deduct.type]   类型等价/不可推导     [temp.arg.explicit] 显式实参（S3）
// 对照 clang：lib/Sema/SemaTemplateDeduction.cpp
//   Sema::DeduceTemplateArguments                      —— deduce() 入口
//   TemplateDeductionCallback::Deduce                  —— deducePair() 逐对合一
//   DeduceTemplateArguments(…, ExplicitTemplateArgumentList) —— S3 显式前缀
//
// 推导 = 合一（unification）全景（以 twice(3) 为例）：
//   蓝图: template<typename T> void twice(T x) { ... }
//
//        deduce()
//          │  ① S3 显式实参占前缀（本例无）
//          │  ② S2 逐 (P, A) 配对合一：
//          │       P = T ── A = int  ⇒ bind: T := int ✓（写入替换表）
//          │       （同一 T 再次出现时只允许"一致 ✓"，否则冲突 ✗）
//          │  ③ S4 收尾：检查每个模板参数都已被绑定
//          ▼
//   替换表 { T := int } ──交给 S5──▶ instantiateFunction → twice<int>
//
// trace 日志含义（逐对打印，供教学观测）：
//   [deduction] ▶ …                 推导开始（模板名、模板参数表、实参个数）
//   P=… A=… ⇒ T := int              一对 P/A 合一成功，产生新绑定
//   P=… A=… ⇒ T := int（一致 ✓）     同一参数再次绑定且类型一致
//   [deduction]   ✗ …               本对失败（冲突/左值性/指针失配/恒等失败）
//   [deduction] ◀ 推导成功: <T=int> 全部参数绑定完毕，产出最终模板实参
// =============================================================================

#include "template_deduction.h"
#include "sfinae.h"
#include <format>
#include <iostream>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// 偏特化匹配（[temp.class.spec.match]）—— 复用同一套合一算法
// ─────────────────────────────────────────────────────────────────────────────
// 【做什么】用「使用点的实参类型 A」去推导「偏特化模式 P」中的模板参数，
//           逐位合一；全位成功才算该偏特化匹配，否则换下一个偏特化。
//
// 【为什么能复用 deducePair】
//   函数模板实参推导与偏特化匹配在算法上是**同一件事**：
//     · 函数模板：用调用实参 A 推导形参模式 P（如 T* 配 int* ⇒ T := int）
//     · 偏特化  ：用使用点实参 A 推导特化模式 P（如 Box<T*,T> 对 Box<int*,int>）
//   两者都是"受限合一"（把模式里的未知量解出来）。
//   对照 clang：二者也确实共用 DeduceTemplateArguments 一族入口
//   （SemaTemplateDeduction.cpp），只是传入的参数种类不同。
//
// 【demo】模式 [T*, T] 对实参 [double*, double]：
//     P=T*   A=double*  ⇒ T := double ✓
//     P=T    A=double   ⇒ T := double（一致 ✓）
//   ⇒ 匹配成功，替换表 {T := double} —— 交给 instantiate 替换特化体
//
// 【demo 失败】模式 [T*, T] 对实参 [int, int]：
//     P=T*   A=int      ⇒ 指针结构失配 ✗ ⇒ 该偏特化不匹配
//
// 【职责边界】本函数只回答"这一条偏特化匹配不匹配"（逐条试，不含比较）。
//   "多条都匹配时谁胜"不在这里 —— 由调用方 SemanticAnalyzer::selectClassTemplate
//   用 dominance 循环裁决 [temp.class.order]（semantic_analyzer.cpp:2994 一带，
//   比较器见同文件 :223 classSpecAtLeastAsSpecialized），见 docs/learn/21。
//   ※ 旧注释写"本项目不做偏序裁决，取第一个成功的" —— 那是主线 H 之前的实情，
//     现已实现，注释与代码不符，2026-09-14 更正。
//
// 返回：全部位匹配成功 → true，subst 填好可交付实例化；任一位失败 → false
bool TemplateDeducer::matchPattern(
    const std::vector<TypePtr>& pattern,
    const std::vector<TypePtr>& args,
    const std::vector<std::string>& paramNames,
    std::unordered_map<std::string, TypePtr>& subst,
    std::string& failReason) {

    if (pattern.size() != args.size()) {
        failReason = std::format("pattern has {} argument(s) but {} given",
                                 pattern.size(), args.size());
        return false;
    }

    DeductionResult out;
    for (size_t i = 0; i < pattern.size(); i++) {
        // ── SFINAE 的关键一步：先把模式位"归约"成具体类型 ──
        // void_t<...> / decltype(...) 这类模式位本身不是类型，而是待求值的表达式。
        // 归约要用的替换表 = 前面各位【已经推出来的】绑定（左边第 1 位刚推出 T := ...，
        // 右边第 2 位的 void_t 就用得上它）—— 这正是从左到右逐位匹配的价值。
        // 求值失败会抛 SubstitutionFailure，由下面的 catch 接住 → 判定为"不匹配"。
        // ★ SFINAE 吸收点 ①（全项目三处之一，见 include/sfinae.h 的收口点一览）
        //   归约失败 = 这个偏特化不成立 → 移出候选集，交给下一个偏特化。
        TypePtr P;
        std::string substReason;
        if (!Sfinae::attempt(
                std::format("偏特化模式第 {} 位 '{}'", i + 1, pattern[i]->toString()),
                [&] { P = reducePattern(pattern[i], subst); },
                &substReason)) {
            failReason = std::format("substitution failed in pattern position {}: {}",
                                     i + 1, substReason);
            return false;
        }

        // ── 引用的结构匹配前置检查 ★ ──
        // 【为什么必须单独查】类模板偏特化的匹配是【结构等价】，
        //   不是函数调用那套"左值可绑定"（[temp.deduct.call]）。
        //   deducePair 里 P=T& 的分支只看 argIsLValue，会把 A=int 也放行，
        //   于是 `Probe<T&>` 错误地匹配上了 `Probe<int>`。
        //   标准依据：[temp.class.spec.match]/2 —— 偏特化匹配要求模板实参
        //   能从实参表【推导】出来，且类型结构必须对应；
        //   引用位只能由引用实参填充（clang 同样在
        //   DeduceTemplateArguments 的 TDK_Reference 分支做这个判定）。
        // demo：Probe<T&> 对 Probe<int>  → 模式要引用、实参不是 → 不匹配 ✓
        //       Probe<T&> 对 Probe<int&> → 两边都是左值引用 → 继续推导 ✓
        {
            // 剥掉顶层 const 再判引用性（const T& 的 const 是外层修饰）
            TypePtr pc = P;
            while (pc && pc->isConst() && pc->innerType) pc = pc->innerType;

            auto refKind = [](const TypePtr& t) -> int {
                if (!t) return 0;
                if (t->isLValueReference())  return 1;
                if (t->isRValueReference())  return 2;
                return 0;
            };
            int pr = refKind(pc);
            int ar = refKind(args[i]);
            if (pr != 0 && pr != ar) {
                failReason = std::format(
                    "reference structure mismatch: pattern '{}' requires {} but "
                    "argument '{}' is {}",
                    P->toString(),
                    pr == 1 ? "an lvalue reference" : "an rvalue reference",
                    args[i] ? args[i]->toString() : "?",
                    ar == 0 ? "not a reference"
                            : (ar == 1 ? "an lvalue reference" : "an rvalue reference"));
                std::cout << std::format("  [deduction]   ✗ 引用结构不匹配：{}\n", failReason);
                return false;
            }
        }

        // argIsLValue 传 true：类模板特化模式里的引用（Box<T&>）按"可绑定"处理，
        // 不做函数调用那套左值/右值绑定检查（那是 [temp.deduct.call] 的规则）。
        // 注：上面已单独把关"引用必须对引用"，故此处继续传 true 是安全的。
        if (!deducePair(P, args[i], /*argIsLValue=*/true, paramNames, subst, out)) {
            failReason = out.failureReason;
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// desugarAlias —— 模式位里的别名模板 id 解糖（[temp.alias]/1）
// ─────────────────────────────────────────────────────────────────────────────
// 【为什么推导器也要管别名】推导做的是"模式 P ↔ 实参 A"的合一，比的是【名字】。
//   而别名模板的整个意义就是"同一个类型的另一个名字"：
//     template<class T> using Vec = MyPtr<T>;
//     template<class T> Vec<T> pass(Vec<T> v);   // P 侧写的是 Vec<T>
//     pass(v);                                    // A 侧是 MyPtr<int>
//   同一次调用的两侧各自走了不同的解糖路径（A 侧在调用点由 Sema 解糖），
//   若 P 侧不解，合一会直接判"P 名字 ≠ A 名字"→ 候选被静默剔除。
// 【为什么是"解糖"不是"实例化"】T 还是形参，`MyPtr<T>` 原样保持依赖，
//   正好交给逐位合一去绑定 —— 这一步只剥名字，不产生任何具体类型。
// 【demo】desugarAlias(Vec<T>, {})            → MyPtr<T>（T 未绑，保持依赖）
//         desugarAlias(Vec<T>, {T := int})   → MyPtr<int>
//         desugarAlias(Box<int>, {...})      → Box<int>（原名返回，未变）
// 对照 clang：DeduceTemplateArguments 之前在 CanonicalType 层面比较，
//   TypeAliasTemplateDecl 在这一步已被 getCanonicalType 完全剥掉。
TypePtr TemplateDeducer::desugarAlias(
    const TypePtr& P,
    const std::unordered_map<std::string, TypePtr>& subst) {

    if (!P || !m_aliasResolver) return P;
    if (!P->isClass() || P->templateArgs.empty()) return P;
    if (!m_aliasResolver->isAliasTemplate(P->name)) return P;

    // 推导器的替换表是裸 TypePtr，替换引擎要 tagged —— 在这条缝上做转换
    TemplateInstantiator::TypeSubstitution tagged;
    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);

    TemplateInstantiator inst;
    inst.setDecltypeEvaluator(m_decltypeEval);
    inst.setMemberTypeResolver(m_memberResolver);
    inst.setAliasTemplateResolver(m_aliasResolver);

    TypePtr desugared = inst.substituteType(P, tagged);
    return desugared ? desugared : P;
}

// reducePattern —— 把模式位归约成具体类型（void_t / decltype 的求值点）
// ─────────────────────────────────────────────────────────────────────────────
// 【理论】void_t 探测惯例（[temp.deduct]/8，CWG 1558）：
//   template<class...> using void_t = void;
//   void_t<E...> 的语义是"若 E... 全部合法则等价于 void，否则替换失败"。
//   别名模板的实参替换失败【也算】SFINAE（这是 CWG 1558 的修正）——
//   正是靠这一条，is_range 的偏特化才能"不命中就静默退化"。
//
// 【本实现的简化】minicc 不支持别名模板（`using X = Y` 无解析），
//   故 std::void_t 以【编译器内建】形式提供：名字命中即按上述语义处理，
//   不走别名模板机制。详见 SemanticAnalyzer::registerBuiltins 的说明。
//
// 【demo】reducePattern(void_t<decltype(declval<T>().begin())>, {T→vector<int>})
//           ① 对每个实参做替换：decltype(declval<T>().begin())
//                                → decltype(declval<vector<int>>().begin())
//           ② 替换触发的 decltype 求值 → int（合法）
//           ③ 全部合法 ⇒ 归约为 void
//         若 {T→int} ⇒ ② 抛 SubstitutionFailure ⇒ 整个模式位不匹配
TypePtr TemplateDeducer::reducePattern(
    const TypePtr& P,
    const std::unordered_map<std::string, TypePtr>& subst) {

    if (!P) return P;

    // ── 别名模板 id：先解糖成底层类型，再拿底层类型去匹配（[temp.alias]/1）──
    // 【为什么必须在【模式】这一侧也解糖】推导是"模式 P ↔ 实参 A"的合一，
    //   而 `Vec<T>` 这个名字与 `MyPtr<int>` 根本不同名 —— 不解糖就永远合不上：
    //     template<class T> Vec<T> pass(Vec<T> v);   // P = Vec<T>
    //     pass(v);  // A = MyPtr<int>（Vec<int> 在调用点已解糖成它）
    //   解糖后 P = MyPtr<T>，合一算法立刻推出 T := int。
    //   【关键】解糖用的是"解糖而非实例化"：T 仍是形参，`MyPtr<T>` 保持依赖，
    //   正好交给下面逐位的合一去绑定。
    // 对照 clang：DeduceTemplateArguments 之前先把 P 与 A 都做
    //   getCanonicalType（别名在这一步被剥掉），故别名不参与合一。
    if (TypePtr desugared = desugarAlias(P, subst); desugared != P) {
        std::cout << std::format("  [deduce:alias] 模式位 {} 解糖 ⇒ {}\n",
            P->toString(), desugared->toString());
        // 递归：别名可能指向另一个别名，逐层剥到底
        return reducePattern(desugared, subst);
    }

    // ── void_t<...>：归约为 void（前提是各实参替换后都合法）──
    bool isVoidT = P->isClass() &&
                   (P->name == "std::void_t" || P->name == "void_t");
    if (!isVoidT) return P;

    if (P->templateArgs.empty()) {
        // 空实参表的 void_t<> 直接合法 → void
        Sfinae::probeNoArgs();
        return Type::makeVoid();
    }

    if (!m_decltypeEval) {
        Sfinae::probeNoEvaluator();
        return P;
    }

    // 替换表从 deducer 的 Subst（名字→TypePtr）转成 instantiator 的 tagged 表
    TemplateInstantiator::TypeSubstitution tagged;
    for (auto& [k, v] : subst) tagged[k] = TemplateArg::ofType(v);

    TemplateInstantiator inst;
    inst.setDecltypeEvaluator(m_decltypeEval);
    inst.setMemberTypeResolver(m_memberResolver);
    inst.setAliasTemplateResolver(m_aliasResolver);

    Sfinae::probeBegin(P->templateArgs.size());
    for (size_t i = 0; i < P->templateArgs.size(); i++) {
        if (!P->templateArgs[i].isType() || !P->templateArgs[i].type) continue;
        // ★ 关键：substituteType 内部遇到 decltype 会真的求值；
        //   表达式不合法则抛 SubstitutionFailure，向上冒泡给 matchPattern。
        TypePtr t = inst.substituteType(P->templateArgs[i].type, tagged);
        Sfinae::probeStep(i + 1, t ? t->toString() : "?");
    }
    Sfinae::probeAllOk();
    return Type::makeVoid();
}

// 【做什么】取出类型节点对应的"模板参数名"，作为替换表的键
// 【demo】TemplateParam("T") → "T"；Class("T")（parseType 的产物）→ "T"；其余 → "?"
std::string TemplateDeducer::paramKey(const TypePtr& t) const {
    if (!t) return "?";
    if (t->isTemplateParam()) return t->templateParamName;
    if (t->isClass()) return t->name;
    return "?";
}

// 【做什么】判定类型节点是否是"可绑定的变量"（本次模板参数表中的某个名字）
// 【理论】合一算法的前提：区分模式中的"变量"与"常量结构"——
//         变量才进 bind()，常量结构走恒等比较。
// 【注意】要同时认 TemplateParam("T") 与 Class("T") 两种形态：
//         parseType 尚不知道 T 是模板参数，会先建成 Class 节点（见 isTemplateParamName 头注）。
bool TemplateDeducer::isTemplateParamName(const TypePtr& t,
                                          const std::vector<std::string>& paramNames) const {
    if (!t) return false;
    if (!t->isTemplateParam() && !t->isClass()) return false;
    std::string n = t->isTemplateParam() ? t->templateParamName : t->name;
    for (auto& p : paramNames) if (p == n) return true;
    return false;
}

// 【做什么】把一次绑定 paramName := type 写入替换表 subst；
//           若 paramName 已有绑定，则要求新旧类型相等（一致性检查）。
// 【理论】合一算法的 unify(变量, 类型) 步骤：变量只能被绑定一次，
//         重复绑定必须一致，否则两个子树无法合一，整次推导失败
//         （对应 clang 诊断 "conflicting types for deduction of template parameter"）。
// 【demo】max(x:int, y:int)
//   第一次 bind(T, int)：入表，trace 记 "T := int"
//   第二次 bind(T, int)：查表 equals ✓，trace 记 "T := int（一致 ✓）"
//   若第二次 bind(T, double)：查表不等 → failureReason 记冲突，返回 false
bool TemplateDeducer::bind(const std::string& paramName, const TypePtr& type,
                           Subst& subst, DeductionResult& out,
                           const std::string& patternStr, const std::string& argStr) {
    auto it = subst.find(paramName);
    if (it == subst.end()) {
        subst[paramName] = type;
        std::string msg = std::format("{} := {}", paramName, type->toString());
        out.trace.push_back({patternStr, argStr, msg, true});
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ {}\n",
            patternStr, argStr, msg);
        return true;
    }
    if (it->second->equals(type)) {
        std::string msg = std::format("{} := {}（一致 ✓）", paramName, type->toString());
        out.trace.push_back({patternStr, argStr, msg, true});
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ {}\n",
            patternStr, argStr, msg);
        return true;
    }
    out.failureReason = std::format(
        "conflicting types for deduction of '{}': previously deduced '{}', now '{}'",
        paramName, it->second->toString(), type->toString());
    out.trace.push_back({patternStr, argStr, out.failureReason, false});
    std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
    return false;
}

// 【做什么】单个 (P, A) 配对的结构化合一：自外向内递归剥壳，分支见下方各段
// 【理论】对应 [temp.deduct.call] / [temp.deduct.type] 的调整规则：
//   P=const X → 剥顶层 const      P=T&  → 左值检查后深入内层
//   P=T&&     → 万能引用折叠/深入  P=T*  → 对 pointee 深入
//   P=T       → 值传递调整后绑定   P非依赖 → 恒等比较
// 【demo】f(const T& x)，实参为 const int& 左值：
//   (Const(LRef(T)), int&) → 剥const → (LRef(T), int&) → 左值✓深入 → (T, int)
//   → 裸 T 绑定 ⇒ T := int ✓
bool TemplateDeducer::deducePair(const TypePtr& P, const TypePtr& A, bool argIsLValue,
                                 const std::vector<std::string>& paramNames,
                                 Subst& subst, DeductionResult& out) {
    if (!P || !A) {
        out.failureReason = "internal: null type in deduction";
        return false;
    }
    std::string pStr = P->toString();
    std::string aStr = A->toString();

    // ── 别名模板 id 先解糖（[temp.alias]/1）──
    // 放在【每一个递归层】而不只是最外层，是因为别名常被引用/const 包着：
    //   `const Vec<T>&`  →  剥 const、剥引用之后才露出 `Vec<T>`。
    // 若只在入口解一次，这两层壳里的别名就漏网了。
    // desugarAlias 对非别名是"查一下名字就返回"，开销可以忽略。
    // 对照 clang：这一步等价于 clang 在比较前对两侧取 getCanonicalType。
    if (TypePtr desugared = desugarAlias(P, subst); desugared != P) {
        std::cout << std::format("  [deduction]   P={:<12} ⇒ 别名解糖为 {}\n",
            pStr, desugared->toString());
        return deducePair(desugared, A, argIsLValue, paramNames, subst, out);
    }

    // ── P = const X：剥 P 的顶层 const ──
    // （本项目 parseType 把 const T& 解析成 Const(LRef(T))，这里剥掉外层 const 后走引用分支）
    // 标准依据：顶层 cv 限定不影响推导（[temp.deduct.type]：顶层 cv 被忽略）；
    // 若 const 在内层（如 const T*）则不会被这里剥掉，随递归继续参与推导。
    if (P->isConst()) {
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 剥顶层 const\n", pStr, aStr);
        return deducePair(P->innerType, A, argIsLValue, paramNames, subst, out);
    }

    // ── P = T&：左值引用参数，实参必须左值 ──
    // 标准依据：[temp.deduct.call]——P 是引用类型时，以被引用类型参与推导；
    // 右值无法绑定到非 const 左值引用（与普通引用绑定语义一致，此处提前拒绝）。
    if (P->isLValueReference()) {
        if (!argIsLValue) {
            out.failureReason = std::format(
                "cannot bind lvalue reference '{}' to rvalue argument '{}'", pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }
        return deducePair(P->referencedType, A, argIsLValue, paramNames, subst, out);
    }

    // ── P = T&&：万能引用（forwarding reference）──
    // 左值实参 ⇒ T := A&（实例化时 T&& 折叠为 A&）；右值实参 ⇒ T := A
    // 标准依据：[temp.deduct.call]/3——P 为"模板参数的右值引用"且实参为左值时，
    // 推导把 A 按左值引用处理（即 T := A&）；这是 std::forward 完美转发的根基。
    if (P->isRValueReference()) {
        TypePtr inner = P->referencedType;
        if (isTemplateParamName(inner, paramNames) && argIsLValue) {
            std::cout << std::format(
                "  [deduction]   P={:<12} A={:<12} (lvalue) ⇒ 万能引用折叠路径\n", pStr, aStr);
            return bind(paramKey(inner), Type::makeLValueReference(A),
                        subst, out, pStr, aStr);
        }
        return deducePair(inner, A, argIsLValue, paramNames, subst, out);
    }

    // ── P = T*：实参必须是指针，递归 pointee ──
    // 标准依据：[temp.deduct.type] 结构化等价——复合类型逐层拆解后递归合一
    // （demo：P=T*、A=int* ⇒ 递归到 (T, int) ⇒ T := int ✓）
    if (P->isPointer()) {
        if (!A->isPointer()) {
            out.failureReason = std::format(
                "parameter pattern '{}' expects pointer argument, got '{}'", pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }
        return deducePair(P->pointeeType, A->pointeeType, argIsLValue, paramNames, subst, out);
    }

    // ── P = T（裸模板参数，值传递）──
    // 值传递调整：实参的顶层引用/const 不参与推导（[temp.deduct.call]）
    // demo：identity(x) 中 x 是 const int& → 剥顶层引用、再剥顶层 const → T := int
    if (isTemplateParamName(P, paramNames)) {
        TypePtr adjusted = A;
        if (adjusted->isReference()) adjusted = adjusted->referencedType; // ① 剥顶层引用：值传递拷贝的是对象本身
        if (adjusted->isConst())     adjusted = adjusted->innerType;      // ② 剥顶层 const：拷贝后副本可写，顶层 cv 丢失
        if (adjusted != A) {
            std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 值传递调整 A'={}\n",
                pStr, aStr, adjusted->toString());
        }
        return bind(paramKey(P), adjusted, subst, out, pStr, aStr);
    }

    // ── P = C<T1...>：类模板 id 的结构合一 ──
    // 标准依据：[temp.deduct.type]/8 的 "T<T1, T2, ..., Tn>" 情形 ——
    //   同类模板、实参个数相同，则逐位递归合一。
    // 【demo】P=MyPtr<T>、A=MyPtr<int>（实例类型 MyPtr_int）
    //         第 1 位：T ↔ int ⇒ T := int ✓
    // 【关键】A 侧是【实例化后】的类型，名字已被改写成 MyPtr_int，
    //   靠名字反推是错的（清洗规则不是单射）；必须查实例记下的出身
    //   （templateOriginName / templateOriginArgs，见 getOrInstantiateClass）。
    // 【别名模板为什么依赖这条】`Vec<T>` 解糖成 `MyPtr<T>` 之后，
    //   能不能推出 T 全看这一分支 —— 没有它，别名只能当"写出来好看"
    //   的糖，一放进函数模板形参就废。
    // 对照 clang：DeduceTemplateArguments 对 TemplateSpecializationType
    //   递归处理 TemplateArgumentList（clang 的实例类型仍带实参表，不需出身字段）。
    if (P->isClass() && !P->templateArgs.empty()) {
        bool sameTemplate =
            A->isClass() &&
            (A->isTemplateInstance() ? A->templateOriginName == P->name
                                     : A->name == P->name);
        const std::vector<TemplateArg>& aArgs =
            A->isTemplateInstance() ? A->templateOriginArgs : A->templateArgs;

        if (!sameTemplate || aArgs.size() != P->templateArgs.size()) {
            out.failureReason = std::format(
                "class template pattern '{}' does not match argument '{}'",
                pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }

        std::cout << std::format(
            "  [deduction]   P={:<12} A={:<12} ⇒ 同类模板 {}（出身 {}），逐位合一\n",
            pStr, aStr, P->name, A->templateOriginName);

        for (size_t i = 0; i < P->templateArgs.size(); i++) {
            const TemplateArg& pa = P->templateArgs[i];
            const TemplateArg& aa = aArgs[i];

            // 模式位是类型 ⇒ 实参位也必须是类型，递归合一
            if (pa.isType() && pa.type) {
                if (!aa.isType() || !aa.type) {
                    out.failureReason = std::format(
                        "template argument {} of '{}': pattern expects a type, "
                        "argument is a value '{}'", i + 1, P->name, aa.toString());
                    out.trace.push_back({pStr, aStr, out.failureReason, false});
                    return false;
                }
                if (!deducePair(pa.type, aa.type, argIsLValue, paramNames, subst, out))
                    return false;
                continue;
            }

            // 模式位是值（NTTP）⇒ 要求逐位相等（教学简化：
            // 不做 `Buf<N>` 从值反推 N 的算式推导，见 docs/learn/18 的边界说明）
            if (pa.toString() != aa.toString()) {
                out.failureReason = std::format(
                    "non-type template argument {} of '{}' mismatches: pattern "
                    "'{}' vs argument '{}'", i + 1, P->name, pa.toString(), aa.toString());
                out.trace.push_back({pStr, aStr, out.failureReason, false});
                return false;
            }
        }
        out.trace.push_back({pStr, aStr, "类模板结构合一 ✓", true});
        return true;
    }

    // ── P 非依赖：要求恒等 ──
    // 标准依据：[temp.deduct.type]——P 中不含模板参数的部分必须与 A 结构一致，
    // 否则该候选不可行（在重载决议 S6 中被淘汰）
    if (P->equals(A)) {
        out.trace.push_back({pStr, aStr, "恒等 ✓", true});
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 恒等 ✓\n", pStr, aStr);
        return true;
    }
    out.failureReason = std::format(
        "no match for non-dependent parameter: P='{}' vs A='{}'", pStr, aStr);
    out.trace.push_back({pStr, aStr, out.failureReason, false});
    std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
    return false;
}

// 【做什么】一次函数模板调用的完整推导流程：S3 显式前缀 → S2 逐对合一 → S4 收尾检查
// 【理论】合一算法主循环：先注入已知绑定（显式实参），再对每对 (P, A) 递归合一，
//         最后验证所有变量均已绑定；任一步失败即返回。
// 【demo】twice(3)：S3 无显式实参 → S2 P=T, A=int ⇒ T := int ✓ → S4 全绑定 → success
// 【demo】cast<int>(1.0)：S3 显式 T := int 直接进表 → S2 对非依赖 P=int 做恒等检查
// 【失败路径】实参个数不符 / 某对 P-A 合一失败 / 存在不可推导参数 → success=false
//            （失败的候选由重载决议 S6 丢弃，全部失败才报 "no matching function"）
DeductionResult TemplateDeducer::deduce(
    const TemplateDeclPtr& tmpl,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::vector<TypePtr>& explicitArgs) {

    DeductionResult result;
    Subst subst;
    auto& func   = tmpl->funcTemplate;
    auto& params = tmpl->typeParams;

    std::string paramList;
    for (size_t i = 0; i < params.size(); i++) {
        if (i > 0) paramList += ", ";
        paramList += params[i];
    }
    std::cout << std::format("  [deduction] ▶ {} — 模板参数 <{}>，实参 {} 个\n",
        func->name, paramList, argTypes.size());

    // ── S3：显式实参占模板参数表前缀 ──
    // [temp.arg.explicit]：显式实参按位置绑定模板参数表的前缀，
    // 剩余参数继续靠调用推导补全（如 make<int>(3.14) 只显式给第一个）
    for (size_t i = 0; i < explicitArgs.size() && i < params.size(); i++) {
        subst[params[i]] = explicitArgs[i];
        std::cout << std::format("  [deduction]   显式给定: {} := {}\n",
            params[i], explicitArgs[i]->toString());
    }
    if (!explicitArgs.empty()) {
        std::cout << std::format("  [deduction]   ── 显式 {} 个 / 推导补全剩余 {} 个 ──\n",
            explicitArgs.size(),
            params.size() > explicitArgs.size() ? params.size() - explicitArgs.size() : 0);
    }

    // ── 函数参数个数检查 ──
    // 推导前先做形参/实参个数匹配（[expr.call]），不符则该候选直接出局
    if (argTypes.size() != func->parameters.size()) {
        result.failureReason = std::format(
            "argument count mismatch: {} expects {}, got {}",
            func->name, func->parameters.size(), argTypes.size());
        std::cout << std::format("  [deduction]   ✗ {}\n", result.failureReason);
        return result;
    }

    // ── S2：逐对 P/A 推导 ──
    // [temp.deduct.call]：对每个形参类型模式 P_i 与实参类型 A_i 递归合一，
    // 任一对失败立即返回（failureReason 已由 deducePair 填好）
    for (size_t i = 0; i < func->parameters.size(); i++) {
        // 形参模式里若写着别名模板 id（`Vec<T>`），先解糖成底层类型再合一：
        // 实参那侧（`MyPtr<int>`）早已在调用点解糖，两侧不同名就永远合不上。
        TypePtr P = desugarAlias(func->parameters[i].type, subst);
        if (P != func->parameters[i].type) {
            std::cout << std::format("  [deduction]   形参位 {} 解糖 ⇒ {}\n",
                func->parameters[i].type->toString(), P->toString());
        }
        if (!deducePair(P, argTypes[i], argIsLValue[i], params, subst, result)) {
            return result; // failureReason 已置
        }
    }

    // ── S4：不可推导上下文 —— 走完实参仍有未绑定参数 ──
    // [temp.deduct.type]：只出现在返回类型等"不可推导位置"的参数无法从调用推出，
    // 必须显式给定（如 make<T>()）；诊断文案会提示用户写出显式实参
    for (auto& p : params) {
        if (!subst.count(p)) {
            result.failureReason = std::format(
                "non-deduced context: template parameter '{}' cannot be deduced "
                "(only appears in non-deduced positions such as the return type; "
                "specify it explicitly, e.g. {}<...>())", p, func->name);
            std::cout << std::format("  [deduction]   ✗ {}\n", result.failureReason);
            return result;
        }
    }

    // ── 成功：按模板参数表顺序输出最终实参（显式 + 推导补全），交给 S5 实例化 ──
    result.success = true;
    std::string final;
    for (auto& p : params) {
        result.deducedArgs.push_back(subst[p]);
        if (!final.empty()) final += ", ";
        final += p + "=" + subst[p]->toString();
    }
    std::cout << std::format("  [deduction] ◀ 推导成功: <{}>\n", final);
    return result;
}

} // namespace minicc
