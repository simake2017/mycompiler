// =============================================================================
// 测试：【正例】值实参到形参类型的整型转换（[temp.arg.nontype]/1 + [expr.const]/10）
// =============================================================================
// ★ 回归说明：本文件的三个用法此前**全被拒收**（BUGS.md B17）。旧实现要求
//   实参形态与形参类型**精确相等**（`a.valueType->equals(p.nonType)`），
//   而标准要的是"转换后常量表达式"（converted constant expression）：
//   [temp.arg.nontype]/1 规定实参须是形参类型的 converted constant expression，
//   该定义（[expr.const]/10）直接引用 [dcl.init]/7 的初始化规则，而 [dcl.init]/7
//   对**常量表达式**豁免了"值恰好装得下"之外的窄化禁令 ——
//   于是判据是【能不能无损表示】，不是【形态是否字面相等】。
//
// 考察理论点：
//   · `Flag<1>`   —— 形参 bool，实参是 int 字面量 1。int → bool 的转换在
//       [conv.bool] 下合法（1 ⇒ true），且值 1 恰在 bool 的可表示集合 {0,1} 内 ⇒ 合法。
//   · `A<4L>`     —— 形参 int，实参是 long 字面量 4。long → int 是 [conv.integral]
//       的整型转换，值 4 在 int 范围内 ⇒ 合法。★ 这正是"形态不等但合法"的核心例子。
//   · `A<true>`   —— 形参 int，实参是 bool 字面量 true。bool → int 是 [conv.prom]
//       的整型提升（0/1），恒不窄化 ⇒ 合法。
//   · ★ 三例的**归一**：通过判据后实参形态被改写成形参类型（"③-c 形态归一"），
//       于是 `A<4L>` 与 `A<4>` 共用同一个实例 —— 日志里只有一份 `A_4`。
//       clang 亦然（`template struct A<4>; template struct A<4L>;` 报 duplicate）。
//
// 预期行为：编译通过，程序返回 7。
//
// 编译过程中的关键日志（Phase 3 [Sema]）：
//   [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'1'(int) ⇒ 'bool'（转换后常量表达式）
//   [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'4'(long) ⇒ 'int'（转换后常量表达式）
//   [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'1'(bool) ⇒ 'int'（转换后常量表达式）
//
// clang 核对：
//   $ clang++-18 -std=c++20 -fsyntax-only test_tmpl_64_nttp_integral_conversion.cpp
//   → 通过，零诊断（旧实现三条全报 "cannot be narrowed to type '…'"）
//   $ clang++-18 -std=c++20 test_tmpl_64_nttp_integral_conversion.cpp -o t && ./t; echo $?
//   → 7
// =============================================================================

template<bool B>
struct Flag {
    // 不用三元 `?:`（本项目尚未实现，见 ROADMAP 主线 C）
    int get() {
        if (B) { return 100; }
        return 0;
    }
};

template<int N>
struct A {
    int get() { return N; }
};

int main() {
    Flag<1> f;      // int 1 ⇒ bool：值域 {0,1} 内 ⇒ 合法（旧实现：拒收 ✗）
    A<4L>   a;      // long 4 ⇒ int：值域内 ⇒ 合法（旧实现：拒收 ✗）
    A<true> b;      // bool ⇒ int：整型提升，恒不窄化（旧实现：拒收 ✗）
    A<4>    c;      // int 4 ⇒ int：形态相等，不过转换

    return f.get() + a.get() + b.get() + c.get() - 102;
    //   100      + 4        + 1        + 4        = 109 ;  109 - 102 = 7
}
