// =============================================================================
// 测试：SFINAE 的硬错误对照 —— 兜底不存在时，同一个替换失败会升级为错误
// =============================================================================
// 考察理论点：
//   本测试是 test_tmpl_36 的【反面】。两例用的是同一个探针、同一个失败原因，
//   唯一差别是【主模板有没有可用的兜底】：
//
//     test_tmpl_36：主模板给了 value（经 false_type）→ 编译通过，静默回退
//     本测试     ：主模板没有 value            → 编译【失败】
//
//   ★ 由此可以精确地说清 SFINAE 的边界：
//     替换失败只是把这个候选【移出候选集】；它本身从不报错。
//     错误是【候选集空了之后】由"没有可用实体"这个事实引发的，
//     与替换失败本身无关。所以：
//       - 软失败（候选被移除）  ← SFINAE 管的
//       - 硬错误（无实体可用）  ← 与 SFINAE 无关，是常规语义错误
//     把这两件事混为一谈，是初学 SFINAE 最常见的偏差。
//
//   标准依据：[temp.deduct]/8 只规定"替换失败不是错误"，
//   它没有、也不可能规定"没有候选时也不是错误"。
//
//   对照 clang：clang 会报 "no member named 'value' in 'OnlyProbe<Empty, void>'"。
//   本项目的诊断文案针对这个 SFINAE 场景做了专门说明（见下），
//   更直白地指出"是回退到的那个类缺成员"，而不是含糊的"变量未定义"。
//
// ── 预期行为（本文件属于【必须编译失败】的负向测试）──
//   期望退出码：minicc 非 0（编译失败）
//   期望报错文案（关键片段）：
//     [Semantic Error] ...: no static member 'value' in class
//     'OnlyProbe_Empty_void' —— 类型限定访问 Cls<Args>::member 只解析静态成员；
//     若此处是 SFINAE 探测，说明偏特化被移除后回退到的主模板没有该成员
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_38_error_no_fallback.cpp -o /tmp/x
//     echo $?        # → 非 0
//     2>&1 | grep "no static member"   # → 命中上式文案
//
//   ★ 注意：clang 同样拒绝本文件（理由等价），故本文件不参与
//     "退出码对齐 clang"的常规回归，只做【报错文案】比对。
// =============================================================================

#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

struct Empty { };

// ── 主模板：确实存在（所以不是"找不到模板"），但【没有 value 成员】──
//    它接不住 SFINAE 移开偏特化之后落下来的那一次查找。
template <typename T, typename = void>
struct OnlyProbe {
    int other() { return 0; }
};

// ── 偏特化：只有 T 有 begin() 时才成立 ──
template <typename T>
struct OnlyProbe<T, std::void_t<decltype(std::declval<T>().begin())>>
    : public std::true_type {};

int main() {
    // Empty 没有 begin() → 上面的偏特化被 SFINAE 移出候选集
    // → 落回主模板 → 主模板没有 value → 硬错误（就在这一行）
    bool v = OnlyProbe<Empty>::value;
    if (v) return 1;
    return 0;
}
