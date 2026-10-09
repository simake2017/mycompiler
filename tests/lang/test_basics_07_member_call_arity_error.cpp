// =============================================================================
// 测试（错误用例）：成员调用的候选集非空但无可行者（[overload.match]/1）
// =============================================================================
// 理论点：
//   · [class.member.lookup] 产出**候选集**（按名字，含沿基类链），
//     [overload.best.viable] 才按实参筛。候选集非空而可行集为空 ⇒ ill-formed，
//     **不许**"退回候选集里第一个"。本文件是"个数不符"这一支。
//   · 名字隐藏（[class.member.lookup]/3）在此之上再砍一刀：派生类声明了 `f` 之后，
//     基类那一族同名函数**全部**被隐藏 —— 所以下面的 `d.f(7)` 不该去找 A::f(int)，
//     而应在 D 这一层就报"没有可行的重载"（BUGS.md B24 / B27）。
// 预期行为：rc=1，报错文案与 clang 逐字相同
//   too many arguments to function call, expected 0, have 1
// ★ 为什么不是笼统的 no matching member function：个数不符与类型不符是**两类**
//   诊断（[expr.call]/1 对 [overload.match]/1），clang 的 CheckFunctionCall 在调用点
//   分开报。本实现按"该类为这个名字声明过哪些参数个数"的区间决定说哪一句 ——
//   区间外说 too few/too many，区间内说 no matching。
// 对照 clang：clang++-18 -std=c++20 同样 rc=1 且文案一致（`too many arguments to
//   function call, expected 0, have 1`）。
// =============================================================================

struct A {
public:
    int f(int x) { return x; }     // 基类这一族在 D 里被隐藏
};

struct D : public A {
public:
    int f() { return 1; }          // 声明了名字 ⇒ 隐藏 A::f
};

int main() {
    D d;
    return d.f(7);   // ✗ 名字隐藏 + 个数不符，两层都错
}
