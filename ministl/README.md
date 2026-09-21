# ministl —— 一个用来读懂标准库的迷你 STL

## 这是什么

一个从零手写的 C++20 容器 / 智能指针 / ranges 库，约 5000 行，只依赖编译器。
它不是 std 的替代品，是一次**逆向工程**：把标准库那些看起来理所当然的设计，
还原成"如果不这么做会出什么问题"。

它和同仓库的 `minicc` 编译器构成一个飞轮：

```text
ministl 用宿主编译器（GCC/Clang）写 → 跑通测试 → 成为 minicc 的验证用例
                                        ↓
                          minicc 能编译它 → 说明 minicc 支持了这些语言特性
```

测试用 GoogleTest，与仓库里 `tests/unit/` 同一套约定（`FetchContent` 拉 v1.14.0，
`gtest_discover_tests` 注册）。代价是 minicc 暂时吃不下这些测试——它连 GTest 的头文件
都过不了，飞轮的第二段要等它追上来才能闭合。这个取舍是明确选的：
**先用上 IDE 的单测体验**，而不是为一个远期目标牺牲每天的开发效率。

## 目录

| 文件 | 内容 |
|---|---|
| `type_traits.h` | 转发 `std::` 的 traits + 自研 `is_memmovable` |
| `utility.h` | move / forward / move_if_noexcept / pair / index_sequence |
| `iterator.h` | 五个迭代器标签、`iterator_adaptor`(CRTP)、reverse/back/insert 迭代器 |
| `memory.h` | allocator、`uninitialized_*`、unique_ptr、shared_ptr、weak_ptr、make_shared |
| `vector.h` | 三指针 + EBO 的动态数组 |
| `list.h` | 哨兵节点的双向链表 |
| `unordered_map.h` | 链地址法哈希表 |
| `hash.h` | MurmurHash3 混合器 + FNV-1a + 各类型特化 |
| `algorithm.h` | copy/find/sort/lower_bound/is_sorted 等 |
| `ranges.h` | 惰性视图与管道组合 |
| `function.h` | 类型擦除（手写 vtable + SBO） |

## 几个关键设计决策

### EBO：为什么 `sizeof(unique_ptr) == 8` 而不是 16

删除器如果是空类（`default_delete`），把它作为**成员**会让对象多占 1 字节，
再经对齐填充直接翻倍成 16。标准做法是让 unique_ptr **私有继承**空删除器——
空基类优化（EBO）把它压成 0 字节。`vector` 里的 allocator 同理，
这就是 `sizeof(vector<int>) == 24`（恰好三个指针）而不是 32 的原因。

测试里有 `CHECK_EQ(sizeof(up), sizeof(void*))` 直接钉死这个结论。

### 三指针布局与倍增扩容

`begin_ / end_ / cap_` 三个指针，而不是"指针 + 大小"。
扩容时容量翻倍，把 n 次 push_back 的总代价摊还成 O(1)。
扩容搬元素时用 `uninitialized_move_if_noexcept`：移动构造是 `noexcept` 就移动，
否则退回拷贝（因为移动到一半抛异常，源对象已经残了，无法回滚）。

测试用一个 `Tracker` 同时数 ctor/move/copy，断言扩容期间 `copy == 0`。

### shared_ptr 的控制块双计数协议

控制块里有 `strong_` 和 `weak_` 两个计数，且 `weak_` 初始为 **1**（不是 0）——
这个 1 代表"强引用组自己还占着一个弱引用名额"。释放顺序必须是：

```text
strong 归零 → dispose() 销毁对象 → 释放 strong 自己占的那个 weak
weak   归零 → 才真正 delete 控制块
```

`weak_ptr::lock()` 不能"先查 expired 再 ++"，那是竞态；
必须 CAS 循环把 strong 从非零值加上去。这是 `weak_ptr` 唯一有难度的地方。

### take_view 的配额券：一个真实的 bug

第一版 `take_view::end()` 是"从 begin 走 n 步"。它有两个问题：
`end()` 变成 O(n)，而且**会重复触发上游谓词**——上游是 `filter_view` 时，
算一次 end() 就要把前 n 个元素重新过滤一遍。实测 filter 被调了 14 次，
而容器里只有 10 个元素。谓词只要有副作用（计数、日志），结果就直接错了。

修法是给迭代器配一张"还能产出几个"的配额券（对应标准库的
`counted_iterator` + `default_sentinel`）。修完 filter 只被调 5 次，
第 6 个元素之后全程未被访问——这才是 take 该有的短路。

细节在于 `operator++`：`--remaining_` 和 `++it_` 必须**分两个 if**。
因为 range-for 收尾是"先 ++ 再比较"，取满 n 个之后还会多调一次 ++，
若那次仍推进上游，就白拉一个元素，把短路收益又抵消掉。

### 视图的持有策略：左值引用，右值搬走

```cpp
template <typename R>
using view_storage_t =
    conditional_t<is_lvalue_reference_v<R>, R, remove_reference_t<R>>;
```

`v | transform(f)` 里的 `v` 是左值，视图只存引用，不拷贝；
`vector<int>{...} | transform(f)` 是右值，视图必须把它**搬进自己内部**，
否则临时对象析构后视图悬空。这一行 `conditional_t` 就是全部奥妙。

### `operator|` 为什么必须放在 `views` 命名空间里

`operator|` 靠 ADL 查找。ADL 只检查实参类型**最内层**的外围命名空间，
**不会向上递归**。闭包类型都定义在 `ministl::ranges::views`，
所以 `operator|` 放在外层 `ministl::ranges` 里是找不到的——这是第一版编译失败的原因。

## 构建与测试

```bash
# 配置 + 构建 + 跑全部（googletest 由 FetchContent 自动拉取）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`gtest_discover_tests` 把每个 `TEST()` 注册成独立的 ctest 用例，所以可以按套件筛：

```bash
ctest --test-dir build -R Ranges --output-on-failure   # 只跑 Ranges.*
./build/test_ranges --gtest_filter='Ranges.TakeShortCircuits'   # 单跑一个
```

在 CLion 里：Reload CMake 后每个 `TEST()` 左侧出现绿色三角，点击即单跑该用例；
底部 Test 面板树状查看全部 55 个用例的通过/失败。

验证内存正确性（建议每次改动后都跑）：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j && ctest --test-dir build-asan
```

当前状态：5 个测试文件、55 个用例、233 处断言，GCC 10 / Clang 18 双编译器通过，
ASan + UBSan 无告警。

## 尚未实现

- `string`（SSO）、`optional`、`variant`、`span`、`string_view`
- 分配器传播（`allocator_traits::propagate_on_container_move_assignment`）
- `vector` 的 `insert` 未做"就地扩容 + 位移"的优化，当前走的是重建路径
- ranges 只做了单向视图，没有 `join` / `split` / `zip` / 自定义 range 适配
- `unordered_map` 未实现 `node_handle`（extract/merge）
