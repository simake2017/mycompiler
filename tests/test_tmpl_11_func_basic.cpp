// =============================================================================
// test_tmpl_11_func_basic.cpp —— S1 正例：函数模板解析
// =============================================================================
// 考察理论点：函数模板声明 = 带参数的函数蓝图（C++ 标准 [temp.fct]）。
//   Parser 必须在 template<...> 之后识别出"这是函数而非类"，
//   且返回类型 T 是模板参数名（词法上只是 Identifier）。
//
// 预期行为：
//   Phase 2 日志：[parse:template] parsing function signature...
//                 blueprint summary: template <typename T> T twice(T x) { ... }
//   Phase 3 日志：[register] template <typename T>
//                 > twice(T x) → T (function blueprint stored...)
//                 ↳ overload candidate set 'twice' size = 1
//   Phase 4 日志：(function template 'twice': blueprint stored; ...)
//   --dump-ast 输出：TemplateDecl(function): <T>
//                    FunctionTemplate: twice(1 params) → T
//   编译全程退出码 0。
//   注意：S1 阶段不允许调用 twice —— 调用点推导是 S2 的任务。
//
// oracle 对照（clang 中的 AST 形态）：
//   clang++-18 -Xclang -ast-dump -fsyntax-only tests/test_tmpl_11_func_basic.cpp
//   可见 FunctionTemplateDecl → FunctionDecl(twice) + TemplateTypeParmDecl(T)
// =============================================================================

template<typename T>
T twice(T x) {
    return x + x;
}

int main() {
    int a = 21;
    return a - 21;
}
