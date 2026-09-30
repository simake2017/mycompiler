// =============================================================================
// tests/lang/test_basics_02_declaratorless.cpp —— 空语句 + 无声明符的声明（bug B16）
// =============================================================================
// 预期行为：编译 rc=0，运行 rc=0
//
// 考察理论点：
//   ① [stmt]/1                 null statement（`;`）—— clang: NullStmt
//   ② [dcl.dcl]/1              simple-declaration 的 init-declarator-list 是【可选】的，
//                              故 `decl-specifier-seq ';'` 合法，语义是"什么都不声明"
//                              （clang 只给 -Wmissing-declarations 警告）
//   ③ ★ 两者都【不实例化任何模板】—— 裸语句不触发偏特化体实例化
//   ④ ★ 实例化成员函数体内的空语句必须被正确克隆（cloneStmt 不能落到 default）
//
// 修复前的症状（本文件此前根本编不过）：
//     A<int*, int**>;
//     ⇒ [Parse Error] 12:7 at 'int': Unexpected token 'int' in expression
//   报错位置指向实参中间，离根因（少了声明符）很远 —— 见 docs/BUGS.md B16。
//
// 观测约定：本项目不链接 libc（没有 printf），故用【退出码】汇总：
//     bad 累加"哪一项不符预期"，run-rc = 0 表示全过，
//     非 0 时其值就是失败项的编号（1..4），便于定位。
//
// 语义 oracle（clang 与本编译器必须一致）：
//     clang++-18 -std=c++20 tests/lang/test_basics_02_declaratorless.cpp -o /tmp/b2 && /tmp/b2; echo $?
//     clang 会给 4 条 -Wmissing-declarations 警告，但 rc=0 —— 警告不影响成败。
// =============================================================================

// ── ⓪ 带偏特化的类模板：用来验证"无声明符的声明不实例化" ────────────────────
// ★ 判据在 Sema 层：写 `A<int*, int**>;`（无声明符）时既不该有 [spec:select]
//   也不该有 [instantiate:class] —— 见单测 DeclaratorlessDecl.DoesNotInstantiateAnyTemplate。
//   本文件只从"能编过、能跑对"这一层兜底。
template<class a, class b>
struct A {
    int primaryTag() { return 0; }
};

// ★ 偏特化里埋一个【只有实例化才会炸】的成员：`typename T::nope`。
//   关键在于它必须是【依赖名】—— 非依赖名（如调用一个不存在的自由函数）
//   在两阶段查找的第一阶段就被查了（[temp.res]/1），clang 会在【定义期】直接报错，
//   那样"不实例化"就没法用编译成败来观测了。依赖名只在【实例化时】解析，
//   于是本文件成为一个可靠的探针：
//     写 `A<int*, int**>;`（无声明符）⇒ 不实例化 ⇒ 编过 ✅
//     写 `A<int*, int**> v;`（变量声明）⇒ 实例化 ⇒ 报错（clang: type 'int *' cannot
//       be used prior to '::' because it has no members；minicc: no type named 'nope'）
//   两边的判据见单测 DeclaratorlessDecl.DoesNotInstantiateAnyTemplate。
template<class T>
struct A<T, T*> {
    typename T::nope boom;
};

// ── ④ 实例化的成员函数体内含空语句：走 cloneStmt 的 Empty 分支 ──────────────
// 若 cloneStmt 漏了这个 case，会落到 default 返回 nullptr，
// 语句体里就多出一个空指针节点，Sema/CodeGen 下钻时崩。
template<typename T>
struct Widget {
    T v;

    int probe() {
        ;                       // 空语句
        int;                    // 无声明符的声明
        const int;              // 同上（const 属 decl-specifier，不是 ptr-operator）
        return 7;
    }
};

int main() {
    int bad = 0;

    // ── ① 空语句：语句体 / if 体 / while 体三个位置 ──
    ;;                          // 连续空语句
    if (1) ;
    while (0) ;
    // 上面三条若被拒收，本文件根本编不过 ⇒ 编过即通过（bad 不加）

    // ── ② 类型关键字开头的无声明符声明 ──
    int;
    const int;
    unsigned int;
    // 同上：编过即通过

    // ── ③ 模板 id / 限定名开头的无声明符声明（★ B16 的正主）──
    A<int*, int**>;             // 修复前死在 [Parse Error] at 'int'
    // 上面两行编过 ⇒ 通过

    // ── ④ 实例化成员函数体内的空语句（走 cloneStmt）+ 正常功能不受影响 ──
    {
        Widget<int> w;
        w.v = 35;
        if (w.probe() != 7) bad = bad * 10 + 4;          // 体内空语句不得吞掉 return
        if (w.v != 35)      bad = bad * 10 + 4;          // 字段读写不受影响
    }

    // ── ⑤ 回归护栏：正常声明没被"无声明符"那段逻辑带偏 ──
    {
        int a;
        const int b = 1;
        int* p;
        a = b + 1;
        p = &a;
        if (*p != b + 1) bad = bad * 10 + 5;
    }

    return bad;
}
