// =============================================================================
// 测试：模板模板实参的【逐位签名匹配】(Template Template Parameter signature
//       matching, [temp.arg.template]/3)
// =============================================================================
// 考察理论点：
//   · 模板模板形参 C 声明的是一张【内层形参表】（`template<template<class, int>
//     class C>` 的内层表 = [class, int]）。实参模板自己的形参表必须逐位对上：
//       ① 位数必须相同（少了 = too few；多了 = too many，**哪怕多出的位有默认
//          实参也不行** —— 旧标准的"多出的位有默认即可"已被 P0522R0 取代）
//       ② 逐位同 kind —— 类型 ↔ 类型、值 ↔ 值、模板 ↔ 模板
//       ③ 值位还要声明类型相同（`template<int>` 与 `template<unsigned>` 不匹配）
//   · ★ 三条口径全部以 clang++-18 -std=c++20 为 oracle 实测确定（探针见
//     docs/learn/33 §3.4）—— 其中第 ① 条的"多了也不行"是我第一版实现写错的：
//     照旧标准放行 `template<class> class` ← `template<class,class=int>`，
//     clang 会报 too many。
//   · ★ 它为什么必须存在：实参位上的 `Pair` 与 `int` 在 Parser 眼里【完全同形】
//     （都被建成 Class 节点），形态只有拿两张形参表对比才判得出。缺这道校验，
//     `Wrap2<Box, int>`（形参位要 2 位、Box 只有 1 位）会一路放行，直到替换出
//     `Box<int,int>` 才在下游报"Box 至多 1 个实参" —— 报是报了，但诊断指向的是
//     派生出来的假类型，不是根因（实参列表）。负向用例见 test_tmpl_58/59。
//
// 预期行为：
//   · Wrap2<Pair>  ⇒ 位数 2=2、逐位 [class, int] 对上
//   · Wrap3<Alias> ⇒ 别名模板作模板模板实参（C++17 起合法），形参表取自别名自身
//   · Wrap4<Rev>   ⇒ 形参顺序换个位（[int, class]）照样逐位对上才算匹配
//   程序返回 0（bad = 第一个失败的用例编号）。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ template template parameter registered: 'C'
//       (accepts a template with 2 parameter(s), [temp.param]/4)
//   Phase 3 [Sema]:
//     [sema:targ]   ⤷ 第 1 位期望模板 ⇒ 实参 'Pair' 按【模板名】处理（[temp.arg.template]）
//     [sema:targ]   ✓ 签名匹配: 'C' ← 'Pair'（2 位逐位一致，[temp.arg.template]/2）
//     [sema:targ]   ✓ param 1: 'C' (template) ← Pair
//
// mangling（模板模板实参按 <name> 直接编码）已用 clang++-18 核对：
//   void f(Wrap2<Pair>*) {}  ⇒  _Z1fP5Wrap2I4PairE
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_57_ttp_signature_match.cpp -o /tmp/t57 && /tmp/t57; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_57_ttp_signature_match.cpp -o /tmp/t57c && /tmp/t57c; echo $?
// =============================================================================

// ── ① 位数相同 + 逐位 kind 对上：内层表 [class, int] ← 实参 [class, int] ──
template <template <class, int> class C>
struct Wrap2 {
    C<int, 7> inner;
};

template <class T, int N>
struct Pair {
    T   first;
    int count;
};

// ── ② 别名模板作模板模板实参（C++17 起合法，[temp.alias]）──
//    形参表取自【别名自己】的模板形参表（不是被别名那个模板的）
template <class T, int N>
using Alias = Pair<T, N>;

template <template <class, int> class C>
struct Wrap3 {
    C<int, 5> inner;
};

// ── ③ 形参顺序也参与匹配：内层表 [int, class] ← 实参 [int, class] ──
template <template <int, class> class C>
struct Wrap4 {
    C<3, int> inner;
};

template <int N, class T>
struct Rev {
    T v;
};

int main() {
    int bad = 0;

    Wrap2<Pair> w2;             // 位数与 kind 都对上
    w2.inner.first = 3;
    w2.inner.count = 4;
    if (w2.inner.first + w2.inner.count != 7) bad = 1;

    Wrap3<Alias> w3;            // 别名模板：形参表取自别名自身 [class, int]
    w3.inner.first = 8;
    w3.inner.count = 9;
    if (w3.inner.first + w3.inner.count != 17) bad = 2;

    Wrap4<Rev> w4;              // 值位在前、类型位在后，逐位对齐
    w4.inner.v = 6;
    if (w4.inner.v != 6) bad = 3;

    return bad;
}
