// =============================================================================
// test_tmpl_54_error_ttp_not_template.cpp —— 错误用例：模板模板形参收到非模板实参
// =============================================================================
// 考察理论点：[temp.arg.template] —— 模板模板形参（template<class> class C）
//   接受的实参必须是【一个类模板】。拿一个普通类型去填（Wrap<int, int>）是硬错误。
//   对照 clang：err_template_template_parm_mismatch
//   / "template template argument must be a class template"。
//
// 为什么本用例值得单独守：实参位上的裸名与类型名在 Parser 眼里【完全同形】
//   （都被建成 Class 节点），`Box` 与 `int` 的差别只有 Sema 逐位看形参表才判得出。
//   缺这道校验，`Wrap<int, int>` 会被当成"实参名是 int 的模板"一路放行，
//   造出一个模板名叫 "int" 的假实例 —— 编译通过、汇编期才炸。
//   这与 NTTP 名在实参位被误判成类型（PITFALLS P6 / docs/learn/27 §3.3）
//   是同一个"形态只能由形参表裁定"的问题。
//
// 预期行为：语义分析阶段（Phase 3）在 resolveType 的模板 id 分支拦下，
//   进入实例化（Phase 4）之前就报错退出。
//
// 期望报错（退出码非 0，日志含）：
//   [ERROR] [Semantic Error] ...
//     template argument 1 for 'Wrap' ('C') must be a class template,
//     but 'int' is not a template
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_54_error_ttp_not_template.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
// =============================================================================

template <template <class> class C, class T>
struct Wrap {
    C<T> inner;
};

int main() {
    Wrap<int, int> w;      // ← int 不是模板：必须在此拦下
    return 0;
}
