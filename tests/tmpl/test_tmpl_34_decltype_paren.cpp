// =============================================================================
// 测试：decltype 的括号规则（[dcl.type.decltype]）
// =============================================================================
// 考察理论点：
//   decltype 里"多写一对括号"会改变结果 —— 这是它最反直觉、也最常被引用的特性。
//
//   标准规则：
//     decltype(e)    e 是【未加括号】的 id-expression 或类成员访问
//                    → 取 e 的【声明类型】（declared type）
//     其它一切情况（包括多加一层括号）
//                    → 取 e 的【表达式类型】，并按值类别调整：
//                        左值 → T&      prvalue → T
//
//   为什么要有这条规则？因为"声明类型"和"表达式类型"本就不同：
//     变量 a 声明为 int，但表达式 a 是个左值，其类型是 int&。
//   decltype 默认给你"声明类型"（更符合直觉），加括号则退回"表达式类型"。
//
//   这也解释了为什么标准库要发明 std::decay_t / std::declval 绕开它：
//   泛型代码常常想要"带引用的表达式类型"，而 decltype 默认不给。
//
// 【怎么把这个差异变成可见的观察点】
//   本项目的变量声明不支持引用类型（`int& r = a;` 报语法错），
//   所以不能直接声明一个引用变量来验证。改用【偏特化匹配】当探针：
//     探针 Probe<T>  主模板   → tag() = 0
//     探针 Probe<T&> 偏特化   → tag() = 1

//   int        不匹配 T&  → 走主模板 → 0
//   int&       匹配   T&  → 走偏特化 → 1
//   类型差异于是变成一个可返回、可断言的整数。
//
// 预期行为：Probe<decltype(a)> 得 0，Probe<decltype((a))> 得 1；
//           同时 decltype((a)) 的推出结果必须是左值引用才能命中偏特化。
//           返回 0 + 1 = ... 见文件末尾（用累加值做校验）。
//
// 编译过程中的关键日志：
//   [decltype] 求值 decltype(e)  —— 未加括号 ⇒ 若为 id-expression 则取【声明类型】
//   [decltype]   ⇒ 声明类型 = int
//   [decltype] 求值 decltype((e)) —— 加括号 ⇒ 取【表达式类型】(左值带 &)
//   [decltype]   ⇒ 左值 ⇒ 表达式类型 = int&
//   [spec:select] ★ selecting class template 'Probe' for <int>
//     └─ ③ falling back to PRIMARY template
//   [spec:select] ★ selecting class template 'Probe' for <int&>
//     ├─ ② 候选：'Probe<T&>' 匹配成功
//     │  ⇒ 最特化者 → USING IT
//
// 对照 clang：
//   int a;  static_assert(!std::is_reference_v<decltype(a)>);   // 通过
//           static_assert( std::is_reference_v<decltype((a))>); // 通过
//   两条断言方向相反 —— 本测试正是要复现这个差别。
// =============================================================================

template <typename T>
struct Probe { int tag() { return 0; } };

template <typename T>
struct Probe<T&> { int tag() { return 1; } };

int main() {
    int a = 5;

    Probe<decltype(a)>   p1;   // decltype(a)   = int  → 主模板 → 0
    Probe<decltype((a))> p2;   // decltype((a)) = int& → 偏特化 → 1

    int t1 = p1.tag();
    int t2 = p2.tag();

    if (t1 != 0) return 91;    // 未加括号却成了引用 → 规则实现反了
    if (t2 != 1) return 92;    // 加括号却没成引用 → 丢了 [dcl.type.decltype] 第二条

    return t1 + t2 - 1;        // 0 + 1 - 1 = 0
}
