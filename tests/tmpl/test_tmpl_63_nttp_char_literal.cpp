// =============================================================================
// 测试：字符字面量作 NTTP 实参（[lex.ccon] × [temp.arg.nontype]）
// =============================================================================
// 考察理论点：
//   · [lex.ccon]：字符字面量 `'a'` 的类型是 **char**（不是 int —— C 里才是 int），
//     值是"执行字符集里该字符的码点"。转义序列（simple-escape-sequence）
//     在**词法期**就翻译完毕：`'\n'` 的 token 正文是真正的 0x0A 字节，
//     而不是两个字符 `\` `n`。★ 故 Lexer 产出的 text 是"已翻译"的。
//   · 多字符字面量 `'ab'`（[lex.ccon]/2 的 conditional 支持）：类型是 int，
//     值由实现定义；GCC/Clang 的取法是"最后一个字符在最低字节"，即
//     `'ab'` = ('a' << 8) | 'b' = 24930。本实现对齐这一取法。
//   · 形态落点：单字符字面量 → `TemplateArg{value, char}`，
//     编码 `_Z1DILc97EE`（c = char）；多字符字面量按 int 走 → `ILi24930EE`。
//   · ★ 字符 → char 是**恒等变换**（形态已相等），故不打归一日志；
//     而 `D<300>` 这种装不下的会走 [dcl.init]/7 的窄化判据被拒（见 test_tmpl_67）。
//
// 预期行为：编译通过，程序返回 7。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:targ] ★ non-type argument (NTTP): char literal 'a' (97)
//     [parse:targ] ★ non-type argument (NTTP): char literal '\n' (10)
//       ★ 回吐时重新转义：日志里看到的是 `\n` 两个字符，而不是一个被换行截断的行
//     [parse:targ] ★ non-type argument (NTTP): char literal '\\' (92)
//     [parse:targ] ★ non-type argument (NTTP): char literal 'ab' (24930)
//   Phase 4 [Instantiation]:
//     ║ Mangled: D_97 → _Z1DILc97EE      ← c = char
//     ║ Mangled: D_10 → _Z1DILc10EE
//     ║ Mangled: I_24930 → _Z1IILi24930EE
//
// clang 核对：
//   $ clang++-18 -std=c++20 -c test_tmpl_63_nttp_char_literal.cpp -o t.o && nm t.o
//   → _ZN1DILc97EE3getEv …（ILc97EE 与本实现逐字符一致）
//   $ clang++-18 -std=c++20 -w - <<< 'template<int N> struct I{int g(){return N;}};
//       int main(){I<'"'"'ab'"'"'> x; return x.g();}'   → 退出码 98 = 24930 & 0xFF ✅
// =============================================================================

template<char C>
struct D {
    int get() { return C; }
};

template<int N>
struct I {
    int get() { return N; }
};

int main() {
    D<'a'>   a;   // 97
    D<'Z'>   b;   // 90
    D<'\n'>  c;   // 10   ← 转义在【词法期】已翻译成 0x0A
    D<'\t'>  d;   // 9
    D<'\\'>  e;   // 92
    D<'\''>  f;   // 39
    I<'ab'>  g;   // 24930 ← 多字符字面量：int 形态

    // 97+90+10+9+92+39 = 337；`(g.get() - 24930)` 恒为 0，但它把
    // 「'ab' 的实现定义值 = 24930」这条断言也钉进了可执行文件里。
    return a.get() + b.get() + c.get() + d.get() + e.get() + f.get()
         + (g.get() - 24930) - 330;
    //   337 + 0 - 330 = 7
}
