// =============================================================================
// 测试：全特化模式里用了未声明的名字 —— 必须报错，不能静默永不匹配
// =============================================================================
// 考察理论点（[temp.expl.spec] / [temp.arg]）：
//   `template <>` 的形参表是【空的】，所以模式里出现的任何名字都【不可能】
//   是模板形参 —— 只能是真实的类型名。于是下面这行必然有问题：
//
//     template <typename T, typename U> struct Box { ... };
//     template <> struct Box<T*, T> { ... };   // T 是哪来的？没声明！
//
//   这行的本意多半是"偏特化"，作者忘了把 T 写进 `template<...>`。
//   正确的写法是：
//     template <typename T> struct Box<T*, T> { ... };
//
//   ★ 为什么这条值得单独一个测试：在【修复前】本文件编译通过（rc = 0），
//     而且是【静默地永不匹配】——
//       · Parser 没有符号表，把裸 `T` 建成 Class("T") 节点，不报错；
//       · 匹配时逐位做 equals，Class("T") 比不中任何真实类型；
//       · 于是这条全特化永远选不上，程序一直走主模板，tag() 返回 0；
//       · 全程零报错、零警告。用户以为在特化，其实特化根本没生效。
//
//     这是"静默选错"里最难发现的一种：连"报错信息指向别处"这种线索都没有。
//     修复位置在 SemanticAnalyzer::processTemplateDecl 注册 ExplicitSpec 时
//     （那里才有 m_classDecls 与 m_classTemplates 可查）——
//     Parser 查不了，它不知道 `T` 是未声明的形参还是别处声明的类。
//
//   ★ 为什么只查 ExplicitSpec 不查偏特化：
//     偏特化（`template <class T> struct Box<T*, T>`）里的裸 T 是【合法】的，
//     Parser 已按形参作用域把它建成 TemplateParam 节点 —— 不能误伤。
//     两条分支互不重叠，所以只拦 ExplicitSpec 恰好安全。
//
//   对照 clang：error: use of undeclared identifier 'T'
//     （注意 clang 报在第 2 行 T 的位置，本实现报在语义阶段、位置退化为 1:1；
//       位置精度是本实现的已知边界。）
//
// ── 预期行为（本文件属于【必须编译失败】的负向测试）──
//   期望退出码：minicc 非 0
//   期望报错文案（关键片段）：
//     [Semantic Error] ...: use of undeclared identifier 'T' in explicit
//     specialization pattern of 'Box' —— `template <>` 的形参表是空的，
//     模式里每个名字都必须是【真实类型】
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_45_error_undeclared_in_explicit_spec.cpp -o /tmp/x
//     echo $?                                       # → 非 0
//     ... 2>&1 | grep "undeclared identifier"       # → 命中上式文案
//
//   ★ 本文件不参与"退出码对齐 clang"的常规回归。
// =============================================================================

template <typename T, typename U>
struct Box {
    int tag() { return 0; }   // 主模板
};

// ★ 非法：`template <>` 却用了 T —— 本意应为
//   `template <typename T> struct Box<T*, T> { ... };`
template <>
struct Box<T*, T> {
    int tag() { return 9; }
};

int main() {
    Box<int*, int> b;
    return b.tag();
}
