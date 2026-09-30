// =============================================================================
// 测试：错误用例 —— 负值实参给无符号形参
// =============================================================================
// 考察理论点：
//   · [conv.integral]/2：有符号 → 无符号的整型转换**是**良定义的（按 2^N 取模），
//     但 [temp.arg.nontype]/1 要的是 converted constant expression，
//     而 [dcl.init]/7 的窄化禁令明确写：由整数类型转换到**不能表示原值**的
//     整数类型属于窄化，**常量表达式也不豁免**（豁免只针对"值恰好装得下"的情形）。
//     -1 显然不是 unsigned 能表示的值 ⇒ `U<-1>` 非法。
//   · clang 原文：non-type template argument evaluates to -1, which cannot be
//     narrowed to type 'unsigned int'。
//   · ★ 与"运行期 -1 赋给 unsigned 得到 4294967295"是两个不同的规则：
//     那是 [conv.integral] 的**运行期**转换（合法、有定义），
//     这里是**模板实参**的常量转换（受 [dcl.init]/7 约束、非法）。
//     minicc 的判据落在 Type::canRepresentValue（见 include/type.h）。
//   · 负号本身也值得一提：`-1` 不是字面量，而是"一元减 + 字面量"的常量表达式
//     （[expr.unary.op]）。本项目只在 NTTP 实参位做这一层（`U<-1>`），
//     任意常量表达式（`U<2+2>`）属 ROADMAP 主线 D。
//
// 预期行为：语义分析阶段（Phase 3）拦下，退出码 1，不产生任何实例。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Semantic Error] … non-type template argument evaluates to -1,
//     which cannot be narrowed to type 'unsigned int'
//
// clang 核对：
//   $ clang++-18 -std=c++20 -fsyntax-only test_tmpl_66_error_nttp_unsigned_negative.cpp
//   → error: non-type template argument evaluates to -1, which cannot be
//            narrowed to type 'unsigned int' [-Wc++11-narrowing]
// =============================================================================

template<unsigned U>
struct G {
    unsigned v;
};

int main() {
    G<-1> a;    // ← -1 装不进 unsigned ⇒ 必须在此拦下
    return 0;
}
