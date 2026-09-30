// =============================================================================
// 测试：错误用例 —— 值实参窄化到 bool（[dcl.init]/7 的窄化禁令，常量表达式不豁免）
// =============================================================================
// 考察理论点：
//   · [temp.arg.nontype]/1 要求实参是形参类型的 converted constant expression。
//     [expr.const]/10 定义该术语时直接引用 [dcl.init]/7 的列表初始化规则 ——
//     而 [dcl.init]/7 允许"常量表达式且值恰好装得下"时跨越窄化，
//     **`2 → bool` 不满足这个豁免**：bool 的可表示集合是 {0,1}，2 装不下。
//     故 `F<2>` 非法（clang 原文：non-type template argument evaluates to 2,
//     which cannot be narrowed to type 'bool'）。
//   · 注意与 `F<1>` 的区别：1 落在 {0,1} 内 ⇒ 合法（见 test_tmpl_64）。
//     这条边界正是"判据是可表示性、不是形态相等"的最锐利证据：
//     形态上 int 与 bool 都不相等，值上 1 过、2 不过。
//
// ★ 与 B17 的关系：旧实现用"形态精确相等"当判据，`F<1>` 与 `F<2>` **都**被拒 ——
//   拒对了 `F<2>`（碰巧），拒错了 `F<1>`。改判据后两者才被分开。
//
// 预期行为：语义分析阶段（Phase 3）checkTemplateArguments 的值位分支拦下，
//   退出码 1，不产生任何实例。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Semantic Error] … non-type template argument evaluates to 2,
//     which cannot be narrowed to type 'bool'
//
// clang 核对：
//   $ clang++-18 -std=c++20 -fsyntax-only test_tmpl_65_error_nttp_bool_narrowing.cpp
//   → error: non-type template argument evaluates to 2, which cannot be
//            narrowed to type 'bool' [-Wc++11-narrowing]
// =============================================================================

template<bool B>
struct F {
    int v;
};

int main() {
    F<2> a;     // ← 2 装不进 bool ⇒ 必须在此拦下
    return 0;
}
