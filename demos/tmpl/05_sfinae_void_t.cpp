// ============================================================================
// demo 05 —— SFINAE：用 void_t 探测「这个类型有没有 begin/end」
// ============================================================================
// 理论：[temp.deduct]/8「替换失败不是错误」。
//
//   std::void_t<...> 的机关只有一句话：不管塞进什么实参，它都等于 void。
//   于是把【探测表达式】放进它的实参列表：
//
//     表达式合法   ⇒ 该偏特化的形参是 <T, void>，与主模板默认实参 void 吻合
//                    ⇒ 偏特化【成立】，选它（true_type）
//     表达式不合法 ⇒ 替换当场失败 ⇒ 【静默剔除该候选】，回退主模板（false_type）
//
//   ★ 关键：失败必须发生在【直接上下文】里（类型/表达式自身的构成过程），
//     void_t 的实参正好就是那个直接上下文。若失败发生在函数体里 ⇒ 硬错误。
//
// 看探测链路：
//   ./build-linux/minicc demos/tmpl/05_sfinae_void_t.cpp -S -o /tmp/demo05.s
// 终端可见（搜 "sfinae"），Container 那次【全部通过】：
//   [sfinae] void_t<2 个实参> —— 逐个替换 + 求值探测
//   [sfinae]   ├─ 实参 1 探测通过 → int
//   [sfinae]   ├─ 实参 2 探测通过 → int
//   [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓
// Empty 那次则是【软失败】（注意它没有变成硬错误，只是被踢出候选）：
//   [sfinae] ⟲ decltype 操作数在内层报错，降级为【软失败】:
//   [sfinae]    [Semantic Error] 46:58: No member 'begin' in class 'Empty'
//
// 预期：退出码 0
// ============================================================================

// minicc 的 std 垫片（void_t / declval / false_type / true_type）由
// SemanticAnalyzer::registerBuiltins() 内建注入，不需要头文件；
// clang 则需要真实的 <type_traits>/<utility>。用条件编译对齐两边。
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

struct Container {          // 有 begin 和 end
    int begin() { return 1; }
    int end()   { return 2; }
};

struct Empty { };           // 一个成员都没有

// ── 主模板：探测不到时的兜底 ──
template <typename T, typename = void>
struct is_range : public std::false_type {};

// ── 偏特化：只有当 T 同时有 begin() 和 end() 时才存在 ──
template <typename T>
struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>>
    : public std::true_type {};

int main() {
    // 注：先落到 bool 变量再判断 —— minicc 的 if 条件暂不接受模板 id 直接作操作数
    bool has_container = is_range<Container>::value;   // 两个探测都成功 → true
    bool has_empty     = is_range<Empty>::value;       // 替换失败 → 回退主模板 → false
    bool has_int       = is_range<int>::value;         // 标量没有成员 → false

    if (!has_container) return 1;
    if (has_empty)      return 2;
    if (has_int)        return 3;

    return 0;
}
