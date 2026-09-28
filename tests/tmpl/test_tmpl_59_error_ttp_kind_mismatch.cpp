// =============================================================================
// test_tmpl_59_error_ttp_kind_mismatch.cpp —— 错误用例：模板模板实参【逐位 kind】不符
// =============================================================================
// 考察理论点（[temp.arg.template]/3，对应 docs/learn/33 §3.4）：
//   位数对上之后还要**逐位同 kind**：类型位 ↔ 类型位、值位 ↔ 值位、模板位 ↔ 模板位。
//   本用例：形参位声明的是**值位**（`template<int> class C`），实参模板的对应位是
//   **类型位**（`template<class T> struct Box`）⇒ 不匹配。
//
// ★ 这一位"是什么"由【形参表】裁定，不由实参的字面形态裁定：
//   `WrapI<Box>` 里的 `Box` 在 Parser 眼里与 `int` 同形（都是 Class 节点），
//   "它需要是个模板" 是第一层（test_tmpl_54 守），"它内层那几位各是什么 kind"
//   是第二层 —— 也就是本用例守的东西。
//
// 姊妹用例：
//   · 位数不符（too few / too many）    → test_tmpl_58
//   · 值位之间类型不同（int vs unsigned）→ 单测 TtpSignature.NonTypeParameterTypeMismatch
//   · 通过的正例                        → test_tmpl_57
//
// 预期行为：语义分析阶段（Phase 3）checkTemplateArguments 的模板位分支拦下。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Semantic Error] ...
//     template template argument has different template parameters than its
//     corresponding template template parameter ('C'): parameter 1 has a
//     different kind: 'C' declares a non-type parameter, but the template
//     argument declares a type parameter
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_59_error_ttp_kind_mismatch.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
//   clang++-18 -std=c++20 -fsyntax-only <同名文件> 2>&1 | head -3
//   应见 clang 的同一诊断（note: template parameter has a different kind
//   in template argument）
// =============================================================================

// 形参位 C 要求一个【含 1 个值位】的模板
template <template <int> class C>
struct WrapI {
    C<3> inner;
};

// 实参模板的对应位是【类型位】⇒ kind 对不上
template <class T>
struct Box {
    T value;
};

int main() {
    WrapI<Box> w;          // ← kind 不符：必须在此拦下
    return 0;
}
