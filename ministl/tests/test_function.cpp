// =============================================================================
// tests/test_function.cpp —— 类型擦除三件套（手写 vtable + 模板派生 + SBO）
// =============================================================================
// CLion 用法：Reload CMake 后每个 TEST() 左侧出现绿色三角，点击即单跑该用例。
// =============================================================================

#include <gtest/gtest.h>

#include "ministl/function.h"
#include "ministl/vector.h"

#include <cstdio>
#include <string>

namespace {

int free_func(int x) { return x + 1; }

struct Functor {
    int base;
    int operator()(int x) const { return base + x; }
};

// 刻意做大：超过 SBO 缓冲区后必须走堆分配
struct BigFunctor {
    char padding[128];
    int operator()(int x) const { return x * 3; }
};

struct Counter {
    static int alive;
    int v;

    explicit Counter(int x = 0) : v(x) { ++alive; }
    Counter(const Counter& o) : v(o.v) { ++alive; }
    ~Counter() { --alive; }
    int operator()(int x) const { return v + x; }
};

int Counter::alive = 0;

}  // namespace

// function — 尺寸（SBO 缓冲区在对象内部）
TEST(Function, SizeFitsSbo) {
    // 若不做 SBO，每个 function 都要为小 lambda 单独堆分配一次。
    std::printf("  [信息] sizeof(function<int(int)>) = %zu\n",
                sizeof(ministl::function<int(int)>));
    EXPECT_GE(sizeof(ministl::function<int(int)>), 2 * sizeof(void*));
}

// function — 包装函数指针
TEST(Function, WrapsFunctionPointer) {
    ministl::function<int(int)> f = free_func;
    EXPECT_FALSE(f.empty());
    EXPECT_TRUE(static_cast<bool>(f));
    EXPECT_EQ(f(41), 42);
    EXPECT_EQ(f(0), 1);
}

// function — 包装有捕获的 lambda
TEST(Function, WrapsLambda) {
    int offset = 100;
    ministl::function<int(int)> f = [offset](int x) { return x + offset; };
    EXPECT_EQ(f(1), 101);

    // 包装能修改自身状态的 lambda —— operator() 是 const 的，
    // 所以 lambda 必须声明 mutable，与 std::function 行为一致
    ministl::function<int()> g = [n = 0]() mutable { return ++n; };
    EXPECT_EQ(g(), 1);
    EXPECT_EQ(g(), 2);
    EXPECT_EQ(g(), 3);
}

// function — 包装仿函数
TEST(Function, WrapsFunctor) {
    ministl::function<int(int)> f = Functor{10};
    EXPECT_EQ(f(5), 15);
}

// function — 空 function 调用抛 bad_function_call
TEST(Function, EmptyThrowsBadFunctionCall) {
    ministl::function<int(int)> f;
    EXPECT_TRUE(f.empty());
    EXPECT_FALSE(static_cast<bool>(f));

    EXPECT_THROW(f(1), ministl::bad_function_call);

    // 赋值后恢复正常
    f = free_func;
    EXPECT_FALSE(f.empty());
    EXPECT_EQ(f(1), 2);

    f = nullptr;  // 显式清空
    EXPECT_TRUE(f.empty());
}

// function — 大小对象：小对象走 SBO，大对象走堆
TEST(Function, SmallAndLargeObjects) {
    ministl::function<int(int)> small = Functor{1};
    ministl::function<int(int)> big   = BigFunctor{};
    EXPECT_EQ(small(1), 2);
    EXPECT_EQ(big(2), 6);
    // 两者行为一致，差别只在线性存储位置 —— 由 vtable 抹平
}

// function — 拷贝与移动
TEST(Function, CopyAndMove) {
    Counter::alive = 0;
    {
        ministl::function<int(int)> a = Counter{7};
        EXPECT_EQ(Counter::alive, 1);  // 一份状态
        EXPECT_EQ(a(1), 8);

        auto b = a;  // 深拷贝被包装对象
        EXPECT_EQ(Counter::alive, 2);
        EXPECT_EQ(b(1), 8);

        auto c = ministl::move(a);
        EXPECT_EQ(c(1), 8);      // 移动后仍可调用
        EXPECT_TRUE(a.empty());  // 源被置空
    }
    EXPECT_EQ(Counter::alive, 0);  // 全部正确析构，无泄漏
}

// function — target<T>() 取回原对象
TEST(Function, TargetRecoversObject) {
    ministl::function<int(int)> f = Functor{3};
    Functor* p = f.target<Functor>();
    EXPECT_NE(p, nullptr);
    EXPECT_EQ(p->base, 3);

    // 类型不匹配时返回空指针，而不是错误转换
    EXPECT_EQ(f.target<BigFunctor>(), nullptr);
    EXPECT_EQ(f.target<Counter>(), nullptr);
}

// function — 塞进容器，做回调表
TEST(Function, CallbackTableInContainer) {
    // 这是 function 最典型的用途：把异构的可调用体装进同构容器
    ministl::vector<ministl::function<int(int)>> ops;
    ops.push_back(free_func);                    // 函数指针
    ops.push_back([](int x) { return x * 2; });  // lambda
    ops.push_back(Functor{100});                 // 仿函数

    EXPECT_EQ(ops.size(), 3);
    EXPECT_EQ(ops[0](1), 2);    // +1
    EXPECT_EQ(ops[1](3), 6);    // *2
    EXPECT_EQ(ops[2](1), 101);  // +100

    int sum = 0;
    for (auto& op : ops) sum += op(10);
    EXPECT_EQ(sum, 11 + 20 + 110);
}

// function — 返回 void 与多参数
TEST(Function, VoidReturnAndMultiArgs) {
    ministl::function<void(int, int)> f = [](int a, int b) {
        EXPECT_EQ(a, 1);
        EXPECT_EQ(b, 2);
    };
    f(1, 2);

    ministl::function<std::string(const std::string&)> greet =
        [](const std::string& n) { return "hi, " + n; };
    EXPECT_EQ(greet("cpp"), "hi, cpp");
}
