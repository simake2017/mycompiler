// =============================================================================
// test_tmpl_20_bare_template_error.cpp —— 错误用例：裸类模板名当类型用
// =============================================================================
// 考察理论点：[temp.arg.explicit]（模板实参必填）——类模板蓝图不是类型，
//   没有实参的模板名不能当类名使用。对照 clang：
//   "use of class template 'Box' requires template arguments"。
//
// 预期行为：语义分析阶段（Phase 3）resolveType 解析 `Box b` 时，
//   命中类模板注册表且实参为空 → 早期报错，而不是放行未解析类型
//   到使用期才报 "Class 'Box' not declared"（诊断不指向根因）。
//
// 期望报错（退出码非 0，日志含）：
//   [ERROR] 'Box' is a class template; provide template arguments (e.g. Box<int>)
//
// 验证命令：
//   ./build-linux/minicc tests/tmpl/test_tmpl_20_bare_template_error.cpp; echo "exit=$?"
//   应见上述报错文案，exit 非 0
//   语义 oracle：clang++-18 -fsyntax-only 也拒绝此代码，但现代 clang
//   先尝试 CTAD，报 "no viable constructor or deduction guide for
//   deduction of template arguments of 'Box'"（本项目未实现 CTAD，
//   报的是经典文案，语义等价：裸类模板名不能当类型用）
// =============================================================================

template <typename T>
class Box {
public:
    T value;
};

int main() {
    Box b;   // 错误：裸类模板名，缺模板实参（应为 Box<int>）
    return 0;
}
