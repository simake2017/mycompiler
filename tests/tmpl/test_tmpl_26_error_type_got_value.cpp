// =============================================================================
// test_tmpl_26_error_type_got_value.cpp —— 错误用例：类型形参收到值实参
// =============================================================================
// 考察理论点：[temp.arg] —— 类型形参（typename/class T）只能由类型实参满足。
//   给 Box<4> 传一个值，C++ 里是硬错误，对照 clang：
//   err_template_arg_must_be_type / "template argument for template type
//   parameter must be a type"。
//
// 配套 test_tmpl_25：两例合起来才构成完整的形态校验——
//   25 守「NTTP 位收到类型」，26 守「类型位收到值」，方向相反、报错互补。
//
// 注意 Parser 侧不需要放过这一步：`Box<4>` 的 4 会被 parseTemplateArgumentList
//   识别成 TemplateArg{Integral}（值），形态信息完整带到语义阶段，
//   由 checkTemplateArguments 判定"第 1 位是 Type 形参，却收到 Integral 实参"。
//
// 预期行为：语义分析阶段（Phase 3）报错退出，不进实例化。
//
// 期望报错（退出码非 0，日志含）：
//   [ERROR] [Semantic Error] ...
//     template argument 1 for 'Box' ('T') must be a type argument,
//     but '4' is a value
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_26_error_type_got_value.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
// =============================================================================

template<class T>
class Box {
public:
    T value;
};

int main() {
    Box<4> b;
    return 0;
}
