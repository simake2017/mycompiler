// =============================================================================
// 测试：NTTP 的形参【类型域】——整型家族 (Integral Type Family)
// =============================================================================
// 考察理论点：
//   · [temp.param]/6 允许的非类型形参类型是"结构化类型"（structural type），
//     整型家族（含 bool / 各字符类型）是其中最基本的一档。此前的实现只认
//     字面关键字 `int`，`unsigned N` / `long L` / `char C` / `short S` 一律解析失败。
//   · [basic.fundamental]/2、/7：`char` / `signed char` / `unsigned char` 是
//     **三个不同的类型**（三个不同的 TypeKind），不是同一个类型的别名 ——
//     `D<'a'>` 与 `D<(signed char)'a'>` 在 clang 下是两份不同的实例。
//   · 形参类型的解析走的是 [dcl.type.simple] 的 type-specifier-seq：
//     多关键字顺序任意、可重复（`long long int` / `unsigned long long` /
//     `long unsigned`），由 Parser 收集后归一（parseBuiltinTypeSpecifierSeq）。
//
// 预期行为：编译通过，程序返回 7。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:type] type-specifier-seq: 'unsigned long' ⇒ base = unsigned long  (无符号，…)
//     [parse:type] type-specifier-seq: 'long unsigned' ⇒ base = unsigned long  (无符号，…)
//       ★ 两种写法归一成同一个 base —— `unsigned long` 与 `long unsigned` 同类型
//     [parse:template]   ★ non-type parameter registered: unsigned long 'N'
//   Phase 4 [Instantiation]:
//     ║ Mangled: UB_8 → _Z2UBILm8EE      ← m = unsigned long  [<builtin-type>]
//     ║ Mangled: UL_9 → _Z2ULILm9EE      ← 同上，m
//     ║ Mangled: SB_2 → _Z2SBILs2EE      ← s = short
//     ║ Mangled: DB_97 → _Z2DBILc97EE    ← c = char
//     ║ Mangled: LB_3 → _Z2LBILx3EE      ← x = long long
//
// mangling 已用 clang++-18 核对：
//   $ clang++-18 -std=c++20 -c test_tmpl_61_nttp_integer_family.cpp -o t.o && nm t.o
//   → _ZN2UBILm8EE3getEv / _ZN2SBILs2EE3getEv / _ZN2DBILc97EE3getEv
//     （ILm8EE / ILs2EE / ILc97EE 三段与本实现逐字符一致）
// =============================================================================

// 多关键字写法：`unsigned long` 与 `long unsigned` 是同一个类型
template<unsigned long N>
struct UB {
    long get() { return N; }
};

template<long unsigned N>
struct UL {
    long get() { return N; }
};

template<short S>
struct SB {
    int get() { return S; }
};

template<char C>
struct DB {
    int get() { return C; }
};

// `long long int` 三段写法
template<long long int LL>
struct LB {
    long long get() { return LL; }
};

int main() {
    UB<8> a;            // 8 (int) ⇒ unsigned long：整型转换
    UL<9> b;            // 9 (int) ⇒ unsigned long：与 UB 是同一个类型
    SB<2> c;
    DB<'a'> d;          // 97
    LB<3> e;

    return a.get() + b.get() + c.get() + d.get() + e.get() - 112;
    //   8        + 9        + 2        + 97       + 3        - 112 = 7
}
