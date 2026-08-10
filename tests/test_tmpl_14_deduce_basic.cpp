// =============================================================================
// test_tmpl_14_deduce_basic.cpp —— S2 正例：基础实参推导 + 实例化 + 执行
// =============================================================================
// 考察理论点：P/A 配对（[temp.deduct.call]）——参数模式 T 与实参类型 int
//   合一出 T := int；推导结果驱动实例化（S5），生成 twice<int>。
//
// 预期行为：
//   [deduction] P=T A=int ⇒ T := int，推导成功 <T=int>
//   [instantiate] _Z5twiceIiE 生成，callq 重写为 mangled 符号
//   汇编链接执行后退出码 = 0（twice(21) = 42）
//
// oracle 对照：
//   clang++-18 tests/test_tmpl_14_deduce_basic.cpp -o /tmp/t14 && /tmp/t14; echo $?
// =============================================================================

template<typename T>
T twice(T x) {
    return x + x;
}

int main() {
    return twice(21) - 42;
}
