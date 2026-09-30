// =============================================================================
// 测试：`template<auto V>` —— 类型由实参反推的非类型形参（[temp.param]/6）
// =============================================================================
// 考察理论点：
//   · [temp.param]/6 的 placeholder 形参：`auto V` 声明一个"类型待定"的
//     非类型形参，其类型由**实参**在推导时确定（deduced non-type parameter）。
//     它对应的不是"某个整型"，而是**一整族**整型 —— 故本测试里 `K<4>` 与
//     `K<4L>` 是**两个不同的实例**，这正是 auto 形参存在的意义。
//   · ★ 形参表里 `auto` 与 `int` 的区别，只有在**实例化之后**才看得见：
//       template<int N>  struct X;   X<4> 与 X<4L>  ⇒ 同一个实例（转成 int）
//       template<auto V> struct K;   K<4> 与 K<4L>  ⇒ 两个实例（保留形态）
//     minicc 的实例缓存键与汇编符号名必须能区分这两者 —— 见下"易漏点"。
//   · ★ **易漏点（本轮修掉的静默 bug）**：实例名 / 缓存键此前由
//     `TemplateArg::toString()` 生成，而它**不带形态** ⇒ `K<4>` 与 `K<4L>`
//     双双得到键 `"K<4>"`、实例名 `K_4`，第二个会**静默复用**第一个实例。
//     修法是让键与实例名都走 `NameMangler::losslessArgumentsKey` /
//     `renderArgLossless`（同一份函数，形态只在 `auto` 形参位才写入），
//     于是实例名变成 `K_4Cint` / `K_4Clong`（`C` 是 `:` 的安全转义）。
//     这与 docs/learn/23 的教训同源：**拿"给人看的字符串"当机器用的键，早晚出事。**
//   · 全特化 `template<> struct K<4>` 只匹配 **int 形态**的 `K<4>`，
//     不匹配 `K<4L>` —— [temp.expl.spec] 的实参等同性按类型判定，
//     `4` 与 `4L` 的转换后类型分别是 int / long，不相等。
//
// 预期行为：编译通过，程序返回 7。日志里 `K<4>` 与 `K<4L>` **各自成实例**，
//   且 `K<4>` 走全特化体、`K<4L>` 走主模板体 —— 两条独立证据都在同一个可执行文件里。
//
// 编译过程中的关键日志：
//   Phase 3 [Sema]:
//     [sema:targ]   ⤷ auto 形参推导（[temp.param]/6）：'V' := int （类型由实参反推）
//     [sema:targ]   ⤷ auto 形参推导（[temp.param]/6）：'V' := long （类型由实参反推）
//   Phase 4 [Instantiation]:
//     ║ Instance:  K_4Cint      ║ Mangled: K_4Cint → _Z1KILi4EE
//     ║ Instance:  K_4Clong     ║ Mangled: K_4Clong → _Z1KILl4EE
//     ║ Instance:  K_120Cchar   ║ Mangled: K_120Cchar → _Z1KILc120EE
//     ║ Instance:  K_1Cbool     ║ Mangled: K_1Cbool → _Z1KILb1EE
//     [spec:select] ① explicit specialization matched (exact argument equality) → USING IT
//     [spec:select] ① no explicit specialization matched   ← 这一条是 K<4L>
//
// clang 核对：
//   $ clang++-18 -std=c++20 -c test_tmpl_68_nttp_auto_param.cpp -o t.o && nm t.o
//   → _ZN1KILi4EE… / _ZN1KILl4EE… / _ZN1KILc120EE… / _ZN1KILb1EE…
//     （ILi4EE / ILl4EE / ILc120EE / ILb1EE 四段与本实现逐字符一致）
//   $ clang++-18 -std=c++20 test_tmpl_68_nttp_auto_param.cpp -o t && ./t; echo $?
//   → 7
// =============================================================================

template<auto V>
struct K {
    int tag() { return 0; }     // 主模板标记
};

// 全特化：只吃掉 **int 形态**的 4
template<>
struct K<4> {
    int tag() { return 100; }
};

template<auto V>
struct W {
    long get() { return V; }
};

int main() {
    K<4>   a;   // int 形态  ⇒ 命中全特化
    K<4L>  b;   // long 形态 ⇒ 主模板（与上面是**两个实例**）
    K<'x'> c;   // char 形态
    K<true> d;  // bool 形态
    W<4>   e;
    W<4L>  f;

    //  a.tag() = 100（全特化）   b/c/d.tag() = 0（主模板）
    //  e.get() = 4（int 形态）   f.get() = 4（long 形态）
    return a.tag() + b.tag() + c.tag() + d.tag() + e.get() + f.get() - 101;
    //   100       + 0        + 0        + 0        + 4        + 4        = 108
    //   108 - 101 = 7
}
