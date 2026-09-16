// =============================================================================
// 测试：void_t 探测是"全有或全无" —— 部分满足不算命中
// =============================================================================
// 考察理论点：
//   void_t 最常见的误用是以为它"逐个条件各自生效"。事实相反：
//
//     std::void_t<decltype(declval<T>().begin()),
//                 decltype(declval<T>().end())>
//
//   这【整串】是一个替换目标。只要其中【任何一个】实参替换失败，
//   整个偏特化就判为不匹配 —— 不存在"匹配一半"这种中间态。
//
//   ★ 与 test_tmpl_35 的区别：
//     35 的 Container 两个成员都有 → 命中；
//     37 特意造一个【只有 begin、没有 end】的半吊子类型 HalfRange，
//     它会掉进主模板，而不是"部分命中"。
//     这正是 SFINAE 常被形容为"要么全对、要么当没看见"的原因。
//
//   标准依据：[temp.deduct]/8 —— 替换失败的作用域是【整个候选】，
//   失败即把该候选从候选集中整个移除，没有部分撤销的余地。
//   对照 clang：Sema::SubstituteExplicitTemplateArguments 里，
//   任意一个实参替换失败都会让 DeduceTemplateArguments 直接返回失败，
//   不会保留已成功替换的前缀。
//
//   另一个常见误解是"失败条件必须写在同一个 void_t 里才管用"——
//   其实写不写在一起无所谓，关键在于它们是否同属【一个偏特化模式】。
//   本测试把它们写在一起，也是真 C++ 里 is_range 的标准写法。
//
// 预期行为：FullRange → true（命中）；HalfRange → false（整体失效）；
//           Empty → false。返回 0。
//
// 编译过程中的关键日志（HalfRange 那一路）：
//   [sfinae]   ├─ 实参 1 探测通过 → int
//   [sfinae] ⤵ 模式第 2 位替换失败，按 SFINAE 判为【不匹配】（非错误）
//   [spec:select]   └─ ③ falling back to PRIMARY template
//   ↑ 注意第 1 个实参已经"探测通过"了 —— 但它被一并作废
//
// 对照 clang：clang++-18 -std=c++20 编译本文件，退出码一致。
// =============================================================================

// minicc 的 std 垫片由 registerBuiltins() 内建注入，无需头文件；
// clang 需要真实头文件。条件编译对齐两边（minicc 不定义 __clang__）。
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

// 两个成员都齐全
struct FullRange {
    int begin() { return 1; }
    int end()   { return 2; }
};

// ★ 半吊子：只有 begin()，没有 end() —— 第一个探测会通过
struct HalfRange {
    int begin() { return 1; }
};

struct Empty { };

template <typename T, typename = void>
struct is_range : public std::false_type {};

template <typename T>
struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>>
    : public std::true_type {};

int main() {
    bool full  = is_range<FullRange>::value;   // 两个探测都过 → true
    bool half  = is_range<HalfRange>::value;   // 第 1 个过、第 2 个失败 → 整体失效 → false
    bool empty = is_range<Empty>::value;       // 第 1 个就失败 → false

    if (!full)  return 91;
    if (half)   return 92;   // ← 若实现错误地"部分命中"，这里会失败
    if (empty)  return 93;

    return 0;
}
