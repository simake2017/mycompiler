// =============================================================================
// test_tmpl_18_nondeduced_error.cpp —— S4 错误用例：不可推导上下文
// =============================================================================
// 考察理论点：[temp.deduct] 不可推导上下文 —— T 只出现在返回类型位置时，
//   调用实参提供不了任何 P/A 配对信息，推导必然失败，必须显式指定
//   （如 gen<int>()）。clang 对应 SemaTemplateDeduction 收尾处的
//   "unable to deduce" 诊断。
//
// 期望报错（退出码 1）：
//   non-deduced context: template parameter 'T' cannot be deduced ...
// =============================================================================

template<typename T>
T gen() {
    return 0;
}

int main() {
    return gen();
}
