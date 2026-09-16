// =============================================================================
// tests/tmpl/test_tmpl_51_ctad_and_guides.cpp
// =============================================================================
// 考察理论点：类模板实参推导 CTAD + 推导指引（[dcl.type.class.deduct] /
//             [temp.deduct.guide]）
//
//   · **CTAD 是什么**：`MyPtr m(7);` 没写 `<...>`，编译器拿**构造实参的类型**
//     反推类模板形参。它是**函数模板实参推导的反向使用** ——
//     同一套合一算法，只是被推的"模式"来自构造函数形参表：
//         f(T x)   ← 从实参类型推【函数模板】形参
//         MyPtr(T) ← 从构造实参推【类模板】形参   （那个 f 就是构造函数）
//
//   · **只在直接初始化触发**（[dcl.type.class.deduct]/1）：`MyPtr m(7);` 触发，
//     `MyPtr<int> m(7);` 不触发（已写实参），`MyPtr m = 7;` 也不触发。
//     所以本用例顺带钉住 `Type name(args);` 这条语法 —— 此前 Parser 不认，
//     直接报 "Expected ';' after variable declaration"。
//
//   · **显式指引优先于构造函数**（[temp.deduct.guide]/1）：指引存在的意义
//     就是"改写默认映射规则"。最典型的用途是补上构造函数**推不出来**的形参。
//
//   · ★ **三个判别性检查点**（任一失效都会让本用例换成别的返回码）：
//       ① CTAD-1：靠【构造函数】隐式指引推出 T
//       ② CTAD-2：靠【模板推导指引】`template<class T> Box(T) -> Box<T>;` 推出
//       ③ CTAD-3：靠【非模板指引】`Two(int) -> Two<int,int>;` 补上构造函数
//          根本推不出的第二个形参 —— 没有这条指引，CTAD 必须失败
//          （clang 同款报错：no viable constructor or deduction guide）
//
// 预期行为：退出码 173（已用 clang++-18 -std=c++20 核对，clang 173）
//   c1 = MyPtr(7)   ⇒ MyPtr<int>   ，取 field            = 7
//   c2 = Box(8)     ⇒ Box<int>（模板指引），取 field     = 8
//   c3 = Two(9)     ⇒ Two<int,int>（非模板指引），a+b     = 9 + 10 = 19 ?
//   见文件尾的退出码推导。
//
// 运行：
//   ./minicc tests/tmpl/test_tmpl_51_ctad_and_guides.cpp -o /tmp/t51 && /tmp/t51; echo $?
//
// 关键日志（可复现）：
//   [parse:guide] ★ deduction guide: Box(T) -> Box<T>
//   [register] deduction guide for 'Box' (#1) registered
//   [ctad] ▶ m MyPtr(int) —— 未写模板实参，尝试类模板实参推导
//   [ctad] ✔ 由构造函数推出 ⇒ MyPtr<int>
//   [ctad] ✔ 命中推导指引 ⇒ Box<int>
// =============================================================================

// ── ① 构造函数隐式指引：T 出现在构造参数里，可直接推 ──
template <class T>
struct MyPtr {
    T value;
    MyPtr(T q) : value(q) {}
};

// ── ② 模板推导指引：写法与隐式规则一致，用来观察"走的是指引这条路" ──
template <class T>
struct Box {
    T value;
    Box(T q) : value(q) {}
};
template <class T>
Box(T) -> Box<T>;

// ── ③ 非模板指引：构造函数【推不出】U，只能靠指引写死 ──
template <class T, class U>
struct Two {
    T a;
    U b;
    Two(T x) : a(x) {}
};
Two(int) -> Two<int, int>;

// ── ④ 带参构造也能写进函数模板形参，且随 CTAD 一起工作 ──
template <class T>
T readOf(MyPtr<T> p) {
    return p.value;
}

int main() {
    // ① CTAD-1：由构造函数推出 MyPtr<int>
    MyPtr m(7);
    if (m.value != 7) return 91;

    // ② CTAD-2：由模板推导指引推出 Box<int>
    Box b(8);
    if (b.value != 8) return 92;

    // ③ CTAD-3：由非模板指引补出 Two<int, int>
    //    没有这条指引，U 无处可推 ⇒ 编译失败（clang 同样拒绝）
    Two t(9);
    t.b = 10;
    if (t.a != 9)  return 93;
    if (t.b != 10) return 94;

    // ④ CTAD 推出来的类型参与普通函数模板推导
    int r = readOf(m);
    if (r != 7) return 95;

    // ⑤ 已写实参时【不】走 CTAD（老路径），行为必须与从前一致
    MyPtr<int> explicitOne(11);
    if (explicitOne.value != 11) return 96;

    return t.a + t.b + m.value + b.value + r + 132;   // 9+10+7+8+7+132 = 173
}

// ★ 退出码推导（与 clang 逐位核对过；改动任一常量都会失配）：
//   t.a=9, t.b=10, m.value=7, b.value=8, r=7
//   9 + 10 + 7 + 8 + 7 = 41；再加 132 = 173
//   真实考察点全在 91~96 那几个提前返回分支里 —— 任一断言不成立就换码。
