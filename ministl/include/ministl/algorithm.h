#pragma once
// =============================================================================
// ministl/algorithm.h —— 基础算法
// =============================================================================
// 学习要点：
//   1. 算法只认**迭代器**，不认容器 —— 这是 STL 最重要的一条解耦。
//      copy 不关心源是 vector 还是数组，只要求「能 ++、能 *」。
//   2. sort 需要随机访问迭代器（要能 first + n、能相减），所以 list 用不了，
//      list 得自己实现成员 sort（归并）。
//   3. 快排的小区间优化：长度 ≤ 16 时改用插入排序。原因是快排的递归调用
//      开销在小数组上超过收益，而插入排序在近乎有序的小数组上极快。
// =============================================================================

#include "iterator.h"
#include "type_traits.h"
#include "utility.h"

#include <cstddef>

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、遍历与复制
// ─────────────────────────────────────────────────────────────────────────────

template <typename InputIt, typename Func>
Func for_each(InputIt first, InputIt last, Func f) {
    for (; first != last; ++first) f(*first);
    return f;
}

template <typename InputIt, typename OutputIt>
OutputIt copy(InputIt first, InputIt last, OutputIt d_first) {
    for (; first != last; ++first, ++d_first) *d_first = *first;
    return d_first;
}

template <typename InputIt, typename Size, typename OutputIt>
OutputIt copy_n(InputIt first, Size n, OutputIt d_first) {
    for (Size i = 0; i < n; ++i, ++first, ++d_first) *d_first = *first;
    return d_first;
}

template <typename InputIt, typename OutputIt, typename Pred>
OutputIt copy_if(InputIt first, InputIt last, OutputIt d_first, Pred pred) {
    for (; first != last; ++first) {
        if (pred(*first)) {
            *d_first = *first;
            ++d_first;
        }
    }
    return d_first;
}

template <typename ForwardIt, typename T>
void fill(ForwardIt first, ForwardIt last, const T& value) {
    for (; first != last; ++first) *first = value;
}

template <typename ForwardIt, typename Size, typename T>
ForwardIt fill_n(ForwardIt first, Size n, const T& value) {
    for (Size i = 0; i < n; ++i, ++first) *first = value;
    return first;
}

template <typename ForwardIt1, typename ForwardIt2>
void swap_ranges(ForwardIt1 first1, ForwardIt1 last1, ForwardIt2 first2) {
    for (; first1 != last1; ++first1, ++first2) {
        ministl::swap(*first1, *first2);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  二、查找与计数
// ─────────────────────────────────────────────────────────────────────────────

template <typename InputIt, typename T>
InputIt find(InputIt first, InputIt last, const T& value) {
    for (; first != last; ++first) {
        if (*first == value) return first;
    }
    return last;
}

template <typename InputIt, typename Pred>
InputIt find_if(InputIt first, InputIt last, Pred pred) {
    for (; first != last; ++first) {
        if (pred(*first)) return first;
    }
    return last;
}

template <typename InputIt, typename Pred>
InputIt find_if_not(InputIt first, InputIt last, Pred pred) {
    for (; first != last; ++first) {
        if (!pred(*first)) return first;
    }
    return last;
}

template <typename InputIt, typename T>
typename iterator_traits<InputIt>::difference_type
count(InputIt first, InputIt last, const T& value) {
    typename iterator_traits<InputIt>::difference_type n = 0;
    for (; first != last; ++first) {
        if (*first == value) ++n;
    }
    return n;
}

template <typename InputIt, typename Pred>
typename iterator_traits<InputIt>::difference_type
count_if(InputIt first, InputIt last, Pred pred) {
    typename iterator_traits<InputIt>::difference_type n = 0;
    for (; first != last; ++first) {
        if (pred(*first)) ++n;
    }
    return n;
}

template <typename InputIt, typename Pred>
bool all_of(InputIt first, InputIt last, Pred pred) {
    for (; first != last; ++first) {
        if (!pred(*first)) return false;
    }
    return true;
}

template <typename InputIt, typename Pred>
bool any_of(InputIt first, InputIt last, Pred pred) {
    for (; first != last; ++first) {
        if (pred(*first)) return true;
    }
    return false;
}

template <typename InputIt, typename Pred>
bool none_of(InputIt first, InputIt last, Pred pred) {
    return !any_of(first, last, pred);
}

template <typename InputIt1, typename InputIt2>
bool equal(InputIt1 first1, InputIt1 last1, InputIt2 first2) {
    for (; first1 != last1; ++first1, ++first2) {
        if (!(*first1 == *first2)) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  三、最值与累加
// ─────────────────────────────────────────────────────────────────────────────

template <typename ForwardIt>
ForwardIt min_element(ForwardIt first, ForwardIt last) {
    if (first == last) return last;
    ForwardIt smallest = first;
    ++first;
    for (; first != last; ++first) {
        if (*first < *smallest) smallest = first;
    }
    return smallest;
}

template <typename ForwardIt>
ForwardIt max_element(ForwardIt first, ForwardIt last) {
    if (first == last) return last;
    ForwardIt largest = first;
    ++first;
    for (; first != last; ++first) {
        if (*largest < *first) largest = first;
    }
    return largest;
}

template <typename T>
const T& min(const T& a, const T& b) {
    return (b < a) ? b : a;
}

template <typename T>
const T& max(const T& a, const T& b) {
    return (a < b) ? b : a;
}

template <typename InputIt, typename T>
T accumulate(InputIt first, InputIt last, T init) {
    for (; first != last; ++first) {
        init = init + *first;
    }
    return init;
}

template <typename InputIt, typename T, typename BinaryOp>
T accumulate(InputIt first, InputIt last, T init, BinaryOp op) {
    for (; first != last; ++first) {
        init = op(init, *first);
    }
    return init;
}

// ─────────────────────────────────────────────────────────────────────────────
//  四、变换
// ─────────────────────────────────────────────────────────────────────────────

template <typename InputIt, typename OutputIt, typename UnaryOp>
OutputIt transform(InputIt first, InputIt last, OutputIt d_first,
                   UnaryOp op) {
    for (; first != last; ++first, ++d_first) {
        *d_first = op(*first);
    }
    return d_first;
}

template <typename InputIt1, typename InputIt2, typename OutputIt,
          typename BinaryOp>
OutputIt transform(InputIt1 first1, InputIt1 last1, InputIt2 first2,
                   OutputIt d_first, BinaryOp op) {
    for (; first1 != last1; ++first1, ++first2, ++d_first) {
        *d_first = op(*first1, *first2);
    }
    return d_first;
}

template <typename ForwardIt>
void reverse(ForwardIt first, ForwardIt last) {
    while (first != last && first != --last) {
        ministl::swap(*first, *last);
        ++first;
    }
}

template <typename ForwardIt, typename T>
ForwardIt remove(ForwardIt first, ForwardIt last, const T& value) {
    // 把「不等于 value」的元素前移，返回新区间的尾
    first = find(first, last, value);
    if (first == last) return last;
    ForwardIt result = first;
    ++first;
    for (; first != last; ++first) {
        if (!(*first == value)) {
            *result = ministl::move(*first);
            ++result;
        }
    }
    return result;
}

template <typename ForwardIt, typename Pred>
ForwardIt remove_if(ForwardIt first, ForwardIt last, Pred pred) {
    first = find_if(first, last, pred);
    if (first == last) return last;
    ForwardIt result = first;
    ++first;
    for (; first != last; ++first) {
        if (!pred(*first)) {
            *result = ministl::move(*first);
            ++result;
        }
    }
    return result;
}

template <typename ForwardIt>
ForwardIt unique(ForwardIt first, ForwardIt last) {
    if (first == last) return last;
    ForwardIt result = first;
    while (++first != last) {
        if (!(*result == *first)) {
            ++result;
            if (result != first) {
                *result = ministl::move(*first);
            }
        }
    }
    return ++result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  五、二分查找（要求已排序）
// ─────────────────────────────────────────────────────────────────────────────

template <typename ForwardIt, typename T>
ForwardIt lower_bound(ForwardIt first, ForwardIt last, const T& value) {
    auto count = ministl::distance(first, last);
    while (count > 0) {
        auto step = count / 2;
        ForwardIt it = first;
        ministl::advance(it, step);
        if (*it < value) {
            first = ++it;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

template <typename ForwardIt, typename T>
ForwardIt upper_bound(ForwardIt first, ForwardIt last, const T& value) {
    auto count = ministl::distance(first, last);
    while (count > 0) {
        auto step = count / 2;
        ForwardIt it = first;
        ministl::advance(it, step);
        if (!(value < *it)) {
            first = ++it;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

template <typename ForwardIt, typename T>
bool binary_search(ForwardIt first, ForwardIt last, const T& value) {
    first = lower_bound(first, last, value);
    return first != last && !(value < *first);
}

// ─────────────────────────────────────────────────────────────────────────────
//  六、排序
// ─────────────────────────────────────────────────────────────────────────────

// is_sorted：判断区间是否已排好序。
// 判据是「不存在逆序对」—— 对所有 i>0 都有 !comp(*i, *(i-1))。
// 【为什么用 !comp(cur, prev) 而不是 comp(prev, cur)】
//   前者把相等元素也判为有序。严格弱序下 comp(a,a) 必须为 false，
//   所以只有真正逆序时才会返回 false。用后者会把「相等」误判成无序。
template <typename ForwardIt, typename Compare>
bool is_sorted(ForwardIt first, ForwardIt last, Compare comp) {
    if (first == last) return true;
    ForwardIt prev = first;
    ForwardIt cur = first;
    while (++cur != last) {
        if (comp(*cur, *prev)) return false;
        prev = cur;
    }
    return true;
}

template <typename ForwardIt>
bool is_sorted(ForwardIt first, ForwardIt last) {
    return is_sorted(first, last,
                     less<typename iterator_traits<ForwardIt>::value_type>{});
}

namespace detail {

// 插入排序：小数组上比快排快（无递归开销，且对近乎有序的数据接近线性）
template <typename RandomIt, typename Compare>
void insertion_sort(RandomIt first, RandomIt last, Compare comp) {
    if (first == last) return;
    for (RandomIt i = first + 1; i < last; ++i) {
        auto key = ministl::move(*i);
        RandomIt j = i;
        while (j > first && comp(key, *(j - 1))) {
            *j = ministl::move(*(j - 1));
            --j;
        }
        *j = ministl::move(key);
    }
}

// Lomuto 分区：把 ≤ pivot 的放左边，> pivot 的放右边
template <typename RandomIt, typename T, typename Compare>
RandomIt partition(RandomIt first, RandomIt last, const T& pivot, Compare comp) {
    while (true) {
        while (comp(*first, pivot)) ++first;      // 找左边第一个 ≥ pivot
        while (comp(pivot, *--last)) {}           // 找右边第一个 ≤ pivot
        if (!(first < last)) return first;
        ministl::swap(*first, *last);
        ++first;
    }
}

}  // namespace detail

// sort：快排 + 小区间插入排序。
// 尾递归被手动改成循环，避免最坏情况下栈深度达到 O(n)。
template <typename RandomIt, typename Compare>
void sort(RandomIt first, RandomIt last, Compare comp) {
    while (last - first > 16) {
        // 三数取中：取首、中、尾的中位数作 pivot，避免有序输入退化成 O(n²)
        RandomIt mid = first + (last - first) / 2;
        if (comp(*mid, *first)) ministl::swap(*mid, *first);
        if (comp(*(last - 1), *first)) ministl::swap(*(last - 1), *first);
        if (comp(*(last - 1), *mid)) ministl::swap(*(last - 1), *mid);
        // 现在 *mid 是中位数，把它换到 first 位置作为 pivot
        ministl::swap(*mid, *first);

        RandomIt cut = detail::partition(first + 1, last, *first, comp);
        ministl::swap(*first, *(cut - 1));

        // 先排小的那一半，大的那半用循环继续（尾递归消除）
        if (cut - 1 - first < last - cut) {
            sort(first, cut - 1, comp);
            first = cut;
        } else {
            sort(cut, last, comp);
            last = cut - 1;
        }
    }
    detail::insertion_sort(first, last, comp);
}

template <typename RandomIt>
void sort(RandomIt first, RandomIt last) {
    sort(first, last, less<typename iterator_traits<RandomIt>::value_type>{});
}

template <typename RandomIt, typename Compare>
void stable_sort(RandomIt first, RandomIt last, Compare comp) {
    // 简化实现：插入排序天然稳定，代价是 O(n²)。
    // 真实实现在此基础上做归并，换取 O(n log n)。
    detail::insertion_sort(first, last, comp);
}

template <typename RandomIt>
void stable_sort(RandomIt first, RandomIt last) {
    stable_sort(first, last,
                less<typename iterator_traits<RandomIt>::value_type>{});
}

}  // namespace ministl
