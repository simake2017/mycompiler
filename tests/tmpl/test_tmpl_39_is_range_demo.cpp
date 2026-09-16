// =============================================================================
// 测试：用户原始用例的 minicc 化 —— is_range 探测 + Box 偏特化
// =============================================================================
// 考察理论点：
//   本测试把用户实战代码（管道操作符那一段）里能落地的两半提取出来：
//
//     ① is_range<T>  —— 用 void_t + decltype + declval 做"类型能力探测"
//     ② Box<T, U = void> + Box<T*, T>  —— 默认实参 + 偏特化择优
//
//   两半各自对应一条主线 H 的理论：
//
//   ① 探测链路的完整推导（对照 docs/learn/20）：
//        is_range<decltype(numbers)>
//          → decltype(numbers) 求值为 Vec            [dcl.type.decltype]
//          → 偏特化模式 is_range<T, void_t<...>> 与之匹配
//          → T := Vec
//          → 替换 void_t 的两个实参：
//              decltype(std::declval<Vec>().begin())  → int   ✓
//              decltype(std::declval<Vec>().end())    → int   ✓
//          → void_t<...> 全合法 ⇒ 归约为 void
//          → 与主模板默认实参 void 对齐 ⇒ 偏特化胜出 ⇒ true_type
//
//      ★ 换成 is_range<decltype(1)>（即 is_range<int>）时，
//        int 是标量、没有成员，第一项探测就失败 ⇒ 偏特化整体消失
//        ⇒ 静默回退主模板 ⇒ false_type。全程无报错（见 test_tmpl_36/37）。
//
//   ② Box<decltype(&a)> 的择优（对照 docs/learn/19）：
//        decltype(&a) = int*  ⇒ 实例化请求是 Box<int*, void>
//        （第二实参缺省 → 取主模板默认 void）
//        ★ 偏特化 Box<T*, T> 的匹配要逐位合一（不是"字面相等"）：
//            位置 0 模式 T* 对实参 int*  ⇒ T := int   （注意：是 int，不是 int*！
//                                         T* 匹配 int* 时 T 取的是【被指类型】）
//            位置 1 模式 T  对实参 void  ⇒ 要求 T := void，与上面冲突 ⇒ 不匹配
//        ⇒ 走主模板 → hello() = 1
//        而 Box<int*, int>：位置 0 ⇒ T := int；位置 1 实参 int 与 T 一致
//        ⇒ 偏特化命中 → hello() = 2
//
//      ★ 这里有个极易踩的陷阱：Box<int*, int*> 【不会】命中 Box<T*, T>。
//        因为 T* 对 int* 推出 T = int，第二个位置就要求 int* == int，矛盾。
//        写测试时我最初就写成了 int*，被 clang 的实测结果纠正 —— 见下方 94 分支。
//
//   ★ 与用户原代码的差距（本测试【故意】保留差异，便于对照学习）：
//     - 去掉了 <iostream> 的 std::cout：本项目尚无输出能力
//       （自研链接器只注入 _start/malloc/free，没有 write 系统调用），
//       故把打印改成返回码，用【退出码】观测结果 —— 见"验证方式"决策。
//     - std::vector<int> → 手写的 Vec（本项目没有 STL 容器）。
//     - 去掉了 operator| / operator|| 那一半：它依赖
//       enable_if_t（别名模板）、std::decay_t、std::is_base_of，
//       均超出本轮范围，见 docs/ROADMAP.md 主线 H 的"未做"清单。
//
// 预期行为：探测两真一假、Box 择优各就各位，返回 0。
//
// 编译过程中的关键日志：
//   [decltype]   ⇒ 声明类型 = Vec
//   [sfinae] void_t<2 个实参> —— 逐个替换 + 求值探测
//   [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓
//   [spec:select] ★ selecting class template 'Box' for <int*, void>
//
// 对照 clang：clang++-18 -std=c++20 编译本文件，退出码一致。
// =============================================================================

// minicc 的 std 垫片由 registerBuiltins() 内建注入，无需头文件；
// clang 需要真实头文件。条件编译对齐两边（minicc 不定义 __clang__）。
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

// 手写的最小"容器"（对应原用例的 std::vector<int>）
struct Vec {
    int begin() { return 1; }
    int end()   { return 2; }
};

// ── ① 能力探测 ──

template <typename T, typename = void>
struct is_range : public std::false_type {};

template <typename T>
struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>>
    : public std::true_type {};

// ── ② 默认模板实参 + 偏特化（原用例的 Box）──

template <typename T, typename U = void>
struct Box {
    int hello() { return 1; }   // 主模板
};

template <typename T>
struct Box<T*, T> {
    int hello() { return 2; }   // 偏特化：两个位置绑定同一个 T
};

int main() {
    // ── ① 探测 ──
    Vec numbers;
    bool value  = is_range<decltype(numbers)>::value;  // Vec 有 begin/end → true
    bool value1 = is_range<decltype(1)>::value;        // int 是标量       → false

    if (!value) return 91;
    if (value1) return 92;

    // ── ② Box 择优 ──
    int a = 10;

    // decltype(&a) = int*  ⇒ Box<int*, void>：偏特化要求 T 同时是 int* 和 void → 不匹配
    Box<decltype(&a)> primary_box;
    if (primary_box.hello() != 1) return 93;

    // Box<int*, int>：位置 0 推出 T := int，位置 1 实参 int 与之一致 ⇒ 偏特化命中
    // （写成 Box<int*, int*> 是【不命中】的 —— 见文件头 ② 的陷阱说明）
    Box<int*, int> spec_box;
    if (spec_box.hello() != 2) return 94;

    return 0;
}
