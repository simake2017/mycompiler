// =============================================================================
// 测试：模板模板参数 (Template Template Parameter, [temp.param]/4)
// =============================================================================
// 考察理论点：
//   · 模板形参有三种形态——类型（typename T）、值（int N）、**模板**（template<class> class C）。
//     第三种接受的实参是【一个模板】而不是类型或值，标准依据
//     [temp.param]/4（形参声明）与 [temp.arg.template]（实参匹配）。
//   · 引入的是一条【二级替换】：模板体内写 `C<T>`，C 先是形参、实例化时被换成
//     实参给的模板名，于是 `C<T>` → `Box<T>`，再由既有的"落地解析"把它兑现成
//     `Box_int`。★ 替换完仍是半成品，不是具体类型 —— 与模板体内直写 `Box<T>` 同路。
//   · 实参在 Parser 眼里长得像类型（裸名 `Box` 被建成 Class("Box")），形态判定
//     只能由 Sema 逐位看形参表做出（与 NTTP 名的还原是同一类问题，见 docs/learn/27 §3.3）。
//
// 预期行为：
//   · Wrap<Box, int>   ⇒ inner 的类型是 Box<int>，可读写 value
//   · Wrap<Two, int>   ⇒ 同一个 wrapper 接另一个模板，inner 有 first/second
//   · Repeat<Box, 5>   ⇒ 模板模板位与 NTTP 位【混排】也各就各位
//   程序返回 0（bad = 第一个失败的用例编号）。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ template template parameter registered: 'C' (accepts a template with 1 parameter(s), [temp.param]/4)
//   Phase 3 [Sema]:
//     [sema:targ]   ⤷ 第 1 位期望模板 ⇒ 实参 'Box' 按【模板名】处理（[temp.arg.template]）
//   Phase 4 [Instantiation]:
//     [subst] ★ 模板模板形参 'C' → 模板 'Box'：C<int> → Box<int>
//     ║ Mangled: Wrap_Box_int → _Z4WrapI3BoxiE
//
// mangling 已用 clang++-18 核对：
//   void f(Wrap<Box, int>*) {}  ⇒  _Z1fP4WrapI3BoxiE
//   （模板模板实参按 <name> 直接编码：`3Box`，不套 X…E —— 因为它是简单模板名）
//   本实现只编码到类名一层：`4WrapI3BoxiE`，与 clang 的类名段逐字符相同。
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_53_template_template_param.cpp -o /tmp/t53 && /tmp/t53; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_53_template_template_param.cpp -o /tmp/t53c && /tmp/t53c; echo $?
// =============================================================================

// ── 接受"一个单形参类模板"的 wrapper ──
template <template <class> class C, class T>
struct Wrap {
    C<T> inner;            // ★ C 是形参：这一行在实例化时被换成 Box<int> / Two<int>
};

template <class T>
struct Box {
    T value;
};

template <class T>
struct Two {
    T first;
    T second;
};

// ── 模板模板位与 NTTP 位混排 ──
template <template <class> class C, int N>
struct Repeat {
    C<int> item;
    int    count;
};

int main() {
    int bad = 0;

    // 1. 基本形态：C := Box
    Wrap<Box, int> w;
    w.inner.value = 7;
    if (w.inner.value != 7) bad = 1;

    // 2. 同一个 wrapper 接另一个模板：C := Two
    Wrap<Two, int> p;
    p.inner.first  = 3;
    p.inner.second = 4;
    if (p.inner.first + p.inner.second != 7) bad = 2;

    // 3. 混排：第 0 位是模板、第 1 位是值
    Repeat<Box, 5> r;
    r.item.value = 2;
    r.count      = 5;
    if (r.item.value + r.count != 7) bad = 3;

    return bad;
}
