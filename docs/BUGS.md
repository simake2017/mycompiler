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
| [B10](#b10-带参成员方法的调用符号拼不出来) | 带参成员方法调用全线 `undefined reference`（连下标糖一起） | `src/semantic_analyzer.cpp` inferCall + `src/codegen.cpp` | **大**（4 个集成测试长期 rc=1） | ✅ 已修 |
| [B11](#b11-本类自身多态而基类全非多态时-_vptr-与首基类字段压在同一-offset) | 本类有虚函数且无多态基类 ⇒ `_vptr` 与字段都占 0 | `src/semantic_analyzer.cpp` `[relocate]` | **大**（合法程序编译通过、运行 SIGSEGV） | ✅ 已修 |
| [B12](#b12-继承来的成员方法查不到) | `D d; d.g()`（g 在基类）报 `No member 'g'` | `src/semantic_analyzer.cpp` inferMember | 中（合法程序被拒） | ✅ 已修 |
| [B13](#b13-两个次基类各有一个同名字段时显示名塌陷且第二条偏移算成-0) | 两条记录压成同名 ⇒ 第二条偏移算成 0；歧义不报错 | 扁平化循环 + `computeClassLayout` | 中（非法程序被静默接受 + 布局打印错） | ✅ 已修 |
| [B14](#b14-派生类同名字段的隐藏方向做反了) | `d.x` 静默指向 `A::x` 而非 `D::x`（隐藏方向反了） | `include/type.h` `findField` | **大**（合法程序静默取错成员） | ✅ 已修 |
| [B15](#b15-祖辈前缀与直接基类撞名时偏移算到错的子对象) | 祖辈带来的前缀被当成"本类直接基类" ⇒ 偏移算错 | 扁平化循环 + `computeClassLayout` | 中（同 B13 的性质，根因更本质） | ✅ 已修 |
| [B16](#b16-无声明符的声明被拒收aint-int-报-parse-error) | 无声明符的声明（`A<int*,int**>;` / `int;`）被拒收 | `src/parser.cpp` `parseStatement` + `isDeclaratorlessDecl` | 小-中（拒收合法程序，且报错点离根因远） | ✅ 已修 |
| [B17](#b17-值位实参要求形态精确相等-拒收合法程序) | 值位实参要求"形态精确相等" ⇒ `Flag<1>` / `A<4L>` / `A<true>` 全被拒 | `src/semantic_analyzer.cpp` checkTemplateArguments ③-b | 中（拒收合法程序；同批还牵出实例键撞车） | ✅ 已修 |
| [B18](#b18-类模板里的成员模板在实例类里丢失) | 类模板里的成员模板没被带进实例类 ⇒ 实例上调成员模板全线失败 | `src/template_instantiation.cpp` instantiateClassTemplate | 中（合法程序不可用；同批牵出 static/virtual 位置） | ✅ 已修 |
| [B19](#b19-static--virtual-的识别位置写反标准写法被拒非法写法被收) | `template<class U> static U f(U)` 被拒、`static template<…>` 反被收 | `src/parser.cpp` 成员模板分支 | 小（两个方向都反了） | ✅ 已修 |
| [B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) | vtable 槽名与定义点名**两处各拼一遍** ⇒ 带参虚函数链接失败、次基类捏出假符号 | `src/semantic_analyzer.cpp` processClassDecl / registerFunction | 中（合法程序链接失败；另暴露一条静默算错，缺陷 c 未修） | ✅ 缺陷 a/b 已修 |
| [B21](#b21-限定名-sv-访问成员数据被拒收) | `S::v`（限定名 + 隐式 this 访问成员**数据**）报 `Undefined variable 'S::v'` | `src/semantic_analyzer.cpp` inferVar（Parser 把 `A::B` 拼成名字串） | 小（拒收合法程序，多写一个限定者而已） | ⬜ 未修 |
| [B22](#b22-同名同个数的成员重载sema-静默取第一个汇编期撞符号) | 成员重载只按"名字 + 个数"选 ⇒ 同名同个数在汇编期撞符号 `C_f_1` | `src/semantic_analyzer.cpp` findMethodInClass:4558 + memberMethodSymbolName:1445 | 中（拒收合法程序，报错点落在 as 的符号上） | ⬜ 未修 |
| [B23](#b23-浮点字面量缺失--运算按整数发射) | 无浮点字面量（`1.5` 切成 `1` `.` `5`）；`double` 运算发 `idivq` ⇒ 1/2 得 0 | `src/lexer.cpp`（[lex.fcon]）+ `src/codegen.cpp`（零 SSE 指令） | 中（a 响亮失败但文案误导；b **静默算错**） | ⬜ 未修 |

> **B11~B15 是同一轮排查的产物**（起点是"多继承下基类字段要不要改名"这个问题）。
> B13/B14/B15 三条根因相同 —— 见文末[小结](#小结-字符串兼任-id-与路径)。

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
符号仍唯一（模板实参段已区分实例），此差异单列为 [B8](#b8-mangling-没有替换表--符号与-clang-不逐字符等同)。

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

## B10. 带参成员方法的调用符号拼不出来

**复现**（不需要下标糖 —— 普通方法调用就够了）

```cpp
class C { public: int f(int x) { return x; } };
int main() { C c; return c.f(1) - 1; }   // clang = 0
```

minicc：`[LINK ERROR] undefined reference to 'C_f'`

**性质**：**正常程序编不过**，且错误一路推迟到链接期 —— 编译期的 Parser/Sema 全程绿灯，
用户看到的是"符号没定义"，完全指不到真正的原因。

**根因**：同一条语义判断写在了两个地方，两侧算法不一致。

| 位置 | 规则 | `IntVec::at(int)` 的产物 |
|---|---|---|
| 定义点 `semantic_analyzer.cpp` registerFunction | 带参成员方法名追加"参数个数"后缀 | `IntVec_at_1` |
| 调用点 `codegen.cpp` visit(CallExpr) | 硬拼 `类名 + "_" + 方法名` | `IntVec_at` |

```text
定义：.globl IntVec_at_1   .globl IntVec_set_2
调用：callq IntVec_at      callq IntVec_set        ⇒ 谁也匹配不上
```

**影响面**：**大** —— 凡"类方法带参数"即中招。集成测试里 `test_stl_02..05`
四个长期 rc=1；因为 logdiff 基线把 rc=1 当成了契约，**坏了很久没人发现**
（这本身就是教训：*基线固化失败 ⇒ 缺陷变成"预期行为"*）。

**修复** ✅ —— 沿用项目既有的"符号回填"套路

`MemberExpr::resolvedCalleeSymbol` 早已存在（成员模板 `Acc_add_int` 靠它绕开硬拼），
本次把**普通成员方法**也接上同一根线：

| 改动点 | 内容 |
|---|---|
| `semantic_analyzer.cpp` `inferCall` | 成员方法命中 ⇒ `mem->resolvedCalleeSymbol = method->mangledName` |
| `include/ast.h` `IndexExpr` | 新增 `atSymbol` / `setSymbol` 两个回填槽 |
| `semantic_analyzer.cpp` `inferIndex` | 查 `at()` 时顺带回填 `set()` 符号（`set` 只填不强制） |
| `codegen.cpp` `visit(IndexExpr)` / `AssignStmt(Index)` | 优先用回填符号，空则退回旧硬拼 |

**为什么虚调用不受影响**：`CodeGen::visit(CallExpr)` 先查 vtable 条目，命中即
`emitVirtualCall` 并 `return` —— 回填值只作用于其后的"非虚直接调用"分支。
修复后的基线差异**恰好只有那 4 个 stl 文件**，其余 90 个集成测试逐字节不变，
即是此判断的实证。

**回归用例**：`tests/stl/test_stl_02..05`（四个文件头都写明了本回归点）。

**顺带暴露（未处理）**：`inferIndex` 只校验 `at()`、从不校验 `set()` ——
类里没写 `set()` 时，错误同样一路推到链接期。本次按"只回填不强制"保持既有行为，
补校验是另一件事。

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

---

## B11. 本类自身多态而基类全非多态时 _vptr 与首基类字段压在同一 offset

**位置**：`src/semantic_analyzer.cpp` 的 `[relocate]` 子对象摆放阶段（`processClassDecl` 内）

**复现**（最朴素的单继承 + 一个虚函数）

```cpp
struct B { int name; };
struct A : B { virtual int f() { return 5; } };
int main() { A a; a.name = 3; return a.f() + a.name; }   // clang = 8
```

| | 编译 | 运行 | A 的布局 |
|---|---|---|---|
| clang++-18 | ✅ | `8` | `sizeof(A)=16`、`offsetof(A,name)=8` |
| minicc（修复前） | ✅ rc=0 | **SIGSEGV** | `+0: _vptr`、`+0: B.name` ← 同一格 |

**性质**：**真 miscompile** —— 合法程序编译通过、运行崩溃。写基类字段即写坏虚表指针，
下一次虚调用经 `[vptr=3]` 跳飞（实测 `exit=139`）。

**根因**：primary 的选择只扫了基类，没扫自己。

Itanium 的主基类优化：**primary base 只在动态（多态）基类里选**。一个多态基类都没有、
而本类**自身**有虚函数时 ⇒ **没有主基类**：vptr 自己占 offset 0，全部基类子对象从 8 起摆。

`[relocate]` 里那份判据只写了两态：

| 基类里有多个态基类吗 | 本类自身多态吗 | 旧行为 | 正确行为 |
|---|---|---|---|
| 有 | 任意 | primary = 第一个多态基类 @0 | ✅ 同左 |
| 无 | 否 | 首个基类当 primary @0 | ✅ 同左（全非多态，单继承兼容） |
| 无 | **是** | 首个基类当 primary @0 ❌ | **无 primary**：vptr@0、基类从 8 起 |

漏的正是第三态。而"本类自身多态"要到**更后面**的方法注册循环才置
`classLayout.hasVTable = true`，`[relocate]` 时还看不见 ——
两处各自看到的"多态性"不一致，又是一次"同一判断两处各算一份"。
`struct A : B, C { virtual ... }`（两个非多态基类）同理：首个基类 @0 与 vptr 重叠。

**影响面（大）**：单继承最朴素的写法就会中招，且**编译期全程绿灯**。
不崩只是因为恰好没人读那个被踩烂的 vptr —— 本 bug 的第一版用例返回 6"看着对"，正是如此。

**修复** ✅：判据补上第三态。

| 改动 | 内容 |
|---|---|
| 新增 `selfPoly` 预判 | 无多态基类时，扫 `decl->methods` 有无带 `virtual` 的方法 |
| 新增 `noPrimary` | `!anyPoly && selfPoly` ⇒ 所有子对象 `isPrimary=false`，`place` 从 8 起 |
| 日志 | 该分支打印"本类自身多态 ⇒ 无主基类，_vptr 占 0" |

★ `selfPoly` 只需看 `virtual` 关键字：无多态基类 ⇒ 没有可覆写的虚函数，
"覆写"这条来源不可能成立（与下方 override 检测同源，不重复实现）。
★ 既有分支的日志**逐字节不变**（`primary='X' 占 0` 原文保留）——
94 个集成测试零漂移，是"只影响此前算错的那一支"的实证。

**回归用例**：
- `tests/mi/test_mi_08_self_poly_no_poly_base.cpp` —— 两场景合一，返回 `111`
  （`5+3` 单非多态基类、`100+1+2` 双非多态基类）。
- `tests/unit/test_layout_lookup.cpp` 的 `LayoutLookup.VptrNeverOverlapsFields`
  —— 断言**不变量**「多态类的任何字段都不得落在 `[0,8)`」，覆盖三种继承形态；
  而不是"某类某字段在 8"这种症状值。
- **负向已验证**（单测）：`noPrimary` 改回恒 `false` ⇒ 立刻报
  "字段 'B.name' 偏移 0 落在 _vptr 区（0..7）—— 写它即写坏虚表指针"。
- **负向已验证**（集成）：同一突变重编 minicc ⇒ `test_mi_08` 运行 `-11`（SIGSEGV）。

---

## B12. 继承来的成员方法查不到

**位置**：`src/semantic_analyzer.cpp` 的 `inferMember`（查方法那一段）

**复现**

```cpp
struct A { int g() { return 5; } };
struct D : A { };
int main() { D d; return d.g(); }        // clang = 5
```

minicc：`[ERROR] [Semantic Error] 3:27: No member 'g' in class 'D'` —— **合法程序被拒**。

**根因**：成员查找只查了本类。

| 成员种类 | 存放在哪 | 查得到吗 |
|---|---|---|
| 字段 | 已**扁平化**进 `classLayout.fields`（含继承来的） | ✅ |
| 方法 | 各基类自己的 `decl->methods` 里，**没有**扁平化 | ❌ 只扫了本类 |

讽刺的是 `inferCall` 里**另有一条**搜基类的 BFS（成员调用走它），
但它被更早的 `inferType(callee)` → `inferMember` 的 `error()` 挡死 ——
`d.g()` 永远走不到那条 BFS。**同一条查找规则写在两处，只有一处带基类链**，
与 [B10](#b10-带参成员方法的调用符号拼不出来) 的"双份判据"同型。

**修复** ✅：把判据收口成两个原语，两条通路共用。

| 原语 | 作用 | 调用者 |
|---|---|---|
| `findMethodInClass(类, 名, arity)` | 在**单个类**里找方法；`arity < 0` 表示不看参数个数 | `inferCall`（arity=实参个数）、`findMethodInHierarchy` |
| `findMethodInHierarchy(类, 名, *declaringClass)` | 沿 `baseClassNames` 遍历（含本类），回填命中层 | `inferMember` |

★ 遍历顺序与 `inferCall` 的搜索**逐字一致**（基类名 `insert` 到队首、从队尾取）——
否则"两个基类都有同名方法"时两条通路会选中不同的那一个。
★ 隐藏规则由此自动成立：本类先于基类 ⇒ `D::g` 胜过 `A::g`。
★ 日志保持原文，命中基类时追加 `via 'A'`（本类命中不加 ⇒ 既有输出逐字节不变）。

**回归用例**：`tests/mi/test_mi_09_inherited_member.cpp`（多级链 `C:B:A`、指针形态、
同名隐藏三件事合一，返回 8）；单测 `LayoutLookup.InheritedMethodIsFound`。
**负向已验证**：`findMethodInHierarchy` 换回 `findMethodInClass` ⇒ 单测红；
重编 minicc ⇒ `test_mi_09` 编译 `rc=1`。

---

## B13. 两个次基类各有一个同名字段时显示名塌陷且第二条偏移算成 0

**复现**

```cpp
struct B { int name; };
struct C { int name; };
struct A : B, C { };
struct D : A { int tag; };
```

修复前的 `--dump-layout`：

```text
    ══ Memory Layout of 'D' ══
    +0: A.name : int (4 bytes)     ← 应该是 B::name
    +0: A.name : int (4 bytes)     ← ★ 应该是 +8，却算成了 0
    +16: tag : int (4 bytes)
```

**性质**：**只影响真 C++ 本来就 ill-formed 的程序**（两个子对象各有一份 `name`
⇒ [class.member.lookup]/8 歧义，`d.name` 必须报错）—— 不是 miscompile，
而是"静默接受非法程序 + 布局打印错"。但它与 B14/B15 同根，故一并根治。

**根因（两处叠加）**

| # | 位置 | 症状 |
|---|---|---|
| ① | 扁平化循环 | 改名时"剥掉第一个 `.` 再拼直接基类名" ⇒ `B.name` 与 `C.name` 剥成裸名 `name` 后都变成 `A.name` |
| ② | `computeClassLayout` | 主/次基类分支都按**裸名**在基类布局里找**第一个**命中 ⇒ 两条记录都命中 `B.name@0` |

**修法（根治）：让名字不再兼任"路径"**

`FieldInfo` 补三项，把"住在哪个子对象里"变成可精确回答的问题：

| 新字段 | 含义 |
|---|---|
| `declaredName` | 权威裸名 —— 名字查找的键（不再用显示名反拼前缀） |
| `viaBase` | 装着它的**直接基类**子对象名（空 = 本类自身字段） |
| `baseFieldIndex` | 在 `viaBase` 那个基类布局 `fields[]` 中的**下标** |

`computeClassLayout` 的字段放置随之合并成一条式子：

```text
field.offset = 子对象偏移(viaBase) + 基类布局[baseFieldIndex].offset
```

主基类子对象偏移恒为 0 ⇒ 该式自动退化成"直接复用基类偏移"，与旧实现同值；
两条分支合一后，`currentPrimaryBase` 变量连同"先挑主基类再分派"的逻辑一并删除。
**对照 clang**：成员是 `FieldDecl*` + `getFieldIndex()`，`RecordLayoutBuilder` 从不做字符串匹配。

**配套：歧义诊断**（[class.member.lookup]/8）

`findField` 只能回答"取哪一条"，回答不了"是不是只有一条"。新增
`ClassLayout::findFields(name)`（自身字段命中即隐藏基类，否则返回**全部**继承命中），
`inferMember` 在 `size() > 1` 时报：

```text
[ERROR] [Semantic Error] 43:6: Member 'name' is ambiguous in class 'D':
  found in 2 base-class subobjects (经 'A' 的 'A.name'、经 'A' 的 'A.name')
```

**回归用例**：`tests/mi/test_mi_11_ambiguous_member.cpp` —— **预期编译失败 rc=1**，
基线固化其报错原文（rc 若变 0，即说明歧义检测退化）；
单测 `LayoutLookup.LookupIsPrefixIndependentAndAmbiguityIsVisible` 的后半段断言必须抛错。
**负向已验证**：关掉歧义检测 ⇒ 单测红；重编 minicc ⇒ `test_mi_11` 变成 `rc=0`
（正是修复前"静默接受"的样子）。

---

## B14. 派生类同名字段的隐藏方向做反了

**位置**：`include/type.h` 的 `ClassLayout::findField`

**复现**

```cpp
class A { public: int x; void setA(int v) { x = v; } int getA() { return x; } };
class D : public A { public: int x; };
int main() { D d; d.setA(7); d.x = 5; return d.getA() * 10 + d.x; }   // clang = 75
```

minicc（修复前）= **55** —— `d.x` 落到了 `A::x`（`d.getA()` 也读出 5 而不是 7）。

**性质**：**真 miscompile** —— 合法、**无歧义**（[class.member.lookup]/3：派生类声明
**隐藏**基类同名声明，`d.x` 唯一 = `D::x`），却静默绑到基类那一份。

**根因**：把**显示名**当成了**查找键**。

| 步骤 | 旧行为 |
|---|---|
| 扁平化 | 自身字段 `x` 与基类字段撞名 ⇒ 把**自身**改名成 `D.x`，基类那条留 `A.x` |
| 查找 | 第二步按 `f.name == f.sourceClass + "." + name` **反拼前缀** ⇒ 裸名 `x` 精确配上 `A.x` ⇒ 命中基类 |

方向就这样反了：为了让字符串不撞，动的是自身字段；而"精确匹配先到先得"
又让 `d.x` 指到了基类成员 —— **显示层的改动泄漏进了索引层**。

**修复** ✅：`findField` 改三级，键换成 `declaredName`

| 顺序 | 命中条件 | 依据 |
|---|---|---|
| ① | `f.name == name` | 调用方自己写了全限定名 `"A.a"` |
| ② | **自身字段**（`sourceClass` 空或 == 本类名）且 `declaredName == name` | [class.member.lookup]/3 隐藏 |
| ③ | 继承字段且 `declaredName == name` | 兜底 |

★ 显示名（`D.x` / `A.x`）仍照旧生成 —— 它只用于打印，不再参与判定。

**回归用例**：`tests/mi/test_mi_10_field_hiding.cpp`（返回 75）；
单测 `LayoutLookup.OwnFieldHidesInheritedOne`（断言**不变量**：`d.x` 命中的那条
必须 `viaBase` 为空 = 本类自身）。
**负向已验证**：把 ② 的条件改回"只认 `sourceClass` 为空"（= 旧行为）⇒ 单测红；
重编 minicc ⇒ `test_mi_10` 运行 `55`。

---

## B15. 祖辈前缀与直接基类撞名时偏移算到错的子对象

**位置**：扁平化循环 + `computeClassLayout`（与 B13 同一段代码）

**复现**

```cpp
struct Q { virtual int g() { return 0; } };
struct B { int x; };
struct P : Q, B { int y; };
struct X : P, B { int tag; };
```

修复前：从 `P` 主基类链带出来的那份 `B.x` 被算到了 **X 的直接 B 子对象**上，
于是两条记录**同偏移**（本该一条 +8、一条 +24）。

**性质**：clang 对 `o.x` 报 ill-formed ——
`non-static member 'x' found in multiple base-class subobjects of type 'B'`。
与 B13 同属"本就非法 + 布局错"，单列是因为它的根因更本质。

**根因**：`sourceClass` 一个字段兼任了两件事。

| 它想说的事 | 它实际是 | 破口 |
|---|---|---|
| 谁**声明**了这个字段 | 一个裸类名 | 同一类名在一条链上出现两次就分不开 |
| 它住在哪个**子对象**里 | 靠 `sourceClass` 去 `bases[]` 里**反查** | 反查命中的是**本类**的同名直接基类 |

主基类分支是**原样拷贝**（`FieldInfo fi = baseField;`），祖辈留下的 `sourceClass="B"`
一路带到 X —— 而 X 恰好真有一个直接基类叫 `B`。

**修复** ✅：`viaBase` 在扁平化时**覆盖**为本层的直接基类、`baseFieldIndex` 记录下标；
`computeClassLayout` 按 `(viaBase, 下标)` 定位，不再看 `sourceClass`、也不再按名字匹配。
祖辈前缀从此只是显示标签，去多少代都不影响查找；它与"源自 B"这件事各由
`declaredName` / `sourceClass` 独立承载。

**回归用例**：单测 `LayoutLookup.LookupIsPrefixIndependentAndAmbiguityIsVisible`
—— 断言两条同名记录**偏移必须不同**、且各自的 `viaBase` 必须是**本类的直接基类**。
**负向已验证**：把派发改回 `sourceClass` + 按名字查内部偏移（**忠实复刻旧代码**）⇒
该单测报 `Expected: (hits[0]->offset) != (hits[1]->offset), actual: 24 vs 24` —— 正是旧 bug 的形态。

---

## B16. 无声明符的声明被拒收（`A<int*, int**>;` 报 Parse Error）

**复现**

```cpp
template<class a, class b> struct A { };
template<class T>          struct A<T, T*> { };   // 偏特化

int add(int a, int b) { return a + b; }

int main() {
    A<int*, int**>;                 // ← 无声明符的声明
    auto result = add(1, 2);
    return 0;
}
```

minicc：`[Parse Error] 12:7 at 'int': Unexpected token 'int' in expression`（rc=1）
clang++-18：接受，只给 `warning: declaration does not declare anything [-Wmissing-declarations]`

**四形态对照**（前两行是 bug，后两行是"本来就该拒"的对照组）

| 语句 | clang++-18 `-std=c++20` | minicc | 备注 |
|---|---|---|---|
| `A<int*, int**>;` | ✅ 仅 warning | ❌ Parse Error，**报错点在实参中间** | 走前瞻分支 |
| `int;` | ✅ 仅 warning | ❌ `Expected variable name` | 走类型关键字分支 |
| `int*;` | ❌ `declaration of 'int *' has no name` | ❌ 同上 | ✅ 两边一致：`*` 后不能省名字 |
| `S;`（S 为类名） | ✅ 仅 warning（**判为声明**） | ✅ **但判为表达式语句** | 分类不同、当前都无副作用 |

**性质**：**错误拒绝合法程序** —— 与 [B9](#b9-struct-的默认继承级别被当成-private-处理) 同类。

**根因**：[dcl.dcl]/1 的 simple-declaration 里 `init-declarator-list` 是**可选**的 ——
`decl-specifier-seq ;` 本身合法，语义是"什么都不声明"（clang 的 `-Wmissing-declarations`
正是为它准备的）。minicc 的两条声明路径都**默认"类型后面必有名字"**：

1. **类型关键字开头**：`src/parser.cpp:1928` → `parseVarDeclStmt` →
   `src/parser.cpp:2025` 的 `expect(Identifier, "Expected variable name")` 撞死在 `;` 上。
2. **标识符开头**：`src/parser.cpp:1982` 的前瞻跑完"跳类型名 + `<...>` 深度配对 + `*`/`&`"
   之后停在 `;`，`check(Identifier)` 不成立 ⇒ 回滚到 `src/parser.cpp:1990` 走表达式分支
   ⇒ `parseExprOrAssignStmt` 把 `A` 当标识符表达式，再撞死在第 2 个实参的 `int` 上。

★ 第 2 条的**诊断质量**尤其差：报错位置指向实参中间的 `int`，用户完全看不出
真正的问题是"这条声明没有声明符"。根因在语句层，症状在表达式层 —— 中间隔着一次回滚。

**为何不影响偏特化结论**（本轮排查的起点是"`A<int*, int**>` 走哪个裁决"）：
本 bug 只卡在**无声明符**这一种写法上。改成 `A<int*, int**> x;` 后链路完全正常，
`selectClassTemplate` 三路走 ②（`A<T, T*>`，合一推出 `T := int*`），与 clang 的
`static_assert(A<int*,int**>::tag == 1)` 一致 —— **偏特化匹配本身是好的，卡的是 Parser。**

**影响面**：小（纯语法缺口，不会产出错误代码）。但两处：一是合法程序被拒，
二是**报错点离根因太远**，属于"诊断指向派生现象而非根因"的老毛病。

**修法**：新增 `EmptyStmt` 节点（`NodeKind::Empty`，对应 clang 的 `NullStmt`），
把「空语句 `;`」与「无声明符的声明」收在同一个节点上 —— 两者语义同为"不做任何事"。
空语句是**纯叶子**（只有位置、无子节点、无类型），于是 Sema 根本无从 `resolveType`，
"不实例化任何模板"由**结构**保证，而不是靠约定。

**修复** ✅

| 改动点 | 文件 | 内容 |
|---|---|---|
| 新节点 | `include/ast.h` | `NodeKind::Empty` + `struct EmptyStmt : Statement` |
| 访问者 | `include/ast_visitor.h` | `visit(EmptyStmt&)`（语句 8 → 9，总节点 32 → 33） |
| 路径 ① | `src/parser.cpp` `parseStatement` 开头 | 裸 `;` ⇒ `EmptyStmt`（[stmt]/1） |
| 路径 ② | `src/parser.cpp` 类型关键字分支 | 类型后跟 `;` 且**声明符为空** ⇒ `EmptyStmt` |
| 路径 ③ | `src/parser.cpp` 标识符前瞻分支 | 前瞻吃到了**类型 id 语法**且停在 `;` ⇒ 回滚 `parseType` ⇒ `EmptyStmt` |
| 判据 | `src/parser.cpp` `isDeclaratorlessDecl` | 剥掉最外层 `const` 后仍是指针/引用 ⇒ 声明符非空 ⇒ **不放行** |

★ 判据 `isDeclaratorlessDecl` 是这条修复的**难点所在**：minicc 的 `parseType`
把 ptr-operator 一并吃进类型（`int*` ⇒ `Pointer(Int)`），所以"声明符是否为空的
要在**类型上反推** —— 剥掉最外层 `const`（它属 decl-specifier）后若仍是
指针/引用，说明吃进了 ptr-operator。逐例与 clang 核对：

| 写法 | 类型 | 剥 const 后 | clang | minicc 修复后 |
|---|---|---|---|---|
| `int;` | `Int` | `Int` | ✅ 仅 warning | ✅ |
| `const int;` | `Const(Int)` | `Int` | ✅ 仅 warning | ✅ |
| `unsigned int;` | `UInt` | `UInt` | ✅ 仅 warning | ✅ |
| `A<int*,int**>;` | `Class("A")` | 同左 | ✅ 仅 warning | ✅ ★ 正主 |
| `Box<int>::type;` | 成员类型 | 同左 | ✅ 仅 warning | ✅ |
| `int*;` | `Pointer(Int)` | `Pointer` | ❌ `no name` | ❌ （不回归） |
| `int* const;` | `Const(Pointer(Int))` | `Pointer` | ❌ `expected unqualified-id` | ❌ |
| `int&;` | `LValueRef` | `LValueRef` | ❌ | ❌ |

★ **路径 ③ 的判据必须带 `sawTypeIdSyntax`**：`<...>` 与 `::` 在表达式里不可能出现，
故一旦见到就坐实了"这是类型"。裸 `S;` **不走这条路** —— 它也可能是【同名对象】的
表达式语句，Token 前瞻分不出来，需要查符号表（留待 Sema）。这与 `[stmt.ambig]` 是
同族问题：**能当声明就当声明**，而"能不能当"要靠名字查找。

**下游三处必须同步补**（漏一处就是静默丢语句或运行期崩溃）：

| 位置 | 漏了会怎样 |
|---|---|
| `src/template_instantiation.cpp` `cloneStmt` | 落到 `default: return nullptr` ⇒ 实例化方法体里混进空指针节点 |
| `src/main.cpp` `AstDumper::visit` | `--dump-ast` 对该节点无输出 |
| `tests/unit/test_ast_visitor.cpp` `auditStmt` | 该测试的 `default:` 分支报"未覆盖的语句 kind" |

**回归用例**：

- `tests/unit/test_declaratorless_decl.cpp`（`DeclaratorlessDecl.*` 8 例）——
  含三条**不变量**断言，而非具体实现细节：
  ① `DoesNotInstantiateAnyTemplate`：裸声明**零实例化**（日志里既无 `[spec:select]`
     也无 `A_intP_intPP`），且同文件的变量声明版**必须实例化**作对照；
  ② `EmptyStmtSurvivesInstantiationClone`：实例化方法体里**不得出现空指针节点**、
     空语句**条数守恒**；
  ③ `PointerDeclaratorIsStillRejected`：三条 ptr-operator 形态**必须继续报错**。
- `tests/lang/test_basics_02_declaratorless.cpp`（集成级）——
  偏特化体里埋 `typename T::nope boom;` 作**实例化探针**（★ 必须是**依赖名**：
  非依赖名在两阶段查找的第一阶段就被查了，clang 定义期即报错，探针失效）。
  裸语句 ⇒ 两边都编过；改成变量声明 ⇒ 两边都报错（clang: `type 'int *' cannot
  be used prior to '::'`；minicc: `no type named 'nope' in 'int*'`）。

**负向验证**（四条改动逐个突变，全部变红）：

| 突变 | 单测 | 集成 |
|---|---|---|
| M1 拆掉 `!isPointer() && !isReference()` 守卫 | `PointerDeclaratorIsStillRejected` ✗ | — |
| M2 拆掉类型关键字分支的 `;` 放行 | `TypeKeywordWithout…` ✗ | 编译失败 ✗ |
| M3 拆掉标识符前瞻分支 | `TemplateId…` / `QualifiedTypeId…` / `DoesNotInstantiate…` ✗ | 编译失败 ✗ |
| M4 拆掉 `cloneStmt` 的 `case NodeKind::Empty` | `EmptyStmtSurvivesInstantiationClone` ✗ | 编译失败 ✗ |

**残留（本轮未覆盖，与本 bug 同族）**

- `S;`（类名 / 任何裸标识符开头的无声明符声明）：minicc 仍按**表达式语句**处理，
  clang 按**声明**。当前两者都无副作用，但分类不同 —— 要正确定性需要 Parser
  有符号表，或引入"声明/表达式待定"的中间节点。与 `[stmt.ambig]` 的完整裁决
  （`a < b > c;`）是同一件事，见 docs/ROADMAP 主线 C。
- `T;`（模板形参作为裸类型名）同理：定义期无法判定 `T` 是类型还是值。

**影响面**：小-中（原先拒收合法程序；修复后 110 个既有集成用例**零漂移**，
仅新增用例使基线数量 110 → 111、单测 249 → 257）。

**状态**：✅ 已修

---

## B17. 值位实参要求「形态精确相等」⇒ 拒收合法程序

**复现**（三形态，只有"形态恰好等于形参类型"的那一种活）

```cpp
template<bool B> struct Flag { int v; };
template<int  N> struct A    { int v; };

int main() {
    Flag<1>  f;   // int 1  ⇒ bool ：本应合法
    A<4L>    a;   // long 4 ⇒ int  ：本应合法
    A<true>  b;   // bool   ⇒ int  ：本应合法
    return 0;
}
```

minicc：三条全报 `non-type template argument … cannot be narrowed to type '…'`（rc=1）
clang++-18 `-std=c++20`：三条**全过**，零诊断。

**性质**：**错误拒绝合法程序** —— 与 [B9](#b9-struct-的默认继承级别被当成-private-处理) 同类
（B9 拒的是 `struct D : Base` 的默认继承级别，这条拒的是合法实参转换）。

**根因**：判据选错了。旧版第三道检查写的是

```cpp
else if (!a.valueType->equals(p.nonType)) { error("… cannot be narrowed …"); }
```

即要求"实参的字面量形态与形参类型**精确相等**"。标准要的不是这个：

| 标准位置 | 说的是什么 |
|---|---|
| [temp.arg.nontype]/1 | 实参须是形参类型的 **converted constant expression** |
| [expr.const]/10 | 该术语的定义直接引用 [dcl.init]/7 的初始化规则 |
| [dcl.init]/7 | 窄化禁令，但**常量表达式豁免"值恰好装得下"的那部分** |

⇒ 正确判据是**可表示性**（值装不装得下），不是**形态相等**。三条合法用例恰好各踩一个面：

| 用法 | 标准依据 | 形态 | 值 |
|---|---|---|---|
| `Flag<1>` | [conv.bool]：int → bool，1 ⇒ true | int ≠ bool | 1 ∈ {0,1} ✅ |
| `A<4L>` | [conv.integral]：long → int | long ≠ int | 4 ∈ int ✅ |
| `A<true>` | [conv.prom]：bool → int 整型提升 | bool ≠ int | 恒不窄化 ✅ |

**为什么"碰巧拒对了"掩盖了它**：`F<2>`（2 → bool）这类**真该拒**的用例旧版也拒，
于是负向测试全绿 —— 只有拿正向用例去撞才会露馅。这正是
「负向测试全绿 ≠ 判据正确」的又一例（与 [B10](#b10-带参成员方法的调用符号拼不出来) 的
"错误停在链接期、编译期全程绿灯"是同一类盲区）。

**修法**：判据换成 `Type::canRepresentValue(v)`（[dcl.init]/7 + [expr.const]/10 的合体）：

```cpp
else if (p.nonType && !p.nonType->canRepresentValue(a.value)) {
    // clang 原文：non-type template argument evaluates to N, which cannot be
    // narrowed to type 'T' [-Wc++11-narrowing]（本项目按错误处理）
    error("non-type template argument evaluates to {}, which cannot be narrowed to type '{}'");
}
else if (!a.valueType->equals(p.nonType)) {
    // ③-c 形态【归一】：把实参形态改写成形参类型
    a.valueType = p.nonType;
}
```

★ 两条设计约束：

1. **判据单点**：`canRepresentValue` 定义在 `src/type.cpp`，被 semantic_analyzer
   与 `src/main.cpp` 的 Phase 4 演示路径共用 —— 不允许在调用点再写一份
   （承 B10 / B12 的教训："同一判据不许写两份"）。
2. **归一不是修饰而是语义必需**：归一之后 `Buf<4L>` 与 `Buf<4>` 的可读串 / 缓存键 /
   mangling 才会一致。clang 也认它们是同一实例（`template struct A<4>;
   template struct A<4L>;` 报 duplicate explicit instantiation）。

**同批牵出的第二个 bug（同源，也是"字符串当键"）**：
`template<auto V>` 下 `K<4>`（int）与 `K<4L>`（long）是**两个实例**，
但实例名 / 缓存键此前由 `TemplateArg::toString()` 生成 —— 它对两者都产 `"4"`，
于是第二个**静默复用**第一个实例。既没有报错，也没有算错（值恰好都是 4），
**连"看起来不对"的症状都没有**。修法：键与实例名同源于
`NameMangler::losslessArgumentsKey` / `renderArgLossless`，形态只在 `auto` 形参位写入
⇒ `K_4Cint` / `K_4Clong`（`C` 是 `:` 的安全转义，`sanitizeSymbolChars` 同批补的）。
★ 这与 [B13~B15](#小结-字符串兼任-id-与路径) 和 docs/learn/23 的坑**同源**：
**拿"给人看的字符串"当"机器用的键"，早晚出事。**

**影响面**：中。
① 拒收合法程序（正向用例）；
② 静默撞键（`auto` 形参下的实例复用）—— 后者无声无息，更难发现。

**修复记录**：`src/semantic_analyzer.cpp` checkTemplateArguments ③-b/③-c；
`Type::canRepresentValue` 新增于 `include/type.h` + `src/type.cpp`；
`NameMangler::losslessArgumentsKey` / `renderArgLossless` 新增于
`include/template_instantiation.h` + `src/template_instantiation.cpp`。

**回归用例**：
· 集成 `tests/tmpl/test_tmpl_64_nttp_integral_conversion.cpp`（三条正向，文件头注明回归 B17）
· 集成 `tests/tmpl/test_tmpl_65/66/67_error_nttp_*`（三条负向，文案与 clang 逐字同）
· 集成 `tests/tmpl/test_tmpl_68_nttp_auto_param.cpp`（`K<4>` / `K<4L>` 各自成实例 + 全特化只吃 int 形态）
· 单测 `tests/unit/test_nttp_type_domain.cpp`（`NttpTypeDomain.*` 10 例）

**突变负向验证**（每条判据删掉后必须变红，已逐条实跑）：

| 突变 | 红掉的用例 |
|---|---|
| `canRepresentValue` 的无符号分支改成 `return true` | `CanRepresentValueBoundaries` + `NarrowingIsRejectedWithClangWording` |
| 判据退回 `a.valueType->equals(p.nonType)` | `IntegralConversionIsAcceptedWhenValueFits` + `ConversionNormalizesArgumentFormToOneInstance` |
| 缓存键退回 `args[i].toString()` | `AutoParamDistinguishesArgumentForms` |
| 字符字面量日志不转义 | `CharLiteralLogStaysOnOneLine` |

**状态**：✅ 已修

---

## B18. 类模板里的成员模板在实例类里丢失

**复现**

```cpp
template <class T>
struct Box {
    template <class U>
    U pick(U x) { return x; }
};
int main() { Box<int> b; return b.pick(7); }
```

minicc：`[Semantic Error] 3:34: No member 'pick' in class 'Box_int'`（rc=1）
clang++-18 `-std=c++20`：正常，rc=7。

**对照组**（把缺口钉死在同一处）：同样的成员模板放在**普通类**里（
`struct Acc { template<class T> T add(T x); };`，见 tests/tmpl/test_tmpl_56）
一路正常。所以缺的不是"成员模板"这个特性，而是**"类本身也是模板"时多出来的那一跳**。

**性质**：**错误拒绝合法程序**（与 [B9](#b9-struct-的默认继承级别被当成-private-处理)、
[B17](#b17-值位实参要求形态精确相等-拒收合法程序) 同类）。★ 症状停在**语义期**，
而解析期一切正常 —— 日志里明明白白印着
`[parse:member] ★ 'pick' 是成员模板（1 个模板形参，[temp.mem]）`，
"解析对了"不等于"这条特性通了"。

**根因**：两层模板形参的绑定时机不同，而"类实例化"这一步只搬了一半东西。

| 层 | 谁绑定 | 何时绑定 |
|---|---|---|
| 外层 `T` | 类实例化（`Box<int>`） | 建实例类那一刻 |
| 内层 `U` | 调用点实参推导（`b.pick(7)`） | 每次调用 |

于是"类实例化"必须产出：把蓝图里的成员模板**复制一份、只替换外层形参、保留内层形参**，
挂到实例类名下。旧版三个环节恰好都缺：

1. `instantiateClassTemplate`（src/template_instantiation.cpp）克隆了字段（步骤 4）、
   方法（步骤 5）、类内类型别名（步骤 5.5），**唯独没有 `memberTemplates`**；
2. `src/semantic_analyzer.cpp:1822` 那张成员模板登记表挂在 `processClassDecl` 里
   —— 而类模板蓝图在 Pass 1 只登记模板（`↳ class template 'Box' registered`），
   **从不经过 `processClassDecl`** ⇒ 蓝图那张表从来没进过注册表；
3. 调用点 `inferCall` 查的键是**对象的类名**（src/semantic_analyzer.cpp:3375
   `m_classMemberTemplates.find(clsName)`）⇒ `"Box_int"` 查不到 ⇒ No member。

**修法**：`instantiateClassTemplate` 新增步骤 **5.6 克隆成员模板**（
`mt` 取自 `templateDecl->classTemplate` —— 本次**命中**的那份蓝图，主模板 / 偏特化 /
全特化一视同仁）：

```cpp
for (auto& mt : templateDecl->classTemplate->memberTemplates) {
    auto clonedMt = std::make_shared<TemplateDecl>();
    clonedMt->templateParams = mt->templateParams;   // 内层形参 U 原样带过
    clonedMt->typeParams     = mt->typeParams;
    clonedMt->isMemberTemplate = true;
    clonedMt->funcTemplate   = cloneMethod(mt->funcTemplate, subst, instanceName);
    newClass->memberTemplates.push_back(clonedMt);
}
```

★ 关键在 `subst` 里**只有外层形参**（`{T → int}`）：`substituteType` 的 Case 1
对不在表中的名字走"原样保留"分支（src/template_instantiation.cpp 的注释早已写明
"如外层模板的形参"，这次是它第一次真正派上用场）。
实跑日志即证据：`[clone:method] 'convert' return type: T → int` 与
`[clone:method] param 'x' : U → U` **同一行输出里两种命运**。

对照 clang：`SemaTemplateInstantiateDecl.cpp` 的 `InstantiateDecl` 对 `TemplateDecl`
走 `TransformTemplateDecl` —— 同样是"带着类的实参重建一遍、内层形参保持未绑定"。

**三种错法各有各的症状**（本条的判据表）：

| 做法 | 症状 | 严重性 |
|---|---|---|
| 忘了搬（旧版） | `No member 'pick' in class 'Box_int'` | 错误**拒绝合法程序** |
| 搬了、但替换**就地改蓝图** | 第二个实例串到第一个的绑定（`Box<long>` 的 `convert` 也返回 `int`） | **静默算错** |
| 把内层 `U` 也替换掉 | 调用点无可推导 ⇒ 每个调用都推不出来 | 错误拒绝 |

**影响面**：小-中。原先拒收合法程序；修复后既有 **111 个集成用例逐字节零漂移**
（只有新增用例进基线）—— 反过来证明**此前没有任何既有用例覆盖"类模板 × 成员模板"
这个组合**：一个真缺口可以长时间藏在"特性各自都有测试"的缝里。

**修复记录**：`src/template_instantiation.cpp` instantiateClassTemplate 步骤 5.6
（+ 日志 `║ ── Member Template Substitution ──`，仅在该表非空时打印 ⇒ 既有用例零漂移）。

**回归用例**：
· 集成 `tests/tmpl/test_tmpl_69_member_template_in_class_template.cpp`
  （外层 T 兑现 / 两实例不串味 / 内层 U 两实例两符号 / 偏特化蓝图同样生效）
· 单测 `tests/unit/test_member_template_in_class_template.cpp`（`MemberTemplateInClassTemplate.*` 5 例）

**突变负向验证**（把步骤 5.6 的循环整体停掉后实跑）：

| 突变 | 红掉的用例 |
|---|---|
| `for (auto& mt : templateDecl->classTemplate->memberTemplates)` 换成空容器 | 本套件 **5/5 全红**（`InstanceCarriesMemberTemplates` / `OuterBoundInnerKept` / `InstancesDoNotShareBlueprintState` / `DistinctInnerArgsDistinctSymbols` / `EveryCallResolves`） |

**状态**：✅ 已修

---

## B19. static / virtual 的识别位置写反：标准写法被拒、非法写法被收

**复现**（两种顺序，判据正好与语言相反）

```cpp
struct O {
    template <class U> static U f(U x) { return x; }   // ① 标准写法（[temp.pre]：template-head 在最前）
    static template <class U> U g(U x) { return x; }   // ② 非法写法
};
```

| | 写法 ①（合法） | 写法 ②（非法） |
|---|---|---|
| minicc（修前） | `[Parse Error] … at 'static': Expected type name`（rc=1） | 解析放行（`isStatic` 已被吃掉），一路走到调用点 |
| clang++-18 `-std=c++20` | rc=0 | `error: expected member name or ';' after declaration specifiers` |

**性质**：同一处判据的**两个方向都反了** —— 拒收合法程序（①）+ 接受非法程序（②）。
① 与 [B9](#b9-struct-的默认继承级别被当成-private-处理)、[B17](#b17-值位实参要求形态精确相等-拒收合法程序)、
[B18](#b18-类模板里的成员模板在实例类里丢失) 同类；② 属"静默接受"那一类。

**根因**：`static` / `virtual` 的识别写在类体循环里**成员模板分支之前**
（src/parser.cpp 的 1641 起，注释还专门交代了"位置必须在成员模板之前"）——
于是它只能认到**说明符写在 `template` 之前**的顺序。而
[temp.pre] 规定 `template-head` 必须在声明最前，说明符只能写在**形参表之后**：
标准写法在 1666 的分支进门时 `static` 还没出现，吃完 `template <…>` 后代码
**不再前瞻**，直接 `parseMethodDecl`（`parseFunctionDecl` 的第一步是 `parseType()`）
⇒ 撞在 `static` 上 ⇒ `Expected type name`。

★ 原注释的理由（"`isStatic`/`isVirtual` 必须先于任何使用它们的成员分支声明"）
说的是**变量声明顺序**，但实现把它与**语法位置**绑在了一起 ——
"先声明"与"先出现"是两件事。对照 clang：`ParseTemplateDeclarationOrSpecialization`
先把整条声明解析完（decl-specifier-seq 天然含 static/virtual），再按结果定 Decl 种类
—— **判据来自声明本身，而不是位置**。

**修法**：在形参表解析完、`parseMethodDecl` 之前**再认一次**，且两种说明符**处置相反**：

```cpp
bool mtIsStatic = isStatic;                     // 前缀位置已吃过 ⇒ 兼容既有写法
if (match(TokenType::KwStatic)) mtIsStatic = true;
if (check(TokenType::KwVirtual) || isVirtual) {
    // 文案与 clang 逐字相同
    error("'virtual' cannot be specified on member function templates");
}
```

| 说明符 | 标准 | 本实现 | 依据 |
|---|---|---|---|
| `static` | 合法 | 收下（`method->isStatic = true`） | [class.static]/2：无隐式 this |
| `virtual` | **非法** | 当场报错 | [temp.mem]/2 末句：member function templates shall not be virtual |

★ virtual 必须拒的理由：虚函数要求"每个动态类型在 vtable 里占一条固定条目"，
而模板实例是**按需产生**的 —— 声明处根本不知道要有几条、更不知道 `U` 有哪些取值。
（本轮**没有**顺手把 `virtual` 也一起"支持"掉，是判据决定的，不是工作量决定的。）

**影响面**：小。① 原先拒收合法程序；② 原先静默接受非法程序。
修复后既有 **112 个集成用例逐字节零漂移**（含 B18 的用例）。

**修复记录**：`src/parser.cpp` 成员模板分支（形参表之后新增 static/virtual 识别）；
同批在该分支补了"**本项目只做成员函数模板**"的代码位置标注（成员类模板 /
成员别名模板 / 静态数据成员模板一律响亮拒收，文案与现状见 docs/learn/34 §3.5）。

**回归用例**：
· 集成 `tests/tmpl/test_tmpl_70_member_template_static.cpp`
  （普通类 + 类模板（B18 × B19 交叉点）+ 非 static 对照，三者都返回 0）
· 集成 `tests/tmpl/test_tmpl_71_error_virtual_member_template.cpp`
  （rc=1，文案与 clang 逐字相同，报错位置也对上：`27:5`）

**突变负向验证**（逐个拆掉后实跑）：

| 突变 | 红掉的用例 |
|---|---|
| 删掉 `if (match(KwStatic)) mtIsStatic = true;` | `test_tmpl_70` 退回 `[Parse Error] … at 'static'`（rc=1） |
| 删掉 virtual 的 error 分支 | `test_tmpl_71` 不再报错（rc≠1），文案断言失效 |
| 把 `mtIsStatic` 写回 `isStatic` | `test_tmpl_70` 的静态分支失效（同第一条） |

**状态**：✅ 已修

---

## B20. vtable 槽里的符号名与定义点不同源（带参虚函数链接失败 / 次基类假符号）

一句话：**vtable 槽里的名字是"拼"出来的，定义点的名字也是"拼"出来的，两处各拼一遍。**

### 缺陷 a：带参虚函数的槽少了 `_<形参个数>` 后缀

**复现**

```cpp
struct A { virtual int f(int x) { return x; } };
struct B : A { int f(int x) { return x + 1; } };
int main() { B b; A* pa = &b; return pa->f(3) - 4; }
```

| | 结果 |
|---|---|
| clang++-18 `-std=c++20` | rc=0 |
| minicc（修前） | 编译全绿，**链接期** `undefined reference to 'A_f'`（B 那侧 `B_f` 同理） |

汇编证据（修前）：

```text
.globl A_f_1        ← 定义点：带形参 ⇒ 追加 _1
_ZTV1A:
    .quad A_f       ← 槽位：硬拼"类名_方法名"，没有 _1 ⇒ 这个符号没人定义
```

**根因**：同一条命名规则写在两处 —— 定义点 `registerFunction`（Pass 2，写
`decl->mangledName`）与 vtable 条目点 `processClassDecl`（Pass 1，写
`entry.mangledName`，三个落点：主表覆写 / 次表覆写 / 新条目）。后者只会
`decl->name + "_" + methodNameInVTable`。**为什么当时能"跑"**：无参虚函数两侧
算出的都是 `A_f`，规则恰好重合；缺口只在带参虚函数上，而此前没有任何用例写过它。

### 缺陷 b：次基类里【未覆写】的槽被重造出一个假符号

**复现**

```cpp
struct A { virtual int f() { return 1; } };
struct B : A {};                       // 次基类：没覆写 f
struct C { virtual int g() { return 2; } };
struct D : C, B {};
int main() { D d; B* pb = &d; return pb->f() - 1; }
```

| | 结果 |
|---|---|
| clang++-18 `-std=c++20` | rc=0 |
| minicc（修前） | 编译全绿，链接期 `undefined reference to 'B_f'` |

收集次基类条目时名字被按 `次基类名 + "_" + 裸名` **重造**了一遍（原意："这条槽
属于 B，那就该叫 B_f"）。但 B 没覆写 f，槽里原本指向的是**更上游**的 A::f ——
重造等于凭空捏造一个没人定义的符号。**名字是路径（谁提供的实现），不是位置
（这是谁的表）** —— 承 [B13](#b13-两个次基类各有一个同名字段时显示名塌陷且第二条偏移算成-0)~[B15](#b15-祖辈前缀与直接基类撞名时偏移算到错的子对象) 与
[docs/learn/23](learn/23-cv-position-and-type-identity.md) 的同一句教训。

**现场**：这个缺陷一直藏在 `tests/mi/test_mi_04_error.cpp` 里。该文件当时写的是
菱形继承，头注释把 rc=1 解释成"菱形本该被拒收"—— 实测 clang rc=0：非虚继承的
菱形在标准下**合法**（两个 X 子对象，只有成员访问歧义才报错）。rc=1 的真原因就是
这句 `undefined reference to 'Q_f'`。**又一次印证：logdiff 基线会把失败固化成契约**。

**修法**（两处，方向相反）：

1. **判据单点**：抽 `memberMethodSymbolName(owner, name, paramCount, earlierSameNameCount)`
   （src/semantic_analyzer.cpp:1445），`registerFunction` 与 vtable 三个落点
   **共用同一个函数**；
2. **次表名字原样透传**：`VTableEntry secEntry = baseEntry;`，不再重造。
   真覆写由后面的方法循环负责（那时才改指本类实现并配 thunk）。

★ 计数口径有个坑：`earlierSameNameCount` 只数**排在当前方法之前**的同名方法
（Pass 2 注册时扫 `m_functions` 也只看得见前者）。数成"同类同名总数"会让
`struct X { virtual int f(); virtual int f(int); }` 里 f() 得到 `X_f_0`，而定义点是
`X_f` —— 又是一个只在链接期炸的错配。故 vtable 侧按 `decl->methods` 的**下标顺序**数。

**顺带发现（缺陷 c，未修）**：槽位匹配只比**裸名**、不比形参表 ⇒
`class C { virtual int f(); int f(int); };` 里 `f(int)` 会认领 `f()` 的槽，且被
**误标成 virtual**。实测 `C c; return c.f() + c.f(2) - 3;`：clang rc=0，
minicc 编译 rc=0 但**运行返回 255**（静默算错）。判据应是（裸名 + 形参个数），
与符号名规则同源。**本轮不修**：它改的是"覆写判据"本身，影响面比命名大，单独一轮。

### 影响面与验证

| 项 | 结果 |
|---|---|
| 既有集成用例漂移 | **恰好 1 个**：`tests/mi/test_mi_04_error.cpp`（缺陷 b 的现场，rc 1→0，已改写为私有继承错误用例）；其余 115 个逐字节不变 |
| 单测 | 262 → 266（新增 `VTableSymbols.*` 4 例） |
| 集成 | 114 → 116（新增 `tests/mi/test_mi_12_secondary_inherited_slot.cpp`、`tests/lang/test_basics_03_virtual_with_params.cpp`） |
| 有意日志漂移 | 无（命名结果对无参虚函数逐字未变） |

**回归用例**：

- 单测 `tests/unit/test_vtable_symbols.cpp`（`VTableSymbols.*` 4 例，断言**不变量**：
  每条 `.quad <sym>`（`.L*`/纯数字除外）都必须有 `.globl <sym>` 定义 ——
  命名规则随便改都不会误报，但"两处各算一遍"必红）
- 集成 `tests/mi/test_mi_12_secondary_inherited_slot.cpp`（菱形次表透传 + 次基类自身覆写走 thunk）
- 集成 `tests/lang/test_basics_03_virtual_with_params.cpp`（带参虚函数覆写 / 未覆写 / 基类自身三条路）

**突变负向验证**（逐个拆掉后实跑）：

| 突变 | 红掉的用例 |
|---|---|
| 三个 vtable 落点改回 `decl->name + "_" + methodNameInVTable` | `VTableSymbols.ParameterizedVirtualSlotMatchesDefinition`、`...SecondaryInheritedSlotKeepsUpstreamSymbol` |
| 次表条目改回 `baseName + "_" + 裸名` | 只有 `...SecondaryInheritedSlotKeepsUpstreamSymbol` 红（外科级定位） |

**状态**：✅ 缺陷 a、b 已修；⚠ 缺陷 c 未修（见上）

---

## B21. 限定名 `S::v` 访问成员数据被拒收

**复现**（同一个类里两种限定名，一个通一个不通）

```cpp
struct S {
public:
    int v;
    int f() { return 5; }
    int g() { return S::v; }     // ① 限定名的【数据】成员
    int h() { return S::f(); }   // ② 限定名的【函数】成员
};
```

| | ① `S::v` | ② `S::f()` |
|---|---|---|
| minicc | `[ERROR] [Semantic Error] 5:22: Undefined variable 'S::v'`（rc=1） | rc=0 |
| clang++-18 `-std=c++20` | rc=0（`-Xclang -ast-dump` 见下） | rc=0 |

**性质**：**拒收合法程序**。与 [B9](#b9-struct-的默认继承级别被当成-private-处理)、
[B12](#b12-继承来的成员方法查不到)、[B17](#b17-值位实参要求形态精确相等-拒收合法程序)、
[B18](#b18-类模板里的成员模板在实例类里丢失) 同类。（② 能过是**侥幸**：inferCall 有一条
限定名分支认 `类名_方法名` 的 mangled 符号，与"成员访问"无关。）

**clang 的落点（oracle）**：`S::v` 在成员函数体内是**隐式 this 的成员访问** ——
[expr.prim.id.general]/3，AST 里就是一个正牌 MemberExpr：

```text
`-ReturnStmt
    `-MemberExpr  'int' lvalue ->v        ← 注意是 ->v，base 是隐式 this
        `-CXXThisExpr  'S *' implicit this
```

**根因**：`A::B` 在 Parser 就被**拼成一整个字符串**（parser.cpp:2679 的
`while (::) name += "::" + ident`），此后全项目只按"名字串"查表。而三条限定名通路
覆盖的分别是 **函数**（inferCall 的限定名分支）、**类内类型别名**（符号表 `Cls::alias`）、
**嵌套类型名**（resolveType 的 nested-name 分支）—— **数据成员一条都没有**：
inferVar 只认裸字段名（外加"当前类"的类作用域回退），查 `"S::v"` 这个字符串必然落空。

★ 有意思的是**方向正好错开**：本项目唯一从 `NodeKind::Member` 分支过的限定名是
`Cls<int>::value`（Parser 造 `isTypeAccess=true` 的 MemberExpr，供 `foldStaticConst`
折叠静态常量）—— 而它在 clang 里是 **DeclRefExpr**，不是成员表达式。也就是说
"clang 里是 MemberExpr 的形态我们收不到，我们造 MemberExpr 的形态 clang 不是"。

**要修的话**：inferVar 里先把 `A::B` 按 `::` 切开，若限定者是**当前类**（或落在其基类链上）
则退化为"字段查找 + 隐式 this"，与 `b`（裸名走 m_currentClassName 回退）汇成同一条路；
`self()` 那条 `this->b` 已经能跑，收口点是现成的。
判据要按 [expr.prim.id.general]/3 写：**限定者是不是一个类型** 决定这是不是隐式成员访问
（命名空间限定名仍走符号表）。

**影响面**：小（写法少见，且只是**多写**了一个限定者；去掉 `S::` 就恢复正常）。
但属"拒收合法程序"，与 B12 同族 —— 记在这里是因为**推导分支表的边界**值得留痕。

**回归用例**：暂**不加**钉住现状的用例 —— 那会把失败固化成契约
（正是 logdiff 教训：基线会把 rc=1 也固化成"契约"）。修完再补正例。

**状态**：⬜ 未修

---

## B22. 同名同个数的成员重载：Sema 静默取第一个、汇编期撞符号

**复现**（一个类里两个同名、同个数、不同类型的成员方法）

```cpp
struct S { public: int v; };
struct C { public: int f(int x) { return 1; } int f(S s) { return 2; } };
int main() { C c; S s; s.v = 0; return c.f(1) + c.f(s) - 3; }   // clang: rc=0
```

| 情形 | 源码 | minicc | clang++-18 `-std=c++20` |
|---|---|---|---|
| 同名、**个数不同** | `f(int)` + `f(int,int)` | ✅ rc=0 | ✅ rc=0 |
| 同名、**个数相同**、类型不同 | `f(int)` + `f(S)` | ❌ ``minicc_o4.s:43: Error: symbol `C_f_1' is already defined`` ⇒ `[ERROR] 汇编失败 (as 退出码 256)` | ✅ rc=0 |
| 同上，声明顺序对调 | `f(S)` + `f(int)` | ❌ 同上（与顺序无关） | ✅ rc=0 |

**性质**：**拒收合法程序**，且**报错点离根因很远** —— as 只甩一个符号名，不说
"这个类里有两个同名同个数的 `f`"。

**根因**：**"名字 + 参数个数"被当成了身份**，两处各用一次（正是 B10 那类"同一判据写在两条通路上"）。

| # | 位置 | 判据 | 后果 |
|---|---|---|---|
| ① | `findMethodInClass`（`src/semantic_analyzer.cpp:4558`） | `name` 相同 ∧ `parameters.size()` 相同 ⇒ `return method`（**在循环内**） | 同分者**声明序第一个赢**，全程不比较类型 |
| ② | `memberMethodSymbolName`（`src/semantic_analyzer.cpp:1445`） | `sym = owner_name`，后缀**只编码 paramCount**（`earlierSameNameCount` 只决定"要不要加后缀"，不进后缀内容） | 同名同个数 ⇒ **生成同一个符号** `C_f_1` |

调用点（inferCall:3439）传的就是 `argTypes.size()`。于是 ② 保证"两个都定义必然撞名"，
① 保证"若只有一处定义则会静默选错" —— **前者让后者至今没机会暴露**。

**标准视角**：`[class.member.lookup]` 只产出**候选声明集**（不筛类型）；筛类型是
`[overload.resolve]` 的事：候选集 → **可行集**（[overload.best.viable]：个数 + 隐式转换序列）
→ 最优（[overload.icp] 排序）。本项目四条通路的判据粗细度：

| 通路 | 判据 | 粗细 |
|---|---|---|
| 函数模板 | 推导 + 偏序（[temp.func.order]） | 最细 |
| 自由函数（含 ADL） | 候选合并 + 精确匹配裁决 | 中 |
| **成员方法** | **名字 + 个数** | 粗（本条） |
| vtable 槽认领 | **只有裸名** | 最粗（[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) 缺陷 c，**已是静默算错**） |

**要修的话**（两件事缺一不可）：① 候选集改成"同名全部"，按参数类型做可行/最优；
② 符号名带上**参数签名**（clang：`_ZN1C1fEi` / `_ZN1C1fE1S`）—— 不加的话，同名同个数的
重载永远在汇编期撞名。

**影响面**：中（合法程序不可用；症状落在汇编期）。**回归用例**：暂不加（同
[B21](#b21-限定名-sv-访问成员数据被拒收)，不把失败固化成契约）。

**状态**：⬜ 未修

---

## B23. 浮点：字面量缺失 + 运算按整数发射

**复现**

```cpp
int main() { double d = 1.5; return 0; }                            // 缺陷 a
int main() { double a = 1; double b = 2; double c = a / b;
             if (c > 0) { return 1; } return 0; }                   // 缺陷 b
```

| | minicc | clang++-18 `-std=c++20` |
|---|---|---|
| a. `double d = 1.5;` | `[ERROR] [Parse Error] 1:27 at '5': Expected member name: expected Identifier, got '5'`（rc=1） | rc=0 |
| a'. `double d = 15e2;` | `[ERROR] [Parse Error] 1:27 at 'e2': Expected ';' after variable declaration`（rc=1） | rc=0 |
| b. `a`=1、`b`=2（都 `double`），判 `a / b > 0` | **rc=0**（`1/2` 按整数算 ⇒ 0，判假） | rc=**1**（0.5 > 0） |

**性质**：**两个不同层的缺陷**。a 是**未实现的词法形态**（响亮失败，但报错文案指向
`.` **后面那个数字**，完全看不出"本实现没有浮点字面量"）；b 是**静默算错**
（编译通过、运行给出错的结果 —— 与 [B14](#b14-派生类同名字段的隐藏方向做反了) /
[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) 缺陷 c 同类）。

**根因**：

- **a**：Lexer 只有整数那条路（[lex.icon]：进制前缀 / 后缀 / 数字分隔符，见 docs/learn/35），
  [lex.fcon] 的小数点 / 指数 / 后缀一概没有 ⇒ `1.5` 被切成 `1` `.` `5`，
  而 `.` 在 `parsePostfixExpr` 里是**成员访问** ⇒ `Expected member name`。
- **b**：`src/codegen.cpp` 里搜不到任何 SSE 指令（`movsd`/`addsd`/`xmm` 计数为 **0**），
  也搜不到 `isDouble` —— `double` 在 CodeGen 眼里就是一个**8 字节标量**，
  `a / b` 照发 `idivq`（实测 `.s` 第 27 行）⇒ `1/2 = 0`。

**边界（实测）**：`float` 类型根本没有（`unknown type name 'float'`）；但 `double`
**类型**是通的 —— `double g(double x) { return x + 1; }` + `double d = 1;`（int → double
走 [conv.fpint]）编译运行 rc=0。**缺的是字面量与运算**，不是类型。

**要修的话**：a 在 Lexer 认小数点/指数/`f` 后缀，Parser 给出 `double`（要不要连 `float`
一起补需先定 CodeGen 的浮点宽度分派）；b 要引入 SSE 寄存器与指令（`movsd`/`addsd`/`divsd`、
调用约定里 xmm0 传参）—— 是**一条独立主线**，不是顺手项。

**状态**：⬜ 未修（a、b 都未修）

---

## 小结 字符串兼任 ID 与路径

三条 bug 的最小复现各不相同，根因却是同一句话：**把"显示名"当成了"索引"。**

| 层 | 它该用来做什么 | 用错之后的后果 |
|---|---|---|
| 显示名 `"B.x"` | 打印给人看 | 看不出前缀是第几代留下的 ⇒ **B15** |
| 查找键 | 必须是**声明名**（`declaredName`） | 反拼前缀去配对 ⇒ **B14** |
| 子对象定位 | 必须是**结构信息**（`viaBase` + 下标） | 靠裸名在基类布局里找第一个命中 ⇒ **B13 / B15** |

这与踩坑史 **T1**（`Type::toString()` 不是单射、而实例缓存键吃它 —— 见
[docs/learn/23](learn/23-cv-qualifier-position.md)）是**同一个坑的另一次现形**；
[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) 是**第三次**
（vtable 槽里的函数名 = 机器用的键，而定义点与槽位**各拼一遍**）。
可以概括成一条项目级教训：

> **凡是拿"给人看的字符串"当"机器用的键"，早晚出事。**

clang 那边这两种东西从设计上就是分开的：成员是 `FieldDecl*`，
布局按 `getFieldIndex()` 寻址，名字只交给 `LookupResult` 做查找、
且查找结果自带"来自哪个子对象"（歧义因此天然可检出）。

**顺带记录的两条方法论**：

1. **补第三态**（B11）：`if/else` 写"优化决策"时，两态往往是从旧代码继承来的，
   新情形出现时最容易漏 —— 先枚举出**全部**情形再写分支。
2. **判据只许有一处**（B12，承 B10）：同一句"成员叫什么/是不是它"写在两条通路上，
   就会各自演化。本轮的收口是 `findMethodInClass` / `findMethodInHierarchy` 两个原语。
