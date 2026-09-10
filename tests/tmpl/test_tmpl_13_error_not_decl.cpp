// =============================================================================
// test_tmpl_13_error_not_decl.cpp —— S1 错误用例：模板体既不是类也不是函数
// =============================================================================
// 考察理论点：template<...> 之后语法上只允许声明（[temp] 规定模板声明一个
//   类模板/函数模板/别名模板等）。变量声明不是合法模板体。
//
// 期望报错（退出码非 0，stderr/日志含）：
//   走到 parseFunctionDecl：类型 T 解析成功、名字 value 解析成功，
//   随后在期望 '(' 处失败：Expected '(' after function name
//   （因为 value 后面是 ';' 而不是参数列表）
//
// 验证命令：
//   ./build-linux/minicc tests/test_tmpl_13_error_not_decl.cpp; echo "exit=$?"
//   应见 [ERROR] ... Expected '(' after function name，exit=1
// =============================================================================

template<typename T>
T value;

int main() {
    return 0;
}
