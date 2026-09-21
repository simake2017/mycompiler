#pragma once
// =============================================================================
// ministl/hash.h —— 哈希函数
// =============================================================================
// 学习要点：
//   1. hash 的价值不在「算得快」，而在「分布均匀」。整型的恒等映射看似最省，
//      但用作哈希表下标时低位聚集会严重退化（比如指针总是 8 字节对齐，
//      低 3 位恒为 0）——所以必须做位混合（avalanche）。
//   2. mix64 是 MurmurHash3 的 finalizer：三步「异或右移 + 乘法」把输入的
//      每一位都扩散到全部输出位。这是哈希函数设计的核心手法。
//   3. 对 float 要特判 0.0：+0.0 与 -0.0 按位不同但按 == 相等，
//      若不归一化，哈希表里会出现「相等的键落在不同桶」的 bug。
//   4. 主模板**故意不定义**：让不可哈希的类型在编译期报错，而非运行期出乱。
// =============================================================================

#include "memory.h"
#include "type_traits.h"

#include <cstddef>
#include <string>       // 为 std::string 提供特化（字符串是最常用的 map 键）
#include <string_view>

namespace ministl {

namespace detail {

// MurmurHash3 finalizer（64 位）：
// 三步「异或右移 + 乘法」把输入位充分扩散。
// 这类收尾混合是哈希函数的标准做法——没有它，相邻输入会产生相邻输出，
// 哈希表退化成链表。
inline std::size_t mix64(std::size_t x) noexcept {
    // 32 位平台上 size_t 只有 32 位，右移 33 位是 UB，故按位宽分支
    if constexpr (sizeof(std::size_t) >= 8) {
        x ^= x >> 33;
        x *= static_cast<std::size_t>(0xff51afd7ed558ccdULL);
        x ^= x >> 33;
        x *= static_cast<std::size_t>(0xc4ceb9fe1a85ec53ULL);
        x ^= x >> 33;
    } else {
        x ^= x >> 16;
        x *= static_cast<std::size_t>(0x85ebca6bU);
        x ^= x >> 13;
        x *= static_cast<std::size_t>(0xc2b2ae35U);
        x ^= x >> 16;
    }
    return x;
}

// FNV-1a：对任意字节序列做哈希。用于浮点（按位哈希）和字符串。
inline std::size_t hash_bytes(const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    // 64 位 FNV 的 offset basis 与 prime
    std::size_t h = static_cast<std::size_t>(1469598103934665603ULL);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::size_t>(p[i]);
        h *= static_cast<std::size_t>(1099511628211ULL);
    }
    return h;
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  一、hash 主模板 —— 故意只有声明，没有定义
// ─────────────────────────────────────────────────────────────────────────────
// 这样 hash<MyType> 若未被特化，会在**编译期**报「不完整类型」错误，
// 而不是在运行期算出垃圾值。标准库同款设计。

template <typename T>
struct hash;

// ─────────────────────────────────────────────────────────────────────────────
//  二、整型特化
// ─────────────────────────────────────────────────────────────────────────────

#define MINISTL_INTEGRAL_HASH(TYPE)                                    \
    template <>                                                        \
    struct hash<TYPE> {                                                \
        std::size_t operator()(TYPE v) const noexcept {                \
            return detail::mix64(static_cast<std::size_t>(v));         \
        }                                                              \
    };

MINISTL_INTEGRAL_HASH(bool)
MINISTL_INTEGRAL_HASH(char)
MINISTL_INTEGRAL_HASH(signed char)
MINISTL_INTEGRAL_HASH(unsigned char)
MINISTL_INTEGRAL_HASH(wchar_t)
MINISTL_INTEGRAL_HASH(char16_t)
MINISTL_INTEGRAL_HASH(char32_t)
MINISTL_INTEGRAL_HASH(short)
MINISTL_INTEGRAL_HASH(unsigned short)
MINISTL_INTEGRAL_HASH(int)
MINISTL_INTEGRAL_HASH(unsigned int)
MINISTL_INTEGRAL_HASH(long)
MINISTL_INTEGRAL_HASH(unsigned long)
MINISTL_INTEGRAL_HASH(long long)
MINISTL_INTEGRAL_HASH(unsigned long long)

#undef MINISTL_INTEGRAL_HASH

// ─────────────────────────────────────────────────────────────────────────────
//  三、浮点特化 —— 注意 ±0.0 归一化
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct hash<float> {
    std::size_t operator()(float v) const noexcept {
        // +0.0 与 -0.0 按 == 相等，但按位不同。
        // 不归一化的话，哈希表会认为它们是两个不同的键 —— 隐性 bug。
        if (v == 0.0f) v = 0.0f;
        return detail::hash_bytes(&v, sizeof(v));
    }
};

template <>
struct hash<double> {
    std::size_t operator()(double v) const noexcept {
        if (v == 0.0) v = 0.0;
        return detail::hash_bytes(&v, sizeof(v));
    }
};

template <>
struct hash<long double> {
    std::size_t operator()(long double v) const noexcept {
        if (v == 0.0L) v = 0.0L;
        return detail::hash_bytes(&v, sizeof(v));
    }
};

template <typename T>
struct hash<T*> {
    std::size_t operator()(T* p) const noexcept {
        return detail::mix64(reinterpret_cast<std::size_t>(p));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  四、字符串特化
// ─────────────────────────────────────────────────────────────────────────────
// 逐字节 FNV-1a。虽然比「每次只取前 8 字节」慢，但分布质量高得多 ——
// 对短字符串（键的常见形态）尤其重要。

template <>
struct hash<std::string> {
    std::size_t operator()(const std::string& s) const noexcept {
        return detail::hash_bytes(s.data(), s.size());
    }
};

template <>
struct hash<std::string_view> {
    std::size_t operator()(std::string_view s) const noexcept {
        return detail::hash_bytes(s.data(), s.size());
    }
};

template <>
struct hash<const char*> {
    std::size_t operator()(const char* s) const noexcept {
        if (!s) return 0;
        std::size_t h = static_cast<std::size_t>(1469598103934665603ULL);
        for (; *s; ++s) {
            h ^= static_cast<std::size_t>(static_cast<unsigned char>(*s));
            h *= static_cast<std::size_t>(1099511628211ULL);
        }
        return h;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  五、智能指针特化（让 shared_ptr 能直接当 unordered_map 的键）
// ─────────────────────────────────────────────────────────────────────────────
// 按所指对象地址哈希——两个指向同一对象的 shared_ptr 必然同桶。

template <typename T>
struct hash<shared_ptr<T>> {
    std::size_t operator()(const shared_ptr<T>& sp) const noexcept {
        return detail::mix64(reinterpret_cast<std::size_t>(sp.get()));
    }
};

template <typename T>
struct hash<weak_ptr<T>> {
    std::size_t operator()(const weak_ptr<T>& wp) const noexcept {
        return detail::mix64(reinterpret_cast<std::size_t>(wp.lock().get()));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  五、pair 的哈希
// ─────────────────────────────────────────────────────────────────────────────
// 组合技巧：把第二个哈希值旋转后异或进第一个。
// 直接相加会让 (a,b) 和 (b,a) 碰撞，异或同理；旋转能打破这种对称性。

template <typename T1, typename T2>
struct hash<pair<T1, T2>> {
    std::size_t operator()(const pair<T1, T2>& p) const noexcept {
        std::size_t h1 = hash<T1>{}(p.first);
        std::size_t h2 = hash<T2>{}(p.second);
        // 黄金比例常数做混合，避免 h1/h2 的对称组合碰撞
        return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
                     (h1 << 6) + (h1 >> 2));
    }
};

}  // namespace ministl
