# 待修 bug 清单（BUGS）

> 与 [PITFALLS.md](PITFALLS.md) 的分工：
>
> - **本文件 = 未修**，每条含「复现 → 根因 → 修法 → 影响面」；
> - **PITFALLS.md = 已修**，修完一条就从这里挪过去。
>
> 每条都附**可复现的最小用例**——没有用例的 bug 条目不算数。

## 索引

| 编号 | 一句话 | 位置 | 影响面 |
|---|---|---|---|
| [B1](#b1-auto-占位符被壳包住) | `const auto` / `auto*` / `auto&` 全编不过 | `src/semantic_analyzer.cpp:2537` | 小（一个分支） |
| [B2](#b2-函数模板-mangling-漏了参数签名) | `_Z5twiceIiE` 少了签名 ⇒ 重载撞名、实例串味 | `src/template_instantiation.cpp:433` | **大**（全量重刷基线） |
| [B3](#b3-字段写入路径-3-硬编码-movl) | 裸字段名赋值写死 `movl` ⇒ 8B 字段高 32 位被截断 | `src/codegen.cpp:1123` | 小（一处指令） |

---

## B1. auto 占位符被壳包住

**复现**（六形态，只有裸 `auto` 活）

```cpp
int main() {
    int a = 5;
    const auto  v1 = a;    // ❌ Type mismatch in 'v1': declared 'const auto', got 'int'
    auto const  v2 = a;    // ❌ 同上
    auto*       v3 = &a;   // ❌ declared 'auto*', got 'int'
    auto&       v4 = a;    // ❌ declared 'auto&', got 'int'
    const auto& v5 = a;    // ❌ declared 'const auto&', got 'int'
    auto&&      v6 = 7;    // ❌ declared 'auto&&', got 'int'
    auto        v7 = a;    // ✅
    return 0;
}
```

**oracle**（clang++-18 全过，推导结果依次为 `const int` / `const int` / `int*` / `int&` / `const int&` / `int&&`）

**根因**：auto 分支只认**光杆** `Auto`，而占位符外面包了壳就看不见了。

| 源码 | parseType 建的树 | `type->isAuto()` | 落到哪 |
|---|---|---|---|
| `auto` | `Auto` | ✅ | auto 分支，正确 |
| `const auto` | `Const(Auto)` | ❌ | else 分支 ⇒ 严格类型检查 ⇒ 误报 |
| `auto*` | `Pointer(Auto)` | ❌ | 同上 |
| `auto&` | `LValueRef(Auto)` | ❌ | 同上 |

两个来源：前缀 const 由 `parser.cpp:336`（Step 2.5）套上；`*` / `&` / 后缀 const 由
`parser.cpp:350` 的后缀循环套上。**parser 无错**——按 [dcl.type.cv]/1，`const auto`
就是「const 修饰声明类型的基类型」，clang 的表示同构（const 在 QualType 上、AutoType 在里面）。
缺的是下游没人去壳里找占位符。

**修法**（[dcl.spec.auto]/7：用初始化式反推，再套回声明的 cv/指针/引用）

| declared | initializer | 重建 |
|---|---|---|
| `Auto` | `int` | `int` |
| `Const(Auto)` | `int` | `Const(int)` ← cv 是**声明的一部分**，不参与反推 |
| `Pointer(Auto)` | `int*` | `Pointer(int)` ← 要求实参确实是指针 |
| `LValueRef(Auto)` | `int` | `LValueRef(int)` |
| `RValueRef(Auto)` | `int` | `RValueRef(int)` |

一层递归剥壳 + 同层剥初始值，形状与 `TemplateDeducer::deducePair` 的合一循环同源。
对照 clang：`Sema::DeduceAutoType`。

**改动点**：`src/semantic_analyzer.cpp:2537` 与 `:2570` 的 `type->isAuto()` 两处入口。

**三件套**：`docs/learn/31-auto-placeholder-deduction.md` ＋
`tests/tmpl/test_tmpl_55_auto_cv_forms.cpp`（六形态 + 两个负向：`auto* v = a;` 类型不匹配、
`auto v;` 无初值报错）＋ demo。

---

## B2. 函数模板 mangling 漏了参数签名

**复现**（两个重载，参数个数相同 ⇒ 撞同一个符号）

```cpp
struct S { int v; };
int pick(int x)    { return 1; }
int pick(S x)      { return 2; }     // ← 与上一个同名同 arity

template <class T> int twice(T x) { return x + x; }

int main() {
    S s; s.v = 5;
    return twice(s.v) + pick(s);     // clang=21，minicc=22（pick 调错了重载）
}
```

**根因**：`NameMangler::mangleTemplateInstance` 只编到模板实参就收尾，**不带参数签名**：

| | 符号 |
|---|---|
| 真实 Itanium | `_Z5twiceIiEvT_` |
| minicc | `_Z5twiceIiE` |
| clang 报错时的提示 | `undefined reference to '_Z5twiceIiEvT_'` |

函数模板实例因此与「同名 + 同模板实参 + 不同参数签名」的另一个实例**撞符号**，
后写入者覆盖先写入者 ⇒ 调用点解析到错误的函数体。

**修法**：`mangleTemplateInstance` 增加参数签名段（复用既有的 `mangleFunction` 编码逻辑），
或直接拼 `mangleTemplateInstance(...) + 参数编码`。

**影响面（大）**：所有函数模板实例的汇编符号都会变 ⇒ **`logdiff.sh save` 全量重刷基线**，
`tests/tmpl/test_tmpl_11..19` 等断言日志原文的用例需同步更新。

**现役案发现场**：`demos/tmpl/04_partial_order.cpp` 的「⚠ 已知边界」注释。

---

## B3. 字段写入路径 3 硬编码 movl

**位置**：`src/codegen.cpp:1123`（`emitAssign` 的 `NodeKind::Var` 分支 —— 裸字段名赋值）

```cpp
emit(std::format("movl %eax, {}(%rcx)    # .{} = ...（偏移 {}，4 字节写入）", ...));
```

**复现**（`ptr` 是 8 字节指针，写入被截掉高 32 位）

```cpp
struct MyPtr {
    int* ptr;
    MyPtr(int* x) : ptr(x) {}     // ← 初始化列表走路径 2，是对的
    void set(int* x) { ptr = x; } // ← 路径 3，写死 movl ⇒ 高 32 位丢
};
```

**三条写字段的路**（读路径已按宽度分派，写路径只修了两条）

| 路径 | 写法 | 指令 |
|---|---|---|
| ① 成员表达式 | `q.p = v;` | 已按宽度分派 ✅ |
| ② 初始化列表 | `: ptr(x)` | 已按宽度分派 ✅ |
| ③ **裸字段名（函数体内）** | `ptr = x;` | **硬编码 `movl`** ❌ |

同理路径 1 的初始化端 `movq`（标量一律 8 字节）也会踩坏相邻字段——与
[PITFALLS C3/C4](PITFALLS.md#c4-字段写入硬编码-movl) 同族，那两条修的是 ①②。

**修法**：与读路径对称，按 `field->type` 的宽度选 `movb/movw/movl/movq`。

**影响面（小）**：只改这一条指令的发射逻辑，`size <= 4` 的分支保持原指令与原文案不动，
避免无谓的汇编/日志漂移。

**现役案发现场**：`demos/tmpl/07_alias_and_ctad.cpp` 的「⚠ 已知边界」注释。
