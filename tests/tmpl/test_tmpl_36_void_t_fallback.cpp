// =============================================================================
// 测试：SFINAE 软失败 —— 静默回退主模板，绝不报错
// =============================================================================
// 考察理论点：
//   SFINAE 最难讲清、也最关键的一点是"软失败"到底软在哪：
//
//     替换失败【不是】"程序错了"，而只是"这个候选不成立"。
//     失败被【局限在候选集内部消化】—— 把该候选移出，继续看别的；
//     只有当候选集【全军覆没】时，才升级为真正的编译错误。
//
//   本测试专门断言"消化"这一半：
//     Probe<Empty> 的偏特化替换失败（Empty 没有 begin），
//     但编译【照常通过】，且回退到主模板、拿到 false_type 与可用的 tag()。
//
//   ★ 与 test_tmpl_38 构成对照：同一个探针，若无任何候选兜底，
//     同一个替换失败就会升级成硬报错。两例合起来才是 SFINAE 的全貌。
//
//   标准依据：[temp.deduct]/8 —— 只在函数类型、模板形参类型的
//   "直接上下文（immediate context）"里的替换失败才享受这个待遇；
//   错误若发生在被调用函数的【函数体内】（非直接上下文），
//   那就与 SFINAE 无关，是真错误（见 test_tmpl_38）。
//
//   ★ 回退版本仍然完全可用 —— 这点常被误解。
//     SFINAE 淘汰的是"偏特化这个候选"，不是"Empty 这个类型"。
//     主模板照常实例化：Empty 依然是合法的模板实参。
//
// 预期行为：Probe<HasBegin> 走偏特化（tag=1），
//           Probe<Empty> 静默回退主模板（tag=0），编译全程无错误，返回 0。
//
// 编译过程中的关键日志（Empty 那一路）：
//   [sfinae] ⟲ decltype 操作数在内层报错，降级为【软失败】
//   [sfinae] ⤵ 模式第 2 位替换失败，按 SFINAE 判为【不匹配】（非错误）
//   [spec:select]   └─ ③ falling back to PRIMARY template
//
// 对照 clang：clang++-18 -std=c++20 编译本文件，退出码一致。
// =============================================================================

// minicc 的 std 垫片由 registerBuiltins() 内建注入，无需头文件；
// clang 需要真实头文件。条件编译对齐两边（minicc 不定义 __clang__）。
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

struct HasBegin { int begin() { return 3; } };
struct Empty { };

// ── 主模板：兜底版本。★ 它自己也有可用的成员函数 tag() ──
template <typename T, typename = void>
struct Probe : public std::false_type {
    int tag() { return 0; }
};

// ── 偏特化：只有 T 有 begin() 时才成立 ──
template <typename T>
struct Probe<T, std::void_t<decltype(std::declval<T>().begin())>>
    : public std::true_type {
    int tag() { return 1; }
};

int main() {
    Probe<HasBegin> hit;
    Probe<Empty>    miss;   // ← 这一步的替换失败必须被"消化"掉

    if (hit.tag()  != 1) return 91;
    if (miss.tag() != 0) return 92;

    // 走到这里本身就是断言：Empty 的替换失败没有中断编译
    return 0;
}
