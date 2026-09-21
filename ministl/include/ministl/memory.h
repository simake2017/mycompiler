#pragma once
// =============================================================================
// ministl/memory.h —— 分配器与智能指针
// =============================================================================
// 这个文件是 ministl 的地基：vector / list / unordered_map / function 全都要
// 在这里拿到「分配内存」和「管理生命周期」的能力。
//
// 学习要点：
//   1. allocator 把「内存分配」与「对象构造」拆成两步（allocate / construct）：
//      容器因此可以「先要一块生内存，再逐个构造对象」——这正是 vector 扩容
//      和 list 插入的实现基础。C++ 的 new 表达式把两件事绑死了，反而不够用。
//   2. unique_ptr 靠 EBO（空基类优化）做到与裸指针同尺寸：空删除器被继承而
//      非作为成员，编译器不为它分配空间。
//   3. shared_ptr 的引用计数必须是原子的，但真正的难点在 weak_ptr::lock()——
//      「检查计数非零」和「计数加一」必须是**一个**原子操作，否则有竞态。
//      解法是 CAS 循环。
//   4. make_shared 只分配一次（对象与控制块同一块内存），比 shared_ptr<T>(new T)
//      少一次分配；代价是对象内存要等 weak 计数也归零才能归还。
//   5. uninitialized_* 系列必须提供**异常安全**：构造到一半抛异常时，
//      要把已构造的部分析构掉，否则泄漏。这是容器「强异常保证」的基础。
// =============================================================================

#include "iterator.h"
#include "type_traits.h"
#include "utility.h"

#include <atomic>
#include <cstddef>
#include <new>  // placement new, std::bad_alloc

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、addressof
// ─────────────────────────────────────────────────────────────────────────────
// 不能用 &obj —— 若类型重载了 operator&，那得到的是重载结果而非真实地址。
// 编译器内建 __builtin_addressof 绕过一切重载，是唯一可靠的做法。

template <typename T>
constexpr T* addressof(T& arg) noexcept {
    return __builtin_addressof(arg);
}

template <typename T>
const T* addressof(const T&&) = delete;  // 禁止对右值取址（防悬垂）

// ─────────────────────────────────────────────────────────────────────────────
//  二、allocator —— 把「分配」与「构造」拆开
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
class allocator {
public:
    using value_type      = T;
    using pointer         = T*;
    using const_pointer   = const T*;
    using reference       = T&;
    using const_reference = const T&;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    // 容器要求 allocator 可默认构造、可拷贝
    allocator() noexcept = default;
    allocator(const allocator&) noexcept = default;
    template <typename U>
    allocator(const allocator<U>&) noexcept {}

    // 只分配「生内存」，不构造对象（不调用构造函数）
    T* allocate(size_type n) {
        if (n == 0) return nullptr;
        if (n > max_size()) throw std::bad_alloc();
        // ::operator new 失败时抛 std::bad_alloc，与标准行为一致
        void* p = ::operator new(n * sizeof(T));
        return static_cast<T*>(p);
    }

    // 只归还内存，不调用析构（析构由容器显式负责）
    void deallocate(T* p, size_type /*n*/) noexcept {
        ::operator delete(static_cast<void*>(p));
    }

    size_type max_size() const noexcept {
        return static_cast<size_type>(-1) / sizeof(T);
    }

    // 在已分配的生内存上构造对象 —— 这就是「先分配后构造」的第二步
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(ministl::forward<Args>(args)...);
    }

    // 显式调用析构，不归还内存
    template <typename U>
    void destroy(U* p) {
        p->~U();
    }
};

// allocator 之间的相等比较：无状态分配器一律相等
template <typename T, typename U>
constexpr bool operator==(const allocator<T>&, const allocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
constexpr bool operator!=(const allocator<T>&, const allocator<U>&) noexcept {
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  三、未初始化内存算法（异常安全是重点）
// ─────────────────────────────────────────────────────────────────────────────
// 这些函数的共同模式：在「生内存」上逐个构造对象。
// 关键难点是异常安全——若第 k 个构造抛异常，前 k-1 个必须被析构，否则泄漏。
// try/catch 回滚是标准做法，也是容器能提供强异常保证的根基。

// destroy_range：析构一段区间。对可平凡析构的类型直接跳过整轮循环——
// 这是 vector 析构近似零成本的原因（std::string 这类有堆资源的就省不掉）。
template <typename ForwardIt>
void destroy_range(ForwardIt first, ForwardIt last) {
    using T = typename iterator_traits<ForwardIt>::value_type;
    if (!is_trivially_destructible<T>::value) {
        for (; first != last; ++first) {
            first->~T();
        }
    }
}

template <typename InputIt, typename ForwardIt>
ForwardIt uninitialized_copy(InputIt first, InputIt last, ForwardIt d_first) {
    using T = typename iterator_traits<ForwardIt>::value_type;
    ForwardIt current = d_first;
    try {
        for (; first != last; ++first, ++current) {
            ::new (static_cast<void*>(addressof(*current))) T(*first);
        }
        return current;
    } catch (...) {
        // 回滚：析构已构造的部分，然后把异常继续抛给调用者
        destroy_range(d_first, current);
        throw;
    }
}

template <typename InputIt, typename ForwardIt>
ForwardIt uninitialized_move(InputIt first, InputIt last, ForwardIt d_first) {
    using T = typename iterator_traits<ForwardIt>::value_type;
    ForwardIt current = d_first;
    try {
        for (; first != last; ++first, ++current) {
            ::new (static_cast<void*>(addressof(*current)))
                T(ministl::move(*first));
        }
        return current;
    } catch (...) {
        destroy_range(d_first, current);
        throw;
    }
}

// uninitialized_move_if_noexcept：vector 扩容专用的移动算法。
//
// 【为什么不能用 uninitialized_move】移动构造若中途抛异常，**源对象已被
// 破坏**（资源被掏空），既无法完成搬迁、也无法回滚原状 —— 强异常保证丢失。
// 拷贝构造则源对象完好，抛异常时回滚即可。
// 所以：移动构造 noexcept → 用移动（快）；否则 → 用拷贝（安全）。
// 这是「性能向安全让步」的经典权衡，也是 std::vector 扩容的真实行为。
template <typename InputIt, typename ForwardIt>
ForwardIt uninitialized_move_if_noexcept(InputIt first, InputIt last,
                                         ForwardIt d_first) {
    using T = typename iterator_traits<ForwardIt>::value_type;
    ForwardIt current = d_first;
    try {
        for (; first != last; ++first, ++current) {
            ::new (static_cast<void*>(addressof(*current)))
                T(ministl::move_if_noexcept(*first));
        }
        return current;
    } catch (...) {
        destroy_range(d_first, current);
        throw;
    }
}

template <typename ForwardIt, typename T>
void uninitialized_fill(ForwardIt first, ForwardIt last, const T& value) {
    using ValueT = typename iterator_traits<ForwardIt>::value_type;
    ForwardIt current = first;
    try {
        for (; current != last; ++current) {
            ::new (static_cast<void*>(addressof(*current))) ValueT(value);
        }
    } catch (...) {
        destroy_range(first, current);
        throw;
    }
}

template <typename ForwardIt, typename Size, typename T>
ForwardIt uninitialized_fill_n(ForwardIt first, Size n, const T& value) {
    using ValueT = typename iterator_traits<ForwardIt>::value_type;
    ForwardIt current = first;
    try {
        for (; n > 0; ++current, --n) {
            ::new (static_cast<void*>(addressof(*current))) ValueT(value);
        }
        return current;
    } catch (...) {
        destroy_range(first, current);
        throw;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  四、default_delete
// ─────────────────────────────────────────────────────────────────────────────
// 注意它是**空类**——这正是 unique_ptr 能用 EBO 压到裸指针大小的前提。

template <typename T>
struct default_delete {
    constexpr default_delete() noexcept = default;

    template <typename U,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    default_delete(const default_delete<U>&) noexcept {}

    void operator()(T* p) const noexcept {
        static_assert(sizeof(T) > 0, "不能 delete 不完整类型");
        delete p;
    }
};

// 数组特化：用 delete[] 而非 delete —— 二者不可混用，否则 UB
template <typename T>
struct default_delete<T[]> {
    constexpr default_delete() noexcept = default;

    template <typename U,
              typename = enable_if_t<is_convertible<U (*)[], T (*)[]>::value>>
    default_delete(const default_delete<U[]>&) noexcept {}

    template <typename U>
    void operator()(U* p) const noexcept {
        static_assert(sizeof(U) > 0, "不能 delete[] 不完整类型");
        delete[] p;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  五、unique_ptr —— 独占所有权 + EBO
// ─────────────────────────────────────────────────────────────────────────────
// 存储策略：删除器为空类时，**继承**它（不占空间）；否则作为成员保存。
// 这是 std::unique_ptr 能做到 sizeof == sizeof(T*) 的原因。

namespace detail {

template <typename T, typename Del,
          bool = is_empty<Del>::value && !is_final<Del>::value>
class unique_ptr_storage {
protected:
    T* ptr_;
    Del del_;

public:
    unique_ptr_storage() noexcept : ptr_(nullptr), del_() {}
    explicit unique_ptr_storage(T* p) noexcept : ptr_(p), del_() {}
    unique_ptr_storage(T* p, const Del& d) noexcept : ptr_(p), del_(d) {}
    unique_ptr_storage(T* p, Del&& d) noexcept
        : ptr_(p), del_(ministl::move(d)) {}

    T* get() const noexcept { return ptr_; }
    Del& deleter() noexcept { return del_; }
    const Del& deleter() const noexcept { return del_; }
};

// 空删除器特化：私有继承，EBO 生效，sizeof 与裸指针相同
template <typename T, typename Del>
class unique_ptr_storage<T, Del, true> : private Del {
protected:
    T* ptr_;

public:
    unique_ptr_storage() noexcept : ptr_(nullptr) {}
    explicit unique_ptr_storage(T* p) noexcept : ptr_(p) {}
    unique_ptr_storage(T* p, const Del& d) noexcept : Del(d), ptr_(p) {}
    unique_ptr_storage(T* p, Del&& d) noexcept
        : Del(ministl::move(d)), ptr_(p) {}

    T* get() const noexcept { return ptr_; }
    Del& deleter() noexcept { return *this; }
    const Del& deleter() const noexcept { return *this; }
};

}  // namespace detail

template <typename T, typename Deleter = default_delete<T>>
class unique_ptr : public detail::unique_ptr_storage<T, Deleter> {
    using storage = detail::unique_ptr_storage<T, Deleter>;

public:
    using pointer      = T*;
    using element_type = T;
    using deleter_type = Deleter;

    // ── 构造 ──
    constexpr unique_ptr() noexcept : storage() {}
    constexpr unique_ptr(std::nullptr_t) noexcept : storage() {}

    explicit unique_ptr(pointer p) noexcept : storage(p) {}
    unique_ptr(pointer p, const Deleter& d) noexcept : storage(p, d) {}
    unique_ptr(pointer p, Deleter&& d) noexcept
        : storage(p, ministl::move(d)) {}

    // 从派生类指针构造（支持多态）
    template <typename U, typename E,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    unique_ptr(unique_ptr<U, E>&& other) noexcept
        : storage(other.release(), ministl::forward<E>(other.get_deleter())) {}

    // ── 移动语义：独占所有权，禁止拷贝 ──
    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;

    unique_ptr(unique_ptr&& other) noexcept
        : storage(other.release(), ministl::forward<Deleter>(other.get_deleter())) {}

    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this != &other) {
            reset(other.release());
            this->deleter() = ministl::forward<Deleter>(other.get_deleter());
        }
        return *this;
    }

    ~unique_ptr() {
        if (this->ptr_) {
            this->deleter()(this->ptr_);
        }
    }

    // ── 观察 ──
    pointer get() const noexcept { return this->ptr_; }
    Deleter& get_deleter() noexcept { return this->deleter(); }
    const Deleter& get_deleter() const noexcept { return this->deleter(); }
    explicit operator bool() const noexcept { return this->ptr_ != nullptr; }

    // ── 解引用 ──
    // 用 add_lvalue_reference 而非 T&：支持 unique_ptr<void> 这类特化场景
    add_lvalue_reference_t<T> operator*() const {
        return *this->ptr_;
    }

    pointer operator->() const noexcept { return this->ptr_; }

    // ── 修改 ──
    pointer release() noexcept {
        pointer tmp = this->ptr_;
        this->ptr_ = nullptr;
        return tmp;
    }

    void reset(pointer p = pointer()) noexcept {
        pointer old = this->ptr_;
        this->ptr_ = p;
        if (old) {
            this->deleter()(old);
        }
    }

    void swap(unique_ptr& other) noexcept {
        ministl::swap(this->ptr_, other.ptr_);
        ministl::swap(this->deleter(), other.deleter());
    }
};

// 数组特化：提供 operator[]
template <typename T, typename Deleter>
class unique_ptr<T[], Deleter> : public detail::unique_ptr_storage<T, Deleter> {
    using storage = detail::unique_ptr_storage<T, Deleter>;

public:
    using pointer      = T*;
    using element_type = T;
    using deleter_type = Deleter;

    constexpr unique_ptr() noexcept : storage() {}
    constexpr unique_ptr(std::nullptr_t) noexcept : storage() {}
    explicit unique_ptr(pointer p) noexcept : storage(p) {}
    unique_ptr(pointer p, const Deleter& d) noexcept : storage(p, d) {}
    unique_ptr(pointer p, Deleter&& d) noexcept
        : storage(p, ministl::move(d)) {}

    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;

    unique_ptr(unique_ptr&& other) noexcept
        : storage(other.release(), ministl::forward<Deleter>(other.get_deleter())) {}

    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this != &other) {
            reset(other.release());
            this->deleter() = ministl::forward<Deleter>(other.get_deleter());
        }
        return *this;
    }

    ~unique_ptr() {
        if (this->ptr_) {
            this->deleter()(this->ptr_);
        }
    }

    pointer get() const noexcept { return this->ptr_; }
    Deleter& get_deleter() noexcept { return this->deleter(); }
    const Deleter& get_deleter() const noexcept { return this->deleter(); }
    explicit operator bool() const noexcept { return this->ptr_ != nullptr; }

    T& operator[](std::size_t i) const { return this->ptr_[i]; }

    pointer release() noexcept {
        pointer tmp = this->ptr_;
        this->ptr_ = nullptr;
        return tmp;
    }

    void reset(pointer p = pointer()) noexcept {
        pointer old = this->ptr_;
        this->ptr_ = p;
        if (old) {
            this->deleter()(old);
        }
    }

    void swap(unique_ptr& other) noexcept {
        ministl::swap(this->ptr_, other.ptr_);
        ministl::swap(this->deleter(), other.deleter());
    }
};

// ── 比较 ──
template <typename T1, typename D1, typename T2, typename D2>
bool operator==(const unique_ptr<T1, D1>& a, const unique_ptr<T2, D2>& b) {
    return a.get() == b.get();
}

template <typename T1, typename D1, typename T2, typename D2>
bool operator!=(const unique_ptr<T1, D1>& a, const unique_ptr<T2, D2>& b) {
    return !(a == b);
}

template <typename T, typename D>
bool operator==(const unique_ptr<T, D>& a, std::nullptr_t) noexcept {
    return !a;
}

template <typename T, typename D>
bool operator==(std::nullptr_t, const unique_ptr<T, D>& a) noexcept {
    return !a;
}

template <typename T, typename D>
bool operator!=(const unique_ptr<T, D>& a, std::nullptr_t) noexcept {
    return static_cast<bool>(a);
}

template <typename T, typename D>
bool operator!=(std::nullptr_t, const unique_ptr<T, D>& a) noexcept {
    return static_cast<bool>(a);
}

// ── make_unique ──
// 为什么需要它：new T(args...) 的实参求值顺序未规定，
// f(unique_ptr<T>(new T), g()) 中若 g() 抛异常，new T 的内存就泄漏了。
// make_unique 把 new 封进函数体内，杜绝这个窗口。
// 【为什么两个重载必须用 enable_if 互斥】
//   否则 make_unique<int[]>(5) 会匹配上 Args... = {int} 的那个版本，
//   展开成 new int[](5)，编译器立刻报「数组边界不确定」。
//   数组/非数组是两类语义，必须在重载决议阶段就分开。
template <typename T, typename... Args>
enable_if_t<!is_array_v<T>, unique_ptr<T>> make_unique(Args&&... args) {
    return unique_ptr<T>(new T(ministl::forward<Args>(args)...));
}

template <typename T>
enable_if_t<is_array_v<T>, unique_ptr<T>> make_unique(std::size_t n) {
    return unique_ptr<T>(new remove_extent_t<T>[n]());
}

// ─────────────────────────────────────────────────────────────────────────────
//  六、控制块 —— shared_ptr 的心脏
// ─────────────────────────────────────────────────────────────────────────────
// 引用计数协议（两个独立计数）：
//   strong_ 初始 1：代表所有 shared_ptr 的强引用
//   weak_   初始 1：代表「所有强引用隐含持有的那个弱引用」
//   strong 归零 → dispose()（销毁被管理对象），然后释放隐式弱引用
//   weak   归零 → destroy()（释放控制块自身内存）
//
// 这样设计的后果：只要还有 weak_ptr，控制块就不释放（weak_ptr 需要它来判断
// 对象是否已死）。这也是 make_shared 的对象内存要等 weak 归零才归还的原因。

namespace detail {

class control_block {
public:
    std::atomic<long> strong_{1};
    std::atomic<long> weak_{1};

    control_block() noexcept = default;
    control_block(const control_block&) = delete;
    control_block& operator=(const control_block&) = delete;

    virtual void dispose() noexcept = 0;  // 销毁被管理对象
    virtual void destroy() noexcept = 0;  // 释放控制块自身

    void add_strong() noexcept {
        strong_.fetch_add(1, std::memory_order_relaxed);
    }

    void add_weak() noexcept {
        weak_.fetch_add(1, std::memory_order_relaxed);
    }

    // acq_rel 语义：确保 dispose() 能看到其他线程对对象的全部写入，
    // 且此前的写入对后续观察者可见。
    void release_strong() noexcept {
        if (strong_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dispose();
            release_weak();
        }
    }

    void release_weak() noexcept {
        if (weak_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy();
        }
    }

    long use_count() const noexcept {
        return strong_.load(std::memory_order_relaxed);
    }

protected:
    // 非虚析构：控制块永远通过 destroy() 显式销毁，不会走 delete 基类指针
    ~control_block() = default;
};

// 控制块 A：持有裸指针 + 删除器（shared_ptr<T>(new T) 路径）
template <typename T, typename Deleter>
class ptr_control_block final : public control_block {
    T* ptr_;
    Deleter del_;

public:
    ptr_control_block(T* p, Deleter d) : ptr_(p), del_(ministl::move(d)) {}

    void dispose() noexcept override { del_(ptr_); }

    void destroy() noexcept override {
        this->~ptr_control_block();
        ::operator delete(static_cast<void*>(this));
    }
};

// 控制块 B：对象与控制块同一块内存（make_shared 路径）—— 只分配一次
template <typename T, typename... Args>
class inplace_control_block final : public control_block {
    aligned_storage_t<sizeof(T), alignof(T)> storage_;

public:
    template <typename... A>
    explicit inplace_control_block(A&&... args) {
        ::new (static_cast<void*>(&storage_)) T(ministl::forward<A>(args)...);
    }

    T* ptr() noexcept { return reinterpret_cast<T*>(&storage_); }

    void dispose() noexcept override { ptr()->~T(); }

    void destroy() noexcept override {
        this->~inplace_control_block();
        ::operator delete(static_cast<void*>(this));
    }
};

}  // namespace detail

// 前向声明：shared_ptr 与 weak_ptr 相互引用
template <typename T> class weak_ptr;
template <typename T> class shared_ptr;

// 内部标记：表示「接管一个已经持有强引用的控制块」，不要再自增计数。
//
// 【为什么需要这个 tag】若内部构造写成 shared_ptr(T*, control_block*)，
// 调用 shared_ptr<T>(p, cb) 时重载决议会选中公开的
//      template <typename U, typename Deleter> shared_ptr(U*, Deleter)
// 因为 Deleter 推导为 control_block* 是**精确匹配**，而非模板版本需要的
// 「派生类→基类」转换只是标准转换，精确匹配更优。结果控制块指针被当成
// 删除器存了起来，析构时对指针调用 operator() → 编译错误。
// 加一个 tag 参数即可让模板构造不参与竞争。
struct adopt_control_block_t {
    explicit adopt_control_block_t() = default;
};

// ─────────────────────────────────────────────────────────────────────────────
//  七、shared_ptr —— 共享所有权
// ─────────────────────────────────────────────────────────────────────────────

template <typename T>
class shared_ptr {
    template <typename U> friend class shared_ptr;
    template <typename U> friend class weak_ptr;
    template <typename U, typename... Args>
    friend shared_ptr<U> make_shared(Args&&... args);

    T* ptr_ = nullptr;
    detail::control_block* cb_ = nullptr;

    // 内部构造：接管一个「已经持有强引用」的控制块，不再自增。
    // weak_ptr::lock() 和 make_shared 都走这条路。tag 见文件上文说明。
    shared_ptr(T* p, detail::control_block* cb, adopt_control_block_t) noexcept
        : ptr_(p), cb_(cb) {}

public:
    using element_type = T;
    using weak_type    = weak_ptr<T>;

    // ── 构造 ──
    constexpr shared_ptr() noexcept = default;
    constexpr shared_ptr(std::nullptr_t) noexcept {}

    template <typename U,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    explicit shared_ptr(U* p)
        : ptr_(p),
          cb_(p ? new detail::ptr_control_block<U, default_delete<U>>(
                      p, default_delete<U>{})
                : nullptr) {}

    template <typename U, typename Deleter,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    shared_ptr(U* p, Deleter d)
        : ptr_(p),
          cb_(p ? new detail::ptr_control_block<U, Deleter>(p, ministl::move(d))
                : nullptr) {}

    // 别名构造：共享 o 的所有权，但指向 p。
    // 用途：让 shared_ptr 指向对象的一个**成员**，同时保证对象存活。
    template <typename U>
    shared_ptr(const shared_ptr<U>& o, T* p) noexcept : ptr_(p), cb_(o.cb_) {
        if (cb_) cb_->add_strong();
    }

    shared_ptr(const shared_ptr& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        if (cb_) cb_->add_strong();
    }

    template <typename U,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    shared_ptr(const shared_ptr<U>& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        if (cb_) cb_->add_strong();
    }

    shared_ptr(shared_ptr&& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        o.ptr_ = nullptr;
        o.cb_ = nullptr;
    }

    template <typename U,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    shared_ptr(shared_ptr<U>&& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        o.ptr_ = nullptr;
        o.cb_ = nullptr;
    }

    ~shared_ptr() {
        if (cb_) cb_->release_strong();
    }

    // ── 赋值 ──
    shared_ptr& operator=(const shared_ptr& o) noexcept {
        // 先自增再自减：即使 o 和 *this 共享同一个控制块也安全
        shared_ptr(o).swap(*this);
        return *this;
    }

    shared_ptr& operator=(shared_ptr&& o) noexcept {
        shared_ptr(ministl::move(o)).swap(*this);
        return *this;
    }

    template <typename U>
    shared_ptr& operator=(const shared_ptr<U>& o) noexcept {
        shared_ptr(o).swap(*this);
        return *this;
    }

    // ── 观察 ──
    T* get() const noexcept { return ptr_; }
    long use_count() const noexcept { return cb_ ? cb_->use_count() : 0; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    bool unique() const noexcept { return use_count() == 1; }

    // ── 解引用 ──
    add_lvalue_reference_t<T> operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    // 注：shared_ptr 故意不提供 operator[]，数组场景请用 shared_ptr<T[]>

    // ── 修改 ──
    void reset() noexcept { shared_ptr().swap(*this); }

    template <typename U>
    void reset(U* p) {
        shared_ptr(p).swap(*this);
    }

    template <typename U, typename Deleter>
    void reset(U* p, Deleter d) {
        shared_ptr(p, ministl::move(d)).swap(*this);
    }

    void swap(shared_ptr& o) noexcept {
        ministl::swap(ptr_, o.ptr_);
        ministl::swap(cb_, o.cb_);
    }
};

// ── 比较 ──
// 注意：比较的是 get()，即所指对象地址，而非控制块地址
template <typename T, typename U>
bool operator==(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept {
    return a.get() == b.get();
}

template <typename T, typename U>
bool operator!=(const shared_ptr<T>& a, const shared_ptr<U>& b) noexcept {
    return a.get() != b.get();
}

template <typename T>
bool operator==(const shared_ptr<T>& a, std::nullptr_t) noexcept {
    return !a;
}

template <typename T>
bool operator==(std::nullptr_t, const shared_ptr<T>& a) noexcept {
    return !a;
}

template <typename T>
bool operator!=(const shared_ptr<T>& a, std::nullptr_t) noexcept {
    return static_cast<bool>(a);
}

template <typename T>
bool operator!=(std::nullptr_t, const shared_ptr<T>& a) noexcept {
    return static_cast<bool>(a);
}

// owner_less：按「所有权」而非「地址」排序。
// 用 shared_ptr 作 map 的键时必须用它——两个指向同一对象不同成员的
// shared_ptr（别名构造产生）地址不同但所有权相同，默认 less 会判它们不等。
template <typename T>
struct owner_less;

template <typename T>
struct owner_less<shared_ptr<T>> {
    bool operator()(const shared_ptr<T>& a, const shared_ptr<T>& b) const noexcept {
        return a.get() < b.get();
    }
};

template <typename T>
struct owner_less<weak_ptr<T>> {
    bool operator()(const weak_ptr<T>& a, const weak_ptr<T>& b) const noexcept;
};

// 注：hash<shared_ptr<T>> 定义在 hash.h —— 那里才有 hash 主模板。

// ─────────────────────────────────────────────────────────────────────────────
//  八、weak_ptr —— 观察但不持有
// ─────────────────────────────────────────────────────────────────────────────
// 存在的意义：打破 shared_ptr 的循环引用。
//   A 持有 shared_ptr<B>，B 持有 shared_ptr<A> → 两者计数永不归零 → 泄漏。
//   把其中一边改成 weak_ptr，环就断了。

template <typename T>
class weak_ptr {
    template <typename U> friend class shared_ptr;
    template <typename U> friend class weak_ptr;

    T* ptr_ = nullptr;
    detail::control_block* cb_ = nullptr;

public:
    constexpr weak_ptr() noexcept = default;

    weak_ptr(const shared_ptr<T>& sp) noexcept : ptr_(sp.ptr_), cb_(sp.cb_) {
        if (cb_) cb_->add_weak();
    }

    template <typename U,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    weak_ptr(const shared_ptr<U>& sp) noexcept : ptr_(sp.ptr_), cb_(sp.cb_) {
        if (cb_) cb_->add_weak();
    }

    weak_ptr(const weak_ptr& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        if (cb_) cb_->add_weak();
    }

    template <typename U,
              typename = enable_if_t<is_convertible<U*, T*>::value>>
    weak_ptr(const weak_ptr<U>& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        if (cb_) cb_->add_weak();
    }

    weak_ptr(weak_ptr&& o) noexcept : ptr_(o.ptr_), cb_(o.cb_) {
        o.ptr_ = nullptr;
        o.cb_ = nullptr;
    }

    ~weak_ptr() {
        if (cb_) cb_->release_weak();
    }

    weak_ptr& operator=(const weak_ptr& o) noexcept {
        weak_ptr(o).swap(*this);
        return *this;
    }

    weak_ptr& operator=(weak_ptr&& o) noexcept {
        weak_ptr(ministl::move(o)).swap(*this);
        return *this;
    }

    template <typename U>
    weak_ptr& operator=(const shared_ptr<U>& sp) noexcept {
        weak_ptr(sp).swap(*this);
        return *this;
    }

    // ── 核心：lock() ──
    // 难点在于「判断对象是否还活着」与「增加强计数」必须原子完成。
    // 若分两步（先查 use_count() 再构造 shared_ptr），中间可能被其他线程
    // 析构掉对象 —— 这就是经典的 TOCTOU 竞态。
    // 解法：CAS 循环，只有当强计数非零且成功加一时才返回有效 shared_ptr。
    shared_ptr<T> lock() const noexcept {
        if (!cb_) return shared_ptr<T>();

        long expected = cb_->strong_.load(std::memory_order_relaxed);
        while (expected != 0) {
            if (cb_->strong_.compare_exchange_weak(
                    expected, expected + 1,
                    std::memory_order_acq_rel,   // 成功：获取对象所有权的可见性
                    std::memory_order_relaxed))  // 失败：重读 expected
            {
                // 计数已加，走内部构造（tag 避免与「带删除器」构造混淆）
                return shared_ptr<T>(ptr_, cb_, adopt_control_block_t{});
            }
        }
        return shared_ptr<T>();  // 对象已死
    }

    bool expired() const noexcept {
        return !cb_ || cb_->use_count() == 0;
    }

    long use_count() const noexcept { return cb_ ? cb_->use_count() : 0; }

    void reset() noexcept { weak_ptr().swap(*this); }

    void swap(weak_ptr& o) noexcept {
        ministl::swap(ptr_, o.ptr_);
        ministl::swap(cb_, o.cb_);
    }
};

template <typename T>
bool owner_less<weak_ptr<T>>::operator()(const weak_ptr<T>& a,
                                         const weak_ptr<T>& b) const noexcept {
    return a.lock().get() < b.lock().get();
}

// ─────────────────────────────────────────────────────────────────────────────
//  十、enable_shared_from_this
// ─────────────────────────────────────────────────────────────────────────────
// 场景：对象内部需要拿到「指向自己的 shared_ptr」。
// 直接写 shared_ptr<T>(this) 会创建**第二个**控制块 → 双重析构。
// 正确做法是让对象继承本类，由 shared_ptr 构造时把控制块记进 weak_this_。

template <typename T>
class enable_shared_from_this {
protected:
    weak_ptr<T> weak_this_;

    constexpr enable_shared_from_this() noexcept = default;

    // 拷贝构造**不复制** weak_this_：新对象是独立个体，尚未被任何
    // shared_ptr 接管。若复制了，两个对象会误以为共享控制块。
    enable_shared_from_this(const enable_shared_from_this&) noexcept {}
    enable_shared_from_this& operator=(const enable_shared_from_this&) noexcept {
        return *this;
    }
    ~enable_shared_from_this() = default;

public:
    // 由 make_shared 在构造完成后自动调用（见文件末尾 detail::esft_init）。
    // 之所以要显式方法而非直接让 shared_ptr 访问 weak_this_：
    // 那会造成 shared_ptr → enable_shared_from_this → weak_ptr → shared_ptr
    // 的头文件循环依赖。
    void _internal_set_weak(const weak_ptr<T>& w) noexcept { weak_this_ = w; }

    shared_ptr<T> shared_from_this() { return shared_ptr<T>(weak_this_); }
    shared_ptr<const T> shared_from_this() const {
        return shared_ptr<const T>(weak_this_);
    }
    weak_ptr<T> weak_from_this() noexcept { return weak_this_; }
    weak_ptr<const T> weak_from_this() const noexcept { return weak_this_; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  十一、指针转换
// ─────────────────────────────────────────────────────────────────────────────
// 用别名构造实现：新旧 shared_ptr 共享同一个控制块，
// 因此计数正确、不会出现双控制块问题。

template <typename T, typename U>
shared_ptr<T> static_pointer_cast(const shared_ptr<U>& sp) noexcept {
    return shared_ptr<T>(sp, static_cast<T*>(sp.get()));
}

template <typename T, typename U>
shared_ptr<T> dynamic_pointer_cast(const shared_ptr<U>& sp) noexcept {
    // 依赖多态类型的 dynamic_cast，失败返回 nullptr（此时控制块仍被正确持有）
    if (auto* p = dynamic_cast<T*>(sp.get())) {
        return shared_ptr<T>(sp, p);
    }
    return shared_ptr<T>();
}

template <typename T, typename U>
shared_ptr<T> const_pointer_cast(const shared_ptr<U>& sp) noexcept {
    return shared_ptr<T>(sp, const_cast<T*>(sp.get()));
}

template <typename T, typename U>
shared_ptr<T> reinterpret_pointer_cast(const shared_ptr<U>& sp) noexcept {
    return shared_ptr<T>(sp, reinterpret_cast<T*>(sp.get()));
}

template <typename T, typename U>
unique_ptr<T> static_pointer_cast(unique_ptr<U>&& p) noexcept {
    return unique_ptr<T>(static_cast<T*>(p.release()));
}

template <typename T, typename U>
unique_ptr<T> dynamic_pointer_cast(unique_ptr<U>&& p) noexcept {
    T* p2 = dynamic_cast<T*>(p.get());
    if (p2) {
        p.release();
        return unique_ptr<T>(p2);
    }
    return unique_ptr<T>();
}

// ─────────────────────────────────────────────────────────────────────────────
//  十二、make_shared
// ─────────────────────────────────────────────────────────────────────────────
// 【为什么定义在文件末尾】enable_shared_from_this<T> 必须在 shared_ptr 之后
// 才能定义（它持有 weak_ptr<T> 成员），而 shared_ptr 的**成员函数**又想在
// 构造后初始化 weak_this_。限定名 detail::esft_init 在定义点就要可见，
// 循环依赖无解。折中：把自动初始化放在 make_shared 这个自由函数里——
// 它的定义点在所有类型之后，一切就绪。代价是 shared_ptr<T>(new T) 这条
// 路径不会自动接上 weak_this_（推荐用 make_shared）。

namespace detail {

// 探测 T 是否提供 _internal_set_weak（即是否继承自 enable_shared_from_this<T>）
template <typename T>
auto esft_init(T* p, const shared_ptr<T>& sp, int)
    -> decltype(p->_internal_set_weak(sp), void()) {
    p->_internal_set_weak(sp);
}

// 兜底重载：类型没有该方法时走这里，什么也不做
template <typename T>
void esft_init(T*, const shared_ptr<T>&, ...) {}

}  // namespace detail

template <typename T, typename... Args>
shared_ptr<T> make_shared(Args&&... args) {
    using block_t = detail::inplace_control_block<T, Args...>;
    // 一次分配搞定：控制块与对象在同一块内存里
    block_t* cb = new block_t(ministl::forward<Args>(args)...);
    shared_ptr<T> sp(cb->ptr(), cb, adopt_control_block_t{});  // 内部构造，不自增
    // 若 T 继承自 enable_shared_from_this<T>，在此接上 weak_this_
    detail::esft_init(cb->ptr(), sp, 0);
    return sp;
}

}  // namespace ministl
