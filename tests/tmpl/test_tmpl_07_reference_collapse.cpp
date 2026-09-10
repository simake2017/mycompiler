// =============================================================================
// 测试：引用折叠 (Reference Collapsing)
// =============================================================================
// ★ 万能引用(Forwarding Reference)的核心机制 ★
//
// C++ 标准规定，当引用嵌套时按以下规则折叠：
//   T&  &   → T&    (左值引用 + 左值引用 → 左值引用)
//   T&  &&  → T&    (左值引用 + 右值引用 → 左值引用)
//   T&& &   → T&    (右值引用 + 左值引用 → 左值引用)
//   T&& &&  → T&&   (右值引用 + 右值引用 → 右值引用)
//
// 简言之：只要有一个是左值引用，结果就是左值引用。
//
// 编译过程中的关键日志：
//   Phase 4 [Instantiation]:
//
//   实例化 Forwarder<int>:
//     field 'data' : T&& → int&&  (普通右值引用)
//     [subst] RValueRef(T&&) → recursing...
//     [subst] ★ RValueRef substituted: T&& → int&&
//
//   实例化 Forwarder<int&>:
//     field 'data' : T&& → int& && → int&  (折叠为左值引用！)
//     [subst] RValueRef(T&&) → recursing...
//     [subst] ★ TemplateParam 'T' → 'int&' (direct replacement)
//     [subst] ★ Reference collapsing: int&&& → int& (lvalue ref wins!)
//
//   实例化 Forwarder<int&&>:
//     field 'data' : T&& → int&& && → int&&  (保持右值引用)
//     [subst] RValueRef(T&&) → recursing...
//     [subst] ★ TemplateParam 'T' → 'int&&' (direct replacement)
//     [subst] ★ Reference collapsing: int&&&& → int&& (rvalue ref stays)
// =============================================================================

template<typename T>
class Forwarder {
public:
    T&& data;

    T&& get() {
        return data;
    }

    void set(T&& val) {
        data = val;
    }
};

int main() {
    auto x = 42;
    return 0;
}
