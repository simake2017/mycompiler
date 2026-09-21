// =============================================================================
// tests/test_containers.cpp —— list（哨兵双向链表） / unordered_map（哈希表）
// =============================================================================
// CLion 用法：Reload CMake 后每个 TEST() 左侧出现绿色三角，点击即单跑该用例。
// =============================================================================

#include <gtest/gtest.h>

#include "ministl/algorithm.h"
#include "ministl/list.h"
#include "ministl/unordered_map.h"

#include <cstdio>
#include <string>

namespace {

int g_alive = 0;

struct Tracked {
    int v;
    explicit Tracked(int x = 0) : v(x) { ++g_alive; }
    Tracked(const Tracked& o) : v(o.v) { ++g_alive; }
    Tracked(Tracked&& o) noexcept : v(o.v) { ++g_alive; }
    ~Tracked() { --g_alive; }
};

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  list
// ═════════════════════════════════════════════════════════════════════════════

// list — 基本操作
TEST(List, BasicOps) {
    ministl::list<int> l;
    EXPECT_TRUE(l.empty());
    l.push_back(2);
    l.push_back(3);
    l.push_front(1);
    EXPECT_EQ(l.size(), 3);
    EXPECT_EQ(l.front(), 1);
    EXPECT_EQ(l.back(), 3);

    int expect = 1;
    for (int x : l) EXPECT_EQ(x, expect++);

    l.pop_front();
    l.pop_back();
    EXPECT_EQ(l.size(), 1);
    EXPECT_EQ(l.front(), 2);
}

// list — 中间插入 O(1)
TEST(List, InsertInMiddle) {
    ministl::list<int> l{1, 2, 3, 4};
    auto it = l.begin();
    ++it;              // 指向 2
    l.insert(it, 99);  // 在 2 前面插
    EXPECT_EQ(l.size(), 5);
    auto p = l.begin();
    EXPECT_EQ(*p, 1);
    EXPECT_EQ(*(++p), 99);
    EXPECT_EQ(*(++p), 2);

    auto nx = l.erase(l.begin());  // erase 返回下一个位置
    EXPECT_EQ(*nx, 99);
    EXPECT_EQ(l.size(), 4);
}

// list — 增删不使其它迭代器失效
TEST(List, IteratorStability) {
    // 链表相对 vector 的核心优势：节点不搬家，只有被删的那个失效
    ministl::list<int> l{1, 2, 3};
    auto mid = l.begin();
    ++mid;  // 指向 2
    l.push_front(0);
    l.push_back(4);
    EXPECT_EQ(*mid, 2);  // 插入后依然有效

    l.erase(l.begin());  // 删掉别的节点
    EXPECT_EQ(*mid, 2);  // 仍然有效
}

// list — sort / merge / splice / reverse / unique
TEST(List, SortMergeSpliceReverseUnique) {
    ministl::list<int> l{5, 2, 9, 1, 7, 2};
    l.sort();
    int expect[] = {1, 2, 2, 5, 7, 9};
    int i = 0;
    for (int x : l) EXPECT_EQ(x, expect[i++]);

    l.unique();  // 去掉相邻重复
    EXPECT_EQ(l.size(), 5);

    ministl::list<int> other{0, 3, 8};
    l.merge(other);        // 归并两个有序链表
    EXPECT_TRUE(other.empty());  // 被搬空
    EXPECT_EQ(l.size(), 8);
    EXPECT_TRUE(ministl::is_sorted(l.begin(), l.end()));

    l.reverse();
    EXPECT_EQ(l.front(), 9);
    EXPECT_TRUE(ministl::is_sorted(l.begin(), l.end(),
                                   [](int a, int b) { return a > b; }));

    // splice：整段摘链，O(1)，不拷贝任何元素
    ministl::list<int> dst{100};
    dst.splice(dst.begin(), l);
    EXPECT_EQ(dst.size(), 9);
    EXPECT_TRUE(l.empty());
}

// list — 元素析构不漏不重
TEST(List, ElementLifetime) {
    g_alive = 0;
    {
        ministl::list<Tracked> l;
        for (int i = 0; i < 4; ++i) l.emplace_back(i);
        EXPECT_EQ(g_alive, 4);
        l.pop_back();
        EXPECT_EQ(g_alive, 3);
        l.clear();
        EXPECT_EQ(g_alive, 0);
    }
    EXPECT_EQ(g_alive, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  unordered_map
// ═════════════════════════════════════════════════════════════════════════════

// unordered_map — 插入与查找
TEST(UnorderedMap, InsertAndFind) {
    ministl::unordered_map<std::string, int> m;
    m["apple"] = 1;
    m["banana"] = 2;
    m.insert({"cherry", 3});
    m.emplace("durian", 4);

    EXPECT_EQ(m.size(), 4);
    EXPECT_EQ(m["apple"], 1);
    EXPECT_EQ(m.at("banana"), 2);
    EXPECT_TRUE(m.contains("durian"));
    EXPECT_FALSE(m.contains("elderberry"));
    EXPECT_EQ(m.count("apple"), 1);
    EXPECT_EQ(m.count("nope"), 0);
}

// unordered_map — operator[] 会插入，at 会抛
TEST(UnorderedMap, SubscriptInsertsAtThrows) {
    ministl::unordered_map<std::string, int> m;
    EXPECT_EQ(m.size(), 0);
    int& ref = m["new"];  // [] 对不存在的键插入值初始化的值
    EXPECT_EQ(m.size(), 1);
    EXPECT_EQ(ref, 0);
    ref = 42;
    EXPECT_EQ(m["new"], 42);

    EXPECT_THROW(m.at("missing"), std::out_of_range);
}

// unordered_map — insert 不覆盖已有键
TEST(UnorderedMap, InsertDoesNotOverwrite) {
    ministl::unordered_map<std::string, int> m{{"k", 1}};
    auto r = m.insert({"k", 999});
    EXPECT_FALSE(r.second);  // 插入失败：键已存在
    EXPECT_EQ(m["k"], 1);

    m.insert_or_assign("k", 7);  // 这个才覆盖
    EXPECT_EQ(m["k"], 7);
}

// unordered_map — 遍历与 erase
TEST(UnorderedMap, IterateAndErase) {
    ministl::unordered_map<int, int> m;
    for (int i = 0; i < 10; ++i) m[i] = i * i;
    EXPECT_EQ(m.size(), 10);

    int sum_keys = 0, sum_vals = 0;
    for (auto& kv : m) {
        sum_keys += kv.first;
        sum_vals += kv.second;
    }
    EXPECT_EQ(sum_keys, 45);
    EXPECT_EQ(sum_vals, 285);

    EXPECT_EQ(m.erase(3), 1);
    EXPECT_EQ(m.erase(999), 0);
    EXPECT_EQ(m.size(), 9);
    EXPECT_FALSE(m.contains(3));

    m.clear();
    EXPECT_TRUE(m.empty());
    EXPECT_TRUE(m.begin() == m.end());
}

// unordered_map — rehash 与负载因子
TEST(UnorderedMap, RehashAndLoadFactor) {
    ministl::unordered_map<int, int> m;
    std::size_t c0 = m.bucket_count();
    for (int i = 0; i < 500; ++i) m[i] = i;

    EXPECT_EQ(m.size(), 500);
    // 桶数必须随元素增长，且负载因子压在阈值内，否则退化成 O(n) 查找
    EXPECT_GT(m.bucket_count(), c0);
    EXPECT_LE(m.load_factor(), m.max_load_factor());
    std::printf("  [信息] 500 元素：bucket=%zu load=%.2f\n", m.bucket_count(),
                static_cast<double>(m.load_factor()));

    for (int i = 0; i < 500; ++i) EXPECT_EQ(m[i], i);  // rehash 搬对了
}

// unordered_map — 拷贝/移动/相等
TEST(UnorderedMap, CopyMoveEquality) {
    ministl::unordered_map<int, int> a{{1, 10}, {2, 20}};
    auto b = a;  // 深拷贝
    b[1] = 999;
    EXPECT_EQ(a[1], 10);  // 原表不受影响
    EXPECT_TRUE(a != b);

    ministl::unordered_map<int, int> d{{1, 10}, {2, 20}};
    EXPECT_TRUE(d == a);
}

// unordered_map — 自定义哈希与键类型
TEST(UnorderedMap, CustomHashAndKey) {
    struct Point {
        int x, y;
        bool operator==(const Point& o) const { return x == o.x && y == o.y; }
    };
    struct PointHash {
        std::size_t operator()(const Point& p) const {
            // 手工把两个坐标混进一个 size_t
            return static_cast<std::size_t>(p.x) * 1315423911u ^
                   static_cast<std::size_t>(p.y) * 2654435761u;
        }
    };

    Point p12{1, 2}, p34{3, 4}, p99{9, 9};

    ministl::unordered_map<Point, std::string, PointHash> m;
    m[p12] = "origin-ish";
    m[p34] = "other";
    EXPECT_EQ(m.size(), 2);
    EXPECT_EQ(m[p12], "origin-ish");
    EXPECT_EQ(m.at(p34), "other");
    EXPECT_FALSE(m.contains(p99));
}
