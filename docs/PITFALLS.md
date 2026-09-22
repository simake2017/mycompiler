# 踩坑史（PITFALLS）

> 本文件收纳**已修复 bug 的现场记录**。它们原本写在代码注释里，但复盘经过对
> 读代码的人是无用的噪音。规则：
>
> - **代码里只留"当前必须遵守的约束"**（一句话，如「必须按字段宽度分派」）；
> - **复盘经过全部搬到这里**（症状 / 根因 / 修法 / 复现用例）。
>
> 每条都对应一个**回归用例**——踩过的坑必须有用例钉住，否则会再踩一次。

## 索引

| 编号 | 模块 | 一句话 |
|---|---|---|
| [C1](#c1-帧大小漏算序言区) | codegen | 帧大小漏算序言区 ⇒ 最深的局部被 `pushq`/返回地址踩掉 |
| [C2](#c2-嵌套类字段初始化走了标量路径) | codegen | 嵌套类字段初始化走了标量路径 ⇒ `f(7,9)` 把 7 当指针存 |
| [C3](#c3-标量字段初始化一律-movq) | codegen | 标量字段初始化一律 `movq` ⇒ 4B 字段踩坏相邻字段 |
| [C4](#c4-字段写入硬编码-movl) | codegen | 字段**写入**硬编码 `movl` ⇒ 8B 字段高 32 位被截断 |
| [C5](#c5-嵌套类字段取值走了-movq) | codegen | 嵌套类字段取值走了 `movq` ⇒ 段错误 |
| [P1](#p1-模板形参注册晚于默认实参) | parser | 形参注册晚于默认实参 ⇒ `U = T` 里的 T 不认识 |
| [P2](#p2-帧出栈靠手工配对异常路径泄漏) | parser | 帧出栈靠手工配对 ⇒ 抛异常时作用域泄漏 |
| [P3](#p3-语句层前瞻不跳-) | parser | 语句层前瞻不跳 `&` ⇒ `S& r = a;` 不可解析 |
| [P4](#p4-缺直接初始化文法ctad-无从触发) | parser | 缺 `Type name(args);` 文法 ⇒ CTAD 无从触发 |
| [P5](#p5-const-在类型说明符侧的应用位置) | parser | const 应用位置错 ⇒ 偏特化**静默选错** |
| [P6](#p6-nttp-名在模板实参位置被误判) | parser | NTTP 名在实参位被误判成类型名 |
| [T1](#t1-typetostring-不是单射) | type | `toString()` 不是单射 ⇒ 缓存键与实例名**串味** |
| [T2](#t2-模板实参不能一律存成-typeptr) | type | 实参一律存 `TypePtr` ⇒ NTTP 的值无处安放 |
| [T3](#t3-引用折叠必须在构造点完成) | type | 折叠只写在一条路径 ⇒ 同一函数实例化出两个符号 |
| [I1](#i1-类模板-id-的实参不参与替换) | 实例化 | 只按名字查替换表 ⇒ 造出**假实例** `MyPtr_T` |
| [I2](#i2-删除语句在克隆时被静默丢弃) | 实例化 | `cloneStmt` 漏 case ⇒ `delete p;` 凭空消失 |
| [I3](#i3-实例名清洗不是单射) | 实例化 | `Box<int*>` 与 `Box<int&>` 撞汇编符号 |
| [I4](#i4-蓝图摘要读错了形参表) | 实例化 | 读 `typeParams` 渲染 ⇒ NTTP 被打成 `typename N` |
| [F1](#f1-istypekeyword-缺-kwconst) | 前端 | `isTypeKeyword()` 缺 `KwConst` ⇒ 语句层 `const` 声明不可解析 |
| [F2](#f2-演示代码固定传-double-去填-nttp-槽) | 前端 | 按类型传实参 ⇒ NTTP 槽被塞进一个类型（**且只看一位**） |
| [S1](#s1-resolvetype-查不到就静默放行) | sema | 符号表查不到就放行 ⇒ `Undeclared q;` 静默通过 |
| [S2](#s2-类内别名的登记时机) | sema | 别名登记太晚 ⇒ 类体内 `Int a;` 报错 |
| [S3](#s3-继承布局边扫边放) | sema | 边扫边放 offset ⇒ 非多态基类与 `_vptr` 重叠 |
| [S4](#s4-place-被-anypoly-守卫) | sema | 全非多态时 `place` 停在 0 ⇒ 两个基类字段互踩 |
| [S5](#s5-两处选了不同的-primary-基类) | sema | 两处各自判 primary ⇒ 布局自相矛盾 |
| [S6](#s6-classtype-的挂载时机) | sema | 布局算在孤儿类型上 ⇒ 按 0 字节 malloc |
| [S7](#s7-嵌套实例化未保存上下文) | sema | 实例化嵌套调回 ⇒ 外层 `return` 按 void 检查 |
| [S8](#s8-成员-callee-漏了补推导) | sema | 提前 return ⇒ 虚调用退化成静态调用 |
| [S9](#s9-以裸名为键的方法表) | sema | `Box_int::get` 与 `Box_double::get` 互相覆盖 |
| [S10](#s10-候选池混进了模板实例) | sema | 模板实例抢走精确匹配 ⇒ 链接失败 |
| [S11](#s11-pass-23-漏掉命名空间) | sema | 扁平分派漏命名空间（**且有一个镜像 bug**） |
| [S12](#s12-实参个数校验读错形参表) | sema | 读 `typeParams` 数形参 ⇒ NTTP 被当成类型 |
| [S13](#s13-放宽数据结构时忘了拆守卫) | sema | 放宽 `specPattern` 却留着旧守卫 ⇒ 值位偏特化被整条掐死 |

---

## codegen

### C1. 帧大小漏算序言区

**症状**

极具迷惑性：变量**单独**读出来是对的，一旦参与「需要压栈暂存」的二元表达式、
或此前发生过一次函数调用，读到的就是邻居的值。

```cpp
int a = 1; int h = 8;
return a + h;      // 期望 9，实际返回 2
```

**根因**

局部偏移从 `-8` 起步（`emitFunction` 里 `paramOffset = -8`），每个 spill 的形参
再各占 8 字节，局部变量接着往下排；而 `estimateBlockSize` **只数了局部变量的尺寸**。

⇒ 帧比实际用到的最深偏移**浅了 8~56 字节**，最深那几个槽位落在 `rsp` 之下，
被两样东西踩掉：

- 表达式求值的 `pushq %rax`（左操作数暂存）—— 正好落在 `rsp-8`；
- `callq` 压入的返回地址 —— 同样在 `rsp-8`。

**修法**

`estimateFrameSize` = 局部变量总尺寸 **+** 序言区（帧基 + 形参 spill 槽）。

**对照**：真实编译器的帧大小是序言与局部布局**同一份**分配器的产物
（LLVM `PrologEpilogInserter` / `X86FrameLowering::determineFrameLayout`），
不存在「两处各算一份、彼此对不上」的可能。

**回归用例**：`tests/unit/test_codegen_frame.cpp`（`CodegenFrame.*`）——
断言不变量「帧 N ≥ 最深 `-X(%rbp)`」，而不是断言某个具体数字。

---

### C2. 嵌套类字段初始化走了标量路径

**症状**

```cpp
struct Five { int a, b, c, d, e; Five(int, int); };
struct Wrap { Five f; int tag; };
Wrap w(7, 9);      // 应该是"在 w.f 子对象上调用 Five::Five(7,9)"
                   // 实际变成"把 7 当指针，存进 w.f 的前 8 字节"
```

**根因**

字段初始化没区分「类类型字段」与「标量字段」——前者要在**子对象上调用构造函数**，
后者才是存值。

**修法**

类类型字段 ⇒ 实参进寄存器，`this` = 原始 `this` + 字段偏移，直接 `callq`
子对象的构造函数（与基类构造同构）。

**对照**：clang 对初始化列表里的 `f(7,9)` 生成对 `Five::Five(int,int)` 的直接调用
（`this` 已调整）。

---

### C3. 标量字段初始化一律 movq

**症状**

4 字节 `int` 字段被写 8 字节 ⇒ 踩坏相邻字段。
典型：`tag@0` 写 8B，把 `f` 的 `b1..b4` 一并覆盖。

**根因**

初始化路径硬编码 `movq`（8B），与字段实际宽度无关。

**修法**

按 `field->size` 选 `movb`(1) / `movl`(4) / `movq`(8)。

**对照**：clang 按 `TI.Width` 选存储指令。

**与 [C4](#c4-字段写入硬编码-movl) 的关系**：C3 是「初始化列表」路径，C4 是
「赋值语句」路径 —— **两条独立的写路径，各自硬编码了不同的宽度，先后各踩一次**。
凡「写入」必查宽度，两条路径都要查。

---

### C4. 字段写入硬编码 movl

**症状**

给 8 字节字段（指针 / 引用 / `long`）赋值时高 32 位被直接截掉。表现为
**「写得进、读出来错」**：读路径按 8B 用 `movq`，于是读回
「低 32 位正确 + 高 32 位垃圾」。

```cpp
PtrBox<int> q;
q.p = pv;          // 写完 q.p != pv
```

**根因**

**读路径**按宽度分派，**写路径**硬编码 `movl` —— 两者不对称。

**修法**

写入也按 `field->size` 分派，与读路径对称。

**为什么长期没暴露**：此前从没有用例给 8 字节字段赋过值。

**排查干扰**：`tests/tmpl/test_tmpl_53` 的 ② 一度被误判成「依赖默认模板实参」的锅
—— 那个改动是好的，这个 bug 只是恰好同时暴露。

**★ 同族第三处（发现于 2026-09-21，尚未修）**

字段写入在本项目共有**三条独立路径**，改了一条不等于改完：

| # | 路径 | 例子 | 宽度分派 |
|---|---|---|---|
| 1 | 成员表达式赋值 | `q.p = pv;` | ✅ 已修（本节） |
| 2 | 构造函数初始化列表 | `MyPtr(T* x) : ptr(x) {}` | ✅ 正确 |
| 3 | **方法体内裸字段名** | `ptr = x;` | ❌ **硬编码 `movl`** |

路径 3 落在 `emitAssign` 的 `NodeKind::Var` 分支。它只在**方法体/构造函数体内部**
出现，而此前从没有用例在类方法里给 8 字节字段赋过值 ⇒ 长期潜伏。

**发现经过**：写 `demos/tmpl/07_alias_and_ctad.cpp` 时，构造函数里的 `ptr = x;`
让 8 字节指针只剩低 32 位，`m.ptr != &v`。（该 demo 已改用初始化列表绕过。）

**教训**：修「按宽度分派」这类问题时，先把**所有写路径列全**再动手 ——
否则下一条路径迟早以「看起来毫不相干」的症状冒出来。

---

### C5. 嵌套类字段取值走了 movq

**症状**

```cpp
w->f.a      // 段错误
w->tag      // 正常
```

**根因**

类类型字段**不是"值"**，而是父对象内的**子对象**：`o->f.a` 的 `f` 这一步应产出
地址（`leaq`），供下一层 `.a` 再加偏移。曾按 8B 走 `movq`，把 `f` 的头 8 字节
（成员 `a`、`b` 的内容）当指针解引用。

**修法**

`field->type->isClass()` ⇒ `leaq offset(%rax), %rax` 取地址，而不是取值。

**对照**：clang 把链式访问折叠为常量总偏移（GEP 求和）；minicc 教学版保留
逐层 `leaq`，便于观察每一跳。

---

## parser

### P1. 模板形参注册晚于默认实参

**症状**

`template <class T, class U = T>` 报 `unknown type name 'T'`，连编译都过不去；
而报错点在第一位的 `T` 上，看不出与「注册时机」有关。

**根因**

所有形参在**整个形参表循环结束之后**才统一注册。解析 `U` 的默认实参 `T` 时，
连**前一轮**的 `T` 都还没进作用域。

**修法**

注册动作移进循环，**紧跟该位默认实参的解析之后**（[basic.scope.pdecl]/9：
形参名的作用域从其声明符之后才开始）。

**对照**：clang `ParseTemplate.cpp:654` 的注释原文 ——
"Per C++0x [basic.scope.pdecl]p9, we parse the default argument before we
introduce the type parameter into the local scope"；代码上 `ParseTypeName`
（:671）在前、`Actions.ActOnTypeParameter`（:676）在后。

**回归用例**：`tests/tmpl/test_tmpl_53`（正向）+ `test_tmpl_54`（负向自引用）；
理论见 `docs/learn/30`。

---

### P2. 帧出栈靠手工配对，异常路径泄漏

**症状**

模板体内发生语法错误后，**形参作用域泄漏到后续解析** —— 后面那些本该
报 `unknown type name` 的名字，被残留的形参遮蔽了。

**根因**

出栈靠 `m_templateParamScope.resize(scopeBase)` 手工配对。中途一旦
`error()/errorAt()`（`[[noreturn]]`）抛异常，**恢复动作根本不会执行**。

**修法**

改为 `TemplateParamFrame` RAII：构造即入栈、析构即出栈，异常路径由栈展开
自动保证恢复。

**对照**：clang 用 `MultiParseScope`，同样是「构造 Enter、析构 Exit」。

---

### P3. 语句层前瞻不跳 `&`

**症状**

```cpp
S& r = a;              // 报 Expected ';' after expression
Box<int>& r = b;       // 同样
int& r = a;            // ★ 却一直正常
```

**根因**

语句层前瞻只跳 `*`，不跳 `&`/`&&`。游标停在 `&` 上，发现下一个 Token 不是
Identifier ⇒ 误判为表达式语句。

至于 `int& r = a;` 为什么没事：它走的是**内建类型关键字**分支，根本不经过
那条前瞻 —— 洞只在「标识符开头的类型」上暴露。

**修法**

前瞻循环同时跳 `Star` / `Ampersand` / `AmpAmp`。

**回归用例**：`docs/learn/22` 的 ③（名字在哪一层当真）。

---

### P4. 缺直接初始化文法，CTAD 无从触发

**症状**

`MyPtr m(7);` 报 `Expected ';' after variable declaration` —— CTAD 根本没机会发生。

**根因**

`parseVarDeclStmt` 只认 `Type name;` 与 `Type name = expr;` 两种形态，
缺 `Type name(args);`。

**修法**

补 `'(' ctor-args ')'` 分支收集 `ctorArgs`（[dcl.init]/16）。
★ CTAD **只在直接初始化触发** —— 所以这条文法不是锦上添花，是前置条件。

**回归用例**：`tests/tmpl/test_tmpl_51_ctad_and_guides.cpp`；理论见 `docs/learn/28`。

---

### P5. const 在类型说明符侧的应用位置

**症状**

偏特化**静默选错**：

```cpp
template <class T> struct C<const T&> { ... };   // 永不匹配
C<const int&> x;                                  // 悄悄落到主模板
```

**根因**

类型说明符侧的 `const` 原先在后缀循环**之后**才应用 ⇒ `const int*` 被建成
`Const(Pointer(Int))`。而标准要求 `const int*` 是 `Pointer(Const(Int))` ——
顶层不是 Pointer/引用，偏特化的结构匹配自然对不上。

**修法**

`const` 在 `parseType` 的 Step 2.5（后缀循环**之前**）应用。

**回归用例**：`tests/tmpl/test_tmpl_47_cv_position.cpp`；
理论见 `docs/learn/23`（含 "`Type::toString()` 不是单射导致实例缓存串味" 一节）。

---

### P6. NTTP 名在模板实参位置被误判

**症状**

`template <int N> using BufA = Buf<N>;` 报错（rc 0→1）。

**根因**

一度改成「值形参名一律报错」，但 `parseType` 是**复用**的 —— 它分不清自己
是在类型位置还是**模板实参位置**（后者由 `parseTemplateArgumentList` 拿裸名
试探 `parseType`）。而实参位置上的裸 `N` 是**合法的值实参**。

**修法**

值名沿用旧路径建 `Class(name)`，由替换阶段按「替换表里绑的是值」还原成值实参。

**对照**：clang 把这个区分放在 `ParseTemplateArgument`（实参位），
minicc 没有那层分派，故只能保守处理。

**代价（已知边界）**：`template<int N> struct A { N x; };` 仍报
`unknown type name 'N'`，而非 clang 的 `err_not_type`。

**回归用例**：`tests/tmpl/test_tmpl_50_alias_templates.cpp`。

---

## type

### T1. `Type::toString()` 不是单射

**症状**

`C<const int*>` 与 `C<int* const>` 共用同一条实例缓存，两个实例互相顶掉；
更隐蔽的是实例名清洗后也同名 ⇒ 汇编符号撞车。

**根因**

`toString()` 一律输出 `"const " + inner->toString()` ⇒ **不是单射**：

```text
Const(Pointer(Int))  ┐
                     ├─ 都印成 "const int*"
Pointer(Const(Int))  ┘
```

而**实例缓存键与实例名都吃 `toString()`** —— 于是两个不同的类型被判成同一个。

**修法**

按 `innerType` 的具体形态分派打印（`int* const` / `const int*` 各有其形）。

**当前必须遵守的约束**（代码里只留这一行）：
**`toString()` 不是单射，缓存键与实例名都吃它** —— 任何新的打印分支都不能破坏可区分性。

**回归用例**：`tests/tmpl/test_tmpl_47_cv_position.cpp`；理论见 `docs/learn/23`。

---

### T2. 模板实参不能一律存成 TypePtr

**症状**

`template <int N>` 的实参 `4` **无处安放** —— 值不是类型，而实参容器只装类型。

**根因**

实参一律 `std::vector<TypePtr>`，结构上表达不了"这是个值"。

**修法**

换 `TemplateArg` tagged 联合（`Type` / `Integral` 两态）+ `ofValue()` 工厂；
替换也随之分两层：`substituteType` 管类型位置、`cloneExpr` 管表达式位置。

**对照**：clang 的 `TemplateArgument` 本就是 tagged union。

**回归用例**：`tests/tmpl/test_tmpl_21..26`；理论见 `docs/learn/18`。

---

### T3. 引用折叠必须在构造点完成

**症状**（同一根因的三种表现）

1. 同一个函数被实例化出**两个符号**：`id(a)` → `T := int&` → `_Z2idIRiE`，
   而 `id(rr)` → `T := int& &` → `_Z2idIRRiE`（clang 只产出前者）。
2. `Ref<int&&>` 的成员 `T& r` 被判成 `int&&`（clang 给的是 `int&`）。
3. 系统里出现非法的 `int& &` **嵌套引用节点**。

**根因**

引用折叠只写在 `substituteType` **一条路径**上，其余各处各自为政：

- `substituteType` 的 Case 3 一度直接 `return newInner` —— 内层是右值引用时把
  `&&` 原样返回了；而**注释却写着"& 赢"**，读注释的人会以为这里已经对了。
  （★ 这是"注释与代码相反"这一类问题的又一例，与 `src/type.cpp` 的
  `stripReferences` 同类。）
- 推导器那边：本实现的 A 取自变量的**声明类型**，`int& rr` 交进来时 A 就是 `int&`，
  再套一层即得嵌套引用。（clang 里 [expr.type]/1 保证表达式无引用类型，
  A 到这一步已是裸类型 —— 所以这个现象是 minicc 特有的。）

**修法**

折叠**下沉到 `Type::makeLValueReference` / `makeRValueReference` 构造点**，
让「不存在嵌套引用节点」成为一条**全局不变量** —— 不管谁造的引用类型，
折叠都已经发生过了；**调用点不得自行判断**。

**当前约束**（代码里留的那行）：`★ 折叠只在 Type::makeLValueReference 里实现一份，
调用点不得自行判断。`

**回归用例**：`tests/tmpl/test_tmpl_52_reference_collapsing.cpp`；`docs/learn/12`。

---

## 实例化（instantiation）

### I1. 类模板 id 的实参不参与替换

**症状**

```cpp
template <class T> struct W { Box<T> b; };     // 内层 Box<T> 的实参永远不替换
template <class T> using Vec = MyPtr<T>;       // 别名展开后仍是 MyPtr<T>
```

进而**按形参 T 去实例化**，造出**假实例** `MyPtr_T`（名字里带形参），
方法返回类型跟着一起错。全程一声不吭。

**根因**

`substituteType` 的 Case 1/6 只按【名字】查替换表，而类模板 id 的依赖
藏在【实参】里 —— 名字 `Box` 不在表里，于是整棵树被原样放过。

**修法**

新增 Case 5.2 重建 `templateArgs`。★ 顺序有讲究：它必须排在 Case 5.8
（别名解糖）**之前** —— 别名实参先变成具体类型，展开出的 `MyPtr<int>` 才具体。

**回归用例**：`tests/tmpl/test_tmpl_50_alias_templates.cpp`；`docs/learn/27`。

---

### I2. 删除语句在克隆时被静默丢弃

**症状**

模板体内的 `delete p;` 实例化后**凭空消失**：不报错、不警告，只是内存泄漏。

**根因**

`cloneStmt` 的 `DeleteStmt` / `cloneExpr` 的 `DeleteExpr` 两个分支缺失，
语句落到 `default: return nullptr` —— 被**静默丢弃**。

★ 这是本项目最危险的一类 bug 形态：**漏一个 case = 静默丢一段程序**。
`clone*` 系列必须与 AST 节点种类对账（覆盖率清单见 `docs/learn/10`）。

**修法**

补上这两个 case。代码里保留一行"本分支必须存在"。

**回归用例**：模板析构体里写 `delete data;`，查生成的汇编是否发 `callq free`。

---

### I3. 实例名清洗不是单射

**症状**

`Box<int*>` 与 `Box<int&>` 的实例名撞车 ⇒ 汇编期 `as` 报
`'Box_int__dtor' is already defined` —— 报的是符号重复，**看不出根因**。
而 clang 视二者为两个不同类型，完全合法。

**根因**

早期清洗把 `*` 和 `&` 都归一成 `'_'` —— **不是单射**。
（与 [T1](#t1-typetostring-不是单射) 是同一类错误：拿"给人看的串"当"身份"。）

**修法**

每字符各配一个字母：`*`→`P`、`&`→`R`、`-`→`N`（`+`→`A` 预留）；
另加一道**撞名守卫**兜底仍非单射的 `, < >` → `_` ——
宁可响亮报错，也不产出重复符号。

**回归用例**：`tests/tmpl/test_tmpl_46_ptr_vs_ref_instance.cpp`。

---

### I4. 蓝图摘要读错了形参表

**症状**

`template <int N>` 的形参在日志里被打成 `typename N` —— 蓝图摘要对 NTTP 是错的。
（只影响日志可读性，不影响功能。）

**根因**

`TemplateDecl` 并存**两张形参表**：`typeParams`（裸 `vector<string>` 名字表）
与 `templateParams`（带 kind 的结构化表）。旧实现读前者渲染，分不出哪一位是 NTTP。

**修法**

一律用 `templateParams`：`kind == Type` → `typename X`，否则打
`nonType->toString() + " " + name`。

**修法落地**

修 `src/parser.cpp` 的 blueprint summary 循环（`decl->typeParams.size()` →
`decl->templateParams.size()`，按 `kind` 分支渲染）。同一份判断在
`template_instantiation.cpp` 的 `║ Blueprint:` 行里已经是对的——**又是同一个
"两处写同一条语义判断、只改了一处"的形状**（对比 `emitFunction` 里
`hasThis` 被复制成两份、加 static 特例时只改了一份那次）。

**回归用例**：`tests/tmpl/test_tmpl_21..26` 的蓝图行 —— 现在打的是
`template <int N> class Buf { ... }`（此前误打成 `typename N`）。

---

## 前端（词法 / 预处理 / 驱动）

### F1. `isTypeKeyword()` 缺 `KwConst`

**症状**

```cpp
int main() {
    const int x = 4;      // 报 Unexpected token 'const' in expression
    return x;
}
```

**根因**（★ 值得记住的地方）

`isTypeKeyword()` 没列 `KwConst`（[dcl.type] 允许 `const` 作 type-specifier-seq 的开头）。

但真正的坑在于：**同一个语义在两条路径上判定不一致** ——

- **顶层声明**走「试探性 parseType + 回滚」，**不看** `isTypeKeyword()` ⇒ 顶层一直正常；
- **语句层**看 `isTypeKeyword()` ⇒ 只有它炸。

于是"顶层好好的、语句层不行"，症状极具误导性。

**修法**

`KwConst` 加入 `isTypeKeyword()`。

**当前约束**（代码里留的那行）：`⚠ 顶层声明走的是"试探性 parseType + 回滚"、
不看本函数 —— 改这里（或改那条前瞻）时必须两边一起核对。`

**回归用例**：`docs/learn/22`（名字在哪一层当真）的 ⑤。

---

### F2. 演示代码固定传 double 去填 NTTP 槽

**症状**

混排模板的**演示路径**实例化时形态校验失败 —— 拿一个**类型实参**去填 NTTP 槽。

**根因**

演示代码无条件传 `double`，没读 `templateParams[1].kind`。
对 `template <class T, int N>`，第二位要的是**值**，却收到一个类型。

**修法**

按 `templateParams[1].kind` 分派：`NonType` ⇒ `TemplateArg::ofValue(8)`，
否则走类型实参。

**教训**：凡是「按位次填实参」的地方，都必须先问该位的 `kind` ——
这正是 [T2](#t2-模板实参不能一律存成-typeptr) 那个设计的直接后果。

**残留缺口（本轮补齐）**

上面的修法**只看了第二位** `templateParams[1].kind`。于是
`template <bool B, class T>`（**首位**是 NTTP）落进"两类型形参"分支，
被喂 `<int, double>` ⇒ 实例化抛
`template argument 1 for 'enable_if' ('B') must be a value, but 'int' is a type`。

修法：在两形参分支里**先判 `templateParams[0].kind`**，首位是 NTTP 时走
`<true, int>`（值 + 类型混排）。

★ 这是"只取一位就下结论"的通病：形参表是**逐位**结构，
判据必须逐位看，写死"看第 N 位"等于把其他位当成常量。
同一族还有 [S12](#s12-实参个数校验读错形参表)（读错表）与
[S13](#s13-放宽数据结构时忘了拆守卫)（旧前提没跟着放宽）。

**回归用例**：`tests/tmpl/test_tmpl_22_nttp_mixed.cpp`（`Pair<int,8>`）、
`tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp`（Phase 4 演示
`enable_if<true,int>`）。

---

## sema（语义分析）

### S1. resolveType 查不到就静默放行

**症状**

```cpp
Undeclared q;        // rc=0，静默通过
int a = 1; a q;      // 同样
a < b > z;           // [stmt.ambig] 误判成声明，静默编译出算错的代码
```

**根因**

符号表查不到名字时，**原样返回 Parser 造的那个「空布局 Class 节点」**。
空布局让后续 `sizeof` / 字段偏移拿到 0 或垃圾 —— 错误被推迟到运行期，
而编译期一声不吭。

**修法**

`if (!sym || sym->kind != Type)` 就报 `unknown type name`。

★ 这也是 `a < b > c;`（[stmt.ambig]）能静默编译的原因 —— 它现在会响亮报错。

**回归用例**：`Undeclared q;`；清单见 `docs/learn/22` 的 ⑦。

---

### S2. 类内别名的登记时机

**症状**

```cpp
struct S { using Int = int; Int a; };   // 报 unknown type name 'Int'
```

**根因**

`m_classDecls[decl->name] = decl;` 拖到 `processClassDecl` **末尾**才登记，
而 `resolveType` 的「类作用域回退」正是经 `m_classDecls` 找类声明的。

**修法**

建好 `classType` 后**立刻**登记。

**回归用例**：`tests/tmpl/test_tmpl_48_class_type_aliases.cpp`。

---

### S3. 继承布局边扫边放

**症状**

非多态基类声明在多态基类**之前**时，`A.x` 与 `_vptr` **重叠**：

```cpp
class A { public: int x; };            // 非多态
class D : public A, public B { ... };  // B 是多态基类
```

**根因**

循环内「边扫边放」offset：primary 分支**写死 0**，且它的 `currentOffset` 是
「重置」而非累加 —— 把先摆好的进度**悄悄丢弃**。

**修法**

循环只负责**收集**字段 / vtable 条目；offset 一律由循环之后的 `[relocate]`
阶段统一摆放（primary 恒占 0）。

**镜像约束**：`processClassDecl` 的「子对象偏移不在这里算」与
`computeClassLayout` 的「偏移已由 [relocate] 摆好」是**互为镜像的两处** ——
只删一边会读不通。

---

### S4. place 被 anyPoly 守卫

**症状**

`D : A{int x}, B{int y}` ⇒ `x@0` 与 `y@0` 互踩，构造 B 时覆盖掉 x。

**根因**

`place` 被 `if (anyPoly)` 守卫住：**全非多态**时 `place` 停在 0，
第二个基类 `alignTo(0, 8)` 还是 0。

**修法**

`place` 恒从 primary 子对象尾部起算，去掉 `anyPoly` 守卫。

---

### S5. 两处选了不同的 primary 基类

**症状**

`A.offset` 与 `P.offset` 同为 0，`A.x` 压在 `_vptr` 上。

**根因**

同一个概念、两处各判一次，判据还不一样：

- `computeClassLayout` 用 `bi == 0` 判 primary；
- `processClassDecl` 用「第一个**多态**基类」。

⇒ 非多态基类声明在前时，两边选出**不同的 primary**，布局自相矛盾。

**修法**

统一从 `bases` 读 primary（按 `isPrimary` 标志）。

**教训**：同一个判断出现在两个函数里，就一定会分叉。

---

### S6. classType 的挂载时机

**症状**

字段偏移漏掉 `_vptr`、`totalSize = 0` ⇒ CodeGen 按 **0 字节** malloc，
运行期写越界。

**根因**

`decl->classType` 拖到函数末尾才赋值 —— 布局是在一个 `hasVTable=false` 的
**临时孤儿类型**上算出来的。

**修法**

建好类型对象后立刻挂到 `decl->classType`（代码里保留为一行 ★ 约束）。

★ 与 [S2](#s2-类内别名的登记时机) 是**同一个形状**：**对象建好就登记，
别拖到函数末尾**。两处各踩一次。

---

### S7. 嵌套实例化未保存上下文

**症状**

`main` 分析到一半、中途实例化了一个 **void** 函数后，main 自己的 `return`
被按 **void** 检查，报类型不匹配。

**根因**

模板实例化（S5）会在分析外层函数期间**嵌套**调回 `analyzeFunctionBody`，
而它没有保存 / 恢复 `m_currentReturnType` 等上下文。

**修法**

进入时保存、离开时恢复（RAII 或手工配对）。

---

### S8. 成员 callee 漏了补推导

**症状**

`shape->area()` 的虚调用退化成**直接** `callq`（多态失效）。

**根因**

`inferCall` 下方 `m_functionMap` 命中方法名后**提前 return**，
于是 `mem->object` 的 `resolvedType` 永远没被推导 ⇒
`CodeGen::emitCall` 拿不到对象类型、查不到 vtable 条目。

**修法**

进函数先对 `MemberExpr` callee 补一次 `inferType`。

⚠ **只对 `MemberExpr` 补** —— 对 `VarExpr` callee 补推导会误报
`Undefined variable`。

---

### S9. 以裸名为键的方法表

**症状**

返回类型与参数**全错**（随机命中别的类的方法）。

**根因**

`m_functionMap` 以**裸名**为键 ⇒ `Box_int::get` 与 `Box_double::get`
互相覆盖。

**修法**

成员调用必须走「对象类 → 方法表」（class-scoped + BFS 基类），
全局名字表只作兜底。

---

### S10. 候选池混进了模板实例

**症状**

`test_tmpl_17` 链接失败：`undefined 'mix'`。

**根因**

`m_functions` 里的**函数模板实例** `name` 保留着模板名，被「精确匹配」抢先命中，
而该实例的名字尚未按调用点重写。

**修法**

候选池只收 `isPlainFreeFunction`（`ownerClassName` 空 ∧ `mangledName == name`）——
成员带类名前缀、模板实例 `_Z` 开头，都不等于 `name`。

---

### S11. Pass 2/3 漏掉命名空间

**症状**

链接期 `undefined reference to 'N__S_N__S'`。
★ Sema 侧**一切正常** —— 是 CodeGen 从没收到方法，导致合成构造 / 析构没发射。

**根因**

Pass 2/3 扁平遍历 `unit.declarations`，漏掉了命名空间内的类与函数。

**★ 镜像 bug**：顺手在 Pass 1 里补 `registerFunction` 会导致同一符号发射两次
（`symbol 'N__get' is already defined`）—— **修一个引来另一个**。

**修法**

`forEachFunctionDecl` 递归进命名空间；Pass 1 对命名空间内函数**只改名、不注册**。

---

### S12. 实参个数校验读错形参表

**症状**

`template <class T, int N>` 被数出 **2 个「类型」形参** ⇒ 对 `Buf<int,4>` 必然错位。

**根因**

读 `typeParams`（裸 `vector<string>`，**kind 已经丢了**）。

**修法**

改用 `templateParams`（带 kind 的结构化形参表）。

**同族**：[I4](#i4-蓝图摘要读错了形参表) 是同一个两张表问题的另一个出口。
★ 只要 `TemplateDecl` 还并存两张形参表，"读错表"就还会再犯。
（I4 已于本轮一并修掉，两个出口现在都读 `templateParams`。）

---

### S13. 放宽数据结构时忘了拆守卫

**症状**

`enable_if<true, T>` 的值位偏特化写对了，日志里却完全看不到特化匹配，
只有一行"实参含非类型值，跳过特化匹配"，然后一路走主模板。

**根因**

`selectClassTemplate` 开头有一段**基于旧假设**的守卫：

```cpp
std::vector<TypePtr> argTypes;
bool allTypeArgs = true;
for (const auto& a : args) {
    if (!a.isType()) { allTypeArgs = false; break; }   // 值实参 ⇒ 放弃特化
    argTypes.push_back(a.type);
}
```

它的前提是"`specPattern` 只含类型"。本轮把 `specPattern` 改成了
`vector<TemplateArg>`（值位可入表），**这个前提就没了**——但守卫还在，
于是新能力被旧守卫整条掐死。

★ 这是 [T2](#t2-模板实参不能一律存成-typeptr)（实参一律存 `TypePtr`）的
**下游回声**：同一个简化假设会同时写进「数据结构的类型」和
「使用它的守卫」两处。放宽假设时只改类型、不拆守卫，
结果是**编译通过、日志正常、功能全错**——比编译报错难查得多。

**修法**

拆掉守卫，实参表原样进匹配；值位由 `matchPattern` 的新分支逐位比
（同形态 + 同值，不产生绑定）。

**回归用例**：`tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp`（值位选中/不选中/
全特化带值位），`PartialSpec.NttpPatternPosition*`（4 例）。
