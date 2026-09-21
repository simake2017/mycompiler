#pragma once
// =============================================================================
// ministl/utility.h —— 移动语义与基础工具
// =============================================================================
// 学习要点：
//   1. std::move 不移动任何东西！它只是一个 static_cast<T&&>，
//      把左值「标记」成右值，从而让重载决议选中移动构造函数。
//      真正的搬移发生在移动构造/移动赋值函数体内。
//   2. std::forward 是「保持值类别」的转发：T 由模板推导决定，
//      引用折叠规则（T& & → T&，T& && → T&，T&& && → T&&）让它自动正确。
//   3. move_if_noexcept 是 vector 扩容的异常安全关键：
//      若移动构造可能抛异常且类型可拷贝，则宁可拷贝——因为移动构造抛异常时
//      源对象已被破坏，无法回滚；拷贝则原对象完好，可回退到强保证。
// =============================================================================

#include "type_traits.h"

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、移动语义的两块基石
// ─────────────────────────────────────────────────────────────────────────────

// move：仅做类型转换，无任何运行时动作（可被完全优化掉）
template <typename T>
constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}

// forward：保持原值类别转发。两个重载分别对应左值/右值实参。
template <typename T>
constexpr T&& forward(remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}

template <typename T>
constexpr T&& forward(remove_reference_t<T>&& t) noexcept {
    // 若 T 被推导为左值引用，却传入了右值 —— 这是危险的误用，标准禁止
    static_assert(!is_lvalue_reference_v<T>,
                  "forward: 不能用右值转发为左值引用");
    return static_cast<T&&>(t);
}

// move_if_noexcept：为「异常安全」让步的移动
// 返回 const T& 时会触发拷贝构造；返回 T&& 时触发移动构造。
template <typename T>
constexpr conditional_t<
    !is_nothrow_move_constructible<T>::value && is_copy_constructible<T>::value,
    const T&,
    T&&>
move_if_noexcept(T& x) noexcept {
    return ministl::move(x);
}

// ─────────────────────────────────────────────────────────────────────────────
//  二、swap 与 exchange
// ─────────────────────────────────────────────────────────────────────────────
// swap 的三次移动是「移动语义」最经典的用例：
// 全程不拷贝，仅交接资源所有权，对容器而言是 O(1)。

template <typename T>
void swap(T& a, T& b) noexcept(is_nothrow_move_constructible<T>::value &&
                               is_nothrow_move_assignable<T>::value) {
    T tmp = ministl::move(a);
    a = ministl::move(b);
    b = ministl::move(tmp);
}

// exchange：把新值写入，同时返回旧值（如实现移动赋值）
template <typename T, typename U = T>
constexpr T exchange(T& obj, U&& new_val) {
    T old = ministl::move(obj);
    obj = ministl::forward<U>(new_val);
    return old;
}

// ─────────────────────────────────────────────────────────────────────────────
//  三、pair
// ─────────────────────────────────────────────────────────────────────────────
// 注意转发构造函数用 enable_if 约束：否则 pair<int,int> p(1, 2) 会
// 与「拷贝构造」产生歧义（U1&& 能匹配任意类型，包括 pair 自身）。

template <typename T1, typename T2>
struct pair {
    using first_type  = T1;
    using second_type = T2;

    T1 first;
    T2 second;

    // ── 默认构造 ──
    constexpr pair() : first(), second() {}

    // ── 直接给值（要求可拷贝构造，避免与转发构造冲突）──
    constexpr pair(const T1& a, const T2& b) : first(a), second(b) {}

    // ── 完美转发构造 ──
    template <typename U1, typename U2,
              typename = enable_if_t<is_constructible<T1, U1&&>::value &&
                                     is_constructible<T2, U2&&>::value>>
    constexpr pair(U1&& a, U2&& b)
        : first(ministl::forward<U1>(a)), second(ministl::forward<U2>(b)) {}

    // ── 由异构 pair 转换构造 ──
    template <typename U1, typename U2,
              typename = enable_if_t<is_constructible<T1, const U1&>::value &&
                                     is_constructible<T2, const U2&>::value>>
    constexpr pair(const pair<U1, U2>& p) : first(p.first), second(p.second) {}

    template <typename U1, typename U2,
              typename = enable_if_t<is_constructible<T1, U1&&>::value &&
                                     is_constructible<T2, U2&&>::value>>
    constexpr pair(pair<U1, U2>&& p)
        : first(ministl::forward<U1>(p.first)),
          second(ministl::forward<U2>(p.second)) {}

    pair(const pair&) = default;
    pair(pair&&) = default;

    void swap(pair& other) noexcept {
        ministl::swap(first, other.first);
        ministl::swap(second, other.second);
    }
};

// 推导指引（CTAD）：让 pair p{1, 2.0} 能推导出 pair<int, double>
template <typename T1, typename T2>
pair(T1, T2) -> pair<T1, T2>;

template <typename T1, typename T2>
constexpr pair<decay_t<T1>, decay_t<T2>> make_pair(T1&& a, T2&& b) {
    return pair<decay_t<T1>, decay_t<T2>>(ministl::forward<T1>(a),
                                          ministl::forward<T2>(b));
}

// ── pair 比较：逐元素字典序 ──
template <typename T1, typename T2>
constexpr bool operator==(const pair<T1, T2>& a, const pair<T1, T2>& b) {
    return a.first == b.first && a.second == b.second;
}

template <typename T1, typename T2>
constexpr bool operator!=(const pair<T1, T2>& a, const pair<T1, T2>& b) {
    return !(a == b);
}

template <typename T1, typename T2>
constexpr bool operator<(const pair<T1, T2>& a, const pair<T1, T2>& b) {
    // 字典序：先比 first，相等再比 second
    return a.first < b.first || (!(b.first < a.first) && a.second < b.second);
}

template <typename T1, typename T2>
constexpr bool operator>(const pair<T1, T2>& a, const pair<T1, T2>& b) {
    return b < a;
}

template <typename T1, typename T2>
constexpr bool operator<=(const pair<T1, T2>& a, const pair<T1, T2>& b) {
    return !(b < a);
}

template <typename T1, typename T2>
constexpr bool operator>=(const pair<T1, T2>& a, const pair<T1, T2>& b) {
    return !(a < b);
}

// ─────────────────────────────────────────────────────────────────────────────
//  四、基础仿函数（容器的默认比较器/相等器）
// ─────────────────────────────────────────────────────────────────────────────
// 为什么用仿函数而不是函数指针：仿函数是**空类**，作为容器的成员时
// 可以被 EBO 优化掉，完全不占空间；函数指针则必须存 8 字节。

template <typename T = void>
struct less {
    constexpr bool operator()(const T& a, const T& b) const { return a < b; }
};

template <typename T = void>
struct greater {
    constexpr bool operator()(const T& a, const T& b) const { return a > b; }
};

template <typename T = void>
struct equal_to {
    constexpr bool operator()(const T& a, const T& b) const { return a == b; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  五、as_const
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
constexpr add_const_t<T>& as_const(T& t) noexcept { return t; }

template <typename T>
void as_const(const T&&) = delete;  // 禁止对右值取 const 引用（防止悬垂）

// ─────────────────────────────────────────────────────────────────────────────
//  五、编译期整数序列（tuple / 可变参数展开的基础设施）
// ─────────────────────────────────────────────────────────────────────────────
// 用「二分递归」把 O(N) 的模板实例化深度压到 O(log N)——
// 这是模板元编程里最常用的降低编译期开销手法。

template <size_t... I>
struct index_sequence {
    using type = index_sequence;
    static constexpr size_t size() noexcept { return sizeof...(I); }
};

template <typename S1, typename S2>
struct concat_sequence;

template <size_t... I1, size_t... I2>
struct concat_sequence<index_sequence<I1...>, index_sequence<I2...>>
    : index_sequence<I1..., (sizeof...(I1) + I2)...> {};

template <size_t N>
struct make_index_sequence_impl
    : concat_sequence<typename make_index_sequence_impl<N / 2>::type,
                      typename make_index_sequence_impl<N - N / 2>::type> {};

template <> struct make_index_sequence_impl<0> : index_sequence<> {};
template <> struct make_index_sequence_impl<1> : index_sequence<0> {};

template <size_t N>
using make_index_sequence = typename make_index_sequence_impl<N>::type;

template <typename... T>
using index_sequence_for = make_index_sequence<sizeof...(T)>;

// ─────────────────────────────────────────────────────────────────────────────
//  六、in-place 构造标签
// ─────────────────────────────────────────────────────────────────────────────
// 用于区分「用参数构造」和「用同类型对象构造」——
// 典型场景：optional<string> o(in_place, 5, 'x') 构造 5 个 'x'，
// 而不是拷贝另一个 optional。

struct in_place_t {
    explicit in_place_t() = default;
};
inline constexpr in_place_t in_place{};

struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};
inline constexpr nullopt_t nullopt{0};

}  // namespace ministl
