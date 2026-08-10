// =============================================================================
// test_pp_04_error_unterminated.cpp —— P0 错误用例：#ifdef 无 #endif
// =============================================================================
// 考察理论点：条件栈在文件末尾必须为空（[cpp.cond] 约束），
//   对照 clang 诊断 "unterminated conditional directive"。
//
// 期望报错（退出码 1）：
//   [Preprocessor Error] ...: unterminated conditional directive (1 open)
// =============================================================================

#ifdef X
int a;

int main() {
    return 0;
}
