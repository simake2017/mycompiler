// =============================================================================
// tests/decl/test_decl_02_adl_and_qualified_lookup.cpp
// =============================================================================
// 考察理论点：命名空间下的名字查找 —— 限定名、作用域内非限定名、ADL
//
//   ① [basic.lookup.qual] 限定名查找：`N::S s;` / `N::get(s)`
//      限定名只在【限定者所指的作用域】里找，不进全局。
//
//   ② [basic.scope.namespace] 命名空间内非限定名：
//      `namespace N { struct S{}; int get(S s){...} }` 里那个 S
//      必须能解析 —— 它在全局符号表里根本不存在（只有 "N::S"）。
//      对照 clang：LookupName 沿 DeclContext 链上溯。
//
//   ③ ★ [basic.lookup.argdep] ADL（实参依赖查找）：
//      `only_in_n(s)` 这种非限定调用，普通名字查找找不到它，
//      但实参类型 N::S 的**关联命名空间** N 里有一个同名函数 ⇒ 找到。
//      这条规则是运算符重载与 `std::swap` 惯用法的基础
//      （"ADL 就是为了让 operator<< 能被找到而发明的"）。
//
//   ④ ★★ ADL 是"补进候选集"，不是"查不到才兜底"：
//      `measure(s)` 里 N::measure(S) 与全局 measure(int) 同场竞争，
//      由实参类型决定胜负 —— 若实现成"普通查找命中就返回"，
//      会静默调用类型完全不对的那个函数（能编译、能链接、结果错）。
//
// 预期行为：退出码 40（已用 clang++-18 核对：clang 40）
//   a = only_in_n(s)   → 7     （③ 只有 N 里有，靠 ADL 找到）
//   b = N::get(s)      → 7     （① 限定名调用）
//   c = measure(s)     → 7     （④ ADL 候选胜出，而不是全局 measure(int)=107）
//   d = measure(3)     → 103   （普通查找命中全局版本）
//   e = get(s)         → 7     （② 命名空间内非限定名 S，调用 N::get）
//   五个检查点全过则返回 40；任何一个不对就返回 9x 指明是谁错的。
//
// 运行：
//   ./minicc tests/decl/test_decl_02_adl_and_qualified_lookup.cpp -o /tmp/t | cat
//   /tmp/t; echo $?
//
// 关键日志（可复现）：
//   [resolve] 'S' 在命名空间 'N' 内 ⇒ N::S
//   [call] only_in_n(1 args) → int    [ADL(N)，only_in_n 走 ADL 限定查找：形参类型精确匹配]
//   [call] N::get(1 args) → int       [普通查找：形参类型精确匹配]
//   [call] measure(1 args) → int      [ADL(N)，measure 走 ADL 限定查找：形参类型精确匹配]
//   [call] measure(1 args) → int      [普通查找：形参类型精确匹配]
// =============================================================================

namespace N {
    struct S { int v; };          // 只以 "N::S" 登记在全局符号表里

    int get(S s) { return s.v; }        // N::get
    int only_in_n(S s) { return s.v; }  // 全局没有同名函数 ⇒ 只能靠 ADL
    int measure(S s) { return s.v; }    // 与下面的全局 measure 同名 ⇒ ④ 的竞技场
}

// 全局同名函数：参数是 int，与 N::S 完全不是一回事
int measure(int x) { return x + 100; }

int main() {
    N::S s;                       // ① 限定类型名
    s.v = 7;

    int a = only_in_n(s);         // ③ ADL（普通查找无此名）
    int b = N::get(s);            // ① 限定函数调用
    int c = measure(s);           // ④ ADL 候选 + 精确匹配胜出 → 7（错答会是 107）
    int d = measure(3);           // 普通查找命中全局版本 → 103
    int e = get(s);               // ② 命名空间内非限定名 S + 调用 N::get

    if (a != 7)   return 91;
    if (b != 7)   return 92;
    if (c != 7)   return 93;
    if (d != 103) return 94;
    if (e != 7)   return 95;
    return 40;
}
