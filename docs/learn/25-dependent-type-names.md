# 25 · 依赖类型名 `typename T::type`：从"名字不知道是不是类型"到探测惯用法

> 一句话：`T::x` 在模板定义期是个**悬案** —— 编译器不知道 x 是类型还是值；
> `typename` 是原告的声明，真正的判决要等**替换**那一刻，而那一刻正是
> [temp.deduct]/8 的**直接上下文**：查不到必须**当场软失败**，
> 否则 `void_t<typename T::type>` 这套探测惯用法整个不成立。

---

## 1. 理论背景

### 1.1 为什么需要一个关键字（[temp.res]/5）

```cpp
template <typename T>
struct Get {
    T::type v;        // ← 这里的 type 是类型吗？
};
```

`sruct S { using type = int; };` 里 `type` 是类型；但
`struct S { static int type; };` 里 `type` 是**值**。定义 `Get` 的时候
T 是未知的，编译器**无法**判定 `T::type` 该按类型解析还是按值解析 ——
这是 C++ 语法里少见的"必须靠语义信息才能定句法"的地方。

标准的裁决是：**默认按值解析**，想让它按类型解析就写 `typename`。

```cpp
T::type  v;              // 按"值"解析（会报错，或解析成表达式）
typename T::type  v;     // 声明：它是类型
```

C++20 起在**非依赖**上下文里 `typename` 可省（P0634R3），
但模板里的依赖名仍然必须写。

对照 clang：`Parser::TryAnnotateTypeOrScopeToken` 看到 `typename` 就转
`ParseTypenameType`，产出的节点是 `DependentNameType`（限定者依赖时）
或 `TypenameType`（限定者不依赖）。

### 1.2 "依赖的"是什么意思

只要限定者里含**模板形参**，整条名字就是"依赖的"（dependent）——
实例化前无法确定，实例化后才落地。这与 `decltype` 的延迟求值是**同一类问题**：

| 机制 | 定义期留下什么 | 兑现时刻 |
|---|---|---|
| `decltype(e)` | 表达式 + 是否加括号 | `substituteType` Case 1.5 求值 |
| `typename T::type` | 限定者类型 + 成员名 | `substituteType` Case 5.5 查表 |

本实现两条线**放在同一个函数里**（`substituteType`）不是巧合：
它们在标准里都属于"实例化时才能完成的事"（[temp.inst]）。

### 1.3 为什么必须"当场失败"（[temp.deduct]/8）

```cpp
template <typename T, typename = void>
struct has_type : public std::false_type {};        // 兜底

template <typename T>
struct has_type<T, std::void_t<typename T::type>>
    : public std::true_type {};                     // 探测
```

对 `has_type<WithoutType>`：

```
匹配偏特化 has_type<T, void_t<typename T::type>>
  ① 推出 T := WithoutType
  ② 把 T 代入 void_t 的实参 ⇒ WithoutType::type —— 不存在！
  ③ ③ 就在这一刻：这是"替换失败"，不是"程序错误"
  ④ 该偏特化从候选集移除 ⇒ 回退主模板 ⇒ false_type
```

第 ③ 步的关键在于**失败发生的地点**：它发生在"被替换的那个类型自身的
构成过程"里 —— 标准的说法是**直接上下文**（immediate context）。
同样一个"查不到成员"，如果发生在**实例化出来的函数体内部**，
就不是直接上下文，必须硬报错。

本实现把这个边界做成了可观测状态（`SfinaeContext`，见 `include/sfinae.h`）：

```cpp
std::string why = std::format("no type named '{}' in '{}'", ...);
if (SfinaeContext::inImmediateContext()) Sfinae::fail(why);   // 软失败
error(why, SourceLocation{});                                 // 硬错误
```

对照 clang：`Sema::SubstType` 里对依赖限定名重新做限定名查找
（`getTypeName` + `LookupQualifiedName`），查不到即 `Sema::SubstitutionFailure`；
因为 clang 的 TreeTransform 本身就是 Sema 的一部分，这件事是"顺手"完成的 ——
本实现因此需要 `MemberTypeResolver` 这个回调把分层补回来（见 §3.2）。

---

## 2. 数据流（ASCII 图）

```
定义期（模板蓝图）
─────────────────────────────────────────────────────────────────────────
源码      typename T::type v;
          └──┬───┘
             │ Parser: 首个标识符 T 命中模板形参作用域 且 后面跟 '::'
             ▼
        Class("type", nestedQualifier = TemplateParam("T"))
             │
             │ Sema::resolveType 的 nested 分支：限定者是 TemplateParam ⇒
             ▼
        【保持依赖，原样返回】（不能在这里查，T 还没落地）
─────────────────────────────────────────────────────────────────────────
实例化 Get<WithType>（替换阶段 = 直接上下文）
─────────────────────────────────────────────────────────────────────────
substituteType(Class("type", qual=T), {T := WithType})
   ① Case 5.5：先替换限定者 ⇒ WithType
   ② 限定者仍是形参？否 ⇒ 调 m_memberResolver
   ③ Sema::resolveMemberType：
        resolveType(WithType) → 查到类声明
        findMemberType → typeAliases["type"] = int
        resolveType(int) → int
   ④ 字段 v : int ✓
─────────────────────────────────────────────────────────────────────────
实例化 Get<int>（同样是直接上下文）
─────────────────────────────────────────────────────────────────────────
   ①②③ 同上，但 findMemberType(int, "type") ⇒ nullptr
   ④ Sfinae::fail("no type named 'type' in 'int'")
      ⇒ 冒泡到 matchPattern 的 Sfinae::attempt ⇒ 该候选被移出（软失败）
```

---

## 3. 实现改动清单

| 文件 | 改动 | 作用 |
|---|---|---|
| `src/parser.cpp` | ① `parseType` Step 1.5：收下 `typename` 前缀；② 标识符分支：首名是本模板形参且后跟 `::` ⇒ 建"限定者是 TemplateParam"的嵌套节点；③ `parseStatement` 声明前瞻也接受 `typename` 开头 | 语法层认得依赖限定名 |
| `src/semantic_analyzer.cpp` | ① `resolveType` 的 nested 分支：限定者是形参 ⇒ 保持依赖；② 新增 `findMemberType`（纯查表）与 `resolveMemberType`（查 + 决定软硬） | 三个使用点的语义落点 |
| `src/template_instantiation.cpp` | `substituteType` 新增 Case 5.5：替换限定者 → 查成员表 → 解糖；限定者仍是形参则继续保持依赖 | **兑现时刻**：直接上下文内的失败即软失败 |
| `include/template_instantiation.h` | 新增 `MemberTypeResolver` 抽象接口 + `setMemberTypeResolver` | 让 instantiator 能在不依赖 Sema 的前提下查成员表 |
| `include/template_deduction.h` / `src/template_deduction.cpp` | 推导器持有并转发同一回调 | `reducePattern` 里临时建的 instantiator 也要能查表 |
| `include/semantic_analyzer.h` | `SemanticAnalyzer : public DecltypeEvaluator, public MemberTypeResolver` | Sema 实现该接口 |

### 3.1 修改点为什么落在这三处

`typename T::type` 有**三种出现位置**，各自需要不同的处理：

| 出现位置 | 例子 | 谁负责 |
|---|---|---|
| 字段/形参/返回类型 | `typename T::type v;` | `substituteType` Case 5.5（实例化时替换字段类型） |
| 模板实参内部 | `void_t<typename T::type>` | `reducePattern` → 临时 instantiator 的 Case 5.5 |
| 非模板上下文 | `typename Plain::type v;` | `resolveType` 的 nested 分支（限定者不依赖，直接查） |

### 3.2 为什么要新增一个接口（与 DecltypeEvaluator 同构）

"去某个类里查成员类型"这件事住在 Sema 里（只有它有 `m_classDecls`、
`typeAliases`，以及按需实例化的能力），而需要它的地方在**替换引擎**里。
直接互相依赖会形成 Sema ⇄ Instantiator 双向依赖，也让 `tests/unit/` 里
那些裸构造 instantiator 的单元测试被迫拖进整个 Sema。

于是沿用已有做法：抽一个抽象接口，Sema 实现它，instantiator 持可选指针。

```cpp
class MemberTypeResolver {
public:
    virtual ~MemberTypeResolver() = default;
    virtual TypePtr resolveMemberType(const TypePtr& qualifier,
                                      const std::string& member) = 0;
};
```

指针为空（单元测试路径）时，替换只是把限定者换掉、节点原样保留 ——
"依赖尚未兑现"这个事实因此可被单测观察。对照 clang：
clang 不需要这层间接，因为 TreeTransform 就是 Sema 的一部分。

---

## 4. 可复现实验

```bash
# ① 探测惯用法（期望 0；clang++-18 -std=c++20 同样 0）
./minicc tests/tmpl/test_tmpl_49_dependent_type_name.cpp -o /tmp/t49 && /tmp/t49; echo $?

# ② 看"当场软失败"的日志
./minicc tests/tmpl/test_tmpl_49_dependent_type_name.cpp -S -o /dev/null 2>&1 \
  | grep -E "sfinae|subst\] ★|依赖限定名"

# ③ 与 clang 交叉验证
clang++-18 -std=c++20 tests/tmpl/test_tmpl_49_dependent_type_name.cpp -o /tmp/t49c && /tmp/t49c; echo $?

# ④ 半软半硬对照：把探测搬到"非直接上下文"里就应变成硬错误
cat > /tmp/hard.cpp <<'EOF'
struct WithoutType { };
template <typename T>
struct Get { typename T::type v; };      // 这里查不到 → 硬错误
int main() { Get<WithoutType> g; return 0; }
EOF
./minicc /tmp/hard.cpp -o /tmp/hard; echo $?      # 期望非 0
clang++-18 -std=c++20 /tmp/hard.cpp -o /tmp/hardc; echo $?   # clang 同样非 0

# ⑤ 全量回归
ctest --test-dir build-linux                       # 154/154
```

---

## 5. 边界

| 缺口 | 说明 |
|---|---|
| `typename` 与模板模板形参 | `template<template<class> class C> typename C<T>::type` 未支持 |
| 非类型依赖名 | `T::value`（依赖的静态成员/枚举量）未支持 —— 本项目类内 `static const` 本身也缺 |
| 依赖基类 | `template<class T> struct D : T::base {};` 未支持 |
| `typename` 用在表达式位置 | `typename T::type{}` 这类构造表达式未支持 |
| 直接上下文的粒度 | 判据是"当前是否在 Sfinae::attempt 里"，而非标准的"是否在被替换类型自身的构成过程中"；两者对本项目覆盖的形态等价，但更复杂的嵌套替换可能判宽 |
