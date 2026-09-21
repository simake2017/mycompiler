// ============================================================================
// demo 02 —— 引用折叠：同一个 T&& 抄三遍，结果却是三种类型
// ============================================================================
// 理论：[dcl.ref]/6 —— 引用套引用时按规则折叠，**只要有一个是左值引用，
//   结果就是左值引用**：
//
//     T&  &   → T&       T&  &&  → T&
//     T&& &   → T&       T&& &&  → T&&
//
// 这是「万能引用（forwarding reference）」的全部机关：形参写 T&&，
// T 被推成 int& 时 T&& 就折叠成 int& —— 于是同一个写法既能接左值又能接右值。
//
// 看折叠发生的那一步：
//   ./build-linux/minicc demos/tmpl/02_reference_collapsing.cpp -S -o /tmp/demo02.s
// 终端可见（三次实例化，对照着读）：
//   [instantiate:class] ★ on-demand instantiation: Forwarder<int&>
//   ║   field 'data' : T&& →
//     [subst] ★ TemplateParam 'T' → 'int&' (direct replacement)
//     [subst] ★ Reference collapsing: int&&& → int& (lvalue ref wins!)     ← ★ 折叠
//   同样地 int&& ⇒ [subst] ★ Reference collapsing: int&&&& → int&& (rvalue ref stays)
//
// 预期：退出码 0
// ============================================================================

template <class T>
class Forwarder {
public:
    T&& data;      // 抄的是同一个 T&& —— 折叠结果全看 T 绑成了什么
};

int main() {
    Forwarder<int>   a;    // T = int   ⇒ int&&   （纯右值引用）
    Forwarder<int&>  b;    // T = int&  ⇒ int& && → int&   ★ 折叠
    Forwarder<int&&> c;    // T = int&& ⇒ int&& && → int&&  ★ 保持
    return 0;
}
