#pragma once
// =============================================================================
// ministl/ranges.h —— 惰性视图与管道
// =============================================================================
// 学习要点：
//   1. 【视图 vs 容器】容器**拥有**元素；视图只**引用**别的区间，本身不存数据。
//      拷贝一个视图是 O(1)（拷几个指针），拷贝 vector 则是 O(n)。
//   2. 【惰性求值 = pull 模型】v | transform(f) | filter(p) 不产生任何中间
//      容器，元素在**遍历到的那一刻**才被计算。控制权在消费者手里：
//      你不 ++ 迭代器，它就什么都不做。
//      （对比 Reactor 的 push 模型：数据主动推过来，消费者被动接收。）
//   3. 【管道即函数调用】`r | views::transform(f)` 完全等价于
//      `views::transform(f)(r)`。视图不是「对象」，是「可调用对象」；
//      operator| 只是把左边作为实参喂给右边。这是 range-v3 的核心技巧。
//   4. 【filter 必须存 end】「跳过不满足的元素」需要知道在哪停下；
//      transform 不需要，因为它不改变元素个数。
//   5. 【持有策略是本文件最难的点】见下面 view_storage_t 的说明。
// =============================================================================

#include "algorithm.h"
#include "iterator.h"
#include "memory.h"
#include "type_traits.h"
#include "utility.h"
#include "vector.h"

#include <cstddef>

namespace ministl {
namespace ranges {

// ─────────────────────────────────────────────────────────────────────────────
//  〇、持有策略 —— 本文件最关键的三个类型别名
// ─────────────────────────────────────────────────────────────────────────────
// 管道链 `v | filter(f) | transform(g)` 会先造出一个**临时** filter_view，
// 再把它喂给 transform。所以视图必须能安全地持有两种东西：
//
//   • 用户传进来的是**左值容器**（v）→ 应当只存引用，不拷贝（容器拷贝是 O(n)）
//   • 中间步骤传过来的是**临时视图** → 应当按值存下来（视图很小，且临时对象
//     马上就要销毁，存引用会立刻悬垂）
//
// 用一个类型别名同时覆盖两种情况：
//     左值实参（R 推导为 T&）→ 保持 T&      → 成员是引用
//     右值实参（R 推导为 T ）→ 退化为 T     → 成员是值（移动进来）
// 容器走第一条（零拷贝），临时视图走第二条（安全）。两全。

template <typename R>
using view_storage_t =
    conditional_t<is_lvalue_reference_v<R>, R, remove_reference_t<R>>;

// ─────────────────────────────────────────────────────────────────────────────
//  一、迭代器适配器
// ─────────────────────────────────────────────────────────────────────────────

// transform_iterator：解引用时才把函数作用上去。
template <typename It, typename F>
class transform_iterator {
    It it_;
    F func_;

public:
    using iterator_category = typename iterator_traits<It>::iterator_category;
    using difference_type   = typename iterator_traits<It>::difference_type;

    // 解引用结果是「函数返回值」——可能是值，也可能是引用
    using result_type = decltype(ministl::declval<F&>()(*ministl::declval<It&>()));
    using value_type  = remove_cvref_t<result_type>;
    using reference   = result_type;
    using pointer     = void;

    transform_iterator() = default;
    transform_iterator(It it, F f) : it_(it), func_(ministl::move(f)) {}

    // ★ 惰性求值的落点：函数在这一行才被调用，不在构造视图时
    reference operator*() const { return func_(*it_); }

    transform_iterator& operator++() {
        ++it_;
        return *this;
    }

    transform_iterator operator++(int) {
        transform_iterator tmp = *this;
        ++it_;
        return tmp;
    }

    transform_iterator& operator--() {
        --it_;
        return *this;
    }

    bool operator==(const transform_iterator& o) const { return it_ == o.it_; }
    bool operator!=(const transform_iterator& o) const { return it_ != o.it_; }
};

// filter_iterator：跳过不满足谓词的元素。
// 【关键】必须存 end_ —— 否则「没有元素满足条件时该停在哪」无解。
// 这正是 filter 比 transform 复杂的唯一原因。
template <typename It, typename Pred>
class filter_iterator {
    It it_;
    It end_;
    Pred pred_;

    void skip() {
        while (it_ != end_ && !pred_(*it_)) {
            ++it_;
        }
    }

public:
    // 过滤后无法随机跳转（第 n 个满足条件的元素要数出来），
    // 所以类别固定为 forward，无论底层迭代器多强。
    using iterator_category = forward_iterator_tag;
    using value_type        = typename iterator_traits<It>::value_type;
    using reference         = typename iterator_traits<It>::reference;
    using pointer           = typename iterator_traits<It>::pointer;
    using difference_type   = typename iterator_traits<It>::difference_type;

    filter_iterator() = default;
    filter_iterator(It it, It end, Pred p)
        : it_(it), end_(end), pred_(ministl::move(p)) {
        skip();  // 构造时就要跳过开头不满足的元素
    }

    reference operator*() const { return *it_; }
    pointer operator->() const { return &*it_; }

    filter_iterator& operator++() {
        ++it_;
        skip();
        return *this;
    }

    filter_iterator operator++(int) {
        filter_iterator tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(const filter_iterator& o) const { return it_ == o.it_; }
    bool operator!=(const filter_iterator& o) const { return it_ != o.it_; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  二、视图
// ─────────────────────────────────────────────────────────────────────────────
// 成员 R range_ 的类型由 view_storage_t 决定：引用或值。

template <typename R, typename F>
class transform_view {
    R range_;
    F func_;

public:
    template <typename R2>
    transform_view(R2&& r, F f)
        : range_(ministl::forward<R2>(r)), func_(ministl::move(f)) {}

    auto begin() { return transform_iterator(range_.begin(), func_); }
    auto end()   { return transform_iterator(range_.end(), func_); }
};

template <typename R, typename Pred>
class filter_view {
    R range_;
    Pred pred_;

public:
    template <typename R2>
    filter_view(R2&& r, Pred p)
        : range_(ministl::forward<R2>(r)), pred_(ministl::move(p)) {}

    auto begin() {
        return filter_iterator(range_.begin(), range_.end(), pred_);
    }
    auto end() {
        // end 迭代器不需要跳过任何东西：it_ == end_ 时比较自然成立
        return filter_iterator(range_.end(), range_.end(), pred_);
    }
};

// take_iterator：迭代器 + 一张「还能再产出几个」的配额券。
//
// 【为什么不能让 end() 前进 n 步了事】
//   第一版正是那么写的：end() 从 begin() 走 n 步。两个问题——
//   1) end() 变成 O(n)，每次 range-for 都要重走一遍；
//   2) 致命的是**它会重复触发上游谓词**。上游若是 filter_view，
//      算 end() 就得把前 n 个元素重新过滤一遍。谓词一旦有副作用
//      （计数、日志、改外部状态），结果就直接错了 —— 实测 filter
//      被调了 14 次，而容器只有 10 个元素。
//   标准库为此定义了 counted_iterator 配 default_sentinel；这里的
//   配额券是同一思路：把「走到哪」和「还能走几步」绑在一起带走。
template <typename It>
class take_iterator {
    It it_;
    std::size_t remaining_;

public:
    using iterator_category = typename iterator_traits<It>::iterator_category;
    using value_type        = typename iterator_traits<It>::value_type;
    using reference         = typename iterator_traits<It>::reference;
    using pointer           = typename iterator_traits<It>::pointer;
    using difference_type   = typename iterator_traits<It>::difference_type;

    take_iterator() = default;
    take_iterator(It it, std::size_t n) : it_(it), remaining_(n) {}

    reference operator*() const { return *it_; }
    pointer operator->() const { return &*it_; }

    // 【这两行 if 为什么必须分开写】range-for 的收尾动作是「先 ++ 再比较」：
    // 取满第 n 个元素后还会多调一次 ++。若那次仍去 ++it_，就会平白多拉一个
    // 上游元素（实测多出一次 filter(7)）—— 恰好抵消了 take 本该提供的
    // 短路收益。所以配额一旦归零，位置就冻住不动，靠下面的比较退出循环。
    take_iterator& operator++() {
        if (remaining_ > 0) --remaining_;
        if (remaining_ > 0) ++it_;   // 后面还有元素才推进上游
        return *this;
    }

    take_iterator operator++(int) {
        take_iterator tmp = *this;
        ++(*this);
        return tmp;
    }

    // 到达终点有两种情况，缺一不可：
    //   1) 配额耗尽 —— 取满了 n 个；
    //   2) 位置撞上上游末尾 —— 上游元素不够 n 个。
    // 只判第一种的话，上游提前结束时会把 end 迭代器解引用。
    // end 迭代器用 remaining_ == 0 构造，两端同类型，因此不需要哨兵类型。
    bool operator==(const take_iterator& o) const {
        if (remaining_ == 0 || o.remaining_ == 0) {
            return remaining_ == o.remaining_ || it_ == o.it_;
        }
        return it_ == o.it_;
    }
    bool operator!=(const take_iterator& o) const { return !(*this == o); }
};

// take_view：只取前 n 个。begin/end 都是 O(1)，且不触碰上游谓词。
template <typename R>
class take_view {
    R range_;
    std::size_t n_;

public:
    template <typename R2>
    take_view(R2&& r, std::size_t n)
        : range_(ministl::forward<R2>(r)), n_(n) {}

    auto begin() { return take_iterator(range_.begin(), n_); }
    auto end()   { return take_iterator(range_.end(), 0); }
};

// drop_view：跳过前 n 个
template <typename R>
class drop_view {
    R range_;
    std::size_t n_;

public:
    template <typename R2>
    drop_view(R2&& r, std::size_t n)
        : range_(ministl::forward<R2>(r)), n_(n) {}

    auto begin() {
        auto it   = range_.begin();
        auto last = range_.end();
        for (std::size_t i = 0; i < n_ && it != last; ++i) {
            ++it;
        }
        return it;
    }

    auto end() { return range_.end(); }
};

// reverse_view：不依赖底层容器的 rbegin/rend，而是用 make_reverse_iterator
// 现造。这一点很关键 —— 中间视图（如 take_view）根本没有 rbegin/rend 成员，
// 只有 begin/end。用 make_reverse_iterator(range_.end()) 则对任何 range 都成立。
template <typename R>
class reverse_view {
    R range_;

public:
    template <typename R2>
    explicit reverse_view(R2&& r) : range_(ministl::forward<R2>(r)) {}

    auto begin() { return ministl::make_reverse_iterator(range_.end()); }
    auto end()   { return ministl::make_reverse_iterator(range_.begin()); }
};

// iota_view：不持有任何容器，按需**生成**整数序列。
// 这是「视图」概念最纯粹的体现 —— 没有底层数据，元素是算出来的。
template <typename T>
class iota_view {
    T first_;
    T last_;

public:
    class iterator {
        T value_;

    public:
        using iterator_category = forward_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const T*;
        using reference         = T;  // 按值返回：没有真实存储可供引用

        iterator() = default;
        explicit iterator(T v) : value_(v) {}

        T operator*() const { return value_; }

        iterator& operator++() {
            ++value_;
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            ++value_;
            return tmp;
        }

        bool operator==(const iterator& o) const { return value_ == o.value_; }
        bool operator!=(const iterator& o) const { return value_ != o.value_; }
    };

    iota_view(T first, T last) : first_(first), last_(last) {}

    iterator begin() const { return iterator(first_); }
    iterator end() const { return iterator(last_); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  三、闭包对象 —— 视图的「柯里化」形式
// ─────────────────────────────────────────────────────────────────────────────
// 为什么要多一层闭包，而不让 views::transform(r, f) 直接返回视图？
// 因为管道要求 `views::transform(f)` 先成为一个**只差 range 的一元函数**，
// `r | 它` 才能把 r 喂进去。这层柯里化是管道语法的前提。

namespace views {

template <typename F>
struct transform_closure {
    F func_;

    template <typename R>
    auto operator()(R&& r) const {
        using RR = view_storage_t<R>;
        return transform_view<RR, F>(ministl::forward<R>(r), func_);
    }
};

template <typename Pred>
struct filter_closure {
    Pred pred_;

    template <typename R>
    auto operator()(R&& r) const {
        using RR = view_storage_t<R>;
        return filter_view<RR, Pred>(ministl::forward<R>(r), pred_);
    }
};

struct take_closure {
    std::size_t n_;

    template <typename R>
    auto operator()(R&& r) const {
        using RR = view_storage_t<R>;
        return take_view<RR>(ministl::forward<R>(r), n_);
    }
};

struct drop_closure {
    std::size_t n_;

    template <typename R>
    auto operator()(R&& r) const {
        using RR = view_storage_t<R>;
        return drop_view<RR>(ministl::forward<R>(r), n_);
    }
};

struct reverse_closure {
    template <typename R>
    auto operator()(R&& r) const {
        using RR = view_storage_t<R>;
        return reverse_view<RR>(ministl::forward<R>(r));
    }
};

// ── 对外暴露的视图对象（inline constexpr，C++17 起可头文件定义）──

struct transform_fn {
    template <typename F>
    constexpr auto operator()(F f) const {
        return transform_closure<F>{ministl::move(f)};
    }
};
inline constexpr transform_fn transform{};

struct filter_fn {
    template <typename Pred>
    constexpr auto operator()(Pred p) const {
        return filter_closure<Pred>{ministl::move(p)};
    }
};
inline constexpr filter_fn filter{};

struct take_fn {
    constexpr auto operator()(std::size_t n) const { return take_closure{n}; }
};
inline constexpr take_fn take{};

struct drop_fn {
    constexpr auto operator()(std::size_t n) const { return drop_closure{n}; }
};
inline constexpr drop_fn drop{};

// reverse 用函数形式包一层，保持和 transform/filter/take/drop 一致的
// 「调用一次拿到闭包」用法，避免使用者记两套写法。
struct reverse_fn {
    constexpr auto operator()() const { return reverse_closure{}; }
};
inline constexpr reverse_fn reverse{};

// ─────────────────────────────────────────────────────────────────────────────
//  四、管道操作符
// ─────────────────────────────────────────────────────────────────────────────
// `r | closure` ≡ `closure(r)`。管道本身不含任何魔法。
//
// 【为什么必须定义在 views 里而不是 ranges 里】
// operator| 靠 ADL 查找。ADL 只会检查实参类型**最内层**的外围命名空间，
// **不会向上递归**。闭包类型都在 ministl::ranges::views，所以编译器只查
// views —— 把 operator| 放在 ministl::ranges 里是找不到的（这正是第一版
// 编译失败的原因）。放这里，且因为右操作数永远是闭包，一条重载通吃。
//
// 左操作数可能是：左值容器、临时视图、iota_view…… 所以用 R&& 转发。

template <typename R, typename Closure>
auto operator|(R&& r, const Closure& c) {
    return c(ministl::forward<R>(r));
}

}  // namespace views

// ─────────────────────────────────────────────────────────────────────────────
//  五、生成器与物化
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
iota_view<T> iota(T first, T last) {
    return iota_view<T>(first, last);
}

// iota(n) 生成 0..n-1
template <typename T>
iota_view<T> iota(T n) {
    return iota_view<T>(static_cast<T>(0), n);
}

// to_vector：视图是惰性的，需要「物化」动作才真正计算。
// 遍历一次，把结果逐个 push 进新容器 —— 这是拉取模型里唯一的驱动点。
template <typename R>
auto to_vector(R&& r) {
    using T = remove_cvref_t<decltype(*r.begin())>;
    ministl::vector<T> out;
    for (auto&& x : r) {
        out.push_back(x);
    }
    return out;
}

template <typename R>
auto sum(R&& r) {
    using T = remove_cvref_t<decltype(*r.begin())>;
    T total{};
    for (auto&& x : r) {
        total = total + x;
    }
    return total;
}

}  // namespace ranges
}  // namespace ministl
