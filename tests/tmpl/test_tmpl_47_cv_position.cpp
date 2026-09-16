// =============================================================================
// 测试：cv 限定符的【位置】决定类型结构 —— const 属于谁
// =============================================================================
// 考察理论点：
//   ① [dcl.type.cv] cv 限定符写在【类型说明符】一侧，修饰的是被声明的类型，
//      不是声明符里的指针/引用。`const int*` 是"指向 const int 的指针"，
//      不是"const 的 int 指针"。
//   ② [temp.deduct.type] 偏特化匹配是【结构化】的：先把实参建成正确的类型树，
//      再拿模式去逐层合一。类型树建错了，匹配就必然错 —— 而且可能【静默】错。
//
// ── 背景：这里曾有一个结构性错误（docs/learn/22 的 ⑨）──
//   parseType 先跑后缀循环（* & &&），最后才把 const 套在最外层，于是：
//
//       源码           应有结构                    此前结构
//       const int      Const(Int)                  Const(Int)        ✓
//       const int*     Pointer(Const(Int))         Const(Pointer(Int))  ✗
//       const int&     LValueRef(Const(Int))       Const(LValueRef(Int)) ✗
//       int* const     Const(Pointer(Int))         解析直接失败          ✗
//
//   类型树错了会连锁出三类后果（都是 rc=0 的静默错）：
//     · `C<const int*>` 里顶层节点是 Const，而模式 T* 要的顶层是 Pointer
//       ⇒ C<T*> 匹配失败，反而去匹配 C<const T>（T := int*）——【选错特化】
//     · `C<const int&>` 里顶层不是引用
//       ⇒ C<const T&> 报 "reference structure mismatch" ——【永不匹配，回退主模板】
//     · `const T` 模式过宽：连 int* 都能匹配（只要有"剥顶层 const"就通过），
//       匹配集被污染，只靠偏序兜回来
//
// ── 本文件验证 ──
//   四条偏特化 + 四个使用点，每个使用点的答案都不一样，无法互相遮盖：
//
//       decltype(const int*)   → 指针，指向 const  → C<T*>,       T := const int → 1
//       decltype(int* const)   → const 的指针      → C<const T>,  T := int*      → 2
//       decltype(const int)    → const 的值        → C<const T>,  T := int       → 2
//       decltype(const int&)   → 指向 const 的引用 → C<const T&>, T := int       → 3
//
//   前两条是【同一棵树的两侧】，正是位置错误的照妖镜：
//   修好之前两条都会答成 2；修好之后必须分别是 1 和 2。
//
// ── 预期行为 ──
//   编译成功，退出码 = 1*100 + 2*10 + 3 = 123
//   与 clang 对照：clang++-18 -std=c++20 同源编译运行亦为 123
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_47_cv_position.cpp -o /tmp/t47
//     /tmp/t47; echo $?                       # → 123
//     日志里搜 [spec:select] 可见每个使用点选中的是哪一条偏特化
// =============================================================================

template <typename T> struct C { int tag() { return 0; } };            // 主模板
template <typename T> struct C<T*>       { int tag() { return 1; } };  // 指针
template <typename T> struct C<const T>  { int tag() { return 2; } };  // const 值/const 指针
template <typename T> struct C<const T&> { int tag() { return 3; } };  // 指向 const 的引用

int main() {
    int a = 1;
    int b = 2;

    const int* p = &a;        // 指针【指向】const int → C<T*>，T := const int
    int* const q = &a;        // const 的【指针】     → C<const T>，T := int*
    const int& r = b;         // 指向 const 的引用    → C<const T&>，T := int

    C<decltype(p)> c1;
    C<decltype(q)> c2;
    C<decltype(r)> c3;

    return c1.tag() * 100 + c2.tag() * 10 + c3.tag();   // 123
}
