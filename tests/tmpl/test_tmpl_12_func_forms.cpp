// =============================================================================
// test_tmpl_12_func_forms.cpp —— S1 边界：多参数与各种参数形态
// =============================================================================
// 考察理论点：模板参数表与函数参数表的两个"层"——
//   template<...> 是模板参数层（编译期类型变量），(...) 是函数参数层。
//   P/A 配对推导（S2）就发生在函数参数层的类型模式上，
//   所以 S1 必须正确解析 T / T& / T* / const T& 等形态为类型模式。
//
// 预期行为：三个函数模板全部解析成功：
//   pick<T,U>(T, U) → T            多模板参数
//   addr_of<T>(T&) → T*            左值引用参数 + 指针返回
//   kind_tag<T>(const T&) → int    const 引用参数（typename 与 class 关键字混用）
//   候选集日志：'pick' size=1, 'addr_of' size=1, 'kind_tag' size=1
//   编译全程退出码 0。
// =============================================================================

template<typename T, typename U>
T pick(T a, U b) {
    return a;
}

template<typename T>
T* addr_of(T& x) {
    return nullptr;
}

template<class T>
int kind_tag(const T& v) {
    return 1;
}

int main() {
    return 0;
}
