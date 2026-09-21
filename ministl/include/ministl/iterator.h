#pragma once
// =============================================================================
// ministl/iterator.h —— 迭代器基础设施
// =============================================================================
// 学习要点：
//   1. 迭代器是「容器与算法之间的契约」。算法只认迭代器，不认容器——
//      这就是 std::sort 能同时作用于 vector / deque / 原生数组的原因。
//   2. 五个类别标签的继承链编码了「能力递增」：
//        input → forward → bidirectional → random_access → contiguous
//      算法用 tag dispatch 选择最高效的实现（distance 就是典型）。
//   3. iterator_adaptor（CRTP）用来快速造迭代器适配器：派生类只需实现
//      差异部分，样板代码全在基类。设计参考 LLVM 的 iterator_adaptor_base。
//   4. reverse_iterator 的 operator* 有个反直觉细节：先自减再解引用，
//      因为「反向迭代器的位置」指向的是它前面那个元素。
// =============================================================================

#include "type_traits.h"
#include "utility.h"

#include <cstddef>

namespace ministl {

// ─────────────────────────────────────────────────────────────────────────────
//  一、迭代器类别标签
// ─────────────────────────────────────────────────────────────────────────────
// 空标签类，仅用于重载决议——不占空间、无运行时开销。

struct input_iterator_tag {};
struct output_iterator_tag {};
struct forward_iterator_tag       : input_iterator_tag {};
struct bidirectional_iterator_tag : forward_iterator_tag {};
struct random_access_iterator_tag : bidirectional_iterator_tag {};
struct contiguous_iterator_tag    : random_access_iterator_tag {};

// ─────────────────────────────────────────────────────────────────────────────
//  二、iterator_traits
// ─────────────────────────────────────────────────────────────────────────────
// 算法需要从迭代器类型里「问出」五件事。对类类型直接取内嵌 typedef，
// 对原生指针则必须特化——这是 traits 的经典用法。

template <typename Iter>
struct iterator_traits {
    using difference_type   = typename Iter::difference_type;
    using value_type        = typename Iter::value_type;
    using pointer           = typename Iter::pointer;
    using reference         = typename Iter::reference;
    using iterator_category = typename Iter::iterator_category;
};

template <typename T>
struct iterator_traits<T*> {
    using difference_type   = std::ptrdiff_t;
    using value_type        = T;
    using pointer           = T*;
    using reference         = T&;
    using iterator_category = random_access_iterator_tag;
};

template <typename T>
struct iterator_traits<const T*> {
    using difference_type   = std::ptrdiff_t;
    using value_type        = T;  // 注意：value_type 不带 const
    using pointer           = const T*;
    using reference         = const T&;
    using iterator_category = random_access_iterator_tag;
};

// ─────────────────────────────────────────────────────────────────────────────
//  三、自定义迭代器的基类（省去手写五个 typedef）
// ─────────────────────────────────────────────────────────────────────────────

template <typename Category, typename T, typename Distance = std::ptrdiff_t,
          typename Pointer = T*, typename Reference = T&>
struct iterator {
    using iterator_category = Category;
    using value_type        = T;
    using difference_type   = Distance;
    using pointer           = Pointer;
    using reference         = Reference;
};

// ─────────────────────────────────────────────────────────────────────────────
//  四、advance / distance / next / prev
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// input 及以上：只能一步步走
template <typename Iter, typename Dist>
void advance_impl(Iter& it, Dist n, input_iterator_tag) {
    while (n-- > 0) ++it;
}

// bidirectional：支持负方向
template <typename Iter, typename Dist>
void advance_impl(Iter& it, Dist n, bidirectional_iterator_tag) {
    if (n >= 0) {
        while (n-- > 0) ++it;
    } else {
        while (n++ < 0) --it;
    }
}

// random_access：一步到位（O(1)）
template <typename Iter, typename Dist>
void advance_impl(Iter& it, Dist n, random_access_iterator_tag) {
    it += n;
}

// distance：随机访问迭代器直接相减
template <typename Iter>
typename iterator_traits<Iter>::difference_type
distance_impl(Iter first, Iter last, input_iterator_tag) {
    typename iterator_traits<Iter>::difference_type n = 0;
    while (first != last) {
        ++first;
        ++n;
    }
    return n;
}

template <typename Iter>
typename iterator_traits<Iter>::difference_type
distance_impl(Iter first, Iter last, random_access_iterator_tag) {
    return last - first;
}

}  // namespace detail

// 用迭代器类别做 tag dispatch：实参是派生类对象，重载决议自动选最匹配的版本。
// contiguous / random_access 匹配 random_access_impl；forward / bidirectional
// 落到 input_impl。这正是「标签继承链」的用处。
template <typename Iter, typename Dist>
void advance(Iter& it, Dist n) {
    detail::advance_impl(it, n,
                         typename iterator_traits<Iter>::iterator_category{});
}

template <typename Iter>
typename iterator_traits<Iter>::difference_type distance(Iter first, Iter last) {
    return detail::distance_impl(
        first, last, typename iterator_traits<Iter>::iterator_category{});
}

template <typename Iter>
Iter next(Iter it, typename iterator_traits<Iter>::difference_type n = 1) {
    ministl::advance(it, n);
    return it;
}

template <typename Iter>
Iter prev(Iter it, typename iterator_traits<Iter>::difference_type n = 1) {
    ministl::advance(it, -n);
    return it;
}

// ─────────────────────────────────────────────────────────────────────────────
//  五、iterator_adaptor —— 造迭代器适配器的 CRTP 基类
// ─────────────────────────────────────────────────────────────────────────────
// 设计参考 LLVM 的 iterator_adaptor_base。
// 派生类只需：1) 继承本基类；2) 若语义不同则重写 operator*。
// 递增/递减/比较/下标等样板全部由基类提供。
//
// 为什么用 CRTP 而不是虚函数：迭代器在热循环里被调用，
// 虚函数开销不可接受。CRTP 把多态解析提前到编译期，零运行时成本。

template <typename Derived, typename BaseIt,
          typename Category = typename iterator_traits<BaseIt>::iterator_category,
          typename ValueT   = typename iterator_traits<BaseIt>::value_type,
          typename Distance = typename iterator_traits<BaseIt>::difference_type,
          typename Pointer  = ValueT*,
          typename Reference = ValueT&>
class iterator_adaptor {
protected:
    BaseIt I;  // 被包装的底层迭代器

public:
    using iterator_category = Category;
    using value_type        = ValueT;
    using difference_type   = Distance;
    using pointer           = Pointer;
    using reference         = Reference;

    iterator_adaptor() = default;
    explicit iterator_adaptor(BaseIt it) : I(it) {}

    const BaseIt& base() const { return I; }

    // 默认透传解引用；语义不同的派生类可重写
    reference operator*() const { return *I; }
    pointer operator->() const { return &*I; }

    Derived& operator++() {
        ++I;
        return static_cast<Derived&>(*this);
    }

    Derived operator++(int) {
        Derived tmp = static_cast<Derived&>(*this);
        ++I;
        return tmp;
    }

    Derived& operator--() {
        --I;
        return static_cast<Derived&>(*this);
    }

    Derived operator--(int) {
        Derived tmp = static_cast<Derived&>(*this);
        --I;
        return tmp;
    }

    Derived& operator+=(Distance n) {
        I += n;
        return static_cast<Derived&>(*this);
    }

    Derived& operator-=(Distance n) {
        I -= n;
        return static_cast<Derived&>(*this);
    }

    reference operator[](Distance n) const {
        return *(*static_cast<const Derived*>(this) + n);
    }

    // 注：这些比较运算符是普通成员函数，仅在被使用时才实例化，
    //     所以 list 迭代器（无 operator<）不会因此报错。
    bool operator==(const iterator_adaptor& o) const { return I == o.I; }
    bool operator!=(const iterator_adaptor& o) const { return I != o.I; }
    bool operator<(const iterator_adaptor& o) const { return I < o.I; }
    bool operator>(const iterator_adaptor& o) const { return I > o.I; }
    bool operator<=(const iterator_adaptor& o) const { return I <= o.I; }
    bool operator>=(const iterator_adaptor& o) const { return I >= o.I; }

    // 迭代器算术（随机访问）
    Derived operator+(Distance n) const {
        Derived tmp = static_cast<const Derived&>(*this);
        tmp += n;
        return tmp;
    }

    Derived operator-(Distance n) const {
        Derived tmp = static_cast<const Derived&>(*this);
        tmp -= n;
        return tmp;
    }

    Distance operator-(const iterator_adaptor& o) const { return I - o.I; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  六、reverse_iterator
// ─────────────────────────────────────────────────────────────────────────────
// 关键理解：反向迭代器内部存的 base() 指向「当前元素的下一个」。
// 所以 rbegin() 的 base() 是 end()，rend() 的 base() 是 begin()。
// 这就是 operator* 必须先自减再解引用的原因。

template <typename Iter>
class reverse_iterator {
protected:
    Iter current;

public:
    using iterator_type     = Iter;
    using iterator_category = typename iterator_traits<Iter>::iterator_category;
    using value_type        = typename iterator_traits<Iter>::value_type;
    using difference_type   = typename iterator_traits<Iter>::difference_type;
    using pointer           = typename iterator_traits<Iter>::pointer;
    using reference         = typename iterator_traits<Iter>::reference;

    reverse_iterator() : current() {}
    explicit reverse_iterator(Iter it) : current(it) {}

    template <typename U>
    reverse_iterator(const reverse_iterator<U>& other) : current(other.base()) {}

    Iter base() const { return current; }

    // 先退一格再解引用——current 指向的是「逻辑位置的后一个」
    reference operator*() const {
        Iter tmp = current;
        return *--tmp;
    }

    pointer operator->() const { return &(operator*()); }

    reverse_iterator& operator++() {
        --current;
        return *this;
    }

    reverse_iterator operator++(int) {
        reverse_iterator tmp = *this;
        --current;
        return tmp;
    }

    reverse_iterator& operator--() {
        ++current;
        return *this;
    }

    reverse_iterator operator--(int) {
        reverse_iterator tmp = *this;
        ++current;
        return tmp;
    }

    reverse_iterator& operator+=(difference_type n) {
        current -= n;
        return *this;
    }

    reverse_iterator operator+(difference_type n) const {
        return reverse_iterator(current - n);
    }

    reverse_iterator& operator-=(difference_type n) {
        current += n;
        return *this;
    }

    reverse_iterator operator-(difference_type n) const {
        return reverse_iterator(current + n);
    }

    reference operator[](difference_type n) const { return *(*this + n); }
};

template <typename I1, typename I2>
bool operator==(const reverse_iterator<I1>& a, const reverse_iterator<I2>& b) {
    return a.base() == b.base();
}

template <typename I1, typename I2>
bool operator!=(const reverse_iterator<I1>& a, const reverse_iterator<I2>& b) {
    return !(a == b);
}

template <typename I1, typename I2>
bool operator<(const reverse_iterator<I1>& a, const reverse_iterator<I2>& b) {
    return b.base() < a.base();  // 注意方向相反
}

template <typename Iter>
reverse_iterator<Iter> make_reverse_iterator(Iter it) {
    return reverse_iterator<Iter>(it);
}

// ─────────────────────────────────────────────────────────────────────────────
//  七、插入迭代器（output iterator）
// ─────────────────────────────────────────────────────────────────────────────
// 关键技巧：operator* 和 operator++ 都是空操作，只让 operator= 真正干活。
// 这样 std::copy(first, last, back_inserter(v)) 才能工作——
// 算法写 *result = value; ++result;，落到插入迭代器上就变成 v.push_back(value)。

template <typename Container>
class back_insert_iterator {
protected:
    Container* container;

public:
    using iterator_category = output_iterator_tag;
    using value_type        = void;
    using difference_type   = std::ptrdiff_t;
    using pointer           = void;
    using reference         = void;

    explicit back_insert_iterator(Container& c) : container(&c) {}

    back_insert_iterator& operator=(const typename Container::value_type& v) {
        container->push_back(v);
        return *this;
    }

    back_insert_iterator& operator=(typename Container::value_type&& v) {
        container->push_back(ministl::move(v));
        return *this;
    }

    back_insert_iterator& operator*() { return *this; }
    back_insert_iterator& operator++() { return *this; }
    back_insert_iterator& operator++(int) { return *this; }
};

template <typename Container>
back_insert_iterator<Container> back_inserter(Container& c) {
    return back_insert_iterator<Container>(c);
}

template <typename Container>
class front_insert_iterator {
protected:
    Container* container;

public:
    using iterator_category = output_iterator_tag;
    using value_type        = void;
    using difference_type   = std::ptrdiff_t;
    using pointer           = void;
    using reference         = void;

    explicit front_insert_iterator(Container& c) : container(&c) {}

    front_insert_iterator& operator=(const typename Container::value_type& v) {
        container->push_front(v);
        return *this;
    }

    front_insert_iterator& operator=(typename Container::value_type&& v) {
        container->push_front(ministl::move(v));
        return *this;
    }

    front_insert_iterator& operator*() { return *this; }
    front_insert_iterator& operator++() { return *this; }
    front_insert_iterator& operator++(int) { return *this; }
};

template <typename Container>
front_insert_iterator<Container> front_inserter(Container& c) {
    return front_insert_iterator<Container>(c);
}

template <typename Container>
class insert_iterator {
protected:
    Container* container;
    typename Container::iterator iter;

public:
    using iterator_category = output_iterator_tag;
    using value_type        = void;
    using difference_type   = std::ptrdiff_t;
    using pointer           = void;
    using reference         = void;

    insert_iterator(Container& c, typename Container::iterator it)
        : container(&c), iter(it) {}

    insert_iterator& operator=(const typename Container::value_type& v) {
        iter = container->insert(iter, v);
        ++iter;  // 跳过刚插入的元素，保证连续插入顺序正确
        return *this;
    }

    insert_iterator& operator=(typename Container::value_type&& v) {
        iter = container->insert(iter, ministl::move(v));
        ++iter;
        return *this;
    }

    insert_iterator& operator*() { return *this; }
    insert_iterator& operator++() { return *this; }
    insert_iterator& operator++(int) { return *this; }
};

template <typename Container>
insert_iterator<Container> inserter(Container& c,
                                    typename Container::iterator it) {
    return insert_iterator<Container>(c, it);
}

// ─────────────────────────────────────────────────────────────────────────────
//  八、begin / end 等自由函数
// ─────────────────────────────────────────────────────────────────────────────
// 它们让算法能统一处理「容器」和「原生数组」——数组没有成员 begin()。

template <typename Container>
auto begin(Container& c) -> decltype(c.begin()) {
    return c.begin();
}

template <typename Container>
auto begin(const Container& c) -> decltype(c.begin()) {
    return c.begin();
}

template <typename T, size_t N>
constexpr T* begin(T (&arr)[N]) noexcept {
    return arr;
}

template <typename Container>
auto end(Container& c) -> decltype(c.end()) {
    return c.end();
}

template <typename Container>
auto end(const Container& c) -> decltype(c.end()) {
    return c.end();
}

template <typename T, size_t N>
constexpr T* end(T (&arr)[N]) noexcept {
    return arr + N;
}

template <typename Container>
auto rbegin(Container& c) -> decltype(c.rbegin()) {
    return c.rbegin();
}

template <typename Container>
auto rend(Container& c) -> decltype(c.rend()) {
    return c.rend();
}

template <typename Container>
constexpr auto size(const Container& c) -> decltype(c.size()) {
    return c.size();
}

template <typename T, size_t N>
constexpr size_t size(const T (&)[N]) noexcept {
    return N;
}

template <typename Container>
constexpr bool empty(const Container& c) {
    return c.empty();
}

}  // namespace ministl
