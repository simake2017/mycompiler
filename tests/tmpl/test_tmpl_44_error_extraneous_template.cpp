// =============================================================================
// 测试：`template<>` 却没写 `<...>` —— 必须报错，不能静默当主模板
// =============================================================================
// 考察理论点（[temp.expl.spec]）：
//   `template <>` 是"显式特化（全特化）"的标记，它【必须】搭配类名后的
//   模板实参表出现：
//
//     template <> struct Box<int*, int> { ... };   // ✓ 全特化
//     template <> struct Box            { ... };   // ✗ 写成这样是什么？
//
//   第二行同时满足两件事：形参表空 + 类名后没写 `<...>`。它不是任何
//   合法形态 —— 但很容易被实现"顺手"当成主模板处理，于是用户以为自己在特化，
//   实际在【重定义主模板】，而且全程零报错。
//
//   ★ 这正是本项目最在意的一类问题：**静默选错**。
//     它比直接报错危险得多 —— 程序能跑、结果不对，且没有任何线索指向现场。
//
//   标准依据：全特化的声明必须点明被特化的实参（[temp.expl.spec]/1），
//   少了实参表就不是特化。对照 clang：
//     error: extraneous 'template<>' in declaration of struct 'Box'
//     （DiagnosticSemaKinds.td 的 err_extraneous_template_spec）
//
//   落到本实现的判定代码（src/parser.cpp 的 parseTemplateDecl）：
//     两个条件量的是【两个独立维度】，不可互相替代：
//       specPattern 空    ⇔ 类名后没写 <...>
//       templateParams 空 ⇔ 写了 `template<>`
//     主模板恰好落在"specPattern 空、templateParams 非空"那一格，
//     所以外层先按 specPattern 摘掉主模板，内层再按 templateParams 分全/偏。
//     "两个都空"就是本测试要拦的非法形态。
//
// ── 预期行为（本文件属于【必须编译失败】的负向测试）──
//   期望退出码：minicc 非 0
//   期望报错文案（关键片段）：
//     [Parse Error] ...: extraneous 'template<>' in declaration of class 'Box' ——
//     写了 `template<>` 却没有在类名后写 `<...>`：全特化必须点明模板实参
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_44_error_extraneous_template.cpp -o /tmp/x
//     echo $?                                    # → 非 0
//     ... 2>&1 | grep "extraneous"               # → 命中上式文案
//
//   ★ 本文件不参与"退出码对齐 clang"的常规回归（两边都失败，
//     要比的是【报错文案】而非退出码）。
// =============================================================================

template <typename T>
struct Box {
    int tag() { return 0; }
};

// ★ 非法：写了 `template<>`，类名后却没有 `<...>`
//   修复前：minicc 静默当成主模板，rc = 0（危险）
//   修复后：Parser 报 extraneous 'template<>'
template <>
struct Box {
    int tag() { return 9; }
};

int main() {
    Box<int> b;
    return b.tag();
}
