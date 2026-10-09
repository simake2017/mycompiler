// =============================================================================
// 测试（错误用例）：右值引用不能绑定左值（[dcl.init.ref]/5.3）
// =============================================================================
// 理论点：
//   · 右值引用 `T&&` 只能绑右值；唯一例外是**模板形参**的 T&&（转发引用），
//     它经引用折叠后左右值皆可 —— 而本文件里的 `int&&` 是非模板形参，
//     没有折叠余地，绑左值就是 ill-formed。
//   · 与 [dcl.init.ref]/5 的const 左值引用互补：const T& 能"多收"右值，
//     T&& 不能"多收"左值。本项目此前两半都漏（BUGS.md B25）。
// 预期行为：rc=1，报错文案
//   no matching function for call to 'takeRvalue' — cannot bind rvalue reference
//   'int&&' to an lvalue ('int')
// 对照 clang：clang++-18 -std=c++20 同样 rc=1
//   （`cannot bind rvalue reference of type 'int&&' to lvalue of type 'int'`）。
// =============================================================================

int takeRvalue(int&& x) { return x; }

int main() {
    int a = 1;
    return takeRvalue(a);   // ✗ a 是左值
}
