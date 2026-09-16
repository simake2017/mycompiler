# 20 decltype 与 SFINAE：编译期的"类型查询"与"软失败"

> 主线 H 上半场。本文只讲两件事，但这两件事是 C++ 模板元编程的地基：
> `decltype` 回答"这个表达式的类型是什么"，SFINAE 回答"这个候选不成立时该怎么办"。
> 用户原代码里的 `is_range<T>` 类型探测，正是两者的组合。

---

## ① 理论背景

### decltype：两套求值规则

标准 [dcl.type.decltype] 里，`decltype(e)` 的结果按 **e 的书写形式**分两条路：

| e 的形态 | 结果 | 例 |
|---|---|---|
| **未加括号**的 id-expression 或成员访问 | 该**实体**的**声明类型** | `decltype(a)` → `int` |
| 其余一切（含**多加一层括号**） | 该**表达式**的**类型**，左值带 `&` | `decltype((a))` → `int&` |

这是 C++ 最反直觉的规则之一：**多写一对括号会改变结果**。原因在于第一条规则
要找的是"被命名的那个实体"，而 `(a)` 已经不再是一个"名字"，退化成普通表达式，
于是走第二条路 —— 而 `a` 是左值，所以结果带引用。

> 官方依据（C++11 [dcl.type.simple]p4，现 [dcl.type.decltype]）：
> "if e is an unparenthesized id-expression or an unparenthesized class member
> access, decltype(e) is the type of the entity named by e."
> 对应 clang 实现：`Sema::getDecltypeForExpr`（lib/Sema/SemaType.cpp:9996）。

### 延迟求值：decltype 是"半成品类型"

模板模式里的 decltype 不能立刻求值：

```cpp
template <typename T>
struct is_range<T, std::void_t<decltype(std::declval<T>().begin())>> : std::true_type {};
//                                    ^^^^^^^^^^^^^^^^^^^^^^^^^ 此刻 T 未知
```

解析到这里时 `T` 还是个符号，`.begin()` 无从查起。所以 decltype 必须**两段式**：

1. **解析段**：只把操作数表达式原样存下来，不求值
2. **替换段**：`T` 被换成具体类型后，才真正求值

对照 clang：这就是 `DecltypeType` 的"依赖类型"形态 ——
`Sema::SubstType` 时由 `TemplateInstantiator`（TreeTransform.h）求值。
本项目的节点字段见 `include/type.h` 的 `decltypeExpr` / `decltypeParen`。

### SFINAE：替换失败不是错误

[temp.deduct]/8 的原文要旨：

> 如果模板实参的替换导致类型或表达式非法，**且失败发生在函数类型、
> 模板形参类型的"直接上下文（immediate context）"内**，
> 则该推导失败 —— 这**不是**错误，只是这个候选被移出候选集。

三处容易讲错的细节：

1. **失败≠错误**。失败只是"这个候选不成立"，被候选集内部消化；
   只有当候选集**全军覆没**时才升级为编译错误。
2. **限制在直接上下文**。错误若发生在被调用函数的**函数体内**，
   与 SFINAE 无关，那是真错误。
3. **"移出候选"是全有或全无**。`void_t<A, B, C>` 里任意一个失败，
   整个偏特化一起消失 —— 不存在"匹配一半"。

### void_t：探测惯例（CWG 1558）

```cpp
template <typename...> using void_t = void;   // 标准写法
```

语义被明确为："实参全部合法 ⇒ 等价于 `void`；否则**替换失败**"。
它本身什么都不做，作用是把"这些表达式合法吗"变成一个**类型**，
从而能塞进偏特化的模式里去匹配。

> ★ CWG 1558 之前，大家靠"未被使用的模板形参"这种脆弱写法凑效果，
> 编译器之间行为还不一致。现在是标准行为。

### declval：未求值上下文里的"凭空取值"

```cpp
template <class T> typename add_rvalue_reference<T>::type declval() noexcept;  // [declval]/1
```

它**只声明不定义**，且只准出现在 `decltype` / `sizeof` 这类**未求值上下文**里。
作用是"假装手上有一个 `T` 类型的对象"，这样才能在 decltype 里访问 `T` 的成员。

---

## ② 设计决策

### 决策 0：SFINAE 独立成模块（`include/sfinae.h` + `src/sfinae.cpp`）

**为什么单独立模块**：SFINAE 初看只是"捕获异常继续试"，落到代码里却是一套
**三方协议**。协议不集中，就会散成一堆彼此不认识的 `try/catch`，
读代码的人无法回答"SFINAE 到底在哪儿发生"。

三方角色（`include/sfinae.h` 头注有完整版）：

| 角色 | 谁 | 动作 |
|---|---|---|
| **产生方** | `substituteType` / `evaluateDecltype` | `Sfinae::fail(reason)` 发出信号 |
| **传播方** | 各层递归替换、`DecltypeEvaluator` 回调链 | 不捕获，任其冒泡 |
| **吸收方** | `Sfinae::attempt(candidate, fn)` | 捕获 → 候选移出候选集 |

收口点全项目**仅三处**（头注里有一张必须同步维护的表）：

| 场景 | 吸收点 |
|---|---|
| 类模板偏特化模式匹配 | `TemplateDeducer::matchPattern` |
| 函数模板重载决议 | `SemanticAnalyzer::inferCall` 候选循环 |
| 偏序"至少同样特化"比较 | `SemanticAnalyzer::classSpecAtLeastAsSpecialized` |

两个配套设施：

- **`SfinaeContext`（RAII）** —— 把 [temp.deduct]/8 的"直接上下文"从
  隐式约定变成**可观测状态**。信号发出时日志会标注"直接上下文内/外"；
  单测 `SfinaeProtocol.ImmediateContextDepth` 钉住它的进出配对。
- **统一日志出口** —— 全项目只有 `src/sfinae.cpp` 打印 `[sfinae]` 前缀。
  `grep "\[sfinae\]"` 出来的每一行都能在一个文件里找到出处。

**一个刻意的"不统一"**：异常式（偏特化）与返回码式（函数模板重载）
两种失败表达方式都保留，只是共用 `Sfinae::rejected` 这个日志出口。
原因是二者机制不同 —— 函数模板推导本来就是返回值驱动的，
硬套异常只会让调用栈多一次无谓的展开。
强行统一形式而忽略机制差异，反而更难看懂。

**这条判据是整个机制的命门**，故单立 `SfinaeProtocol.*` 套件钉住：

```
Sfinae::attempt 捕获 SubstitutionFailure  → 软失败（候选移除）
Sfinae::attempt 放行其它异常              → 硬错误（穿过去）
```

写反了（比如 catch 了 `std::runtime_error`）的症状是
**"本该报错的程序静默通过"** —— 比直接报错危险得多，
而且前面所有集成测试照样全绿。`SfinaeProtocol.PropagatesRealErrors`
就是专门为这条存在的。

### 决策 1：std 垫片走【编译器内建注入】，不写头文件

真 C++ 的 `false_type` 长这样：

```cpp
template<class T, T v> struct integral_constant {
    static constexpr T value = v;
};
using false_type = integral_constant<bool, false>;
```

它依赖三样本项目**还没有**的能力：

| 缺口 | 说明 |
|---|---|
| 别名模板 `using X = Y;` | Parser 无解析（实测报错） |
| 类内静态数据成员 `static const T value = v;` | Parser 无解析 |
| NTTP 形参类型依赖前置形参 `template<class T, T v>` | 未实现 |

于是改由 `SemanticAnalyzer::registerBuiltins()` 直接注入等价的类声明
（`injectTraitClass` lambda），绕开 Parser。

**这是有意的简化，不是遗漏**。代价：这些名字用户不能重新定义。
对照 clang：clang 也有内建（`__builtin_*`、`Sema::Initialize` 里预置的声明），
只是范围比我们大得多。

`std::void_t` 与 `std::declval` 更进一步，走**按名识别**：

| 名字 | 处理点 | 语义 |
|---|---|---|
| `std::void_t<...>` | `resolveType` / `TemplateDeducer::reducePattern` | 若每个实参替换+求值都成功 ⇒ 归约为 `void` |
| `std::declval<T>()` | `inferCall` 开头的内建分支 | 直接给出 `T&&` |

### 决策 2：decltype 走"两段式"，节点常驻 AST

不把 decltype 在解析期就地展开，而是保留 `TypeKind::Decltype` 节点，
在 `substituteType`（替换段）才求值。这样同一套节点既能表示
非依赖上下文（可立即求值），也能表示依赖上下文（必须延迟）。

`SemanticAnalyzer` 实现 `DecltypeEvaluator` 接口，
由 `TemplateInstantiator` / `TemplateDeducer` 持有指针按需回调 ——
避免让替换引擎反向依赖整个 Sema。

### 决策 3：`SubstitutionFailure` 是一个独立异常类型

替换失败与真错误必须能被区分。本项目用异常类型区分：

```cpp
class SubstitutionFailure : public std::runtime_error { ... };
```

`evaluateDecltype` 里把内层抛出的**普通** `std::runtime_error`
（如"未定义标识符"）**转译**成 `SubstitutionFailure`；
而 `SubstitutionFailure` 本身则原样重抛（先 catch 它再 catch 基类）。

这一步是让 `decltype(declval<int>().begin())` 变成**软失败**而不是硬报错的关键。

### 决策 4：测试用 `#ifdef __clang__` 对齐头文件

minicc 的 std 垫片是内建注入的，**不需要头文件**；而 clang 需要真实的
`<type_traits>` / `<utility>`。解法：

```cpp
#ifdef __clang__
#include <type_traits>
#include <utility>
#endif
```

minicc 不定义 `__clang__`（已实测），会跳过整个块。
于是**同一份源文件**在两个编译器下语义一致，可交叉验证。

### 决策 5：用【退出码】验证，不用输出

minicc 目前**没有任何输出能力** —— 自研链接器只注入 `_start` / `malloc` / `free`，
没有 `write` 系统调用。所以测试一律用 `return N` 编码结论，跑可执行文件看退出码。

---

## ③ 完整数据流

以 `is_range<decltype(numbers)>` 为例（`numbers` 是 `Vec` 类型的局部变量）：

```
                   源码
  template <typename T, typename = void>
  struct is_range : std::false_type {};

  template <typename T>
  struct is_range<T, std::void_t<decltype(std::declval<T>().begin())>>
      : std::true_type {};

  is_range<decltype(numbers)>
        │
        │ ① Parser：decltype 只存表达式，不求值
        ▼
  Type{Decltype, decltypeExpr=VarExpr(numbers), decltypeParen=false}
        │
        │ ② Sema 看到 is_range<...>，触发 selectClassTemplate
        ▼
  ┌─────────────────────────────────────────────────────┐
  │ [spec:select] 收集所有匹配的偏特化                    │
  │   偏特化模式 = [T, void_t<decltype(declval<T>().begin())>] │
  │   实参       = [Vec, void]                            │
  └─────────────────────────────────────────────────────┘
        │
        │ ③ matchPattern：逐位合一
        ▼
  位置 1: T        ← Vec          ⇒ T := Vec
  位置 2: void_t<...> ← void     ⇒ 进 reducePattern
        │
        │ ④ reducePattern：这是个 void_t，逐个替换+求值
        ▼
  substituteType(decltype(declval<T>().begin()), {T := Vec})
        │
        ├─ Case 1.5：decltype 节点 → 先替换操作数
        │     declval<T>()  →  declval<Vec>()  →  Vec&&   （内建按名识别）
        │     .begin()                            →  int
        │
        └─ 回调 evaluateDecltype(declval<Vec>().begin(), paren=false)
              │
              ├─ 未加括号 & 是成员访问 → 走【声明类型】规则
              └─ ⇒ int ✓   没有异常 ⇒ 这个探测通过
        │
        │ ⑤ 所有探测都通过 ⇒ void_t<...> 归约为 void
        ▼
  位置 2: void     ← void         ⇒ 恒等 ✓
        │
        │ ⑥ 偏特化比主模板更特化 ⇒ 命中
        ▼
  is_range<Vec, void>  ⇒  继承 std::true_type  ⇒  value = 1
```

对应的**失败**路径（`is_range<decltype(1)>`，即 `is_range<int>`）：

```
  位置 1: T ← int                 ⇒ T := int
  位置 2: void_t<...> ← void     ⇒ 进 reducePattern
        │
        └─ evaluateDecltype(declval<int>().begin())
              │
              └─ inferMember 在 int 上找 begin → 内部抛 std::runtime_error
                 → evaluateDecltype 捕获并【转译为 SubstitutionFailure】
        │
        ▼
  [sfinae] ⤵ 模式第 2 位替换失败，按 SFINAE 判为【不匹配】（非错误）
  [spec:select] └─ ③ falling back to PRIMARY template
        │
        ▼
  is_range<int>  ⇒  主模板  ⇒  继承 std::false_type  ⇒  value = 0
```

---

## ④ clang 对照

| clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|
| `SFINAETrap`（Sema.h）<br>标记"此刻失败可恢复" | `SfinaeContext`（RAII 深度计数）<br>include/sfinae.h | clang 用模板技巧 + 编译期分支，本实现就是一个静态计数器 |
| `TDK_SubstitutionFailure` 返回码<br>lib/Sema/SemaTemplateDeduction.cpp | `SubstitutionFailure` 异常<br>include/sfinae.h | clang 走返回码；本实现借 C++ 异常做栈回退，语义等价、代码更短 |
| `Sema::SubstitutionFailure` 的捕获点 | `Sfinae::attempt`<br>include/sfinae.h | 收口点从散落的 try/catch 收敛为三处显式调用 |
| `Sema::getDecltypeForExpr`<br>lib/Sema/SemaType.cpp:9996 | `SemanticAnalyzer::evaluateDecltype`<br>src/semantic_analyzer.cpp | 不支持 `decltype(auto)`、pack indexing、lambda 捕获相关的 decltype 规则；只留两条主线规则 |
| `Sema::BuildDecltypeType`<br>lib/Sema/SemaType.cpp:10069 | `Type::makeDecltype`<br>src/type.cpp | 不做"未求值上下文有副作用"的诊断（clang 会 warn） |
| `DecltypeType` 依赖类型<br>clang/AST/Type.h | `TypeKind::Decltype` + `decltypeExpr`<br>include/type.h | 只区分"加括号/不加括号"一位布尔，不做完整的依赖类型推导 |
| `TemplateInstantiator` 求值 decltype<br>lib/Sema/TreeTransform.h | `substituteType` Case 1.5<br>src/template_instantiation.cpp | 无 |
| `SFINAEFailure` 的传播范围<br>lib/Sema/SemaTemplateDeduction.cpp | `Sfinae::attempt` 的 catch 范围<br>include/sfinae.h | 不做"immediate context"的精确判定；用异常类型 + `SfinaeContext` 标记在关键路径上手工划分 |
| `Sema::SubstituteExplicitTemplateArguments`<br>（void_t 归约的对应物） | `TemplateDeducer::reducePattern`<br>src/template_deduction.cpp | 只识别 void_t 一个名字，不是通用的别名模板展开 |
| libcxx `declval`<br>libcxx/include/utility:63 | `inferCall` 内建分支<br>src/semantic_analyzer.cpp（按名识别） | 不检查"只在未求值上下文出现"；不走函数模板实例化 |
| `Sema::CheckMemberAccess` / `BuildMemberExpr` | `inferMember` | 加了引用穿透（[expr.ref]），但不做访问控制 |

---

## ⑤ 实验

```bash
cd /root/cppproject/mycompiler
cmake --build build-linux -j8

# ① decltype 基础：变量 / 函数调用 / 取地址
./build-linux/minicc tests/tmpl/test_tmpl_33_decltype_basic.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: [decltype]   ⇒ 声明类型 = int
#         [infer] &a → int*

# ② 括号规则：decltype(a)=int vs decltype((a))=int&
./build-linux/minicc tests/tmpl/test_tmpl_34_decltype_paren.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: Probe<int> 走主模板(tag=0)、Probe<int&> 走偏特化(tag=1)
#   即: [decltype]   ⇒ 声明类型 = int
#       [decltype]   ⇒ 左值 ⇒ 表达式类型 = int&

# ③ void_t 探测命中
./build-linux/minicc tests/tmpl/test_tmpl_35_void_t_detect.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: [sfinae] void_t<2 个实参> —— 逐个替换 + 求值探测
#         [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓

# ④ 软失败：静默回退，不报错
./build-linux/minicc tests/tmpl/test_tmpl_36_void_t_fallback.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: [sfinae] ⤵ 模式第 2 位替换失败，按 SFINAE 判为【不匹配】（非错误）
#         [spec:select] └─ ③ falling back to PRIMARY template

# ⑤ 全有或全无：只有一个成员也算不命中
./build-linux/minicc tests/tmpl/test_tmpl_37_void_t_partial.cpp -o /tmp/a && /tmp/a; echo $?
#   关注: HalfRange 那一路 【第 1 个实参探测通过】但整体仍判不匹配

# ⑥ 硬错误对照（本文件必须编译失败）
./build-linux/minicc tests/tmpl/test_tmpl_38_error_no_fallback.cpp -o /tmp/a; echo $?   # 非 0
#   关注: no static member 'value' in class 'OnlyProbe_Empty_void' ——
#         类型限定访问 Cls<Args>::member 只解析静态成员；若此处是 SFINAE 探测，
#         说明偏特化被移除后回退到的主模板没有该成员

# ⑦ 用户原用例的 is_range + Box 半边
./build-linux/minicc tests/tmpl/test_tmpl_39_is_range_demo.cpp -o /tmp/a && /tmp/a; echo $?

# ⑧ 语义 oracle：与 clang 逐个对比退出码
for f in tests/tmpl/test_tmpl_3[3-9]*.cpp; do
  ./build-linux/minicc "$f" -o /tmp/m >/dev/null 2>&1 && /tmp/m; m=$?
  clang++-18 -std=c++20 "$f" -o /tmp/c >/dev/null 2>&1 && /tmp/c; c=$?
  echo "$(basename $f)  minicc=$m clang=$c"
done

# ⑨ 单元测试（进 ctest）
ctest --test-dir build-linux -R 'Decltype|Sfinae' --output-on-failure
#   或只看本主线三个套件：
./build-linux/unit_tests --gtest_filter='Decltype.*:Sfinae.*'
```

### 关键过程 ASCII 图：软失败 vs 硬错误

```
       同一个替换失败（T 没有 begin()）
                 │
      ┌──────────┴──────────┐
      │                     │
  主模板接得住           主模板接不住
  (test_tmpl_36)        (test_tmpl_38)
      │                     │
  偏特化被移出           偏特化被移出
  回退主模板 ✓           回退主模板 ✗（没有 value）
      │                     │
  编译通过              编译错误
  rc = 0                rc ≠ 0
      │                     │
      └──────────┬──────────┘
                 │
      SFINAE 管的是【左半边】：
      "替换失败不是错误" ≠ "没有候选时也不是错误"
```

---

## ⑥ 已知边界

| 边界 | 现状 | 归属 |
|---|---|---|
| 函数形参里的 decltype 依赖表达式 | `f(T t, decltype(t.begin())* g)` 推导期不会把 `t` 绑定到实参类型再求值，会报 "no match for non-dependent parameter" | 后续 |
| `decltype(auto)` | 不支持 | 后续 |
| 函数默认实参 | Parser 不支持 `void f(int x = 0)`，故 enable_if 惯用法暂时写不出 | 后续 |
| 两种重载同为函数模板时的 SFINAE 择优 | 需要 `enable_if_t`（别名模板）才好写；clang 对多数手写形态也判 ambiguous | 见 ROADMAP 主线 H |
| `std::declval` 的使用位置检查 | 不检查"只在未求值上下文" —— `int x = declval<int>();` 也会被接受（真 C++ 报错） | 有意简化 |
| 类内 `using` / 别名模板 / 类内 `static const` | 均不支持，故 std 垫片走内建注入 | 有意简化 |
| `decltype((a))` 作为**局部变量**类型 | 会退化成 int 拷贝且**不报错**（静默偏差）；已知问题，建议改用 `int&` 显式写出 | 待修 |

### 一个值得记住的静默偏差

```cpp
int a = 5;
decltype((a)) y = a;   // y 的真身是 int&
y = 9;                 // 真 C++：a 变成 9
                       // 本实现：y 是独立拷贝，a 不变 —— 且【不报错】
```

写测试时第一次就踩到了这里（`test_tmpl_34` 的一稿）。
绕开办法是改用 `Probe<T&>` 偏特化来观测类型，而不是用变量。
这一条记在这里，因为它属于"编译器没拦住、但结果是错的"那一类问题 ——
比直接报错更危险。

---

## ⑦ 与用户原代码的距离

用户原代码里与本文相关的部分：

```cpp
template <typename T, typename = void>
struct is_range : std::false_type {};

template <typename T>
struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>>
    : std::true_type {};

bool value  = is_range<decltype(numbers)>::value;   // ✅ 已跑通
bool value1 = is_range<decltype(1)>::value;         // ✅ 已跑通
```

**已通**：`is_range` 的完整定义、`decltype(numbers)`、`std::void_t`、
`std::declval`、`std::false_type` / `std::true_type` 及其 `::value`。
集成见 `tests/tmpl/test_tmpl_39_is_range_demo.cpp`。

**仍不通**：`operator|` / `operator||` 那一半，它依赖：

| 依赖 | 缺口 |
|---|---|
| `std::enable_if_t` | 别名模板 |
| `std::decay_t` | 别名模板 + 类型变换 |
| `std::is_base_of` | 内建类型关系查询 |
| `std::vector<int>` / `std::cout` | 无 STL、无输出 |
| 范围 for `for (auto const& item : rng)` | 未实现 |
| 尾置返回类型 + 默认模板实参组合 | 部分可用，见 ROADMAP |

这些都在 `docs/ROADMAP.md` 主线 H 的"未做"清单里。
