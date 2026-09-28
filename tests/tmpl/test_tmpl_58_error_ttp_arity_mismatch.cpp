// =============================================================================
// test_tmpl_58_error_ttp_arity_mismatch.cpp —— 错误用例：模板模板实参【位数】不符
// =============================================================================
// 考察理论点（[temp.arg.template]/3，对应 docs/learn/33 §3.4）：
//   模板模板形参声明的内层形参表是一份【签名】，实参模板的形参表必须**逐位**对上，
//   位数是第一道坎：
//     · 形参位要 2 位、实参只给 1 位  ⇒ too few template parameters
//     · 形参位要 1 位、实参给了 2 位  ⇒ too many template parameters
//       ★ **哪怕多出的那一位带默认实参也不行** —— 旧标准的"多余位有默认即可"
//         已被 P0522R0 取代；clang++-18 -std=c++20 实测拒绝（探针 docs/learn/33 §3.4）。
//         本用例走的是 too few 那半边，too many 那半边见单测
//         tests/unit/test_ttp_signature.cpp（TtpSignature.TooManyTemplateParametersEvenWithDefault）。
//
// ★ 为什么必须报在【实参列表】而不是下游：缺这道校验，`Wrap2<Box, int>`
//   （形参位要 2 位、Box 只有 1 位）会一路放行，直到替换出 `Box<int,int>` 才在下游
//   报"Box 至多 1 个实参" —— 报是报了，但诊断指向派生出的假类型，不是根因。
//   这与 test_tmpl_54（拿类型填模板位）是同一类"形态只能由形参表裁定"的问题。
//
// 预期行为：语义分析阶段（Phase 3）checkTemplateArguments 的模板位分支拦下，
//   进入实例化（Phase 4）之前就退出。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Semantic Error] ...
//     template template argument has different template parameters than its
//     corresponding template template parameter ('C'): too few template
//     parameters: 'C' declares 2 parameter(s), but the template argument provides 1
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_58_error_ttp_arity_mismatch.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
//   clang++-18 -std=c++20 -fsyntax-only <同名文件> 2>&1 | head -3
//   应见 clang 的同一诊断（error: template template argument has different template
//   parameters ... / note: too few template parameters in template template argument）
// =============================================================================

// 形参位 C 要求一个【含 2 个参数】的模板
template <template <class, class> class C, class T>
struct Wrap2 {
    C<T, T> inner;
};

// 实参模板只有 1 个形参 ⇒ 位数对不上
template <class T>
struct Box {
    T value;
};

int main() {
    Wrap2<Box, int> w;     // ← too few：必须在此拦下
    return 0;
}
