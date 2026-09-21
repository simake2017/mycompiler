#pragma once
// =============================================================================
// ministl/unordered_map.h —— 哈希表
// =============================================================================
// 学习要点：
//   1. 【链地址法】桶数组 + 每桶一条单链表。相比开放寻址，链地址法实现简单、
//      删除容易、负载因子可以 >1；代价是指针跳转多、缓存不友好。
//   2. 【为什么桶数取质数】若桶数是合数（尤其 2 的幂），当键的哈希值低位有
//      规律时（比如全是偶数、全是 8 的倍数）会对少数几个桶取模 → 严重聚集。
//      取质数能打散这种规律。现代实现改用好混合函数 + 2 的幂掩码（更快），
//      本实现用质数，因为它更直观地展示了问题本身。
//   3. 【负载因子与 rehash】元素数 / 桶数 > max_load_factor 就扩容并重散列。
//      rehash 是 O(n)，但摊还到每次插入仍是 O(1)。
//   4. 【迭代器跨桶】迭代器要能「跳过空桶」，这是哈希表迭代器比 vector 复杂
//      的唯一原因。规范化逻辑见 normalize()。
//   5. 【迭代器失效规则】rehash 后**所有**迭代器失效（节点被重新链接）；
//      但元素的**指针/引用**始终有效（节点本身没搬家）—— 这点与 vector 不同。
// =============================================================================

#include "hash.h"
#include "iterator.h"
#include "memory.h"
#include "type_traits.h"
#include "utility.h"
#include "vector.h"

#include <cstddef>
#include <initializer_list>
#include <stdexcept>

namespace ministl {

namespace detail {

// 质数桶尺寸表。rehash 时从表中挑「不小于目标值」的第一个质数。
// 表中相邻项约 2 倍关系，保证扩容是几何增长（摊还 O(1) 的前提）。
inline constexpr std::size_t HASH_PRIMES[] = {
    13UL,        29UL,        59UL,        127UL,       257UL,
    541UL,       1109UL,      2357UL,      5087UL,      10273UL,
    20753UL,     42043UL,     85229UL,     172933UL,    351061UL,
    712697UL,    1447153UL,   2938679UL,   5967347UL,   12119671UL,
    24614029UL,   49993357UL,  101542649UL, 206256359UL, 418920971UL,
};

inline std::size_t next_prime(std::size_t n) noexcept {
    for (std::size_t p : HASH_PRIMES) {
        if (p >= n) return p;
    }
    return HASH_PRIMES[sizeof(HASH_PRIMES) / sizeof(HASH_PRIMES[0]) - 1];
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  unordered_map
// ─────────────────────────────────────────────────────────────────────────────

template <typename Key, typename T, typename Hash = hash<Key>,
          typename KeyEqual = equal_to<Key>, typename Alloc = allocator<T>>
class unordered_map {
public:
    using key_type        = Key;
    using mapped_type     = T;
    using value_type      = pair<const Key, T>;
    using hasher          = Hash;
    using key_equal       = KeyEqual;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference       = value_type&;
    using const_reference = const value_type&;

    // ── 节点：桶内单链表 ──
    struct node {
        value_type kv;
        node* next;

        template <typename K, typename V>
        node(K&& k, V&& v)
            : kv(ministl::forward<K>(k), ministl::forward<V>(v)), next(nullptr) {}
    };

private:
    using node_alloc = allocator<node>;

    // 桶数组：每个元素是链表头指针
    ministl::vector<node*> buckets_;
    size_type size_ = 0;
    float max_load_ = 1.0f;
    Hash hash_;
    KeyEqual eq_;

public:
    // ── 迭代器 ──────────────────────────────────────────────────────────────
    // 三个状态：所属表、当前桶下标、当前节点。跨桶前进靠 normalize()。

    template <typename MapT, typename Ref, typename Ptr>
    class iterator_impl {
        friend class unordered_map;
        template <typename, typename, typename> friend class iterator_impl;

        MapT* map_ = nullptr;
        size_type bucket_ = 0;
        node* node_ = nullptr;

        iterator_impl(MapT* m, size_type b, node* n) : map_(m), bucket_(b), node_(n) {}

        // 从 bucket_ 开始向后找第一个非空桶，把 node_ 定位到它的头节点。
        // 找不到则停在 bucket_ == bucket_count()，即 end() 状态。
        //
        // 【易错点】这里必须**从当前 bucket_ 本身开始看**，而不是先 ++bucket_。
        // 而 operator++ 在桶内链表走完后调用它之前，已经手动 ++bucket_ 了。
        // 两者分工明确，否则要么跳过 0 号桶（begin），要么原地打转（++）。
        void find_first_valid() {
            const size_type n = map_->buckets_.size();
            while (bucket_ < n && map_->buckets_[bucket_] == nullptr) {
                ++bucket_;
            }
            node_ = (bucket_ < n) ? map_->buckets_[bucket_] : nullptr;
        }

    public:
        using iterator_category = forward_iterator_tag;
        using value_type        = typename unordered_map::value_type;
        using difference_type   = std::ptrdiff_t;
        using pointer           = Ptr;
        using reference         = Ref;

        iterator_impl() = default;

        // non-const → const 转换
        template <typename R2, typename P2,
                  typename = enable_if_t<is_convertible_v<P2, Ptr>>>
        iterator_impl(const iterator_impl<unordered_map, R2, P2>& o)
            : map_(o.map_), bucket_(o.bucket_), node_(o.node_) {}

        reference operator*() const { return node_->kv; }
        pointer operator->() const { return &node_->kv; }

        iterator_impl& operator++() {
            node_ = node_->next;       // 先在桶内链表上前进一步
            if (node_ == nullptr) {    // 链表走完 → 手动跳到下一个桶再找
                ++bucket_;
                find_first_valid();
            }
            return *this;
        }

        iterator_impl operator++(int) {
            iterator_impl tmp = *this;
            ++(*this);
            return tmp;
        }

        template <typename R2, typename P2>
        bool operator==(const iterator_impl<MapT, R2, P2>& o) const {
            return node_ == o.node_;
        }

        template <typename R2, typename P2>
        bool operator!=(const iterator_impl<MapT, R2, P2>& o) const {
            return node_ != o.node_;
        }
    };

    using iterator       = iterator_impl<unordered_map, value_type&, value_type*>;
    using const_iterator = iterator_impl<const unordered_map, const value_type&,
                                         const value_type*>;

    // ── 构造 / 析构 ─────────────────────────────────────────────────────────

    unordered_map() { rehash(detail::HASH_PRIMES[0]); }

    explicit unordered_map(size_type bucket_count) { rehash(bucket_count); }

    template <typename InputIt, typename = enable_if_t<!is_integral_v<InputIt>>>
    unordered_map(InputIt first, InputIt last) {
        rehash(detail::HASH_PRIMES[0]);
        for (; first != last; ++first) insert(*first);
    }

    unordered_map(std::initializer_list<value_type> il) {
        rehash(detail::next_prime(il.size() * 2));
        for (const auto& v : il) insert(v);
    }

    unordered_map(const unordered_map& other) {
        rehash(other.buckets_.size());
        max_load_ = other.max_load_;
        for (const auto& kv : other) insert(kv);
    }

    unordered_map(unordered_map&& other) noexcept
        : buckets_(ministl::move(other.buckets_)),
          size_(other.size_),
          max_load_(other.max_load_),
          hash_(ministl::move(other.hash_)),
          eq_(ministl::move(other.eq_)) {
        other.buckets_.clear();
        other.size_ = 0;
    }

    ~unordered_map() { destroy_all_nodes(); }

    // ── 赋值 ────────────────────────────────────────────────────────────────

    unordered_map& operator=(const unordered_map& other) {
        if (this != &other) {
            clear();
            max_load_ = other.max_load_;
            for (const auto& kv : other) insert(kv);
        }
        return *this;
    }

    unordered_map& operator=(unordered_map&& other) noexcept {
        if (this != &other) {
            destroy_all_nodes();
            buckets_ = ministl::move(other.buckets_);
            size_ = other.size_;
            max_load_ = other.max_load_;
            hash_ = ministl::move(other.hash_);
            eq_ = ministl::move(other.eq_);
            other.buckets_.clear();
            other.size_ = 0;
        }
        return *this;
    }

    unordered_map& operator=(std::initializer_list<value_type> il) {
        clear();
        for (const auto& v : il) insert(v);
        return *this;
    }

    // ── 迭代器 ──────────────────────────────────────────────────────────────

    iterator begin() noexcept {
        iterator it(this, 0, nullptr);
        it.find_first_valid();
        return it;
    }

    const_iterator begin() const noexcept {
        const_iterator it(this, 0, nullptr);
        it.find_first_valid();
        return it;
    }

    const_iterator cbegin() const noexcept { return begin(); }

    iterator end() noexcept {
        return iterator(this, buckets_.size(), nullptr);
    }

    const_iterator end() const noexcept {
        return const_iterator(this, buckets_.size(), nullptr);
    }

    const_iterator cend() const noexcept { return end(); }

    // ── 容量 ────────────────────────────────────────────────────────────────

    bool empty() const noexcept { return size_ == 0; }
    size_type size() const noexcept { return size_; }
    size_type bucket_count() const noexcept { return buckets_.size(); }
    float load_factor() const noexcept {
        return buckets_.empty() ? 0.0f
                                : static_cast<float>(size_) /
                                      static_cast<float>(buckets_.size());
    }
    float max_load_factor() const noexcept { return max_load_; }
    void max_load_factor(float f) noexcept { max_load_ = f; }

    // ── 查找 ────────────────────────────────────────────────────────────────

    iterator find(const Key& k) {
        size_type idx = bucket_index(k);
        for (node* cur = buckets_[idx]; cur; cur = cur->next) {
            if (eq_(cur->kv.first, k)) return iterator(this, idx, cur);
        }
        return end();
    }

    const_iterator find(const Key& k) const {
        size_type idx = bucket_index(k);
        for (node* cur = buckets_[idx]; cur; cur = cur->next) {
            if (eq_(cur->kv.first, k)) return const_iterator(this, idx, cur);
        }
        return end();
    }

    bool contains(const Key& k) const { return find(k) != end(); }

    size_type count(const Key& k) const { return find(k) != end() ? 1 : 0; }

    // at：找不到抛异常；operator[] 则插入默认值
    T& at(const Key& k) {
        auto it = find(k);
        if (it == end()) throw std::out_of_range("ministl::unordered_map::at");
        return it->second;
    }

    const T& at(const Key& k) const {
        auto it = find(k);
        if (it == end()) throw std::out_of_range("ministl::unordered_map::at");
        return it->second;
    }

    // operator[]：键不存在则**插入**一个值初始化的 T（这是它与 at 的关键差异）
    T& operator[](const Key& k) {
        auto it = find(k);
        if (it != end()) return it->second;
        return insert_node(k, T())->kv.second;
    }

    T& operator[](Key&& k) {
        auto it = find(k);
        if (it != end()) return it->second;
        return insert_node(ministl::move(k), T())->kv.second;
    }

    // ── 修改 ────────────────────────────────────────────────────────────────

    pair<iterator, bool> insert(const value_type& v) {
        return emplace(v.first, v.second);
    }

    pair<iterator, bool> insert(value_type&& v) {
        return emplace(ministl::move(v.first), ministl::move(v.second));
    }

    template <typename K, typename V>
    pair<iterator, bool> emplace(K&& k, V&& v) {
        size_type idx = bucket_index(k);
        // 已存在则不插入（unordered_map 的键唯一）
        for (node* cur = buckets_[idx]; cur; cur = cur->next) {
            if (eq_(cur->kv.first, k)) {
                return {iterator(this, idx, cur), false};
            }
        }
        // insert_node 内部可能触发 rehash，故下标必须按插入后的实际位置重算
        node* n = insert_node(ministl::forward<K>(k), ministl::forward<V>(v));
        return {iterator(this, bucket_index(n->kv.first), n), true};
    }

    // insert_or_assign：存在则赋值，不存在则插入
    template <typename K, typename V>
    pair<iterator, bool> insert_or_assign(K&& k, V&& v) {
        size_type idx = bucket_index(k);
        for (node* cur = buckets_[idx]; cur; cur = cur->next) {
            if (eq_(cur->kv.first, k)) {
                cur->kv.second = ministl::forward<V>(v);
                return {iterator(this, idx, cur), false};
            }
        }
        node* n = insert_node(ministl::forward<K>(k), ministl::forward<V>(v));
        return {iterator(this, bucket_index(n->kv.first), n), true};
    }

    iterator erase(const_iterator pos) {
        size_type idx = pos.bucket_;
        node* target = pos.node_;
        iterator next(this, idx, target->next);
        if (next.node_ == nullptr) {   // 本桶已空，定位到下一个非空桶
            ++next.bucket_;
            next.find_first_valid();
        }

        node* cur = buckets_[idx];
        if (cur == target) {
            buckets_[idx] = target->next;
        } else {
            while (cur && cur->next != target) cur = cur->next;
            if (cur) cur->next = target->next;
        }
        destroy_node(target);
        --size_;
        return next;
    }

    size_type erase(const Key& k) {
        size_type idx = bucket_index(k);
        node* cur = buckets_[idx];
        node* prev = nullptr;
        while (cur) {
            if (eq_(cur->kv.first, k)) {
                if (prev) {
                    prev->next = cur->next;
                } else {
                    buckets_[idx] = cur->next;
                }
                destroy_node(cur);
                --size_;
                return 1;
            }
            prev = cur;
            cur = cur->next;
        }
        return 0;
    }

    void clear() noexcept {
        destroy_all_nodes();
        for (auto& b : buckets_) b = nullptr;
        size_ = 0;
    }

    void swap(unordered_map& other) noexcept {
        buckets_.swap(other.buckets_);
        ministl::swap(size_, other.size_);
        ministl::swap(max_load_, other.max_load_);
        ministl::swap(hash_, other.hash_);
        ministl::swap(eq_, other.eq_);
    }

    // ── 哈希策略 ────────────────────────────────────────────────────────────

    // rehash：重建桶数组并重新散列所有节点。
    // 注意元素节点本身不搬家（只改 next 指针），所以**指向元素的指针/引用
    // 在 rehash 后依然有效**，只有迭代器失效。
    void rehash(size_type count) {
        count = detail::next_prime(count);
        if (count == buckets_.size()) return;

        ministl::vector<node*> new_buckets(count, nullptr);
        for (node* head : buckets_) {
            node* cur = head;
            while (cur) {
                node* next = cur->next;
                size_type idx = hash_(cur->kv.first) % count;
                cur->next = new_buckets[idx];
                new_buckets[idx] = cur;
                cur = next;
            }
        }
        buckets_ = ministl::move(new_buckets);
    }

    void reserve(size_type count) {
        rehash(static_cast<size_type>(
            static_cast<float>(count) / max_load_ + 1.0f));
    }

private:
    // ── 内部实现 ────────────────────────────────────────────────────────────

    size_type bucket_index(const Key& k) const {
        return hash_(k) % buckets_.size();
    }

    // 负载因子超限则扩容。扩容到「当前桶数 * 2」对应的下一个质数。
    void maybe_grow() {
        if (load_factor() >= max_load_) {
            rehash(buckets_.size() * 2);
        }
    }

    // 在正确桶的头部插入节点（不查重，调用方已确认键不存在）。
    //
    // 【易错点】扩容必须在这里做，而不是在 emplace 里。因为 operator[] 和
    // insert_or_assign 也走这条路 —— 早期版本把 maybe_grow 写在 emplace 里，
    // 结果 m[k]=v 插入的元素永远不触发扩容，负载因子涨到 4 以上、性能崩塌。
    // 扩容放这里，所有插入路径自动覆盖。
    template <typename K, typename V>
    node* insert_node(K&& k, V&& v) {
        node_alloc na;
        node* n = na.allocate(1);
        try {
            na.construct(n, ministl::forward<K>(k), ministl::forward<V>(v));
        } catch (...) {
            na.deallocate(n, 1);
            throw;
        }
        maybe_grow();  // 可能 rehash；节点本身不搬家，故 n 依然有效
        // 下标必须在 grow 之后算 —— grow 改变了桶数
        size_type idx = bucket_index(n->kv.first);
        n->next = buckets_[idx];
        buckets_[idx] = n;
        ++size_;
        return n;
    }

    void destroy_node(node* n) noexcept {
        node_alloc na;
        na.destroy(n);
        na.deallocate(n, 1);
    }

    void destroy_all_nodes() noexcept {
        for (node* head : buckets_) {
            while (head) {
                node* next = head->next;
                destroy_node(head);
                head = next;
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  比较运算符
// ─────────────────────────────────────────────────────────────────────────────
// 哈希表无序，所以只能判相等，不能判大小。

template <typename K, typename T, typename H, typename E, typename A>
bool operator==(const unordered_map<K, T, H, E, A>& a,
                const unordered_map<K, T, H, E, A>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end() || !(it->second == kv.second)) return false;
    }
    return true;
}

template <typename K, typename T, typename H, typename E, typename A>
bool operator!=(const unordered_map<K, T, H, E, A>& a,
                const unordered_map<K, T, H, E, A>& b) {
    return !(a == b);
}

}  // namespace ministl
