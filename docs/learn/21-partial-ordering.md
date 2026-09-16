# 21 偏特化偏序裁决：当多条偏特化同时匹配

> 主线 H 下半场。learn/19 只做了"三路择优"的前两路（全特化 / 偏特化），
> 并在⑥里明确记下"多个偏特化同时匹配时取先注册者，不做偏序"。
> 本文把那个洞补上：实现 [temp.class.order] 的部分排序、唯一最特化者判定，
> 以及"互不更特化 ⇒ 报错"这一支。

---

## ① 理论背景

### 问题陈述

类模板偏特化可以有任意多条。当实参同时满足多条时：

```cpp
template <typename T> struct Box;          // 主模板
template <typename T> struct Box<T*>;      // A
template <typename T> struct Box<T**>;     // B

Box<int**> b;   // A 和 B 都匹配 —— 用哪个？
```

标准答案不是"先声明的"、也不是"后声明的"，而是
**选最特化的那一个**。若没有一个比其余都更特化 ⇒ **程序 ill-formed，必须报错**。

### 部分排序（partial ordering）

[temp.class.order] 把问题归约成一次**推导**：

> A 至少与 B 同样特化（记作 A ≥ B） ⟺
> 把 A 的模板形参替换成**唯一合成类型**，得到合成实参表，
> 用它去**推导 B 的模式** —— 能推导成功则 A ≥ B。

判定流程：

```
A ≥ B 且 ¬(B ≥ A)   ⇒  A 更特化，选 A
B ≥ A 且 ¬(A ≥ B)   ⇒  B 更特化，选 B
A ≥ B 且 B ≥ A      ⇒  二者等价（一般不会出现在偏特化里）
¬(A ≥ B) 且 ¬(B ≥ A) ⇒  不可比 ⇒ 歧义 ⇒ 报错
```

### 唯一合成类型（Unique Synthesized Type）为什么必须"唯一"

合成类型是**凭空造出来的、不与任何真实类型相同**的类型。
`clang` 里叫 `UniqueSynthesizedType`，本项目用一个不可能撞车的名字前缀：

```cpp
const std::string prefix = "$ord_" + a->templateName() + "_";   // 如 $ord_Box_T
```

**为什么不能拿真实类型凑合**：看 test_tmpl_42 的歧义对：

```cpp
template <typename T, typename U> struct P<T*, U> { };   // #1 第一个位置是指针
template <typename T, typename U> struct P<T, U*> { };   // #2 第二个位置是指针
```

判 "#1 是否 ≥ #2" 时，把 #1 的形参换成合成类型 `X`、`Y`，得实参 `<X*, Y>`，
去推 #2 的模式 `<T, U*>`：

- `T := X*`
- 要求 `Y` 是指针 —— 而 `Y` 是**凭空造的类型**，什么结构都不带 ⇒ **不是指针** ⇒ 失败

结论 `#1 不 ≥ #2`。对称地 `#2 不 ≥ #1`。⇒ 歧义。

> 若这里用的是真实类型（比如拿 `int` 当 `Y`），结论会依赖"碰巧选了什么类型"，
> 完全失真。这正是"合成类型必须唯一且无结构"的原因。

### 为什么不能"取第一个匹配者"

一个"取第一个/最后一个匹配者"的实现，在单条偏特化命中时看不出问题，
一旦多条同时命中就会**静默选错**。判别性实验（两条测试对照着看）：

| | test_tmpl_40 | test_tmpl_41 |
|---|---|---|
| 声明顺序 | `T*` 先，`T**` 后 | `T**` 先，`T*` 后 |
| 实参 `Box<int*>` | 应得 1 | 应得 1 |
| 实参 `Box<int**>` | 应得 2 | 应得 2 |

- "取第一个" 实现在 41 的 `Box<int*>` 上会错（因为 `T**` 排在前，但它不匹配 —— 这条其实侥幸对）…
  真正露馅的是**较一般的规则排在后面**时，"取第一个"会选中更特化的那条却给不出理由；
- 只有当"更特化者排在后面"（test_tmpl_40 的 `Box<int**>`）时，
  "取最后一个" 才碰巧对，而 test_tmpl_41 会翻车。

**两条一起跑，才能把"实现碰巧对了"和"实现真的对了"区分开。**

---

## ② 设计决策

### 决策 1：复用 `TemplateDeducer`，不新写合一引擎

类模板偏序与函数模板偏序（[temp.func.order]）是**同一类问题**：
都是"拿一方的模式去推另一方的模式"。本项目 S6 已经为函数模板重载实现过
`isAtLeastAsSpecialized`，类模板这一层只需换一下输入。

新增 `SemanticAnalyzer::classSpecAtLeastAsSpecialized(a, b)`：

```
① 取 a 的 specPattern，用 renameTemplateParams 把形参改名为 $ord_<模板名>_<形参名>
   —— 改名是为了避开"双方形参同名 T 被误当成同一个变量"的坑
② 造 TemplateDeducer，挂上 decltype 求值器（模式里可能有 void_t<decltype(...)>）
③ 用改好名的合成实参去 matchPattern(b 的 specPattern)
④ 成功 ⇒ a ≥ b
```

### 决策 2：dominance 循环而不是"两两取冠军"

同时匹配 N 条时，正确判据是：

```
赢家 i  ⟺  ∀ j ≠ i:  ¬( j 比 i 更特化 )
```

不能写成"两两比一遍取第一个胜者"—— 那只在 N=2 时正确，且会掩盖传递性。
本项目 `selectClassTemplate` 先把**所有**匹配的偏特化收集进 `matched` 向量，
再跑这个 dominance 循环；若找不出赢家 ⇒ `error(...)` 报歧义。

三路链 `T* ⊂ T** ⊂ T***` 顺带验证了**传递性**：
实现里 `T*` 与 `T***` 可能从未被直接比较过，
靠 dominance 循环依然能得出正确答案（见 test_tmpl_43）。

### 决策 3：歧义必须【报错】，不静默挑一个

```cpp
error(std::format(
    "ambiguous partial specializations of '{}' for <{}>: "
    "{} are equally specialized, none is more specialized than the others", ...));
```

这条是本主线与 learn/19 那版实现最重要的行为差异：
旧版遇到多候选会静默取先注册者。**静默选错比报错危险得多** ——
用户拿到的是"能跑但结果不对"的程序。

### 决策 4：引用结构必须单独把关

修这一条时踩到的真 bug，值得单独记：

类模板偏特化的匹配是**结构等价**，不是函数调用那套"左值可绑定"（[temp.deduct.call]）。
`matchPattern` 里原先给 `deducePair` 传 `argIsLValue = true`，
于是 `P = T&` 的分支把 `A = int` 也放行了 ——
**`Probe<T&>` 错误地匹配上了 `Probe<int>`**。

诊断现场（`test_tmpl_34` 一稿）：

```
[spec:select]   ├─ ② 候选：'Probe<T&>' 匹配成功     ← 错误！int 不该匹配 T&
```

修法是在 `matchPattern` 里加一段**引用结构前置检查**：
剥掉顶层 const 后，若模式的引用种类非 0（左值/右值引用），
实参的引用种类必须**完全相同**；否则直接判不匹配。

```
Probe<T&> 对 Probe<int>   → 模式要左值引用、实参不是 ⇒ 不匹配 ✓
Probe<T&> 对 Probe<int&>  → 两边都是左值引用       ⇒ 继续推导 ✓
Probe<T&> 对 Probe<int&&> → 左值引用 vs 右值引用   ⇒ 不匹配 ✓
```

> 教训：**"复用现成引擎"和"复用现成语义"是两回事**。
> 引擎（合一算法）可以复用，但**规则集**（这里是 [temp.deduct.call]
> 与 [temp.class.spec.match] 的区别）必须显式切换。
> 原代码里那句"argIsLValue 传 true"的注释其实已经意识到了这一点，
> 但只挡住了半边。

---

## ③ 完整数据流

以 `Box<int**>` 同时匹配 `Box<T*>` 与 `Box<T**>` 为例：

```
  实参请求：Box<int**>
        │
        │ ① 收集所有匹配的偏特化
        ▼
  ┌────────────────────────────────────────────────┐
  │ [spec:select] ★ selecting class template 'Box' │
  │   ├─ ② 候选：'Box<T*>'  匹配成功                │
  │   ├─ ② 候选：'Box<T**>' 匹配成功                │
  │   └─ ★ 2 个偏特化候选同时匹配 → 进入偏序裁决    │
  └────────────────────────────────────────────────┘
        │
        │ ② dominance 循环：对每个候选 i，检查有没有 j 比它更特化
        ▼
  ┌── 比较 (i = T*, j = T**) ─────────────────────┐
  │ j ≥ i？ 合成 j 的形参：T := $ord_Box_T ⇒ <$ord_Box_T**>  │
  │          用它推 i 的模式 'T*'                            │
  │          T* = $ord_Box_T**  ⇒ T := $ord_Box_T*  ✓ 成功   │
  │          ⇒ T** ≥ T*                                      │
  │                                                          │
  │ i ≥ j？ 合成 i 的形参：T := $ord_Box_T ⇒ <$ord_Box_T*>   │
  │          用它推 j 的模式 'T**'                           │
  │          T** = $ord_Box_T*  ⇒ 要求 $ord_Box_T 是指针      │
  │          但它是凭空造的合成类型 ⇒ 失败                    │
  │          ⇒ ¬(T* ≥ T**)                                   │
  │                                                          │
  │ 结论：T** ≥ T* 且 ¬(T* ≥ T**) ⇒ T** 更特化               │
  └──────────────────────────────────────────────────────────┘
        │
        │ ③ 唯一赢家
        ▼
  [order] 'T**' 推 'T*' ⇒ 成功
  [order] 'T*' 推 'T**' ⇒ 失败（parameter pattern 'T*' expects
          pointer argument, got '$ord_Box_T'）
  ⇒ 选中 'Box<T**>'  ⇒  实例化为 Box_intPP  ⇒  tag() 返回 2
```

歧义路径（`P<int*, int*>`）：

```
  实参请求：P<int*, int*>
        │
        ▼
  ├─ ② 候选：'P<T*, U>' 匹配成功    (T := int,  U := int*)
  ├─ ② 候选：'P<T, U*>' 匹配成功    (T := int*, U := int)
  └─ ★ 2 个候选同时匹配 → 偏序裁决
        │
        ├─ #1 ≥ #2？ 合成 <$ord_P_T*, $ord_P_U> 推 'P<T, U*>'
        │            T := $ord_P_T*；要求 U* := $ord_P_U
        │            ⇒ $ord_P_U 必须是指针 ⇒ 失败
        └─ #2 ≥ #1？ 对称地失败
        │
        ▼
  [ERROR] ambiguous partial specializations of 'P' for <int*, int*>:  ...
          are equally specialized, none is more specialized than the others
```

---

## ④ clang 对照

| clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|
| `isAtLeastAsSpecializedAs(Sema&, QualType T1, QualType T2, ...)`<br>lib/Sema/SemaTemplateDeduction.cpp:6226 | `SemanticAnalyzer::classSpecAtLeastAsSpecialized`<br>src/semantic_analyzer.cpp | 只处理类模板偏特化的单层比较，不做函数模板的 `TPOC` 重载上下文排序 |
| `isMoreSpecializedThanPrimary`<br>lib/Sema/SemaTemplate.cpp:4269 | `selectClassTemplate` 里的主模板兜底 | 本项目用"候选集为空则回退主模板"，不做专门的偏序比较 |
| `UniqueSynthesizedType`<br>（clang/AST） | `renameTemplateParams` + `"$ord_"` 前缀<br>src/semantic_analyzer.cpp | 不做完整的合成类型体系，只用名字前缀保证唯一性 |
| 偏序的歧义诊断<br>lib/Sema/SemaTemplate.cpp | `selectClassTemplate` 的 `error(...)` | 文案更详细：列出两个模式的字符串形式与"互不更特化"的结论 |
| `DeduceTemplateArguments` 的 `TDK_Reference` 分支<br>lib/Sema/SemaTemplateDeduction.cpp:589 起 | `matchPattern` 的引用结构前置检查<br>src/template_deduction.cpp | clang 按推导种类（TDK_*）分派；本项目加了一段显式的前置检查 |

---

## ⑤ 实验

```bash
cd /root/cppproject/mycompiler
cmake --build build-linux -j8

# ① T* vs T** 偏序
./build-linux/minicc tests/tmpl/test_tmpl_40_order_pointer_depth.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: [order] 'T**' 推 'T*' ⇒ 成功
#         [order] 'T*' 推 'T**' ⇒ 失败（... got '$ord_Box_T'）
#         ⇒ Box<int**> 选 T** ⇒ tag() = 2

# ② 与声明顺序无关（把更特化的那条挪到前面）
./build-linux/minicc tests/tmpl/test_tmpl_41_order_decl_independent.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: 结论与 ① 逐字相同

# ③ 歧义：互不更特化 ⇒ 必须报错
./build-linux/minicc tests/tmpl/test_tmpl_42_order_ambiguous.cpp -o /tmp/a; echo $?   # 非 0
#   关注: ambiguous partial specializations of 'P' for <int*, int*>: ...
#         are equally specialized, none is more specialized than the others
#   clang 对照：error: ambiguous partial specializations of 'P<int *, int *>'

# ④ 三路链：T* ⊂ T** ⊂ T***，<int***> 让三条同时匹配
./build-linux/minicc tests/tmpl/test_tmpl_43_order_three_way.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: [spec:select] ★ selecting class template 'Box' for <int***>
#         3 个候选同时匹配 → dominance 循环 → 唯一赢家 T***

# ⑤ 语义 oracle：与 clang 对比退出码
for n in 40_order_pointer_depth 41_order_decl_independent 43_order_three_way; do
  f=tests/tmpl/test_tmpl_$n.cpp
  ./build-linux/minicc "$f" -o /tmp/m >/dev/null 2>&1 && /tmp/m; m=$?
  clang++-18 -std=c++20 "$f" -o /tmp/c >/dev/null 2>&1 && /tmp/c; c=$?
  echo "$n  minicc=$m clang=$c"
done

# ⑥ 判别性实验：证伪"取先/后注册者"
#   把 40 与 41 的结论并排看 —— 只有真做偏序才能两条都对
diff <(./build-linux/minicc tests/tmpl/test_tmpl_40_order_pointer_depth.cpp -o /tmp/m40 >/dev/null 2>&1 && /tmp/m40; echo $?) \
     <(./build-linux/minicc tests/tmpl/test_tmpl_41_order_decl_independent.cpp -o /tmp/m41 >/dev/null 2>&1 && /tmp/m41; echo $?)
#   → 无输出即两者退出码相同 ✓

# ⑦ 单元测试（进 ctest）
./build-linux/unit_tests --gtest_filter='PartialOrder.*'
```

### 关键过程 ASCII 图：dominance 循环

```
        同时匹配的候选 { T*, T**, T*** }
                 │
                 ▼
     ┌───────────────────────────┐
     │ for each i in 候选:        │
     │   win ← true               │
     │   for each j ≠ i:          │
     │     if j 比 i 更特化:       │
     │        win ← false; break  │
     │   if win: 返回 i           │
     └───────────────────────────┘
                 │
        ┌────────┴────────┐
        │                 │
    找到唯一赢家       一个都没找到
        │                 │
    实例化该偏特化     error: ambiguous
```

---

## ⑥ 已知边界

| 边界 | 现状 | 归属 |
|---|---|---|
| NTTP 特化模式的偏序 | `Box<int, N>` 形式的偏特化不支持，故其偏序也无从谈起 | Parser 层缺口，见 learn/19 |
| 偏特化与全特化混合择优 | 全特化优先级更高，直接命中，不进偏序循环 | 已按标准处理 |
| 函数模板的 SFINAE 重载择优 | 属 [temp.func.order]，S6 已实现基础版；带约束的版本见 learn/20 ⑥ | 后续 |
| 偏特化成员的类外定义 | 不支持 | 后续 |
| 偏序诊断的候选列表 | 报错文案列出参与裁决的模式，但不逐对解释"为什么不可比" | 可选增强 |

---

## ⑦ 与用户原代码的距离

用户原代码里的 `Box` 部分（原文注释问的是"能否解析出来"）：

```cpp
template <typename T, typename U = void>
struct Box { public: void hello() { ... } };

template <typename T>
struct Box<T*, T> { public: void hello() { ... } };

template <>
struct Box<int*, int> { };

Box<decltype(&a)> b;     // decltype(&a) = int*  ⇒  Box<int*, void>  ⇒ 主模板
b.hello();
```

**已通**（去掉 `std::cout` 后）：

| 特性 | 状态 |
|---|---|
| `struct` 写模板体 | ✅ learn/19 |
| 默认模板实参 `U = void` | ✅ learn/19 |
| 偏特化 `Box<T*, T>` | ✅ learn/19 |
| 全特化 `template<> struct Box<int*, int>` | ✅ learn/19 |
| 多偏特化同时匹配时的**择优** | ✅ **本文**（此前只报"取先注册者"） |
| `decltype(&a)` 作模板实参 | ✅ learn/20 |
| `Box<decltype(&a)>` 的完整链 | ✅ `tests/tmpl/test_tmpl_39_is_range_demo.cpp` |

**一个值得记下的细节**：`Box<int*, int*>` **不会**命中 `Box<T*, T>` ——
因为 `T*` 对 `int*` 推出的是 `T = int`（被指类型），
第二个位置便要求 `int* == int`，矛盾。
写 `test_tmpl_39` 时最初就写成了 `int*`，被 clang 的实测结果纠正。
这类"模板推导不是字面相等"的坑，正是本项目要讲清楚的东西。

**仍不通**：`operator|` / `operator||` 那一半（见 learn/20 ⑦ 与 ROADMAP 主线 H）。
