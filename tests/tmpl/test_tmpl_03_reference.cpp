// =============================================================================
// 测试：左值引用与右值引用模板 (LValue & RValue Reference Templates)
// =============================================================================
// 展示 T& 和 T&& 在模板中的使用。
//
// 编译过程中的关键日志：
//   Phase 4 [Instantiation]:
//     当 T = int 时：
//       field 'ref'  : T& → int&
//       field 'rref' : T&& → int&&
//       [subst] LValueRef(T&) → recursing into referenced type...
//       [subst] ★ TemplateParam 'T' → 'int' (direct replacement)
//       [subst] ★ LValueRef substituted: T& → int&
//       [subst] RValueRef(T&&) → recursing into referenced type...
//       [subst] ★ RValueRef substituted: T&& → int&&
//
//     当 T = int& 时（万能引用触发引用折叠！）：
//       field 'ref'  : T& → int& & → int&   (左值引用胜出)
//       field 'rref' : T&& → int& && → int&  (左值引用永远赢！)
//       [subst] ★ Reference collapsing: int&&& → int& (lvalue ref wins!)
// =============================================================================

template<typename T>
class RefHolder {
public:
    T&  ref;
    T&& rref;

    T& getRef() {
        return ref;
    }

    T&& getRRef() {
        return rref;
    }

    void setRef(T& r) {
        ref = r;
    }

    void setRRef(T&& rr) {
        rref = rr;
    }
};

int main() {
    auto x = 10;
    return 0;
}
