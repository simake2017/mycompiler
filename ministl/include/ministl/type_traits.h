#pragma once
// =============================================================================
// ministl/type_traits.h —— 类型萃取（转发层 + 自有扩展）
// =============================================================================
// 【设计决策记录】
//   最初本文件是「从零实现所有 traits」的，用的是编译器内建 __is_xxx。
//   实测发现 GCC 10 与 Clang 的内建集合不一致：
//     __is_function / __is_trivially_destructible / __is_nothrow_assignable
//     这几个在 GCC 10 上不存在（它们是 Clang 的内建），编译直接失败。
//
//   结论：traits 本就是编译器提供的底层能力，自己再包一层不产生任何
//   学习价值，反而引入可移植性地雷。本项目的价值在**容器、智能指针、
//   算法、ranges 的实现**，不在重造 traits。故改为转发 std::。
//
//   唯一例外是 is_memmovable —— 它不在标准里，是本项目为 vector 扩容
//   优化而组合出来的判据，见文件末尾。
// =============================================================================

#include <cstddef>
#include <type_traits>
#include <utility>  // std::declval

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、整型常量包装
// ─────────────────────────────────────────────────────────────────────────────

using std::integral_constant;
using std::bool_constant;
using std::true_type;
using std::false_type;

// ─────────────────────────────────────────────────────────────────────────────
//  二、类型同一性
// ─────────────────────────────────────────────────────────────────────────────

using std::is_same;
using std::is_same_v;

// ─────────────────────────────────────────────────────────────────────────────
//  三、条件选择与 SFINAE 载体
// ─────────────────────────────────────────────────────────────────────────────

using std::conditional;
using std::conditional_t;
using std::enable_if;
using std::enable_if_t;
using std::void_t;

// ─────────────────────────────────────────────────────────────────────────────
//  四、cv 限定与引用的增删
// ─────────────────────────────────────────────────────────────────────────────

using std::remove_const;
using std::remove_const_t;
using std::remove_volatile;
using std::remove_volatile_t;
using std::remove_cv;
using std::remove_cv_t;
using std::remove_reference;
using std::remove_reference_t;
using std::remove_pointer;
using std::remove_pointer_t;
using std::remove_extent;
using std::remove_extent_t;
using std::add_const;
using std::add_const_t;
using std::add_volatile;
using std::add_volatile_t;
using std::add_pointer;
using std::add_pointer_t;
using std::add_lvalue_reference;
using std::add_lvalue_reference_t;
using std::add_rvalue_reference;
using std::add_rvalue_reference_t;
using std::declval;

// remove_cvref 是 C++20 才进标准的，GCC 10 的 libstdc++ 可能没有。
// 自己拼一个，成本为零。
template <typename T>
struct remove_cvref {
    using type = remove_cv_t<remove_reference_t<T>>;
};
template <typename T>
using remove_cvref_t = typename remove_cvref<T>::type;

// ─────────────────────────────────────────────────────────────────────────────
//  五、主类型类别判定
// ─────────────────────────────────────────────────────────────────────────────

using std::is_void;
using std::is_void_v;
using std::is_integral;
using std::is_integral_v;
using std::is_floating_point;
using std::is_floating_point_v;
using std::is_arithmetic;
using std::is_arithmetic_v;
using std::is_pointer;
using std::is_pointer_v;
using std::is_reference;
using std::is_reference_v;
using std::is_lvalue_reference;
using std::is_lvalue_reference_v;
using std::is_rvalue_reference;
using std::is_rvalue_reference_v;
using std::is_const;
using std::is_const_v;
using std::is_volatile;
using std::is_volatile_v;
using std::is_array;
using std::is_array_v;
using std::is_function;
using std::is_function_v;
using std::is_enum;
using std::is_enum_v;
using std::is_class;
using std::is_class_v;
using std::is_union;
using std::is_union_v;
using std::is_polymorphic;
using std::is_polymorphic_v;
using std::is_empty;
using std::is_empty_v;
using std::is_final;
using std::is_final_v;

// ─────────────────────────────────────────────────────────────────────────────
//  六、构造 / 析构 / 赋值能力探测
// ─────────────────────────────────────────────────────────────────────────────
// 这一组是 vector 性能的关键：
//   - 若 T 可平凡复制 → 扩容时用 memmove 整块搬家，而非逐元素移动构造；
//   - 若 T 可平凡析构 → 析构时跳过循环，直接释放内存。
// C++ 的「零开销抽象」在这里体现得最直白。

using std::is_trivially_copyable;
using std::is_trivially_copyable_v;
using std::is_trivially_destructible;
using std::is_trivially_destructible_v;
using std::is_trivially_copy_constructible;
using std::is_trivially_copy_constructible_v;
using std::is_constructible;
using std::is_constructible_v;
using std::is_default_constructible;
using std::is_default_constructible_v;
using std::is_copy_constructible;
using std::is_copy_constructible_v;
using std::is_move_constructible;
using std::is_move_constructible_v;
using std::is_destructible;
using std::is_destructible_v;
using std::is_copy_assignable;
using std::is_copy_assignable_v;
using std::is_move_assignable;
using std::is_move_assignable_v;
using std::is_nothrow_constructible;
using std::is_nothrow_constructible_v;
using std::is_nothrow_move_constructible;
using std::is_nothrow_move_constructible_v;
using std::is_nothrow_copy_constructible;
using std::is_nothrow_copy_constructible_v;
using std::is_nothrow_destructible;
using std::is_nothrow_destructible_v;
using std::is_nothrow_copy_assignable;
using std::is_nothrow_copy_assignable_v;
using std::is_nothrow_move_assignable;
using std::is_nothrow_move_assignable_v;

// ── 本项目自有扩展：能否用 memmove 整块搬家 ──
// 必须同时满足「可平凡复制」与「可平凡析构」——只搬不析构才算安全。
// vector 扩容、erase 搬移元素时用它决定走 memmove 还是逐元素移动构造。
template <typename T>
struct is_memmovable
    : bool_constant<is_trivially_copyable<T>::value &&
                    is_trivially_destructible<T>::value> {};

template <typename T>
inline constexpr bool is_memmovable_v = is_memmovable<T>::value;

// ─────────────────────────────────────────────────────────────────────────────
//  七、转换、继承与退化
// ─────────────────────────────────────────────────────────────────────────────

using std::is_convertible;
using std::is_convertible_v;
using std::is_base_of;
using std::is_base_of_v;
using std::decay;
using std::decay_t;

// ─────────────────────────────────────────────────────────────────────────────
//  八、可调用性探测（function 的类型擦除要用）
// ─────────────────────────────────────────────────────────────────────────────

using std::is_invocable;
using std::is_invocable_v;
using std::invoke_result;
using std::invoke_result_t;

// ─────────────────────────────────────────────────────────────────────────────
//  九、对齐存储 —— optional / function 的 SBO 缓冲区
// ─────────────────────────────────────────────────────────────────────────────

using std::aligned_storage;
using std::aligned_storage_t;

}  // namespace ministl
