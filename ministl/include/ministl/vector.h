#pragma once
// =============================================================================
// ministl/vector.h —— 动态数组
// =============================================================================
// 学习要点：
//   1. 【三指针布局】begin_ / end_ / cap_ 而非「指针 + size + capacity」。
//      好处：size() 是 end_ - begin_（一次减法），迭代器就是裸指针 T*，
//      begin()/end() 零开销。这是 vector 能做到「零开销抽象」的根源。
//   2. 【迭代器就是裸指针】因为内存连续，T* 天然满足随机访问迭代器的所有
//      要求。其他容器必须写一个迭代器类，vector 不需要。
//   3. 【倍增扩容 → 摊还 O(1)】每次容量不足就翻倍，push_back 的**摊还**
//      复杂度是 O(1)。证明：n 次 push_back 总搬移次数 < 2n。
//   4. 【异常安全】扩容搬元素时用 move_if_noexcept —— 见 memory.h 的说明。
//   5. 【分配与构造分离】allocate 拿生内存，construct 逐个建对象。
//      这是 vector 能「先要一大块地、再慢慢盖房子」的原因。
//   6. 【EBO】allocator 是空类，用继承而非成员，省掉 8 字节。
//      std::vector 是 24 字节（3 个指针），本实现同样做到 24。
// =============================================================================

#include "iterator.h"
#include "memory.h"
#include "type_traits.h"
#include "utility.h"

#include <cstddef>
#include <initializer_list>
#include <stdexcept>  // std::out_of_range

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、EBO 基类：把 allocator 塞进基类，空的就不占空间
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

template <typename T, typename Alloc,
          bool = is_empty<Alloc>::value && !is_final<Alloc>::value>
class vector_base {
protected:
    Alloc alloc_;
    T* begin_ = nullptr;
    T* end_   = nullptr;
    T* cap_   = nullptr;

    Alloc&       alloc() noexcept { return alloc_; }
    const Alloc& alloc() const noexcept { return alloc_; }

    vector_base() noexcept = default;
    explicit vector_base(const Alloc& a) noexcept : alloc_(a) {}
    explicit vector_base(Alloc&& a) noexcept : alloc_(ministl::move(a)) {}
};

// 空 allocator 特化：私有继承，EBO 生效
template <typename T, typename Alloc>
class vector_base<T, Alloc, true> : private Alloc {
protected:
    T* begin_ = nullptr;
    T* end_   = nullptr;
    T* cap_   = nullptr;

    Alloc&       alloc() noexcept { return *this; }
    const Alloc& alloc() const noexcept { return *this; }

    vector_base() noexcept = default;
    explicit vector_base(const Alloc& a) noexcept : Alloc(a) {}
    explicit vector_base(Alloc&& a) noexcept : Alloc(ministl::move(a)) {}
};

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  二、vector
// ─────────────────────────────────────────────────────────────────────────────

template <typename T, typename Alloc = allocator<T>>
class vector : private detail::vector_base<T, Alloc> {
    using base = detail::vector_base<T, Alloc>;

public:
    using value_type             = T;
    using allocator_type         = Alloc;
    using size_type              = std::size_t;
    using difference_type        = std::ptrdiff_t;
    using reference              = T&;
    using const_reference        = const T&;
    using pointer                = T*;
    using const_pointer          = const T*;
    // ↓ 连续存储的红利：迭代器就是裸指针，无需自定义类
    using iterator               = T*;
    using const_iterator         = const T*;
    using reverse_iterator       = ministl::reverse_iterator<iterator>;
    using const_reverse_iterator = ministl::reverse_iterator<const_iterator>;

private:
    using base::begin_;
    using base::end_;
    using base::cap_;
    using base::alloc;

public:
    // ── 构造 / 析构 ─────────────────────────────────────────────────────────

    vector() noexcept = default;
    explicit vector(const Alloc& a) noexcept : base(a) {}

    explicit vector(size_type n) { default_append(n); }

    vector(size_type n, const T& value) { fill_append(n, value); }

    // 范围构造：用 enable_if 排除整型实参，否则 vector<int> v(5, 10)
    // 会被解析成 range 构造（InputIt = int）而非「5 个 10」
    template <typename InputIt,
              typename = enable_if_t<!is_integral_v<InputIt>>>
    vector(InputIt first, InputIt last) {
        for (; first != last; ++first) {
            emplace_back(*first);
        }
    }

    vector(std::initializer_list<T> il) {
        reserve(il.size());
        for (const auto& v : il) {
            emplace_back(v);
        }
    }

    // 拷贝构造复用源容器的 allocator（有状态 allocator 场景下必须如此：
    // 用别的 allocator 分配出来的内存，将来是还不回去的）。
    vector(const vector& other) : base(other.alloc()) {
        reserve(other.size());
        for (const auto& v : other) {
            emplace_back(v);
        }
    }

    vector(vector&& other) noexcept
        : base(ministl::move(other.alloc())) {
        begin_ = other.begin_;
        end_   = other.end_;
        cap_   = other.cap_;
        other.begin_ = other.end_ = other.cap_ = nullptr;
    }

    ~vector() {
        destroy_range(begin_, end_);
        deallocate_buffer();
    }

    // ── 赋值 ────────────────────────────────────────────────────────────────

    vector& operator=(const vector& other) {
        if (this != &other) {
            assign_range(other.begin(), other.end());
        }
        return *this;
    }

    vector& operator=(vector&& other) noexcept {
        if (this != &other) {
            destroy_range(begin_, end_);
            deallocate_buffer();
            begin_ = other.begin_;
            end_   = other.end_;
            cap_   = other.cap_;
            other.begin_ = other.end_ = other.cap_ = nullptr;
        }
        return *this;
    }

    vector& operator=(std::initializer_list<T> il) {
        assign_range(il.begin(), il.end());
        return *this;
    }

    void assign(size_type n, const T& value) {
        clear();
        reserve(n);
        fill_append(n, value);
    }

    template <typename InputIt,
              typename = enable_if_t<!is_integral_v<InputIt>>>
    void assign(InputIt first, InputIt last) {
        assign_range(first, last);
    }

    // ── 元素访问 ────────────────────────────────────────────────────────────

    reference operator[](size_type i) noexcept { return begin_[i]; }
    const_reference operator[](size_type i) const noexcept { return begin_[i]; }

    // at 带边界检查，越界抛异常；operator[] 不检查（性能优先）
    reference at(size_type i) {
        if (i >= size()) throw std::out_of_range("ministl::vector::at");
        return begin_[i];
    }
    const_reference at(size_type i) const {
        if (i >= size()) throw std::out_of_range("ministl::vector::at");
        return begin_[i];
    }

    reference front() noexcept { return *begin_; }
    const_reference front() const noexcept { return *begin_; }
    reference back() noexcept { return *(end_ - 1); }
    const_reference back() const noexcept { return *(end_ - 1); }
    pointer data() noexcept { return begin_; }
    const_pointer data() const noexcept { return begin_; }

    // ── 迭代器 ──────────────────────────────────────────────────────────────

    iterator begin() noexcept { return begin_; }
    const_iterator begin() const noexcept { return begin_; }
    const_iterator cbegin() const noexcept { return begin_; }
    iterator end() noexcept { return end_; }
    const_iterator end() const noexcept { return end_; }
    const_iterator cend() const noexcept { return end_; }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end_); }
    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end_);
    }
    reverse_iterator rend() noexcept { return reverse_iterator(begin_); }
    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin_);
    }

    // ── 容量 ────────────────────────────────────────────────────────────────
    // 全部是「指针相减」——O(1)，无额外存储

    bool empty() const noexcept { return begin_ == end_; }
    size_type size() const noexcept {
        return static_cast<size_type>(end_ - begin_);
    }
    size_type capacity() const noexcept {
        return static_cast<size_type>(cap_ - begin_);
    }
    size_type max_size() const noexcept { return alloc().max_size(); }

    // reserve：只增不减（标准规定）。小于当前容量时是空操作。
    void reserve(size_type new_cap) {
        if (new_cap > capacity()) {
            reallocate(new_cap);
        }
    }

    // shrink_to_fit：把容量缩到恰好等于 size。非强制，本实现尽力而为。
    void shrink_to_fit() {
        if (end_ != cap_) {
            reallocate(size());
        }
    }

    // ── 修改 ────────────────────────────────────────────────────────────────

    void clear() noexcept {
        destroy_range(begin_, end_);
        end_ = begin_;  // 注意：不释放内存，容量保留
    }

    // emplace_back 是唯一的「构造入口」，push_back 都转发到它
    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (end_ == cap_) {
            grow();
        }
        // 直接在末尾生内存上构造，省掉一次临时对象 + 移动
        ::new (static_cast<void*>(end_)) T(ministl::forward<Args>(args)...);
        ++end_;
        return *(end_ - 1);
    }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(ministl::move(value)); }

    void pop_back() {
        destroy_range(end_ - 1, end_);
        --end_;
    }

    void resize(size_type n) {
        if (n < size()) {
            T* new_end = begin_ + n;
            destroy_range(new_end, end_);
            end_ = new_end;
        } else if (n > size()) {
            // 注意：扩容时按「增长策略」而非恰好 n，保持摊还性能
            if (n > capacity()) {
                reallocate(growth_target(n));
            }
            end_ = uninitialized_fill_n(end_, n - size(), T());
        }
    }

    void resize(size_type n, const T& value) {
        if (n < size()) {
            T* new_end = begin_ + n;
            destroy_range(new_end, end_);
            end_ = new_end;
        } else if (n > size()) {
            if (n > capacity()) {
                reallocate(growth_target(n));
            }
            end_ = uninitialized_fill_n(end_, n - size(), value);
        }
    }

    void swap(vector& other) noexcept {
        ministl::swap(begin_, other.begin_);
        ministl::swap(end_, other.end_);
        ministl::swap(cap_, other.cap_);
        ministl::swap(alloc(), other.alloc());
    }

    // ── insert ──────────────────────────────────────────────────────────────

    iterator insert(const_iterator pos, const T& value) {
        return insert_one(pos - begin_, value);
    }

    iterator insert(const_iterator pos, T&& value) {
        return insert_one(pos - begin_, ministl::move(value));
    }

    iterator insert(const_iterator pos, size_type n, const T& value) {
        size_type idx = static_cast<size_type>(pos - begin_);
        for (size_type i = 0; i < n; ++i) {
            insert_one(idx + i, value);
        }
        return begin_ + idx;
    }

    template <typename InputIt,
              typename = enable_if_t<!is_integral_v<InputIt>>>
    iterator insert(const_iterator pos, InputIt first, InputIt last) {
        size_type idx = static_cast<size_type>(pos - begin_);
        for (; first != last; ++first, ++idx) {
            insert_one(idx, *first);
        }
        return begin_ + idx;
    }

    // ── erase ───────────────────────────────────────────────────────────────

    iterator erase(const_iterator pos) {
        T* p = begin_ + (pos - begin_);
        // 后续元素整体前移一位（用移动赋值，避免拷贝）
        for (T* q = p; q + 1 != end_; ++q) {
            *q = ministl::move(*(q + 1));
        }
        --end_;
        destroy_range(end_, end_ + 1);
        return p;
    }

    iterator erase(const_iterator first, const_iterator last) {
        T* f = begin_ + (first - begin_);
        T* l = begin_ + (last - begin_);
        size_type n = static_cast<size_type>(l - f);
        if (n == 0) return f;

        for (T* q = f; q + n != end_; ++q) {
            *q = ministl::move(*(q + n));
        }
        destroy_range(end_ - n, end_);
        end_ -= n;
        return f;
    }

private:
    // ── 内部实现 ────────────────────────────────────────────────────────────

    void deallocate_buffer() noexcept {
        if (begin_) {
            alloc().deallocate(begin_, capacity());
        }
    }

    // 增长目标：至少翻倍，且不小于需求值
    size_type growth_target(size_type needed) const noexcept {
        size_type doubled = capacity() == 0 ? 1 : capacity() * 2;
        return doubled > needed ? doubled : needed;
    }

    void grow() { reallocate(growth_target(0)); }

    // 核心：把元素搬到新缓冲区。这是 vector 唯一「贵」的操作。
    void reallocate(size_type new_cap) {
        T* new_begin = alloc().allocate(new_cap);
        T* new_end = new_begin;
        try {
            // 关键：move_if_noexcept —— 见 memory.h 的异常安全说明
            new_end = uninitialized_move_if_noexcept(begin_, end_, new_begin);
        } catch (...) {
            alloc().deallocate(new_begin, new_cap);  // 搬失败，原缓冲区完好
            throw;
        }
        // 搬完了才能销毁旧的 —— 顺序不能反
        destroy_range(begin_, end_);
        deallocate_buffer();
        begin_ = new_begin;
        end_   = new_end;
        cap_   = begin_ + new_cap;
    }

    void default_append(size_type n) {
        reserve(n);
        end_ = uninitialized_fill_n(end_, n, T());
    }

    void fill_append(size_type n, const T& value) {
        reserve(size() + n);
        end_ = uninitialized_fill_n(end_, n, value);
    }

    template <typename InputIt>
    void assign_range(InputIt first, InputIt last) {
        clear();
        for (; first != last; ++first) {
            emplace_back(*first);
        }
    }

    template <typename U>
    iterator insert_one(size_type idx, U&& value) {
        if (end_ == cap_) {
            grow();
        }
        T* p = begin_ + idx;
        if (p == end_) {
            ::new (static_cast<void*>(end_)) T(ministl::forward<U>(value));
            ++end_;
        } else {
            // 先把最后一个元素移动到未初始化区，腾出空位
            ::new (static_cast<void*>(end_)) T(ministl::move(*(end_ - 1)));
            ++end_;
            // 再从后往前移动赋值，最后把新值放进 p
            for (T* q = end_ - 2; q > p; --q) {
                *q = ministl::move(*(q - 1));
            }
            *p = ministl::forward<U>(value);
        }
        return p;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  三、比较运算符
// ─────────────────────────────────────────────────────────────────────────────
// 字典序比较，与 std 语义一致

template <typename T, typename Alloc>
bool operator==(const vector<T, Alloc>& a, const vector<T, Alloc>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i])) return false;
    }
    return true;
}

template <typename T, typename Alloc>
bool operator!=(const vector<T, Alloc>& a, const vector<T, Alloc>& b) {
    return !(a == b);
}

template <typename T, typename Alloc>
bool operator<(const vector<T, Alloc>& a, const vector<T, Alloc>& b) {
    std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] < b[i]) return true;
        if (b[i] < a[i]) return false;
    }
    return a.size() < b.size();  // 前缀相同则短的更小
}

template <typename T, typename Alloc>
bool operator>(const vector<T, Alloc>& a, const vector<T, Alloc>& b) {
    return b < a;
}

template <typename T, typename Alloc>
bool operator<=(const vector<T, Alloc>& a, const vector<T, Alloc>& b) {
    return !(b < a);
}

template <typename T, typename Alloc>
bool operator>=(const vector<T, Alloc>& a, const vector<T, Alloc>& b) {
    return !(a < b);
}

template <typename T, typename Alloc>
void swap(vector<T, Alloc>& a, vector<T, Alloc>& b) noexcept {
    a.swap(b);
}

}  // namespace ministl
