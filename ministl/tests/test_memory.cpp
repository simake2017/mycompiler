// =============================================================================
// tests/test_memory.cpp —— unique_ptr / shared_ptr / weak_ptr / allocator
// =============================================================================
// CLion 用法：Reload CMake 后每个 TEST() 左侧出现绿色三角，点击即单跑该用例。
// 命令行：./test_memory --gtest_filter='WeakPtr.*'
// =============================================================================

#include <gtest/gtest.h>

#include "ministl/memory.h"

#include <cstdio>
#include <stdexcept>

namespace {

int g_alive = 0;

struct Tracked {
    int v;
    explicit Tracked(int x) : v(x) { ++g_alive; }
    ~Tracked() { --g_alive; }
};

// 可多态的类型，用于验证虚析构 + 指针转换
struct Base {
    virtual ~Base() = default;
    virtual int id() const { return 0; }
};

struct Derived : Base {
    int id() const override { return 1; }
};

// 自定义删除器必须是**无捕获**的函数（要能退化成函数指针），
// 所以计数放在文件作用域而不是 lambda 捕获里。
int g_freed = 0;
void counting_deleter(Tracked* p) {
    ++g_freed;
    delete p;
}

// 前 copies_left 次拷贝成功，之后抛异常 —— 用来验证
// uninitialized_copy 在构造中途失败时能否回滚已构造的部分。
struct ThrowOnCopy {
    static int alive;
    static int copies_left;
    int v;

    explicit ThrowOnCopy(int x) : v(x) { ++alive; }
    ThrowOnCopy(const ThrowOnCopy& o) : v(o.v) {
        if (--copies_left < 0) throw std::runtime_error("拷贝构造失败");
        ++alive;
    }
    ~ThrowOnCopy() { --alive; }
};

int ThrowOnCopy::alive = 0;
int ThrowOnCopy::copies_left = 0;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  unique_ptr
// ─────────────────────────────────────────────────────────────────────────────

// unique_ptr — 独占所有权 + EBO
TEST(UniquePtr, OwnershipAndEbo) {
    g_alive = 0;
    {
        auto up = ministl::make_unique<Tracked>(42);
        EXPECT_EQ(g_alive, 1);
        EXPECT_EQ(up->v, 42);
        EXPECT_TRUE(static_cast<bool>(up));

        // EBO 验证：空删除器被**继承**而非作为成员，故尺寸等于裸指针。
        // 若把 default_delete 写成普通成员，这里会变成 16 字节。
        EXPECT_EQ(sizeof(up), sizeof(void*));
        std::printf("  [信息] sizeof(unique_ptr<Tracked>) = %zu（裸指针 %zu）\n",
                    sizeof(up), sizeof(void*));
    }
    EXPECT_EQ(g_alive, 0);  // 离开作用域自动析构
}

// unique_ptr — 只能移动，不能拷贝
TEST(UniquePtr, MoveOnly) {
    g_alive = 0;
    {
        auto a = ministl::make_unique<Tracked>(1);
        auto b = ministl::move(a);
        EXPECT_EQ(a, nullptr);  // 源被掏空
        EXPECT_EQ(b->v, 1);
        EXPECT_EQ(g_alive, 1);  // 始终只有一个对象，没有多余构造

        static_assert(!std::is_copy_constructible_v<decltype(a)>,
                      "unique_ptr 必须不可拷贝");
        static_assert(std::is_move_constructible_v<decltype(a)>,
                      "unique_ptr 必须可移动");
    }
    EXPECT_EQ(g_alive, 0);
}

// unique_ptr — 数组特化用 delete[]
TEST(UniquePtr, ArraySpecialization) {
    auto arr = ministl::make_unique<int[]>(5);
    for (int i = 0; i < 5; ++i) arr[i] = i * i;
    EXPECT_EQ(arr[4], 16);
}

// unique_ptr — 多态与自定义删除器
TEST(UniquePtr, PolymorphismAndCustomDeleter) {
    ministl::unique_ptr<Base> b = ministl::make_unique<Derived>();
    EXPECT_EQ(b->id(), 1);  // 虚函数正确分派，虚析构也正确

    g_freed = 0;
    {
        ministl::unique_ptr<Tracked, void (*)(Tracked*)> up(new Tracked(9),
                                                            counting_deleter);
        EXPECT_EQ(up->v, 9);
    }
    EXPECT_EQ(g_freed, 1);  // 自定义删除器确实被调用了
}

// ─────────────────────────────────────────────────────────────────────────────
//  shared_ptr / weak_ptr
// ─────────────────────────────────────────────────────────────────────────────

// shared_ptr — 引用计数
TEST(SharedPtr, ReferenceCount) {
    g_alive = 0;
    {
        auto sp = ministl::make_shared<Tracked>(7);
        EXPECT_EQ(sp.use_count(), 1);
        EXPECT_TRUE(sp.unique());
        {
            auto sp2 = sp;
            EXPECT_EQ(sp.use_count(), 2);
            EXPECT_FALSE(sp.unique());
        }
        EXPECT_EQ(sp.use_count(), 1);
        EXPECT_EQ(g_alive, 1);
    }
    EXPECT_EQ(g_alive, 0);  // 最后一个 shared_ptr 析构时对象才销毁
}

// shared_ptr — 别名构造
TEST(SharedPtr, AliasingConstructor) {
    struct Holder {
        int a = 1;
        int b = 2;
    };
    auto h = ministl::make_shared<Holder>();
    ministl::shared_ptr<int> alias(h, &h->b);  // 指向成员 b
    EXPECT_EQ(*alias, 2);
    EXPECT_EQ(alias.use_count(), 2);  // 与 h 共享同一个控制块
    // 两者所指地址不同（类型也不同，比较前先转成 void*）
    EXPECT_NE(static_cast<void*>(h.get()), static_cast<void*>(alias.get()));
}

// shared_ptr — 多态转换
TEST(SharedPtr, PolymorphicCast) {
    ministl::shared_ptr<Base> b = ministl::make_shared<Derived>();
    EXPECT_EQ(b->id(), 1);

    auto d = ministl::dynamic_pointer_cast<Derived>(b);
    EXPECT_NE(d, nullptr);
    EXPECT_EQ(d.use_count(), 2);  // 转换后共享所有权

    ministl::shared_ptr<Base> plain(new Base);
    EXPECT_EQ(ministl::dynamic_pointer_cast<Derived>(plain), nullptr);
}

// weak_ptr — 观察但不持有
TEST(WeakPtr, ObserveWithoutOwning) {
    ministl::weak_ptr<Tracked> wp;
    {
        auto sp = ministl::make_shared<Tracked>(5);
        wp = sp;
        EXPECT_FALSE(wp.expired());
        EXPECT_EQ(wp.use_count(), 1);

        auto locked = wp.lock();  // 原子地「检查存活 + 计数加一」
        EXPECT_NE(locked, nullptr);
        EXPECT_EQ(wp.use_count(), 2);
    }
    // 强引用全部释放 → 对象已销毁；但控制块因 weak 引用仍存活
    EXPECT_TRUE(wp.expired());
    EXPECT_EQ(wp.lock(), nullptr);
    EXPECT_EQ(wp.use_count(), 0);
}

// weak_ptr — 打破循环引用
TEST(WeakPtr, BreakReferenceCycle) {
    struct Node {
        int v;
        ministl::shared_ptr<Node> next;  // 强引用
        ministl::weak_ptr<Node> prev;    // 弱引用 —— 破环的关键
        explicit Node(int x) : v(x) {}
    };
    {
        auto a = ministl::make_shared<Node>(1);
        auto b = ministl::make_shared<Node>(2);
        a->next = b;
        b->prev = a;  // 若这里也用 shared_ptr，两个计数永不归零 → 泄漏
        EXPECT_EQ(a.use_count(), 1);
        EXPECT_EQ(b.use_count(), 2);
    }
    // 双方都能正常释放 —— 无泄漏（ASan 下会直接报出来）
}

// ─────────────────────────────────────────────────────────────────────────────
//  allocator / 未初始化内存算法
// ─────────────────────────────────────────────────────────────────────────────

// allocator — 分配与构造分离
TEST(Allocator, AllocateConstructSeparate) {
    g_alive = 0;
    ministl::allocator<Tracked> alloc;
    // 第一步：只要一块**生内存**，不构造任何对象
    Tracked* raw = alloc.allocate(3);
    EXPECT_EQ(g_alive, 0);

    // 第二步：在生内存上逐个构造
    alloc.construct(raw + 0, 10);
    alloc.construct(raw + 1, 20);
    alloc.construct(raw + 2, 30);
    EXPECT_EQ(g_alive, 3);
    EXPECT_EQ(raw[1].v, 20);

    // 逆序析构，再归还内存 —— 顺序不能反
    for (int i = 2; i >= 0; --i) alloc.destroy(raw + i);
    EXPECT_EQ(g_alive, 0);
    alloc.deallocate(raw, 3);
}

// uninitialized_copy — 构造中途抛异常必须回滚
TEST(Uninitialized, RollbackOnThrow) {
    // 这是 vector 提供**强异常保证**的根基：
    // 拷贝到第 N 个元素时抛异常，前面已构造好的 N-1 个必须被析构，
    // 否则这块生内存上的对象就成了无人认领的泄漏。
    ThrowOnCopy::alive = 0;
    ThrowOnCopy src[3] = {ThrowOnCopy(1), ThrowOnCopy(2), ThrowOnCopy(3)};
    EXPECT_EQ(ThrowOnCopy::alive, 3);

    ThrowOnCopy::copies_left = 2;  // 前 2 次拷贝成功，第 3 次抛

    ministl::allocator<ThrowOnCopy> alloc;
    ThrowOnCopy* buf = alloc.allocate(5);

    EXPECT_THROW(ministl::uninitialized_copy(src, src + 3, buf),
                 std::runtime_error);

    // 拷贝成功的 2 个已被回滚析构，只剩源数组的 3 个
    EXPECT_EQ(ThrowOnCopy::alive, 3);

    alloc.deallocate(buf, 5);
}
