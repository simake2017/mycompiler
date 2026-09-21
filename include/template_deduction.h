#pragma once
// =============================================================================
// 模板实参推导引擎 (Template Argument Deduction) —— S2~S4（理论见 docs/learn/02、03、04）
// =============================================================================
// 推导 = 受限的合一算法（Hindley-Milner 类型推断的简化版）：对每个
//   (参数类型模式 P, 实参类型 A) 配对做模式匹配，绑定 P 中出现的模板参数；
//   同一参数的多次绑定必须一致（替换合成）。
// demo: identity(42) —— P=T、A=int ⇒ 替换表 { T := int }，交 S5 实例化出 identity<int>
//
// ── S1~S6 阶段分工（本项目教学分解）────────────────────────────────────
//   S1 解析           src/parser.cpp parseTemplateDecl（蓝图入 AST）
//   S2 基础推导       本文件 deduce() / deducePair()（逐对合一）
//   S3 显式实参       本文件 deduce() 前缀填充
//   S4 不可推导上下文 本文件 deduce() 收尾检查
//   S5 实例化         src/template_instantiation.cpp（结构化替换）
//   S6 重载决议       src/semantic_analyzer.cpp
//
// ── 标准章节 → 实现 ────────────────────────────────────────────────────
//   [temp.deduct] 推导总则 │ [temp.deduct.call] 调用推导：逐对 P/A、引用/const/值传递调整
//   [temp.deduct.type] 结构化类型等价、不可推导上下文、顶层 cv 忽略
//   [temp.arg.explicit] 显式模板实参（S3 前缀规则）
//
// ── clang 对照（lib/Sema/SemaTemplateDeduction.cpp）─────────────────────
//   DeduceTemplateArguments → 本类 deduce() │ TemplateDeductionCallback::Deduce → deducePair()
//   管线：Parser(S1) → Sema(识别候选) → ★TemplateDeducer(S2~S4)★ → Instantiator(S5)
//         → Sema(S6 重载决议) → CodeGen
// =============================================================================

#include "ast.h"
#include "type.h"
#include "template_instantiation.h"   // DecltypeEvaluator / SubstitutionFailure
#include <string>
#include <unordered_map>
#include <vector>

namespace minicc {

// 单次 P/A 配对的轨迹（教学日志用）
// demo：identity(42) 的推导产生一步 trace ——
//   pattern="T", argument="int", result="T := int", ok=true
// 对应运行日志行：[deduction]   P=T            A=int          ⇒ T := int
struct DeductionStep {
    std::string pattern;   // P：参数类型模式
    std::string argument;  // A：实参类型
    std::string result;    // "T := int" / "一致 ✓" / 失败原因
    bool        ok = true;
};

// 一次完整推导的产出（重载决议 S6 中每试一个候选模板就构造一份）
// demo：identity(42) → success=true, deducedArgs=[int]
//       make(1, 2.0) → success=false，failureReason 记录 T 的冲突绑定
struct DeductionResult {
    bool success = false;
    std::vector<TypePtr> deducedArgs;  // 按模板参数表顺序（显式给定 + 推导补全）
    std::string failureReason;
    std::vector<DeductionStep> trace;
};

// 替换表：模板形参名 → 已推出的类型（合一算法的「解」）
using Subst = std::unordered_map<std::string, TypePtr>;

class TemplateDeducer {
public:
    // 一次函数模板调用的完整推导：S3 显式前缀 → S2 逐对合一 → S4 收尾检查。
    // 理论：形参类型是含"变量"(T) 的模式 P，与实参 A 逐对合一积累替换表，
    //   同一变量多次绑定必须一致；产出交 S5 实例化（[temp.deduct.call]）。
    // demo: identity(42) → T := int ✓，deducedArgs=[int] │ twice(3) → 两次绑定一致 ✓
    //       make(1,2.0) → 先 T := int 再 T := double ⇒ 冲突 ✗，整体失败
    // 参数：tmpl 蓝图（funcTemplate 非空）│ argTypes 各实参类型 │ argIsLValue
    //   各实参是否左值（T& 绑定检查、万能引用折叠要用）│ explicitArgs 显式模板
    //   实参（S3，占模板参数表前缀，可为空）
    DeductionResult deduce(const TemplateDeclPtr& tmpl,
                           const std::vector<TypePtr>& argTypes,
                           const std::vector<bool>& argIsLValue,
                           const std::vector<TypePtr>& explicitArgs = {});

    // 偏特化匹配（[temp.class.spec.match]）：用使用点实参 args 逐位推导偏特化模式
    //   pattern 中的模板参数 —— 与函数模板推导是同一个合一算法（复用 deducePair），
    //   如 Box<T*, T> 对 Box<double*, double> ⇒ {T := double}。
    // demo: pattern=[T*, T]、args=[double*, double] → 成功，subst={T→double}
    //       pattern=[T*, T]、args=[int, int]      → 失败（指针结构失配）
    // 返回：全部位匹配成功 → true；任一位失败 → false 并填 failReason。
    // 注：不做 [temp.class.order] 偏序裁决（多偏特化同时匹配时取先成功者）。
    bool matchPattern(const std::vector<TypePtr>& pattern,
                      const std::vector<TypePtr>& args,
                      const std::vector<std::string>& paramNames,
                      std::unordered_map<std::string, TypePtr>& subst,
                      std::string& failReason);

    // decltype / void_t 的求值回调（见 DecltypeEvaluator）。
    // nullptr（默认，单元测试路径）→ 模式中的 void_t 不求值。
    void setDecltypeEvaluator(DecltypeEvaluator* ev) { m_decltypeEval = ev; }
    // 成员类型查表回调：reducePattern 里临时建的 instantiator 要用它
    // （void_t<typename T::type> 这类模式位要靠它在替换阶段查表）。
    void setMemberTypeResolver(MemberTypeResolver* r) { m_memberResolver = r; }
    // 别名模板展开回调：同样给 reducePattern 里那个临时 instantiator 用
    // （偏特化模式位写成别名模板 id 时，靠它在替换阶段解糖）。
    void setAliasTemplateResolver(AliasTemplateResolver* r) { m_aliasResolver = r; }

    // 逐对 P/A 合一（对应 clang TemplateDeductionCallback::Deduce）：自外向内递归剥壳
    //   （const → 引用 → 指针），直到 P 出现裸模板参数（绑定）或 P 完全非依赖（要求
    //   P == A 恒等）。[temp.deduct.call] 引用/const 调整；[temp.deduct.type] 等价。
    // 【为什么是 public】CTAD（[dcl.type.class.deduct]）拿构造函数形参当模式、构造实参
    //   当被推项直接调它逐位合一（SemanticAnalyzer::deduceClassTemplateArgs）—— 与函数
    //   模板推导共用同一核心，即"同一套合一算法换个方向用"。
    // structuralMatch：本次合一是【类模板偏特化的结构等价】还是【调用的实参推导】。
    //   ★ 两者的差别不是"松紧"，而是【适用哪一套规则】：
    //     调用（false，默认）：[temp.deduct.call] 全套调整，含 /3 万能引用规则 ——
    //       P 是"模板参数的右值引用"且实参左值时，T := A&。
    //     偏特化匹配（true）：[temp.class.spec.match]/2 结构等价，`T&&` 模式对 `int&&`
    //       实参 ⇒ T := int（剥掉实参那层引用）。⚠ 绝不能套用万能引用规则 —— 那会 bind
    //       出 T := int&，使 `Kind<T&&>` 匹配 `Kind<int&&>` 时把 T 绑成 int&（错）。
    //   ⚠ 必须显式区分：matchPattern 为跳过左值/右值绑定检查而传 argIsLValue=true，而它
    //     恰好【同时】是万能引用分支的开关 —— 一个标志被两套规则共用，关掉一个必然误开另一个。
    bool deducePair(const TypePtr& P, const TypePtr& A, bool argIsLValue,
                    const std::vector<std::string>& paramNames,
                    Subst& subst, DeductionResult& out,
                    bool structuralMatch = false);

private:
    // 模式位里的别名模板 id 解糖：`Vec<T>` → `MyPtr<T>`（[temp.alias]/1）。
    // 名字不是别名模板（或没挂解析器）时原样返回。见实现处的详细说明。
    TypePtr desugarAlias(const TypePtr& P,
                         const std::unordered_map<std::string, TypePtr>& subst);

    DecltypeEvaluator* m_decltypeEval = nullptr;
    MemberTypeResolver* m_memberResolver = nullptr;
    AliasTemplateResolver* m_aliasResolver = nullptr;

    // 模式归约：把模式中"需要求值才能变成具体类型"的位置先算出来。
    // 为什么需要：匹配 is_range<T, void_t<decltype(declval<T>().begin())>> 时，
    //   模式第 2 位不是类型而是"一个待求值的表达式工厂"；必须先用【已经积累到的】
    //   替换表（第 1 位刚推出的 T := vector<int>）把它算成 void，才能与实参第 2 位比较。
    // demo: subst={T→vector<int>} ⇒ declval<vector<int>>().begin() → int ⇒ 归约为 void
    //       subst={T→int}        ⇒ declval<int>().begin() 不合法 ⇒ 抛 ⇒ 不匹配 ✓
    // 求值失败抛 SubstitutionFailure，由 matchPattern 捕获为"不匹配"。
    // 对照 clang：DeduceTemplateArguments 模式侧先走 SubstType（含 SFINAE guard），
    //   失败即返回 TDK_SubstitutionFailure，调用方据此移除候选。
    TypePtr reducePattern(const TypePtr& P,
                          const std::unordered_map<std::string, TypePtr>& subst);

    // 绑定模板参数（一致性检查 = 替换合成）：把 paramName := type 写入替换表；
    //   若该名字已有绑定，则要求新旧类型相等，否则整次推导失败。
    // demo: max(x:int, y:int) → 第一次 T := int 入表；第二次 int == int，
    //       记"T := int（一致 ✓）"；若第二次是 double 则冲突 ✗。
    bool bind(const std::string& paramName, const TypePtr& type,
              Subst& subst, DeductionResult& out,
              const std::string& patternStr, const std::string& argStr);

    // 类型是否是"模板参数"（TemplateParam 或被 parseType 解析成 Class 的参数名）
    // 合一算法里"识别变量位置"的谓词：只有模板参数才是可绑定的未知量，
    // 其余类型都是待比较的常量结构。
    bool isTemplateParamName(const TypePtr& t,
                             const std::vector<std::string>& paramNames) const;
    // 取出类型节点对应的模板参数名（如 "T"），作为替换表 Subst 的键
    std::string paramKey(const TypePtr& t) const;
};

} // namespace minicc
