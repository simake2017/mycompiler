# bug 台账（BUGS）

> 与 [PITFALLS.md](PITFALLS.md) 的分工：
>
> - **本文件 = bug 台账**，每条含「复现 → 根因 → 修法 → 影响面 → 修复记录」；
> - **PITFALLS.md = 设计陷阱**（踩坑史/推理复盘），两者主题不同不必互挪。
>
> 每条都附**可复现的最小用例**——没有用例的 bug 条目不算数。
> 修复状态直接看下表的「状态」列；已修条目保留原文（根因有教学价值），
> 末尾追加「**修复**」小节记录改法与回归用例。

## 索引

| 编号 | 一句话 | 位置 | 影响面 | 状态 |
|---|---|---|---|---|
| [B1](#b1-auto-占位符被壳包住) | `const auto` / `auto*` / `auto&` 全编不过 | `src/semantic_analyzer.cpp:2537` | 小（一个分支） | ✅ 已修 |
| [B2](#b2-函数模板-mangling-漏了参数签名) | `_Z5twiceIiE` 少了签名 ⇒ 重载撞名、实例缓存假命中 | `src/template_instantiation.cpp` + `src/semantic_analyzer.cpp:3963` | **大**（全量重刷基线） | ✅ 已修 |
| [B3](#b3-字段写入路径-3-硬编码-movl) | 裸字段名赋值写死 `movl` ⇒ 8B 字段高 32 位被截断 | `src/codegen.cpp:1123` | 小（一处指令） | ✅ 已修 |
| [B4](#b4-带-return-的函数里局部对象析构是死代码) | 析构发射在 `leave; ret` **之后** ⇒ RAII 不执行 | `src/codegen.cpp` emitFunction 尾 | 中（RAII 语义） | ✅ 已修 |
| [B5](#b5-块注释不跨行) | 多行块注释的后续行被当代码分词 | `src/preprocessor.cpp:88` | **大**（25 个外部用例里 15 个） | ✅ 已修 |
| [B6](#b6-引用变量没有别名语义) | `int& r = a; r = 30;` 改不到 `a` | `src/codegen.cpp` 局部槽分配 | 中（引用语义） | ⬜ 未修 |
| [B7](#b7-自由函数重载根本不做-mangling) | `pick(int)` 与 `pick(S)` 同为裸名 `pick` ⇒ 汇编期符号重复 | `src/semantic_analyzer.cpp:2091` | **大**（全量重刷基线 + main/libc 特例） | ⬜ 未修 |
| [B8](#b8-mangling-没有替换表--符号与-clang-不逐字符等同) | 缺 Itanium 替换表（`S0_`）⇒ `_Z5twiceIiET_S0_` 缩不成 | `src/template_instantiation.cpp` encodeType | 中（只影响与 clang 逐字符等同） | ⬜ 未修 |
| [B9](#b9-struct-的默认继承级别被当成-private-处理) | `struct D : Base` 被拒（默认继承级别看错关键字） | `src/parser.cpp` parseClassDecl | 小（一个分支） | ✅ 已修 |

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

**修复** ✅

新增两个成员方法：

| 方法 | 作用 | 位置 |
|---|---|---|
| `containsAuto(t)` | 递归下钻 `Const`/`Pointer`/`Reference` 三层壳，判壳底是不是 `Auto` | `src/semantic_analyzer.cpp` |
| `deduceAutoType(pattern, init, ...)` | 合一：外壳照抄到结果，遇 `Auto` 把 A 绑上去 | 同上 |

`visit(VarDeclStmt&)` 的两处入口由 `type->isAuto()` 换成 `containsAuto(type)`；
日志里的硬编码 `"auto"` 换成 pattern 的 `toString()` —— **裸 `auto` 仍输出 `auto`**，
故既有用例日志逐字节不变（logdiff 已验证）。

实现与 `TemplateDeducer::deducePair` 同源，只是模式侧的"待绑定变量"是 `Auto`
而非 `TemplateParam`。

**回归用例**：
- `tests/tmpl/test_tmpl_55_auto_cv_forms.cpp` —— 六形态编译 + 求值，
  `main` 返回 32（与 clang++-18 同值）。求值式里六个变量都被读到，
  于是"某个形态被静默当成别的类型"也会让返回值改变。
- 推导【结果】的钉子不在运行期（`int&` 与 `int` 的拷贝语义当前不可区分，
  见 [B6](#b6-引用变量没有别名语义)）：靠 `logdiff` 基里那几行
  `[auto] ★ v4 : auto& ⟹ int&` 逐字节钉住。

---

## B2. 函数模板 mangling 漏了参数签名

**复现**（同名模板的两个 arity 重载，T 都推导成 int）

```cpp
template <class T> int f(T x)       { return 1; }
template <class T> int f(T x, int n) { return 2; }
int main() { return f(1) + f(1, 2); }   // clang=3，修复前 minicc=2
```

修复前**只实例化出一个**符号：`f(1,2)` 的推导结果 `f(T,int){T:=int}` 与 `f(1)` 编出
同一个名字，撞上缓存里已有的实例被当成"已实例化"直接返回 ⇒ 两次调用都落到单参函数体，
得 `1+1=2`。

**根因（两处，缺一不可）**

| # | 位置 | 症状 |
|---|---|---|
| ① | `TemplateInstantiator::instantiateFunction` | 只编到模板实参 `_Z1fIiE`，缺 Itanium 的 `<bare-function-type>` |
| ② | `SemanticAnalyzer::getOrInstantiateFunction` | **用同一串当实例缓存的键** ⇒ 不同签名撞成一次"假命中" |

②比①更凶险：①只是符号名不精确；②直接让调用点**静默取回错误的函数体** ——
不报错、不崩溃，只是算错。

| | f(T) | f(T,int) |
|---|---|---|
| clang | `_Z1fIiEiT_` | `_Z1fIiEiT_S0_i` |
| minicc（修复后） | `_Z1fIiEiT_` | `_Z1fIiEiT_i` |

**修法**：新增 `NameMangler::mangleFunctionTemplateInstance`，在 `mangleTemplateInstance`
的 `_Z<名>I<模板实参>E` 之后追加 `<bare-function-type>`（返回类型 + 各参数类型）；
两处调用点**同源**改用新函数 —— 缓存键与实例符号必须逐字符一致。

模板形参在签名段里编成 Itanium `<template-param>`（序号 0 ⇒ `T_`，1 ⇒ `T0_`…），
故 `encodeType` 多收一个 `typeParams` 形参，**带默认空表** ⇒ 类模板路径不传，
符号逐字节不变。

**修复** ✅

| 改动 | 位置 |
|---|---|
| 新增 `mangleFunctionTemplateInstance` | `src/template_instantiation.cpp` |
| `encodeType` 加 `typeParams` 默认形参 + `TemplateParam` 分支 | `src/template_instantiation.cpp:35` |
| 实例化端改用新函数 | `src/template_instantiation.cpp:430` |
| ★ 缓存键改用新函数（与上一条同源） | `src/semantic_analyzer.cpp:3963` |

**回归用例**：`tests/unit/test_template_deduction.cpp` 新增
`Instantiate.MangledNameCarriesSignature`（同名模板不同 arity、同 `T=int`，断言两符号
互不相同且分别为 `_Z1fIiEiT_` / `_Z1fIiEiT_i`）；同文件 `Instantiate.FunctionTemplate`、
`Instantiate.DifferentArgsDifferentSymbols` 的期望值同步更新为含签名的新名。

**影响面（大，已实测）**：131 行 logdiff 差异，**全部**是 `_Z…` 符号名，
`rc` 与程序输出**零变化** ⇒ 基线已全量重刷（`./logdiff.sh save`，89 个集成测试）。

**与 clang 的残留差异**：clang 会把参数表里**重复的类型**压成替换表引用
（`_Z1fIiEiT_S0_i`：第二个 `T` 编成 `S0_`），本实现一律展开成 `T_`。
符号仍唯一（模板实参段已区分实例），此差异单列为 [B8](#b8-mangling-没有替换表-符号与-clang-不逐字符等同)。

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

**修复** ✅

`emitAssign` 的 `NodeKind::Var` 分支改按 `field->size` 分派
（与读路径、初始化列表路径同构）：

| `field->size` | 指令 | 文案 |
|---|---|---|
| `== 1` | `movb %al` | `（偏移 N，1 字节写入）` |
| `<= 4` | `movl %eax` | `（偏移 N，4 字节写入）` ← **原文案一字未改** |
| 其余 | `movq %rax` | `（偏移 N，8 字节写入）` |

`<= 4` 分支保持原样，是为了让既有汇编零漂移 —— 修复只影响此前**写错**的 8B 路径。
重刷基线时的差异恰好是 3 处 `movl %eax, 0(%rcx) # .ptr = ...` → `movq %rax, ...`。

**回归用例**：`tests/unit/test_codegen_field_write.cpp`（`CodegenFieldWrite.*`，3 例）
—— 断言的是**不变量**「每条字段写入指令的宽度 == 该偏移上字段声明的宽度」，
而不是症状文案；三条写路径（成员表达式 / 初始化列表 / 裸字段名）各覆盖一遍，
否则"某条路径整体消失"会让测试静默变弱。

**负向已验证**：把 codegen 退回硬编码 `movl`，该用例立刻报
`偏移 8 的字段应为 q 宽度指令，实际用了 movl —— 指令：movl %eax, 8(%rcx)`。

---

## B6. 引用变量没有别名语义

**位置**：`src/codegen.cpp`（局部变量声明的槽分配 + `visit(VarDeclStmt&)` 发射路径）

**复现**（不需要 auto，裸引用一样）

```cpp
int main() {
    int a = 5;
    int& r = a;
    r = 30;
    return a;        // clang=30，minicc=5
}
```

**根因**：`int& r = a;` 在 Sema 侧类型是对的（`LValueRef(int)`），
但 CodeGen 把 `r` 当成一个**独立的栈槽**：初始化时把 `a` 的**值**拷进去，
`r = 30` 再写回 `r` 自己的槽 —— `a` 全程不受影响。

引用在语义上应当是**别名**（[dcl.ref]/1：引用不是对象、不占存储）：
`r = 30` 要翻译成"对 `r` 绑定的地址做间接写"，而不是"写 `r` 的槽"。
对照 clang：`CodeGenFunction::EmitDeclRefLValue` 对引用声明返回
`EmitLValueForReference`（拿到被绑定的地址），**从不给引用分配 alloca**。

**为什么一直没被发现**：现有用例里引用只出现在两处恰好都正确的位置 ——
① **函数形参**（`void f(T& x)`，走调用方传地址，天生正确）；
② **类型位置**（`Box<int&>` 参与偏特化匹配，不涉及运行期）。
"局部引用改写被引用对象"这个组合此前没有任何用例。

**影响面（中）**：`r = v;`、`r += 1;`、把 `r` 再传给 `f(int&)` 全都不对。
它同时约束了 B1 的测试设计 —— `auto&` 推成 `int&` 还是 `int`，
在运行期**不可区分**（两者读出来都是拷贝值），
所以 `tests/tmpl/test_tmpl_55` 只能钉住"能编过 + 值可读"，
推导结果靠 logdiff 基里的日志行钉住。

**修法**（未做，三步）：

1. **声明**：`visit(VarDeclStmt&)` 遇引用类型时不分配槽，而是求出初始化式的
   **地址**（复用 `&a` 那条路径）并记进别名表 `r → 被绑定槽的偏移`；
2. **读**：`emitVar` 见别名表先取地址再间接加载（`movq off(%rbp),%rcx; movq (%rcx),%rax`）；
3. **写**：赋值左侧同理走间接存储。

★ 注意与 [B4](#b4-带-return-的函数里局部对象析构是死代码) 的交互：
栈对象析构取址（`leaq off(%rbp),%rdi`）走的是**对象自身地址**，
引用别名不能影响它 —— 即"引用是别名，被引用对象仍在原槽"。

---

## B7. 自由函数重载根本不做 mangling

**复现**

```cpp
struct S { int v; };
int pick(int x) { return 1; }
int pick(S x)   { return 2; }
int main() { S s; s.v = 5; return pick(s.v) + pick(s); }   // clang=3，minicc: 汇编失败
```

`as` 报 `pick` 符号重复定义，汇编里两条 `.globl pick`。

**根因**：`SemanticAnalyzer::registerFunction` 对自由函数（`ownerClassName` 为空）
直接填裸名：

```cpp
if (decl->ownerClassName.empty()) {
    decl->mangledName = decl->name;   // ← 重载在这里全塌成一个名字
}
```

成员函数走 `Cls_method_N` 那套（另一条教学简化路径），函数模板实例走 NameMangler ——
只有**自由函数**是裸名。

上游还有一处**同一个病**：`m_functionMap[decl->name] = decl;` 也以裸名作键 ⇒
后注册的重载**覆盖**先注册的，调用点决议阶段连"存在两个候选"都看不见。

**为什么 `mangleFunction` 没救**：它**写好了却零调用点**（全仓只有定义与声明）——
`_Z4picki` / `_Z4pick1S` 的编码逻辑现成，只是从来没人调它。

**修法（草案）**

1. `registerFunction` 的自由函数分支改填
   `NameMangler::mangleFunction(decl->name, "", decl->parameters)`；
2. `m_functionMap` 增设 mangled 键（或改存候选列表），调用点按**实参类型**挑候选 ——
   这是真正的工作量：当前决议靠"形参类型精确匹配"扫 `m_functionMap`，键塌了就没得挑；
3. **两个必须保留裸名的特例**：`main`（`_start` 要调它）与内置 libc 原型
   （`registerBuiltins` 的 `malloc`/`free`/`memcpy`/`realloc` 要由系统链接器绑定）；
4. CodeGen 的 `call` 目标需与 Sema 选中的候选**同源**（沿用既有的
   `mangledName.empty() ? name : mangledName` 约定）。

**影响面（大）**：**每一个**集成测试的汇编都会变（所有自由函数符号）⇒
需独立重刷基线并逐项核对 `rc` 与程序输出不变。

**现状**：⬜ 未修 —— 与 B2 同源（都是 NameMangler 没被善用），但改动波及**决议**与
**CodeGen**，风险高于 B2，宜单独立项。

---

## B8. mangling 没有替换表 ⇒ 符号与 clang 不逐字符等同

**现象**（B2 修复后的残留差异）

| 源码 | clang | minicc |
|---|---|---|
| `template<class T> T twice(T x)` | `_Z5twiceIiET_S0_` | `_Z5twiceIiET_T_` |
| `template<class T> int f(T x, int n)` | `_Z1fIiEiT_S0_i` | `_Z1fIiEiT_i` |

**根因**：Itanium 的 **substitution**（ABI §5.1.9）—— 编码途中遇到的 `<type>` 进一张表，
**再次出现时编成 `S<n>_`**。clang 编返回类型 `T_` 时把它放进 #0，第二个 `T` 于是压成
`S0_`；minicc 的 `encodeType` 无状态，一律展开。

**性质**：**不是正确性问题** —— 符号仍唯一（模板实参段已区分实例），
链接、决议、执行都不受影响。记在这里是因为它影响"与 clang 逐字符核对"这个
教学惯例（NTTP 那批就是逐字符对过的）。

**要修的话**：`encodeType` 需带一张 substitution 表（引用或成员），并确定哪些节点入表
（Itanium 规则里复合类型入表，内建类型的行为需实测确认）。
**注意**：minicc 的类模板实例类型是**改名后的扁平 Type**（`MyPtr_int`），
没有可压缩的嵌套结构，故替换表只对**函数模板签名的参数表**有意义 —— 收益有限。

**现状**：⬜ 未修（教学差异，非缺陷）。

---

## B9. `struct` 的默认继承级别被当成 private 处理

**复现**

```cpp
struct Base { int v; };
struct Derived : Base { int w; };
int main() { Derived d; d.v = 3; d.w = 4; return d.v + d.w; }   // clang=7
```

minicc：`[Parse Error] 2:18 at 'Base': 仅支持 public 继承（每个基类前需写 'public'）`

**性质**：**错误拒绝合法程序** —— 比"不支持某特性"更严重：它让用户以为自己的代码写错了。
外部 25 个模板用例里 5 个倒在这里。

**根因**：[class.derived]/2 规定默认继承级别由**声明关键字**决定（`struct` ⇒ public，
`class` ⇒ private），而 `parseClassDecl` 只看"有没有写 public"、没看关键字 ——
`isStruct` 本来就在手上，却被 `(void)isStruct;` 丢弃了。

**修复** ✅

| 写法 | 修复前 | 修复后 |
|---|---|---|
| `struct D : Base` | ❌ 报错 | ✅ public 继承 |
| `struct D : public Base` | ✅ | ✅ |
| `class D : public Base` | ✅ | ✅ |
| `class D : Base` | ❌ 文案不准 | ❌ 说清"默认 private，未实现" |
| `struct D : private Base` | ❌ 文案不准 | ❌ 说清"private 未实现" |

**设计决策**：private/protected 继承的**语义**（基类子对象的访问控制）本项目不做，
故仍报错；但文案从"语法不允许"改成"语义未实现" —— **报错理由必须准确**。

**回归用例**：`tests/lang/test_basics_01.cpp` 第 ① 项；文档 `docs/learn/31` §1。

---

## B4. 带 return 的函数里局部对象析构是死代码

**复现**（析构里写全局变量，看有没有跑）

```cpp
int g = 0;
struct Dog { int v; ~Dog() { g = 5; } };

int f() {
    Dog d;
    return 0;        // ← 函数体里有 return
}

int main() { f(); return g; }    // clang=5，minicc=0
```

**根因**：`return` 语句自己发射了 `leave; ret`，而「函数末尾的统一析构」是在函数体**发完之后**
无条件追加的 —— 控制流早已返回，那几条析构指令是**不可达的死代码**。

实测 `f` 的汇编（`minicc -S`）：

```asm
    # return expr
    movq $0, %rax           # 整数字面量载入 rax
    leave                         # 恢复栈帧
    ret                           # 返回调用者
    # ~Dog() auto at function end (RAII)
    leaq -16(%rbp), %rdi       # ← 永远执行不到
    callq Dog_dtor
```

**为什么单测没抓到**：函数**没有** `return`（自然落到末尾）时析构是可达的，
所以既有的 RAII 用例全绿；只有「有 return + 有局部对象」这个组合才暴露。

**修法**：析构不该绑在「函数末尾」这个**位置**上，而该绑在**作用域退出点**上
（[stmt.return]/[class.dtor]：形参/局部对象在离开作用域时析构）。两条路：

| 方案 | 做法 | 代价 |
|---|---|---|
| ① 就近发射 | `return` 前先发本作用域的析构，再 `leave; ret` | 每个 return 点都要发一遍；嵌套块要逐层 |
| ② 统一出口 | 各 `return` 只 `jmp` 到函数末尾的收尾标签，收尾处发析构 | 改动小，但所有 return 路径的 rax 传递要保证不被打扰 |

②更接近 clang 的做法（`ReturnStmt` → `EmitBranchThroughCleanup` → 统一 cleanup block）。

**影响面（中）**：这是 **RAII 语义**问题，不是指令细节 —— 目前所有「有 return 的函数里
放局部对象」的代码都在静默泄漏/不析构。修完需要补回归用例。

**修复** ✅

采用**方案①「就近发射」**（`CodeGen::emitDtorsOnReturn`，`src/codegen.cpp`）：
`visit(ReturnStmt)` 在 `leave; ret` **之前**把栈上所有活跃作用域层的对象
从内到外、层内逆序各析构一遍，返回值先 `pushq %rax` 保起来再逐层 `callq`。

选①不选②的理由：②（统一出口）要跨作用域收集对象、把每个 `return` 改成 `jmp`，
会改动**所有**函数的 return 发射；①只动 `visit(ReturnStmt)` 一处，且
**无局部对象时一个字节都不多发** —— 87 个既有集成测试的汇编因此逐字节不变。

实测 `int f() { Dog d; return 0; }`：

```asm
    movq $0, %rax           # 返回值
    pushq %rax              # 保护返回值（析构调用会踩掉 rax）
    # ~Dog() before return (RAII, scope exit)
    leaq -16(%rbp), %rdi
    callq Dog_dtor          # ← 现在在 leave; ret 之前
    popq %rax
    leave
    ret
```

代价：return 之后那段"块尾/函数尾析构"仍会发射（已不可达，无害的冗余）。

**回归用例**：`tests/ctor/test_ctor_03_raii_return.cpp`（集成测试）
—— 返回值 121 同时钉住"跨层"与"LIFO"两件事：
`f()` 析构 a(1) ⇒ g=1；`h()` 先 b(2) 再 a(1) ⇒ g=1*10+2=12 → 12*10+1=121。
只析构最外层 / 顺序反了 / 漏了内层，都会得到别的数。

---

## B5. 块注释不跨行

**位置**：`src/preprocessor.cpp:88`（`Preprocessor::stripComment`）

**复现**（`/*` 独占一行、内容在后续行 —— 最普通的注释写法）

```cpp
/*
 * 说明文字
 */
int main() { return 0; }
```

minicc 报 `Unexpected character '�'`（中文注释），或对 ASCII 注释把
`*` `ascii` `*` `/` 四个记号喂给词法器。

**根因**：`stripComment` 是**逐行**调用的，`*/` 不在同一行时它丢掉本行剩余部分、
**却不记住"还在块注释里"**：

```cpp
size_t end = line.find("*/", i + 2);
out += ' ';
i = (end == std::string::npos) ? line.size() : end + 1;   // ← 跨行状态在此丢失
```

`[lex.phases]` 阶段 3 规定：注释在**分词前**整体替换成一个空格，块注释可以跨行 ——
这是硬要求，不是可选的简化。

**实测**（5 个最小探针）

| 输入 | 旧行为 |
|---|---|
| `/* 中文 */ int x;` | ✅ 同行闭合，正常 |
| `/*` ⏎ `中文` ⏎ `*/` | ❌ `Unexpected character '�'` |
| `/*` ⏎ ` * ascii` ⏎ ` */` | ❌ 分词成 `*` `ascii` `*` `/` |
| `int a; // 中文` | ✅ 行注释无跨行问题 |

**为什么词法器不是元凶**：`Lexer::skipWhitespaceAndComments`（`src/lexer.cpp:183`）
对跨行 `/* */` 的处理是**正确**的 —— 它拿到的是被预处理器破坏过的文本，锅在上一阶段。
判据：`-E` 输出里第 1 行是空格、第 2~3 行**原样保留**（正常应全是空白）。

**影响面（大）**：外部 25 个模板验收用例里 **15 个**倒在这里，且报错形态
（`Unexpected character '�'`）完全掩盖了真实缺口 —— 修掉后 25 个文件**全部**
推进到 Parser 阶段，暴露出的才是真语言缺口。

**修法**：给 `stripComment` 加**跨行状态**参数 `bool& inBlockComment`，
由 `processText` 的逐行循环按文件保管（`#include` 递归时各层独立，不串味）。

**修复** ✅

| 改动 | 位置 |
|---|---|
| 签名加 `bool& inBlockComment` | `include/preprocessor.h` |
| 注释内逐字符换空格、见 `*/` 复位 | `src/preprocessor.cpp` |
| `processText` 循环外持有状态 | `src/preprocessor.cpp:178` |

**回归用例**：`tests/unit/test_preprocessor.cpp` 的 `LexUtils.StripCommentAcrossLines`
—— 断言 ① 两行后仍在注释内 ② 见 `*/` 复位 ③ 同行闭合不留残状态。

> 工程教训：这个 bug 能潜伏这么久，是因为**既有测试里的块注释恰好都写在同一行**，
> 而多行块注释（`/*` 换行正文再 `*/`）才是最普通的写法 —— 测试覆盖的是"能过"的形态。
