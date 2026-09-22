// =============================================================================
// 模板实参推导引擎 —— S2 逐对推导 / S3 显式实参 / S4 不可推导上下文
// （理论见 docs/learn/02、03、04）
// =============================================================================
// 扫到哪种 P ⇒ 走哪条分支 ⇒ 结果（[temp.deduct.call]）：
//   P=const X   ⇒ 剥顶层 const 后递归          │ P=T&   ⇒ 实参须左值，否则 ✗
//   P=T&&       ⇒ 左值实参 ⇒ T := A&（万能引用）│ P=T*   ⇒ 实参须指针，递归 pointee
//   P=T（裸参数）⇒ 值传递调整后绑定              │ P=C<T1..> ⇒ 同类模板同元数 ⇒ 逐位合一
//   P 非依赖    ⇒ 要求 P == A（恒等）
// 三阶段：S3 显式实参占前缀 → S2 逐 (P,A) 配对合一（绑定须一致）→ S4 收尾
//   （仍有未绑定参数 ⇒ 不可推导上下文错）。
//
// 逐对 trace（twice(3)）：S3 无显式 ⇒ S2 P=T A=int ⇒ T := int ✓ ⇒ S4 全绑定
//   ⇒ { T := int } ──交 S5──▶ twice<int>
// 实际日志（逐对打印，供教学观测）：
//   [deduction] ▶ twice — 模板参数 <T>，实参 1 个
//   [deduction]   P=T          A=int        ⇒ T := int
//   [deduction] ◀ 推导成功: <T=int>
//
//   标准章节：[temp.deduct] 总则 │ [temp.deduct.call] 逐对 P/A 调整规则
//             [temp.deduct.type] 类型等价/不可推导 │ [temp.arg.explicit] 显式实参
//   对照 clang：Sema::DeduceTemplateArguments（lib/Sema/SemaTemplateDeduction.cpp）
//     deduce() ≈ 入口 │ deducePair() ≈ 逐对合一回调
// =============================================================================

#include "template_deduction.h"
#include "sfinae.h"
#include <format>
#include <iostream>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// 偏特化匹配 [temp.class.spec.match] —— 复用同一套合一算法（理论见 docs/learn/19）
// ─────────────────────────────────────────────────────────────────────────────
// 【为什么能复用 deducePair】与函数模板推导是同一件事 —— 都是"受限合一"，只是 P 的
//   来源不同：调用点用形参表，这里用特化模式。
// 逐位 trace（模式 [T*, T]）：
//   实参 [double*, double] ⇒ ① P=T* A=double* ⇒ T := double ✓ ② P=T A=double ⇒ 一致 ✓ ⇒ 匹配
//   实参 [int, int]        ⇒ ① P=T* A=int ⇒ 不是指针 ✗ ⇒ 该偏特化不匹配
// 逐位 trace（模式 [T&, T]，走下方"引用结构前置检查"）：
//   实参 [int, int]  ⇒ 引用位收到非引用实参 ✗ │ 实参 [int&, int] ⇒ T := int ✓
// 【职责边界】本函数只回答"这一条偏特化匹配不匹配"（逐条试，不含比较）；
//   "多条都匹配时谁胜"由调用方 selectClassTemplate 用 dominance 循环裁决
//   [temp.class.order]（semantic_analyzer.cpp:2994 一带，比较器见同文件 :223
//   classSpecAtLeastAsSpecialized），见 docs/learn/21。
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
        // void_t<...> / decltype(...) 这类模式位待求值，归约用的替换表正是前面各位
        // 已推出的绑定 —— 这就是从左到右逐位匹配的价值。
        // ★ SFINAE 吸收点 ①（全项目三处之一，见 include/sfinae.h 的收口点一览）：
        //   归约抛 SubstitutionFailure ⇒ 该偏特化不成立 ⇒ 移出候选集，试下一个。
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

        // ── ★ 引用的结构匹配前置检查 ──
        // [temp.class.spec.match]/2：偏特化匹配是【结构等价】，不是函数调用那套
        //   "左值可绑定"。deducePair 的 P=T& 分支只看 argIsLValue 会放行 A=int，
        //   于是 `Probe<T&>` 错误匹配上 `Probe<int>` —— 引用位只能由引用实参填充
        //   （clang 同样在 DeduceTemplateArguments 的 TDK_Reference 分支把关）。
        // demo: Probe<T&> 对 Probe<int>  ⇒ 不匹配 ✓ │ 对 Probe<int&> ⇒ 继续推导 ✓
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

        // argIsLValue 传 true：特化模式里的引用按"可绑定"处理（"引用必须对引用"
        //   已由上面单独把关，故安全）。
        // ★ structuralMatch 传 true：本次是【结构等价】而非调用推导，必须关掉
        //   [temp.deduct.call]/3 的万能引用规则 —— 否则 `Kind<T&&>` 匹配 `Kind<int&&>`
        //   会把 T 绑成 int&（把"实参是引用"当成"实参是左值"），正确答案是 T := int。
        if (!deducePair(P, args[i], /*argIsLValue=*/true, paramNames, subst, out,
                        /*structuralMatch=*/true)) {
            failReason = out.failureReason;
            return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// desugarAlias —— 模式位里的别名模板 id 解糖（[temp.alias]/1，理论见 docs/learn/27）
// ─────────────────────────────────────────────────────────────────────────────
// ★ 推导比的是【名字】，而别名就是"同一个类型的另一个名字" —— 同一次调用两侧各走各的
//   解糖路径，P 侧不解 ⇒ 合一判"名字不等" ⇒ 候选被静默剔除：
//     template<class T> using Vec = MyPtr<T>;
//     template<class T> Vec<T> pass(Vec<T> v);   // P 侧写 Vec<T>
//     pass(v);                                    // A 侧已在调用点解糖成 MyPtr<int>
// 是"解糖"不是"实例化"：T 仍是形参，`MyPtr<T>` 保持依赖，正交给逐位合一去绑定。
// 逐对：desugarAlias(Vec<T>, {}) ⇒ MyPtr<T>（保持依赖）│ 配 {T := int} ⇒ MyPtr<int>
// 对照 clang：DeduceTemplateArguments 之前取 getCanonicalType，别名已被完全剥掉。
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

// ─────────────────────────────────────────────────────────────────────────────
// reducePattern —— 把模式位归约成具体类型（void_t / decltype 的求值点）
// ─────────────────────────────────────────────────────────────────────────────
// 扫到哪种模式位 ⇒ 归约成什么（[temp.deduct]/8，CWG 1558：void_t<E...> 各 E 全合法则等价
//   于 void，否则替换失败；别名模板的实参替换失败【也算】SFINAE —— is_range 的偏特化
//   正是靠这条"不命中就静默退化"）：
//   void_t<E...> ⇒ 逐个替换 E，全合法 ⇒ void；任一不合法 ⇒ 抛 SubstitutionFailure
//   void_t<>     ⇒ 无实参 ⇒ 直接合法 ⇒ void
//   decltype(e)  ⇒ 替换 e 后真求值 ⇒ 具体类型
//   Vec<T>（别名）⇒ 解糖后递归再归约（别名可指向别名）
// 逐位 trace（void_t<decltype(declval<T>().begin())>）：
//   {T→vector<int>} ⇒ ① 替换实参 ② decltype 求值 ⇒ int（合法）⇒ 归约为 void ✓
//   {T→int}         ⇒ ② 抛 SubstitutionFailure ⇒ 整个模式位不匹配 ✓
// 【本实现的简化】void_t 是【编译器内建】（名字命中即按上述语义处理，不走别名模板机制），
//   详见 SemanticAnalyzer::registerBuiltins。
// 对照 clang：TreeTransform 里 void_t 是普通别名模板，替换失败同样被吞掉。
TypePtr TemplateDeducer::reducePattern(
    const TypePtr& P,
    const std::unordered_map<std::string, TypePtr>& subst) {

    if (!P) return P;

    // ── 别名模板 id：先解糖成底层类型再匹配（[temp.alias]/1，理据见 desugarAlias）──
    // ★ P 侧不解糖则 `Vec<T>` 与 `MyPtr<int>` 永远不同名 ⇒ 合不上。
    //   解糖后 P = MyPtr<T>，T 仍是形参、保持依赖，正好交给逐位合一去绑定。
    // 对照 clang：比较前先 getCanonicalType，别名在这一步被剥掉。
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

// 取出类型节点对应的"模板参数名"，作为替换表的键。
// demo: TemplateParam("T") ⇒ "T" │ Class("T")（parseType 的产物）⇒ "T" │ 其余 ⇒ "?"
std::string TemplateDeducer::paramKey(const TypePtr& t) const {
    if (!t) return "?";
    if (t->isTemplateParam()) return t->templateParamName;
    if (t->isClass()) return t->name;
    return "?";
}

// 判定类型节点是否是"可绑定的变量"（本次模板参数表中的某个名字）。
// 【理论】合一算法前提：区分模式中的"变量"（进 bind）与"常量结构"（走恒等比较）。
// ★ 必须同时认 TemplateParam("T") 与 Class("T")：parseType 尚不知道 T 是模板参数，
//   会先建成 Class 节点。
bool TemplateDeducer::isTemplateParamName(const TypePtr& t,
                                          const std::vector<std::string>& paramNames) const {
    if (!t) return false;
    if (!t->isTemplateParam() && !t->isClass()) return false;
    std::string n = t->isTemplateParam() ? t->templateParamName : t->name;
    for (auto& p : paramNames) if (p == n) return true;
    return false;
}

// 把一次绑定 paramName := type 写入替换表 subst；已有绑定时要求新旧类型相等。
// 【理论】合一算法的 unify(变量, 类型)：变量只能绑定一次，重复绑定必须一致，否则整次
//   推导失败（对应 clang 诊断 "conflicting types for deduction of template parameter"）。
// demo: max(x:int, y:int) ⇒ 第一次 bind(T,int) 记 "T := int"；第二次 bind(T,int)
//       equals ✓ 记 "T := int（一致 ✓）"；bind(T,double) ⇒ 冲突 ✗
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

// 单个 (P, A) 配对的结构化合一：自外向内递归剥壳（[temp.deduct.call]/[temp.deduct.type]）。
//   P=const X → 剥顶层 const │ P=T& → 左值检查后深入 │ P=T&& → 万能引用折叠/深入
//   P=T* → 对 pointee 深入    │ P=T → 值传递调整后绑定 │ P 非依赖 → 恒等比较
// demo: f(const T& x) 配 const int& 左值 ⇒ 剥 const ⇒ 左值 ✓ 深入 ⇒ (T, int) ⇒ T := int ✓
bool TemplateDeducer::deducePair(const TypePtr& P, const TypePtr& A, bool argIsLValue,
                                 const std::vector<std::string>& paramNames,
                                 Subst& subst, DeductionResult& out, bool structuralMatch) {
    if (!P || !A) {
        out.failureReason = "internal: null type in deduction";
        return false;
    }
    std::string pStr = P->toString();
    std::string aStr = A->toString();

    // ── 别名模板 id 先解糖（[temp.alias]/1）──
    // ★ 放在【每一个递归层】而不只是入口：别名常被引用/const 包着（`const Vec<T>&`
    //   要剥两层壳才露出 Vec<T>），只在入口解一次就漏网了。desugarAlias 对非别名
    //   只是查一下名字，开销可忽略。
    if (TypePtr desugared = desugarAlias(P, subst); desugared != P) {
        std::cout << std::format("  [deduction]   P={:<12} ⇒ 别名解糖为 {}\n",
            pStr, desugared->toString());
        return deducePair(desugared, A, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = const X：剥 P 的顶层 const ──
    // [temp.deduct.type]：顶层 cv 不影响推导（本项目 parseType 把 const T& 建成
    // Const(LRef(T))，剥掉外层 const 后走引用分支）。内层 const（const T*）不剥，
    // 随递归继续参与推导。
    if (P->isConst()) {
        std::cout << std::format("  [deduction]   P={:<12} A={:<12} ⇒ 剥顶层 const\n", pStr, aStr);
        return deducePair(P->innerType, A, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T&：左值引用参数，实参必须左值 ──
    // [temp.deduct.call]：P 是引用类型时以被引用类型参与推导；右值无法绑定到
    // 非 const 左值引用，此处提前拒绝。
    if (P->isLValueReference()) {
        if (!argIsLValue) {
            out.failureReason = std::format(
                "cannot bind lvalue reference '{}' to rvalue argument '{}'", pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }
        return deducePair(P->referencedType, A, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T&&：万能引用（forwarding reference，[temp.deduct.call]/3）──
    // 扫到的形态 ⇒ 绑定（std::forward 完美转发的根基）：
    //   A 左值（argIsLValue）⇒ T := A&（实例化时 T&& 折叠为 A&）
    //   A 右值               ⇒ T := A
    //   structuralMatch=true ⇒ 【跳过本分支】，只做结构等价：`T&&` 配 `int&&` ⇒ T := int
    // ★ A 取自变量的【声明类型】，可能本身就是引用（`int& rr` 的 A 就是 int&）：再套一层
    //   即得嵌套引用 `int& &` ⇒ id(rr) 推出 T := int& & ⇒ 符号 _Z2idIRRiE，与 id(a) 的
    //   T := int& ⇒ _Z2idIRiE 分叉（clang 只产出后者）。修复不在本处补剥壳，而是由
    //   Type::makeLValueReference 按 [dcl.ref]/6 在构造点归一 —— 折叠规则只写一份。
    //   （[expr.type]/1：表达式永远没有引用类型，故 clang 里 A 到这一步已是裸类型。）
    // ★ 偏特化匹配若套用本分支，`Kind<T&&>` 匹配 `Kind<int&&>` 会 bind 出 T := int&
    //   （把"实参是引用"当成"实参是左值"），而正确答案是 T := int。
    if (P->isRValueReference()) {
        TypePtr inner = P->referencedType;
        if (!structuralMatch && isTemplateParamName(inner, paramNames) && argIsLValue) {
            std::cout << std::format(
                "  [deduction]   P={:<12} A={:<12} (lvalue) ⇒ 万能引用折叠路径\n", pStr, aStr);
            return bind(paramKey(inner), Type::makeLValueReference(A),
                        subst, out, pStr, aStr);
        }
        return deducePair(inner, A, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T*：实参必须是指针，递归 pointee ──
    // [temp.deduct.type] 结构化等价：复合类型逐层拆解后递归合一。
    // demo: P=T* A=int* ⇒ 递归到 (T, int) ⇒ T := int ✓
    if (P->isPointer()) {
        if (!A->isPointer()) {
            out.failureReason = std::format(
                "parameter pattern '{}' expects pointer argument, got '{}'", pStr, aStr);
            out.trace.push_back({pStr, aStr, out.failureReason, false});
            std::cout << std::format("  [deduction]   ✗ {}\n", out.failureReason);
            return false;
        }
        return deducePair(P->pointeeType, A->pointeeType, argIsLValue, paramNames, subst, out, structuralMatch);
    }

    // ── P = T（裸模板参数，值传递）──
    // [temp.deduct.call]：值传递调整 —— 实参的顶层引用/const 不参与推导。
    // demo: identity(x) 中 x 是 const int& ⇒ 剥顶层引用、再剥 const ⇒ T := int
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

    // ── P = C<T1...>：类模板 id 的结构合一（[temp.deduct.type]/8 的 "T<T1,...,Tn>" 情形）──
    //   P=MyPtr<T> A=MyPtr<int>（实例类型 MyPtr_int）⇒ 第 1 位 T ↔ int ⇒ T := int ✓
    //   P=MyPtr<T> A=Box<int>                        ⇒ 不同模板 ✗
    //   P=Box<T*>  A=Box<int,double>                 ⇒ 元数不同 ✗
    //   模式位是值（NTTP）                            ⇒ 要求 toString() 逐位相等（不做算式反推）
    // ★ A 侧是【实例化后】的类型，名字已被改写成 MyPtr_int，靠名字反推是错的（清洗规则不是
    //   单射）；必须查实例记下的出身（templateOriginName/templateOriginArgs，见
    //   getOrInstantiateClass）。clang 不需要出身字段（它的实例仍带实参表）。
    // ★ 别名模板全靠这条：`Vec<T>` 解糖成 `MyPtr<T>` 后能不能推出 T 全看本分支。
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
                if (!deducePair(pa.type, aa.type, argIsLValue, paramNames, subst, out, structuralMatch))
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

    // ── P 非依赖：要求恒等（[temp.deduct.type]）──
    // P 中不含模板参数的部分必须与 A 结构一致，否则该候选在重载决议 S6 中被淘汰。
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

// 一次函数模板调用的完整推导：S3 显式前缀 → S2 逐对合一 → S4 收尾检查。
// 【理论】合一算法主循环：先注入已知绑定（显式实参），再逐对递归合一，
//         最后验证所有变量均已绑定；任一步失败即返回 success=false。
// demo: twice(3) ⇒ S3 无显式 ⇒ S2 P=T,A=int ⇒ T := int ✓ ⇒ S4 全绑定 ⇒ success；
//       失败候选由重载决议 S6 丢弃，全部失败才报 "no matching function"。
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

    // ── S3：显式实参占模板参数表前缀 [temp.arg.explicit] ──
    // 显式实参按位置绑定前缀，剩余参数靠调用推导补全（如 make<int>(3.14) 只给第一个）。
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

    // ── 函数参数个数检查（[expr.call]）：不符则该候选直接出局 ──
    if (argTypes.size() != func->parameters.size()) {
        result.failureReason = std::format(
            "argument count mismatch: {} expects {}, got {}",
            func->name, func->parameters.size(), argTypes.size());
        std::cout << std::format("  [deduction]   ✗ {}\n", result.failureReason);
        return result;
    }

    // ── S2：逐对 P/A 推导 [temp.deduct.call] ──
    // 每个 P_i 与 A_i 递归合一，任一对失败立即返回（failureReason 已由 deducePair 填好）。
    for (size_t i = 0; i < func->parameters.size(); i++) {
        // 形参模式若写着别名模板 id（`Vec<T>`）先解糖：实参那侧（`MyPtr<int>`）
        // 早在调用点解过糖，两侧不同名就永远合不上。
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
    // 必须显式给定（如 make<T>()）；诊断文案提示用户写出显式实参。
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
