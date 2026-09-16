// =============================================================================
// test_tmpl_25_error_nttp_got_type.cpp —— 错误用例：非类型形参收到类型实参
// =============================================================================
// 考察理论点：[temp.arg.nontype] —— 非类型模板形参（int N）只能由
//   「常量表达式」实参来满足。传一个类型（Buf<int>）在 C++ 里是硬错误，
//   对照 clang：err_template_arg_must_be_expr / "template argument for
//   non-type template parameter must be an expression"。
//
// 为什么本用例值得单独守：它正是"类型形参与非类型形参能否区分"的直接检验。
//   若校验仍读退化的 typeParams（只有名字、没有 kind），Buf<int> 会被当成
//   一次正常的类型实参放行，一路实例化到崩溃或产出垃圾符号；
//   只有按 templateParams 的 kind 逐位比对才能在此拦下。
//
// 预期行为：语义分析阶段（Phase 3）在 checkTemplateArguments 拦下，
//   进入实例化（Phase 4）之前就报错退出。
//
// 期望报错（退出码非 0，日志含）：
//   [ERROR] [Semantic Error] ...
//     template argument 1 for 'Buf' ('N') must be a non-type argument of
//     type 'int', but 'int' is a type
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_25_error_nttp_got_type.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
// =============================================================================

template<int N>
class Buf {
public:
    int cap;
};

int main() {
    Buf<int> b;
    return 0;
}
