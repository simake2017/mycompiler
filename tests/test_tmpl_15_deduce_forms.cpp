// =============================================================================
// test_tmpl_15_deduce_forms.cpp —— S2 边界：引用/const/万能引用形态推导
// =============================================================================
// 考察理论点：
//   1. T& 参数要求左值实参（rvalue 会被拒绝）
//   2. const T& 推导时剥顶层 const（P 侧调整）
//   3. T&& 万能引用：右值实参 ⇒ T := int；（左值实参 ⇒ T := int& 并折叠，
//      本用例只测右值路径，折叠路径在类模板 demo 中已验证）
//   4. 值传递调整：裸 T 参数剥离实参的顶层引用/const
//
// 预期行为：三个模板各推导一次，退出码 = 0
//   echo:  P=T&      A=int(lvalue)  ⇒ T := int
//   tag:   剥 const  → P=T& A=int(lvalue) ⇒ T := int
//   fwd:   P=T&&     A=int(rvalue)  ⇒ T := int
// =============================================================================

template<typename T>
T& echo(T& x) {
    return x;
}

template<typename T>
int tag(const T& v) {
    return 7;
}

template<typename T>
T&& fwd(T&& x) {
    return x;
}

int main() {
    int a = 35;
    int r1 = echo(a);   // 35
    int r2 = tag(a);    // 7
    int r3 = fwd(0);    // 0
    return r1 + r2 + r3 - 42;
}
