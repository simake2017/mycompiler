// =============================================================================
// 测试：常量引用模板 (Const Reference Template)
// =============================================================================
// 展示 const T& 在模板中的使用——最常用的函数参数传递方式。
//
// 编译过程中的关键日志：
//   Phase 4 [Instantiation]:
//     当 T = int 时：
//       field 'value' : const T& → const int&
//       [subst] Const(const T&) → recursing into inner type...
//       [subst] LValueRef(T&) → recursing into referenced type...
//       [subst] ★ TemplateParam 'T' → 'int' (direct replacement)
//       [subst] ★ LValueRef substituted: T& → int&
//       [subst] ★ Const substituted: const T& → const int&
//
//     当 T = int* 时：
//       field 'value' : const T& → const int*&
//       const 包裹在指针引用外层
// =============================================================================

template<typename T>
class ConstRefBox {
public:
    const T& value;

    const T& get() {
        return value;
    }
};

int add(int a, int b) {
    return a + b;
}

int main() {
    auto result = add(1, 2);
    return 0;
}
