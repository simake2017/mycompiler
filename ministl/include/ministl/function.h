#pragma once
// =============================================================================
// ministl/function.h —— 类型擦除
// =============================================================================
// 【这是 ministl 的分水岭】看懂这个文件，很多库里的「魔法」都会失效。
//
// 要解决的问题：容器只能存同类型元素。怎么把 lambda、函数指针、仿函数
// 这些**类型各不相同**的东西，塞进同一个 vector<???>？
//
// 答案：类型擦除（type erasure）—— 把「具体类型」藏到运行期的一张函数表里，
// 对外只暴露一个统一接口。三件套：
//
//   1. 静态多态 → 动态多态：用一个模板派生类把「具体类型 F」记下来，
//      把对 F 的每种操作（调用/析构/拷贝/移动）变成一个函数指针。
//   2. vtable（函数表）：一组函数指针，每个 F 一份（靠 static 局部变量实现）。
//   3. SBO（小对象优化）：小的可调用对象直接存在对象内部的缓冲区里，
//      避免一次堆分配。标准库的 std::function 通常有 16 字节 SBO。
//
// 【为什么能同时做到「接口统一」和「零虚函数」】
// 没用继承 + virtual，而是「手写 vtable」。好处是：存进去的 F 不需要继承
// 任何基类（lambda 当然也不可能继承），擦除在**构造时**一次性完成。
//
// 代价：
//   • 每次调用多一次函数指针间接跳转（无法内联，比直接调 lambda 慢）
//   • 可能有一次堆分配（超出 SBO 时）
//   • 丢失具体类型（要用 target<T>() 才能取回）
// =============================================================================

#include "memory.h"
#include "type_traits.h"
#include "utility.h"

#include <cstddef>
#include <exception>

namespace ministl {

// function 为空时调用抛出的异常
class bad_function_call : public std::exception {
public:
    const char* what() const noexcept override {
        return "ministl::bad_function_call: 调用了空的 function";
    }
};

template <typename Signature>
class function;

template <typename R, typename... Args>
class function<R(Args...)> {
public:
    using result_type = R;

    // ── 函数表：每个可调用类型 F 一份 ──────────────────────────────────────
    // 四个操作覆盖了 function 需要对被擦除对象做的所有事。
    // 注意 copy_to / move_to 接收**已分配好的**目标地址 —— 分配由外部负责，
    // 这样 SBO 与堆两条路径能共用同一张表。
    struct vtable {
        R (*invoke)(void* obj, Args&&... args);
        void (*destroy)(void* obj) noexcept;
        void (*copy_to)(void* dst, const void* src);
        void (*move_to)(void* dst, void* src) noexcept;
        std::size_t size;
    };

private:
    // ── 存储：SBO 缓冲区与堆指针共用一块空间 ──────────────────────────────
    static constexpr std::size_t SBO_SIZE = 32;

    union storage_t {
        void* ptr;
        aligned_storage_t<SBO_SIZE, alignof(std::max_align_t)> buf;
        storage_t() noexcept {}
    };

    storage_t storage_{};
    const vtable* vt_ = nullptr;
    bool local_ = false;  // true → 对象在 storage_.buf 里；false → 在堆上

    void* obj_ptr() noexcept {
        return local_ ? static_cast<void*>(&storage_.buf) : storage_.ptr;
    }
    const void* obj_ptr() const noexcept {
        return local_ ? static_cast<const void*>(&storage_.buf) : storage_.ptr;
    }

    // ── 为具体类型 F 生成函数表 ───────────────────────────────────────────
    // 用函数模板的 static 局部变量：每个 F 恰好一份，首次调用时初始化。
    // 这是「编译期生成运行期表」的标准手法。
    template <typename F>
    struct vt_impl {
        static R invoke(void* o, Args&&... args) {
            // R 可能是 void，不能写 return —— 必须编译期分支
            if constexpr (is_void_v<R>) {
                (*static_cast<F*>(o))(ministl::forward<Args>(args)...);
            } else {
                return (*static_cast<F*>(o))(ministl::forward<Args>(args)...);
            }
        }

        static void destroy(void* o) noexcept {
            static_cast<F*>(o)->~F();
        }

        static void copy_to(void* dst, const void* src) {
            ::new (dst) F(*static_cast<const F*>(src));
        }

        static void move_to(void* dst, void* src) noexcept {
            ::new (dst) F(ministl::move(*static_cast<F*>(src)));
            static_cast<F*>(src)->~F();
        }

        static const vtable* get() {
            static const vtable vt = {&invoke, &destroy, &copy_to, &move_to,
                                      sizeof(F)};
            return &vt;
        }
    };

    // 大且不对齐的对象放堆上，其余进 SBO 缓冲区
    template <typename F>
    static constexpr bool fits_sbo =
        sizeof(F) <= SBO_SIZE && alignof(F) <= alignof(std::max_align_t);

    template <typename F>
    void construct(F&& f) {
        using D = decay_t<F>;
        vt_ = vt_impl<D>::get();
        if constexpr (fits_sbo<D>) {
            local_ = true;
            ::new (static_cast<void*>(&storage_.buf)) D(ministl::forward<F>(f));
        } else {
            local_ = false;
            storage_.ptr = ::operator new(sizeof(D));
            try {
                ::new (storage_.ptr) D(ministl::forward<F>(f));
            } catch (...) {
                ::operator delete(storage_.ptr);
                vt_ = nullptr;
                throw;
            }
        }
    }

    void reset() noexcept {
        if (!vt_) return;
        vt_->destroy(obj_ptr());
        if (!local_) {
            ::operator delete(storage_.ptr);
        }
        vt_ = nullptr;
        local_ = false;
    }

public:
    // ── 构造 / 析构 ─────────────────────────────────────────────────────────

    function() noexcept = default;
    function(std::nullptr_t) noexcept {}

    // 万能构造：任何可调用对象都能装进来（除了 function 自己，那是拷贝/移动）
    template <typename F,
              typename = enable_if_t<!is_same_v<decay_t<F>, function>>>
    function(F&& f) {
        construct(ministl::forward<F>(f));
    }

    function(const function& other) {
        if (!other.vt_) return;
        vt_ = other.vt_;
        local_ = other.local_;
        if (local_) {
            vt_->copy_to(&storage_.buf, &other.storage_.buf);
        } else {
            // 堆路径：先分配同样大小的空间，再拷过去
            storage_.ptr = ::operator new(vt_->size);
            try {
                vt_->copy_to(storage_.ptr, other.storage_.ptr);
            } catch (...) {
                ::operator delete(storage_.ptr);
                vt_ = nullptr;
                throw;
            }
        }
    }

    function(function&& other) noexcept {
        if (!other.vt_) return;
        vt_ = other.vt_;
        local_ = other.local_;
        if (local_) {
            vt_->move_to(&storage_.buf, &other.storage_.buf);
        } else {
            storage_.ptr = other.storage_.ptr;  // 直接偷指针，O(1)
        }
        other.vt_ = nullptr;
        other.local_ = false;
    }

    ~function() { reset(); }

    // ── 赋值：copy-and-swap 一并处理拷贝与移动 ─────────────────────────────
    function& operator=(const function& other) {
        function(other).swap(*this);
        return *this;
    }

    function& operator=(function&& other) noexcept {
        function(ministl::move(other)).swap(*this);
        return *this;
    }

    function& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    template <typename F,
              typename = enable_if_t<!is_same_v<decay_t<F>, function>>>
    function& operator=(F&& f) {
        function(ministl::forward<F>(f)).swap(*this);
        return *this;
    }

    // ── 调用 ────────────────────────────────────────────────────────────────
    // 唯一的运行期开销：一次函数指针跳转（无法内联）
    R operator()(Args... args) const {
        if (!vt_) throw bad_function_call{};
        if constexpr (is_void_v<R>) {
            vt_->invoke(const_cast<void*>(obj_ptr()),
                        ministl::forward<Args>(args)...);
        } else {
            return vt_->invoke(const_cast<void*>(obj_ptr()),
                               ministl::forward<Args>(args)...);
        }
    }

    // ── 观察 ────────────────────────────────────────────────────────────────

    bool empty() const noexcept { return vt_ == nullptr; }
    explicit operator bool() const noexcept { return vt_ != nullptr; }

    void swap(function& other) noexcept {
        ministl::swap(storage_, other.storage_);
        ministl::swap(vt_, other.vt_);
        ministl::swap(local_, other.local_);
    }

    // target<T>()：把擦除掉的类型取回来 —— 擦除是可逆的，只要你记得原类型
    template <typename T>
    T* target() noexcept {
        return (vt_ == vt_impl<T>::get()) ? static_cast<T*>(obj_ptr()) : nullptr;
    }

    template <typename T>
    const T* target() const noexcept {
        return (vt_ == vt_impl<T>::get()) ? static_cast<const T*>(obj_ptr())
                                          : nullptr;
    }
};

template <typename R, typename... Args>
bool operator==(const function<R(Args...)>& f, std::nullptr_t) noexcept {
    return !f;
}

template <typename R, typename... Args>
bool operator==(std::nullptr_t, const function<R(Args...)>& f) noexcept {
    return !f;
}

template <typename R, typename... Args>
bool operator!=(const function<R(Args...)>& f, std::nullptr_t) noexcept {
    return static_cast<bool>(f);
}

template <typename R, typename... Args>
bool operator!=(std::nullptr_t, const function<R(Args...)>& f) noexcept {
    return static_cast<bool>(f);
}

}  // namespace ministl
