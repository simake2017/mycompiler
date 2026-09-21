// =============================================================================
// tests/test_ranges.cpp —— 惰性视图与管道组合
// =============================================================================
// 全部用例围绕一个核心命题：**视图的构造不产生任何计算**，
// 只有真正遍历（pull）时才逐元素驱动上游。这就是 push/pull 的对偶。
//
// CLion 用法：Reload CMake 后每个 TEST() 左侧出现绿色三角，点击即单跑该用例。
// =============================================================================

#include <gtest/gtest.h>

#include "ministl/ranges.h"
#include "ministl/vector.h"

#include <cstdio>

namespace rv  = ministl::ranges::views;
namespace rng = ministl::ranges;

// 惰性求值 — 构造视图不触发任何计算
TEST(Ranges, ViewConstructionIsLazy) {
    ministl::vector<int> v{1, 2, 3, 4, 5};
    int calls = 0;

    auto view = v | rv::transform([&calls](int x) {
                    ++calls;
                    return x * 2;
                });
    // 到这里为止：视图已构造完毕，但 transform 一次都没被调用
    EXPECT_EQ(calls, 0);
    std::printf("  [信息] 构造视图后 calls = %d\n", calls);

    // 真正驱动它 —— 此时才逐元素计算
    auto out = rng::to_vector(view);
    EXPECT_EQ(calls, 5);
    std::printf("  [信息] 遍历后   calls = %d\n", calls);
    EXPECT_EQ(out.size(), 5);
    EXPECT_EQ(out[0], 2);
    EXPECT_EQ(out[4], 10);
}

// filter — 只保留谓词为真的元素
TEST(Ranges, FilterKeepsMatching) {
    ministl::vector<int> v{1, 2, 3, 4, 5, 6};
    auto out = rng::to_vector(v | rv::filter([](int x) { return x % 2 == 0; }));
    EXPECT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], 2);
    EXPECT_EQ(out[2], 6);
}

// 管道组合 — filter → transform → take
TEST(Ranges, PipelineComposition) {
    ministl::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8};
    auto out = rng::to_vector(v | rv::filter([](int x) { return x % 2 == 0; }) |
                              rv::transform([](int x) { return x * 10; }) |
                              rv::take(3));
    EXPECT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], 20);
    EXPECT_EQ(out[1], 40);
    EXPECT_EQ(out[2], 60);
}

// take 短路 — 下游够了就不再拉上游
TEST(Ranges, TakeShortCircuits) {
    // pull 模型相对 push 模型的关键收益：
    // take(2) 只需要上游产出 2 个元素，第 3 个永远不会被计算。
    ministl::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int calls = 0;
    auto out = rng::to_vector(v | rv::transform([&calls](int x) {
                                  ++calls;
                                  return x;
                              }) |
                              rv::take(2));
    EXPECT_EQ(out.size(), 2);
    EXPECT_EQ(calls, 2);  // 不是 10！
    std::printf("  [信息] take(2) 只驱动了 %d 次 transform（容器有 10 个元素）\n",
                calls);
}

// drop — 跳过前 n 个
TEST(Ranges, DropSkipsPrefix) {
    ministl::vector<int> v{1, 2, 3, 4, 5};
    auto out = rng::to_vector(v | rv::drop(2));
    EXPECT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], 3);
    EXPECT_EQ(out[2], 5);

    EXPECT_EQ(rng::to_vector(v | rv::drop(99)).size(), 0);  // 超长 → 空
}

// reverse — 反转视图
TEST(Ranges, ReverseView) {
    ministl::vector<int> v{1, 2, 3, 4};
    auto out = rng::to_vector(v | rv::reverse());
    EXPECT_EQ(out.size(), 4);
    EXPECT_EQ(out[0], 4);
    EXPECT_EQ(out[3], 1);

    auto out2 = rng::to_vector(v | rv::reverse() |
                               rv::transform([](int x) { return x + 1; }));
    EXPECT_EQ(out2[0], 5);
}

// iota — 不依赖任何底层容器的序列生成
TEST(Ranges, IotaGeneratesSequence) {
    auto out = rng::to_vector(rng::iota(1, 6));  // [1, 6)
    EXPECT_EQ(out.size(), 5);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[4], 5);

    auto out2 = rng::to_vector(rng::iota(0, 100) |
                               rv::filter([](int x) { return x % 7 == 0; }) |
                               rv::take(3));
    EXPECT_EQ(out2.size(), 3);
    EXPECT_EQ(out2[0], 0);
    EXPECT_EQ(out2[1], 7);
    EXPECT_EQ(out2[2], 14);
}

// sum — 终端操作触发计算
TEST(Ranges, SumIsTerminalOp) {
    ministl::vector<int> v{1, 2, 3, 4, 5};
    EXPECT_EQ(rng::sum(v), 15);
    EXPECT_EQ(rng::sum(v | rv::transform([](int x) { return x * x; })), 55);
}

// 生命周期 — 左值按引用持有，右值按值持有
TEST(Ranges, LvalueByRefRvalueByValue) {
    // 左值：视图只存引用，不拷贝容器
    ministl::vector<int> v{1, 2, 3};
    auto view = v | rv::transform([](int x) { return x + 1; });
    v[0] = 100;  // 改动源容器
    auto out = rng::to_vector(view);
    EXPECT_EQ(out[0], 101);  // 视图看到最新值 → 引用语义

    // 右值：视图必须把容器**搬进自己内部**，否则临时对象析构后悬空
    auto make_view = [] {
        return ministl::vector<int>{1, 2, 3} |
               rv::transform([](int x) { return x * 2; });
    };
    auto owned = make_view();  // 临时容器已移交视图所有
    auto out2  = rng::to_vector(owned);
    EXPECT_EQ(out2.size(), 3);  // 没有悬空，数据完好
    EXPECT_EQ(out2[2], 6);
}

// 整条管道不物化中间结果
TEST(Ranges, PipelineDoesNotMaterialize) {
    ministl::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int filter_calls = 0, transform_calls = 0, tail_calls = 0;

    auto pipeline = v | rv::filter([&](int x) {
                        ++filter_calls;
                        return x % 2 == 1;
                    }) |
                    rv::transform([&](int x) {
                        ++transform_calls;
                        return x * x;
                    }) |
                    rv::take(3) |
                    rv::transform([&](int x) {
                        ++tail_calls;
                        return x;
                    });

    EXPECT_EQ(filter_calls + transform_calls + tail_calls, 0);  // 尚未驱动

    auto out = rng::to_vector(pipeline);
    EXPECT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], 1);   // 1*1
    EXPECT_EQ(out[1], 9);   // 3*3
    EXPECT_EQ(out[2], 25);  // 5*5

    EXPECT_EQ(tail_calls, 3);
    std::printf("  [信息] filter=%d transform=%d tail=%d\n", filter_calls,
                transform_calls, tail_calls);
    // 只看到 1,2,3,4,5 就凑齐了 3 个奇数平方；6 以后的元素全程未被访问。
    // 若这里变成 10，说明 take 没有短路（第一版 end() 前进 n 步的写法
    // 甚至会让它涨到 14 —— 上游谓词被重复触发了）。
    EXPECT_EQ(filter_calls, 5);
    EXPECT_EQ(transform_calls, 3);
}
