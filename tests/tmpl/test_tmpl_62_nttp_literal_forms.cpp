// =============================================================================
// 测试：NTTP 实参的【字面量形态】——进制前缀与整型后缀
// =============================================================================
// 考察理论点：
//   · [lex.icon]/2 的整数字面量类型表：字面量的**类型**由三件事共同决定 ——
//     进制前缀（十进制 / `0x` 十六进制 / `0` 八进制 / `0b` 二进制）、
//     后缀（`u`/`l`/`ll`，大小写随意，可组合成 `ul`/`llu`…）、以及值本身
//     是否落进该进制下候选类型表的第一档。
//       4     → int
//       4L    → long          （后缀升格）
//       4u    → unsigned int  （后缀改变符号性）
//       0x10  → int 值 16     （前缀只改【读法】不改类型）
//       010   → int 值 8      （八进制）
//       0b101 → int 值 5      （二进制，C++14）
//       1'000 → int 值 1000   （数字分隔符 '，C++14；仅词法层面，不进值）
//   · ★ **词法与语义的分工**：Lexer 只负责把字面量切成一个 token（不解释值、
//     不判断类型）；解释成 `(value, type)` 二元组是 Parser 的活
//     （Parser::parseIntLiteral，对应 clang 的 NumericLiteralParser）。
//     所以本测试盯的是 `[parse:targ] … (形态 X)` 里的 **X**。
//   · ★ **归一**：[temp.arg.nontype]/1 的 converted constant expression 允许
//     整型转换，于是 `L<4>`、`L<4L>`、`L<4l>` 三者在 `template<long N>` 下是
//     **同一个实例** —— 见日志里三次 `[instantiate:name]` 只有一份 `L_4`。
//     clang 同样认它们是同一实例（`template struct L<4>; template struct L<4L>;`
//     报 duplicate explicit instantiation）。
//
// 预期行为：编译通过，程序返回 7。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:targ] ★ non-type argument (NTTP): integer literal 4 (形态 long)
//     [parse:targ] ★ non-type argument (NTTP): integer literal 16 (形态 int)
//     [parse:targ] ★ non-type argument (NTTP): integer literal 8 (形态 int)    ← 010
//     [parse:targ] ★ non-type argument (NTTP): integer literal 1000 (形态 int) ← 1'000
//   Phase 3 [Sema]（形态与形参不符时才打）:
//     [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'4'(long) ⇒ 'long'（…）← 不会出现，形态已相等
//   Phase 4 [Instantiation]:
//     ║ Instance:  L_4        ← 4L / 4l / 4 三个写法只落这一份
//     ║ Mangled: L_4 → _Z1LILl4EE
//     ║ Mangled: Uu_4 → _Z2UuILj4EE     ← 后缀 u ⇒ j = unsigned int
//
// clang 核对（数值语义，用 clang++-18 直接跑）：
//   $ clang++-18 -std=c++20 test_tmpl_62_nttp_literal_forms.cpp -o t && ./t; echo $?
//   → 7
// =============================================================================

template<long N>
struct L {
    long get() { return N; }
};

template<unsigned U>
struct Uu {
    unsigned get() { return U; }
};

template<int N>
struct I {
    int get() { return N; }
};

int main() {
    L<4L>  a;   // 4L  → long
    L<4l>  b;   // 4l  → long（小写后缀等价；且与 L<4L> 归一成同一实例）
    Uu<4u> c;   // 4u  → unsigned int
    Uu<4U> d;   // 4U  → unsigned int（同上）
    I<0x10> e;  // 16
    I<0b101> f; // 5
    I<010>  g;  // 8   ← 八进制，不是十进制的 10
    I<1'000> h; // 1000 ← 数字分隔符

    return a.get() + b.get() + c.get() + d.get()
         + e.get() + f.get() + g.get() + h.get() - 1038;
    //   4+4+4+4+16+5+8+1000 = 1045 ;  1045 - 1038 = 7
}
