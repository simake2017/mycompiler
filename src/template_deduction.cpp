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
#include <format>
#include <iostream>

namespace minicc {

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
        if (!deducePair(func->parameters[i].type, argTypes[i], argIsLValue[i],
                        params, subst, result)) {
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
