// =============================================================================
// tests/tmpl/test_tmpl_49_dependent_type_name.cpp
// =============================================================================
// 考察理论点：依赖类型名 `typename T::type`（[temp.res]/5 + [temp.deduct]/8）
//
//   · **为什么需要 typename**：模板里看到 `T::x` 时，编译器在定义期无法判定
//     x 是【类型】还是【静态成员/枚举量】—— T 是谁还不知道。标准要求写
//     typename 消歧（C++20 起在非依赖上下文里可省）。
//     对照 clang：Parser::ParseTypenameType → DependentNameType。
//
//   · **它是"依赖的"**：限定者 T 含模板形参 ⇒ 整条名字要等实例化才知道。
//     本实现把这种节点原样保留到替换阶段（[temp.inst]），在
//     TemplateInstantiator::substituteType 的 Case 5.5 里才查表解糖。
//
//   · ★ **检测惯用法**：`void_t<typename T::type>` 是"这个类型有没有成员
//     类型 type"的探测。它的成立完全依赖 [temp.deduct]/8 的【直接上下文】：
//     T := WithoutType 时 `WithoutType::type` 不存在 ⇒ 必须在【替换的当场】
//     软失败（把该偏特化移出候选集），而不是等到语义分析报硬错误。
//     本实现的落点：resolveMemberType 抛 SubstitutionFailure，
//     由 matchPattern 的 Sfinae::attempt 吸收。
//
// 预期行为：返回值 0（已用 clang++-18 -std=c++20 核对，clang 0）
//   has_type<WithType>::value    == true    （有成员类型 type）
//   has_type<WithoutType>::value == false   （替换失败 → 回退主模板）
//
// 运行：
//   ./minicc tests/tmpl/test_tmpl_49_dependent_type_name.cpp -o /tmp/t49 && /tmp/t49; echo $?
//
// 关键日志（可复现）：
//   [sfinae] void_t<1 个实参> —— 逐个替换 + 求值探测
//   [sfinae]   ├─ 实参 1 探测通过 → int
//   [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓
//   [sfinae] ⚡ 发出替换失败信号（直接上下文内 → 可被吸收为软失败）：
//            no type named 'type' in 'WithoutType'
// =============================================================================

// minicc 的 std 垫片（void_t / false_type / true_type）由 registerBuiltins
// 内建注入，无需头文件；clang 需要真实的 <type_traits>。条件编译对齐两边。
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif

// ── 有成员类型别名的类（别名本身由 tests/tmpl/test_tmpl_48 覆盖）──
struct WithType {
    using type = int;
};

// ── 空类：没有 type 成员 ──
struct WithoutType { };

// ── 主模板：探测不到时的兜底 ──
template <typename T, typename = void>
struct has_type : public std::false_type {};

// ── 偏特化：仅当 T::type 存在时才成立 ──
template <typename T>
struct has_type<T, std::void_t<typename T::type>> : public std::true_type {};

// ── 另一处使用点：字段类型直接用依赖类型名 ──
//    实例化 Get<WithType> 时，字段 v 的类型由 T::type 解糖得到 int。
template <typename T>
struct Get {
    typename T::type v;
};

int main() {
    bool a = has_type<WithType>::value;      // true
    bool b = has_type<WithoutType>::value;   // false（软失败 → 主模板）
    if (!a) return 91;
    if (b)  return 92;

    Get<WithType> g;
    g.v = 5;
    if (g.v != 5) return 93;

    return 0;
}
