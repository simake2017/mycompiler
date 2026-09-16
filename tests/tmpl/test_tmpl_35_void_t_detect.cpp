// =============================================================================
// 测试：void_t 探测命中（[temp.deduct]/8 SFINAE）
// =============================================================================
// 考察理论点：
//   void_t 是"探测型 SFINAE"的载体。写法：
//
//     template <typename T, typename = void>              // 主模板（兜底）
//     struct is_range : public std::false_type {};
//
//     template <typename T>                               // 偏特化（探测）
//     struct is_range<T, std::void_t<decltype(...), ...>> : std::true_type {};
//
//   机制链条（本测试逐步验证）：
//     ① 用实参 is_range<Container> 匹配偏特化 is_range<T, void_t<...>>
//     ② 推导出 T = Container，把 T 代入 void_t 的实参
//     ③ decltype(std::declval<T>().begin()) 求值 —— 成功
//     ④ void_t<...常合法...> 折叠成 void，与主模板的默认实参 void 对齐
//     ⑤ 偏特化比主模板更特化 → 命中 true_type
//
//   ★ 关键在 ③：若 T 没有 begin()，替换【失败】。按 [temp.deduct]/8，
//     这种失败发生在函数类型/模板形参的"直接上下文"里时【不是错误】，
//     只是让这个偏特化从候选集中消失（见 test_tmpl_36）。
//     这一条正是 SFINAE = Substitution Failure Is Not An Error 的字面含义。
//
//   ★ 涉及 CWG 1558：void_t 的语义被明确为"如果所有实参都合法，
//     则等价于 void；否则替换失败"。早期实现靠"未使用的模板形参"
//     这种脆弱方式，现已是标准行为。
//
// 预期行为：三个类型分别判为 有/无/无，返回 0。
//
// 编译过程中的关键日志：
//   [spec:select] ① 全特化候选 / ② 偏特化候选：'is_range<T, void_t<...>>' 匹配成功
//   [sfinae] void_t: 展开 N 个探测条件
//   [decltype]   ⇒ 表达式类型 = int
//
// 对照 clang：clang++-18 -std=c++20 编译本文件，退出码一致。
//
// ★ 逐行拆解见同目录 test_tmpl_35_void_t_detect.md
//   （第 70 行 `is_range<Container>::value` 从 Parser 拆结构到 Sema 折叠成 true 的
//     七步全过程，含 Parser 歧义消解、默认实参补齐、void_t 展开探测、SFINAE 三方协议）
// =============================================================================

// ── 头文件说明（本主线所有 decltype/SFINAE 测试统一采用）──
//   minicc 的 std 垫片（void_t / declval / false_type / true_type）
//   由 SemanticAnalyzer::registerBuiltins() 【内建注入】，不需要头文件；
//   而 clang 需要真实的 <type_traits>/<utility>。
//   用条件编译对齐两边 —— minicc 不定义 __clang__，会跳过整个块，
//   于是两边语义一致、可交叉验证。（见 docs/learn/20 的设计决策一节）
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

// 有 begin() 和 end() 的容器
struct Container {
    int begin() { return 1; }
    int end()   { return 2; }
};

// 一个成员都没有的空类
struct Empty { };

// ── 主模板：探测不到时的兜底 ──
template <typename T, typename = void>
struct is_range : public std::false_type {};

// ── 偏特化：只有当 T 同时有 begin() 和 end() 时才存在 ──
template <typename T>
struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>>
    : public std::true_type {};

int main() {
    bool has_container = is_range<Container>::value;  // 两个探测都成功 → true
    bool has_empty     = is_range<Empty>::value;      // 替换失败 → 回退主模板 → false
    bool has_int       = is_range<int>::value;        // 标量类型没有成员 → false

    if (!has_container) return 91;
    if (has_empty)      return 92;
    if (has_int)        return 93;

    return 0;
}
