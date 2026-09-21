#pragma once
// =============================================================================
// ministl/list.h —— 双向链表
// =============================================================================
// 学习要点：
//   1. 【哨兵节点】用一个「哑节点」把首尾连成环，从此所有插入/删除都是
//      「改 4 个指针」，不需要特判空链表、头插、尾插 —— 代码量减半且不易错。
//      这是链表实现最经典的技巧，std::list 同款。
//   2. 【与 vector 的根本差异】vector 的元素连续，迭代器是裸指针，但插入/
//      删除要搬移后续元素（O(n)）；list 插入/删除只改指针（O(1)），
//      但迭代器是自定义类，且**缓存不友好**（节点散落在堆上）。
//      这是「常数因子 vs 渐进复杂度」的经典权衡。
//   3. 【splice 是 list 的杀手锏】O(1) 把一段节点从一条链表「摘」到另一条，
//      不拷贝、不移动元素、迭代器保持有效。没有任何其他容器能做到。
//   4. 【迭代器失效规则】list 只有「被删除元素的迭代器」失效，其余全部有效。
//      vector 则相反：扩容后**所有**迭代器失效。
// =============================================================================

#include "iterator.h"
#include "memory.h"
#include "type_traits.h"
#include "utility.h"

#include <cstddef>
#include <initializer_list>

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、节点设计
// ─────────────────────────────────────────────────────────────────────────────
// node_base 不含数据，因此哨兵节点可以是 node_base 类型 —— 不需要 T 就能
// 构造，空链表也不需要存一个「默认构造的 T」。

namespace detail {

struct list_node_base {
    list_node_base* next = nullptr;
    list_node_base* prev = nullptr;
};

template <typename T>
struct list_node : list_node_base {
    T value;

    template <typename... Args>
    explicit list_node(Args&&... args)
        : value(ministl::forward<Args>(args)...) {}
};

// ── 迭代器 ──
// 模板参数 Ref/Ptr 区分 const 与 non-const 迭代器（同一份代码两种实例）
template <typename T, typename Ref, typename Ptr>
class list_iterator {
public:
    using iterator_category = bidirectional_iterator_tag;
    using value_type        = T;
    using difference_type   = std::ptrdiff_t;
    using pointer           = Ptr;
    using reference         = Ref;

    list_node_base* node_ = nullptr;

    list_iterator() = default;
    explicit list_iterator(list_node_base* n) : node_(n) {}

    // non-const → const 的隐式转换（这就是 Ref/Ptr 存在的意义）
    template <typename R2, typename P2>
    list_iterator(const list_iterator<T, R2, P2>& o) : node_(o.node_) {}

    reference operator*() const {
        return static_cast<list_node<T>*>(node_)->value;
    }

    pointer operator->() const {
        return &static_cast<list_node<T>*>(node_)->value;
    }

    list_iterator& operator++() {
        node_ = node_->next;
        return *this;
    }

    list_iterator operator++(int) {
        list_iterator tmp = *this;
        node_ = node_->next;
        return tmp;
    }

    list_iterator& operator--() {
        node_ = node_->prev;
        return *this;
    }

    list_iterator operator--(int) {
        list_iterator tmp = *this;
        node_ = node_->prev;
        return tmp;
    }

    template <typename R2, typename P2>
    bool operator==(const list_iterator<T, R2, P2>& o) const {
        return node_ == o.node_;
    }

    template <typename R2, typename P2>
    bool operator!=(const list_iterator<T, R2, P2>& o) const {
        return node_ != o.node_;
    }
};

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  二、list
// ─────────────────────────────────────────────────────────────────────────────

template <typename T, typename Alloc = allocator<T>>
class list {
    using node      = detail::list_node<T>;
    using node_base = detail::list_node_base;

    // 用节点类型重新绑定 allocator —— 容器分配的是 node 而非 T
    using node_alloc = allocator<node>;

    node_base header_;              // 哨兵：header_.next 是首元素
    std::size_t size_ = 0;          // 缓存大小，让 size() 是 O(1)（C++11 起要求）

public:
    using value_type      = T;
    using allocator_type  = Alloc;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;
    using iterator        = detail::list_iterator<T, T&, T*>;
    using const_iterator  = detail::list_iterator<T, const T&, const T*>;
    using reverse_iterator       = ministl::reverse_iterator<iterator>;
    using const_reverse_iterator = ministl::reverse_iterator<const_iterator>;

    // ── 构造 / 析构 ─────────────────────────────────────────────────────────

    list() noexcept { init_sentinel(); }

    explicit list(const Alloc&) noexcept { init_sentinel(); }

    explicit list(size_type n) {
        init_sentinel();
        for (size_type i = 0; i < n; ++i) emplace_back();
    }

    list(size_type n, const T& value) {
        init_sentinel();
        for (size_type i = 0; i < n; ++i) push_back(value);
    }

    template <typename InputIt, typename = enable_if_t<!is_integral_v<InputIt>>>
    list(InputIt first, InputIt last) {
        init_sentinel();
        for (; first != last; ++first) emplace_back(*first);
    }

    list(std::initializer_list<T> il) {
        init_sentinel();
        for (const auto& v : il) push_back(v);
    }

    list(const list& other) {
        init_sentinel();
        for (const auto& v : other) push_back(v);
    }

    list(list&& other) noexcept {
        init_sentinel();
        if (!other.empty()) {
            // 直接把哨兵的指针接过来 —— O(1) 完成整条链表的转移
            header_.next = other.header_.next;
            header_.prev = other.header_.prev;
            header_.next->prev = &header_;
            header_.prev->next = &header_;
            size_ = other.size_;
            other.init_sentinel();
            other.size_ = 0;
        }
    }

    ~list() { clear(); }

    // ── 赋值 ────────────────────────────────────────────────────────────────

    list& operator=(const list& other) {
        if (this != &other) {
            assign(other.begin(), other.end());
        }
        return *this;
    }

    list& operator=(list&& other) noexcept {
        if (this != &other) {
            clear();
            if (!other.empty()) {
                header_.next = other.header_.next;
                header_.prev = other.header_.prev;
                header_.next->prev = &header_;
                header_.prev->next = &header_;
                size_ = other.size_;
                other.init_sentinel();
                other.size_ = 0;
            }
        }
        return *this;
    }

    list& operator=(std::initializer_list<T> il) {
        assign(il.begin(), il.end());
        return *this;
    }

    void assign(size_type n, const T& value) {
        clear();
        for (size_type i = 0; i < n; ++i) push_back(value);
    }

    template <typename InputIt, typename = enable_if_t<!is_integral_v<InputIt>>>
    void assign(InputIt first, InputIt last) {
        clear();
        for (; first != last; ++first) emplace_back(*first);
    }

    // ── 迭代器 ──────────────────────────────────────────────────────────────
    // 注意 begin() 是 header_.next，end() 是 &header_ —— 哨兵即 end

    iterator begin() noexcept { return iterator(header_.next); }
    const_iterator begin() const noexcept { return const_iterator(header_.next); }
    const_iterator cbegin() const noexcept { return const_iterator(header_.next); }
    iterator end() noexcept { return iterator(&header_); }
    const_iterator end() const noexcept { return const_iterator(&header_); }
    const_iterator cend() const noexcept { return const_iterator(&header_); }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    // ── 容量 ────────────────────────────────────────────────────────────────

    bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }
    size_type max_size() const noexcept { return static_cast<size_type>(-1); }

    // ── 元素访问 ────────────────────────────────────────────────────────────

    reference front() noexcept {
        return static_cast<node*>(header_.next)->value;
    }
    const_reference front() const noexcept {
        return static_cast<const node*>(header_.next)->value;
    }
    reference back() noexcept {
        return static_cast<node*>(header_.prev)->value;
    }
    const_reference back() const noexcept {
        return static_cast<const node*>(header_.prev)->value;
    }

    // ── 修改 ────────────────────────────────────────────────────────────────

    template <typename... Args>
    reference emplace_front(Args&&... args) {
        insert_node(header_.next, make_node(ministl::forward<Args>(args)...));
        return front();
    }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        insert_node(&header_, make_node(ministl::forward<Args>(args)...));
        return back();
    }

    void push_front(const T& value) { emplace_front(value); }
    void push_front(T&& value) { emplace_front(ministl::move(value)); }
    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(ministl::move(value)); }

    void pop_front() { erase_node(header_.next); }
    void pop_back() { erase_node(header_.prev); }

    // 在 pos 之前插入
    iterator insert(const_iterator pos, const T& value) {
        return insert_node(pos.node_, make_node(value));
    }

    iterator insert(const_iterator pos, T&& value) {
        return insert_node(pos.node_, make_node(ministl::move(value)));
    }

    iterator insert(const_iterator pos, size_type n, const T& value) {
        iterator ret = iterator(pos.node_);
        for (size_type i = 0; i < n; ++i) {
            ret = insert_node(pos.node_, make_node(value));
        }
        return ret;
    }

    template <typename... Args>
    iterator emplace(const_iterator pos, Args&&... args) {
        return insert_node(pos.node_,
                           make_node(ministl::forward<Args>(args)...));
    }

    iterator erase(const_iterator pos) {
        node_base* next = pos.node_->next;
        erase_node(pos.node_);
        return iterator(next);
    }

    iterator erase(const_iterator first, const_iterator last) {
        while (first != last) {
            first = erase(first);
        }
        return iterator(last.node_);
    }

    void clear() noexcept {
        node_base* cur = header_.next;
        while (cur != &header_) {
            node_base* next = cur->next;
            destroy_node(static_cast<node*>(cur));
            cur = next;
        }
        init_sentinel();
        size_ = 0;
    }

    void resize(size_type n) {
        while (size_ > n) pop_back();
        while (size_ < n) emplace_back();
    }

    void resize(size_type n, const T& value) {
        while (size_ > n) pop_back();
        while (size_ < n) push_back(value);
    }

    void swap(list& other) noexcept {
        // 交换哨兵会破坏自引用（节点指回原哨兵），所以要逐个修正
        list tmp(ministl::move(other));
        other = ministl::move(*this);
        *this = ministl::move(tmp);
    }

    // ── list 特有操作 ───────────────────────────────────────────────────────

    // splice：O(1) 把 other 的整条链摘到 pos 之前 —— 不拷贝任何元素
    void splice(const_iterator pos, list& other) {
        if (other.empty()) return;
        node_base* first = other.header_.next;
        node_base* last  = other.header_.prev;
        node_base* p     = pos.node_;

        // 从 other 摘除
        other.header_.next = &other.header_;
        other.header_.prev = &other.header_;

        // 插入到 *this 的 p 之前
        first->prev = p->prev;
        last->next  = p;
        p->prev->next = first;
        p->prev       = last;

        size_ += other.size_;
        other.size_ = 0;
    }

    // splice 单个元素
    void splice(const_iterator pos, list& other, const_iterator it) {
        node_base* n = it.node_;
        // 先从 other 摘除
        n->prev->next = n->next;
        n->next->prev = n->prev;
        other.size_--;

        // 插入到 *this
        node_base* p = pos.node_;
        n->prev = p->prev;
        n->next = p;
        p->prev->next = n;
        p->prev = n;

        size_++;
    }

    // reverse：只交换每个节点的 next/prev，O(n) 但零分配
    void reverse() noexcept {
        if (size_ < 2) return;
        node_base* cur = &header_;
        do {
            ministl::swap(cur->next, cur->prev);
            cur = cur->prev;  // 注意：交换后应走 prev（它现在是原来的 next）
        } while (cur != &header_);
    }

    // remove：删除所有等于 value 的元素
    void remove(const T& value) {
        for (auto it = begin(); it != end();) {
            if (*it == value) {
                it = erase(it);
            } else {
                ++it;
            }
        }
    }

    template <typename Pred>
    void remove_if(Pred pred) {
        for (auto it = begin(); it != end();) {
            if (pred(*it)) {
                it = erase(it);
            } else {
                ++it;
            }
        }
    }

    // unique：删除**连续**重复元素（只与相邻比较，所以先 sort 才能全局去重）
    void unique() {
        if (size_ < 2) return;
        auto it = begin();
        auto next = it;
        ++next;
        while (next != end()) {
            if (*it == *next) {
                next = erase(next);
            } else {
                it = next;
                ++next;
            }
        }
    }

    // sort：归并排序。链表排序用归并最合适——不需要随机访问，
    // 且可以纯靠改指针完成，不需要额外数组。
    void sort() {
        if (size_ < 2) return;
        list carry;
        list counter[64];  // 二进制计数器式归并：第 i 个桶装 2^i 个元素
        int fill = 0;

        while (!empty()) {
            carry.splice(carry.begin(), *this, begin());
            int i = 0;
            while (i < fill && !counter[i].empty()) {
                counter[i].merge(carry);
                carry.swap(counter[i]);
                ++i;
            }
            carry.swap(counter[i]);
            if (i == fill) ++fill;
        }

        for (int i = 1; i < fill; ++i) {
            counter[i].merge(counter[i - 1]);
        }
        swap(counter[fill - 1]);
    }

    // merge：把有序的 other 并入本链表，保持有序。不稳定，但零拷贝。
    void merge(list& other) {
        merge(other, [](const T& a, const T& b) { return a < b; });
    }

    template <typename Compare>
    void merge(list& other, Compare comp) {
        if (this == &other) return;
        auto first1 = begin();
        auto last1  = end();
        auto first2 = other.begin();

        while (first1 != last1 && first2 != other.end()) {
            if (comp(*first2, *first1)) {
                // 把 other 的当前节点摘到 first1 之前
                auto next = first2;
                ++next;
                splice(first1, other, first2);
                first2 = next;
            } else {
                ++first1;
            }
        }
        if (first2 != other.end()) {
            splice(last1, other, first2);
        }
    }

private:
    // ── 内部实现 ────────────────────────────────────────────────────────────

    void init_sentinel() noexcept {
        header_.next = &header_;
        header_.prev = &header_;
    }

    template <typename... Args>
    node* make_node(Args&&... args) {
        node_alloc na;
        node* n = na.allocate(1);
        try {
            na.construct(n, ministl::forward<Args>(args)...);
        } catch (...) {
            na.deallocate(n, 1);
            throw;
        }
        return n;
    }

    void destroy_node(node* n) noexcept {
        node_alloc na;
        na.destroy(n);
        na.deallocate(n, 1);
    }

    // 把新节点 n 插入到 pos 之前
    template <typename... Args>
    iterator insert_node(node_base* pos, node* n) {
        n->prev = pos->prev;
        n->next = pos;
        pos->prev->next = n;
        pos->prev = n;
        ++size_;
        return iterator(n);
    }

    void erase_node(node_base* n) noexcept {
        n->prev->next = n->next;
        n->next->prev = n->prev;
        destroy_node(static_cast<node*>(n));
        --size_;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  三、比较运算符
// ─────────────────────────────────────────────────────────────────────────────

template <typename T, typename Alloc>
bool operator==(const list<T, Alloc>& a, const list<T, Alloc>& b) {
    if (a.size() != b.size()) return false;
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (!(*ia == *ib)) return false;
    }
    return true;
}

template <typename T, typename Alloc>
bool operator!=(const list<T, Alloc>& a, const list<T, Alloc>& b) {
    return !(a == b);
}

template <typename T, typename Alloc>
bool operator<(const list<T, Alloc>& a, const list<T, Alloc>& b) {
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
        if (*ia < *ib) return true;
        if (*ib < *ia) return false;
    }
    return a.size() < b.size();
}

template <typename T, typename Alloc>
bool operator>(const list<T, Alloc>& a, const list<T, Alloc>& b) {
    return b < a;
}

template <typename T, typename Alloc>
bool operator<=(const list<T, Alloc>& a, const list<T, Alloc>& b) {
    return !(b < a);
}

template <typename T, typename Alloc>
bool operator>=(const list<T, Alloc>& a, const list<T, Alloc>& b) {
    return !(a < b);
}

}  // namespace ministl
