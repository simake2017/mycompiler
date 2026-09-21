// =============================================================================
// tests/test_vector.cpp —— 三指针布局、倍增扩容、移动语义、迭代器失效规则
// =============================================================================
// CLion 用法：Reload CMake 后每个 TEST() 左侧出现绿色三角，点击即单跑该用例。
// =============================================================================

#include <gtest/gtest.h>

#include "ministl/vector.h"

#include <cstdio>
#include <stdexcept>

namespace {

int g_ctor = 0, g_dtor = 0, g_copy = 0, g_move = 0;

struct Tracker {
    int v;

    explicit Tracker(int x = 0) : v(x) { ++g_ctor; }
    Tracker(const Tracker& o) : v(o.v) { ++g_copy; }
    Tracker(Tracker&& o) noexcept : v(o.v) {
        o.v = -1;
        ++g_move;
    }
    Tracker& operator=(const Tracker&) = default;
    Tracker& operator=(Tracker&&) noexcept = default;
    ~Tracker() { ++g_dtor; }
};

void reset_counters() { g_ctor = g_dtor = g_copy = g_move = 0; }

}  // namespace

// 内存布局 — 三个指针共 24 字节
TEST(Vector, LayoutIsThreePointers) {
    // 标准库 vector 用 begin/end/cap 三个指针。本实现用 EBO 把空的
    // allocator 塞进基类，同样做到 24 —— 而不是 32。
    EXPECT_EQ(sizeof(ministl::vector<int>), 3 * sizeof(void*));
    std::printf("  [信息] sizeof(vector<int>) = %zu\n",
                sizeof(ministl::vector<int>));
}

// 基本操作
TEST(Vector, BasicOps) {
    ministl::vector<int> v;
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0);

    for (int i = 0; i < 5; ++i) v.push_back(i * i);
    EXPECT_EQ(v.size(), 5);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[4], 16);
    EXPECT_EQ(v.front(), 0);
    EXPECT_EQ(v.back(), 16);

    v.pop_back();
    EXPECT_EQ(v.size(), 4);
    EXPECT_EQ(v.back(), 9);
}

// 倍增扩容 — 摊还 O(1) 的来源
TEST(Vector, GeometricGrowth) {
    ministl::vector<int> v;
    std::size_t last_cap = 0;
    bool geometric = true;
    for (int i = 0; i < 100; ++i) {
        v.push_back(i);
        if (v.capacity() != last_cap) {
            if (last_cap != 0 && v.capacity() < last_cap * 2) geometric = false;
            last_cap = v.capacity();
        }
    }
    EXPECT_TRUE(geometric);
    EXPECT_EQ(v.size(), 100);
    std::printf("  [信息] 100 次 push_back 后 capacity = %zu\n", v.capacity());
}

// 扩容走移动而非拷贝
TEST(Vector, RelocationMovesNotCopies) {
    reset_counters();
    {
        ministl::vector<Tracker> v;
        v.emplace_back(1);
        v.emplace_back(2);
        v.emplace_back(3);
        // Tracker 的移动构造是 noexcept → 扩容应优先移动
        EXPECT_EQ(g_copy, 0);
        EXPECT_GT(g_move, 0);
        std::printf("  [信息] ctor=%d move=%d copy=%d\n", g_ctor, g_move, g_copy);
    }
    // 构造总数（直接 + 移动）必须等于析构总数，否则就是泄漏
    EXPECT_EQ(g_ctor + g_move, g_dtor);
}

// 迭代器就是裸指针
TEST(Vector, IteratorIsRawPointer) {
    ministl::vector<int> v{1, 2, 3};
    // 连续存储的直接证据：begin() 的类型就是 int*
    static_assert(std::is_same_v<decltype(v.begin()), int*>,
                  "vector 的迭代器必须是裸指针");
    EXPECT_EQ(v.end() - v.begin(), 3);
    EXPECT_EQ(v.data()[1], 2);
}

// range for 与反向迭代
TEST(Vector, RangeForAndReverse) {
    ministl::vector<int> v{1, 2, 3, 4};
    int sum = 0;
    for (int x : v) sum += x;
    EXPECT_EQ(sum, 10);

    ministl::vector<int> rev;
    for (auto it = v.rbegin(); it != v.rend(); ++it) rev.push_back(*it);
    EXPECT_EQ(rev[0], 4);
    EXPECT_EQ(rev[3], 1);
}

// insert / erase
TEST(Vector, InsertErase) {
    ministl::vector<int> v{1, 2, 3, 4, 5};
    v.insert(v.begin() + 2, 99);
    EXPECT_EQ(v.size(), 6);
    EXPECT_EQ(v[2], 99);
    EXPECT_EQ(v[3], 3);

    v.erase(v.begin());
    EXPECT_EQ(v.size(), 5);
    EXPECT_EQ(v[0], 2);

    v.erase(v.begin() + 1, v.begin() + 3);  // 删两个
    EXPECT_EQ(v.size(), 3);
}

// resize / reserve / shrink_to_fit
TEST(Vector, ResizeReserveShrink) {
    ministl::vector<int> v{1, 2, 3};
    v.resize(5);  // 扩大：新元素值初始化
    EXPECT_EQ(v.size(), 5);
    EXPECT_EQ(v[4], 0);

    v.resize(2);  // 缩小
    EXPECT_EQ(v.size(), 2);

    v.reserve(100);
    EXPECT_GE(v.capacity(), 100u);

    v.shrink_to_fit();  // 容量收到恰好等于 size
    EXPECT_EQ(v.capacity(), v.size());
}

// at 做边界检查，operator[] 不做
TEST(Vector, AtThrowsOutOfRange) {
    ministl::vector<int> v{1, 2, 3};
    EXPECT_EQ(v.at(1), 2);
    EXPECT_THROW(v.at(10), std::out_of_range);
}

// 拷贝是深拷贝，移动是接管
TEST(Vector, CopyIsDeepMoveSteals) {
    ministl::vector<int> a{1, 2, 3};
    ministl::vector<int> b = a;
    b[0] = 99;
    EXPECT_EQ(a[0], 1);  // 原容器不受影响
    EXPECT_EQ(b[0], 99);

    ministl::vector<int> c = ministl::move(a);
    EXPECT_EQ(c.size(), 3);
    EXPECT_EQ(c[0], 1);
    EXPECT_TRUE(a.empty());  // 源被掏空
}

// 比较运算符 — 字典序
TEST(Vector, ComparisonIsLexicographic) {
    ministl::vector<int> a{1, 2};
    ministl::vector<int> b{1, 2};
    ministl::vector<int> c{1, 3};
    ministl::vector<int> d{1, 2, 0};

    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
    EXPECT_TRUE(a < c);  // 首元素相同，比第二个
    EXPECT_TRUE(a < d);  // 前缀相同则短的更小
    EXPECT_TRUE(c > a);
}

// initializer_list 与范围构造
TEST(Vector, InitListAndRangeCtor) {
    ministl::vector<int> v{5, 6, 7};
    EXPECT_EQ(v.size(), 3);

    int arr[] = {1, 2, 3};
    ministl::vector<int> w(arr, arr + 3);
    EXPECT_EQ(w.size(), 3);
    EXPECT_EQ(w[2], 3);

    // (n, value) 形式不能被误解析为范围构造
    ministl::vector<int> x(4, 9);
    EXPECT_EQ(x.size(), 4);
    EXPECT_EQ(x[3], 9);
}
