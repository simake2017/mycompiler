# 16 RTTI 存放粒度与 dynamic_cast 运行时判定（问答总结）

> 本文是 11 号文档（dynamic_cast 与 RTTI 基础）的补充，整理自一轮教学问答：
> ① RTTI 信息到底存哪里（按类还是按对象）；
> ② 类没有虚函数时 `dynamic_cast` 会怎样（退化成 `static_cast`？直接拒绝？）；
> ③ 运行期判定成败的精确规则（实际类型的祖先链）；
> ④ 顺带暴露的本项目缺口清单。
> 全部结论均有实测佐证（minicc 运行 + `clang++-18` oracle）。

## ① 核心结论：RTTI 按"类"存，不按"对象"存，与用没用 cast 无关

三种常见猜测与判定：

| 猜测 | 判定 |
|---|---|
| 每个对象都存一份 RTTI 信息 | ❌ 对象里只有一个 8 字节指针（`_vptr`），类型档案另存 |
| 只给"被 cast 过"的对象存 | ❌ cast 不产生任何存储；档案在编译期就按类生成好了 |
| 无虚函数的类也能用 dynamic_cast | ❌ 真 C++ 编译期直接拒绝（见 ②） |

### 1.1 存储分工图

```text
        编译期（编译器写进 .data 段，跟 cast 无关）
        ┌──────────────────────────────────────┐
        │  每个【多态类】生成一份档案：            │
        │                                      │
        │   B1 有虚函数 → 生成 _ZTI2B1          │
        │   B2 有虚函数 → 生成 _ZTI2B2          │
        │   D  有虚函数 → 生成 _ZTI1D           │
        │   Point 无虚函数 → 什么都不生成  ✂     │
        └──────────────────────────────────────┘

        运行期（每个对象）
        ┌──────────────────────┐
        │ D 对象                │   只有 8 字节的 _vptr，
        │  +0: _vptr ──────────┼──→ 指向档案（.data 里那份）
        │  +8: 字段...          │   对象自己不存任何类型信息
        └──────────────────────┘
        ┌──────────────────────┐
        │ Point 对象（无虚函数） │   连 _vptr 都没有，
        │  +0: x  +4: y        │   对象里零 RTTI 痕迹
        └──────────────────────┘
```

- **粒度是"类"**：每个多态类一张 typeinfo（`_ZTI` 前缀，`.data` 段），一张表服务该类的所有对象。
- **对象只存指针**：`_vptr` 由构造函数在构造时写入，指向该类的 vtable；vtable 槽 `[-1]` 再指向 typeinfo。
- **dynamic_cast 是"查"不是"存"**：它顺着 `vptr[-1]` 找到现成档案，没有任何运行期存储分配。

### 1.2 为什么不能"谁被 cast 谁才带指针"

`_vptr` 的值在**构造时**就定死了。而多态的全部意义就是"指针静态类型 ≠ 对象实际类型"——
`A* p` 到底指向 A 对象还是 B 对象，编译期无从得知。如果允许"按需装指针"，
构造时根本不知道该不该装。所以只有两种可能：**整个类的对象都有 `_vptr`，或整个类都没有**。

## ② 无虚函数时的 dynamic_cast：编译期分情况处理（不是运行时退化）

真 C++ 的规则（[expr.dynamic.cast]）：

| 方向 | 非多态类上的结果 | 标准依据 |
|---|---|---|
| 向上 `B* → A*` | ✅ 编译通过，**退化成 `static_cast`** | [expr.dynamic.cast]/4 |
| 向下 `A* → B*` | ❌ 编译期拒绝：`'A' is not polymorphic` | [expr.dynamic.cast]/1 |
| 旁系 `A* → C*` | ❌ 同上拒绝 | 同上 |

### 2.1 实验：向上退化的汇编证据

源码 `/tmp/dc_cmp.cpp`（A、B 均无虚函数）：

```cpp
A* up_dyn(B* q) { return dynamic_cast<A*>(q); }
A* up_sta(B* q) { return static_cast<A*>(q); }
```

`clang++-18 -O2 -S` 产物：

```text
  dynamic_cast<A*>(q)          static_cast<A*>(q)
  ─────────────────            ─────────────────
  movq  %rdi, %rax             movq  %rdi, %rax
  retq                         retq
        ↑                              ↑
        完全一样：纯指针搬移，零开销
```

没有运行期检查、没有查表、没有助手函数——"退化"就是编译期把 `dynamic_cast`
直接改写成 `static_cast` 语义。

### 2.2 实验：向下被编译期拒绝

源码 `/tmp/dc_down.cpp`：

```cpp
B* down(A* p) { return dynamic_cast<B*>(p); }   // A 无虚函数
```

```text
  clang++-18 -fsyntax-only:
  error: 'A' is not polymorphic
```

### 2.3 为什么向上能退化、向下不能

```text
  向上 B*→A*：编译期就知道答案
  ┌────────────────────┐
  │ B 对象              │
  │ +0: A 部分 (x)  ◄──┼── A* 一定指这里（public 单继承偏移固定）
  │ +4: y              │    不需要问"实际类型"，直接减偏移
  └────────────────────┘

  向下 A*→B*：编译期不知道答案
  ┌─────────────┐   ┌─────────────┐
  │ 可能是 A 对象 │   │ 也可能是 B 对象 │
  │  +0: x      │   │  +0: x      │
  └─────────────┘   │  +4: y      │
       A* 指向谁？──┴─────────────┘
       静态类型都是 A*，必须运行时问"你到底是什么"
       → 需要 RTTI → 需要 _vptr → 类必须多态 → 否则无解，只能拒绝
```

一句话：**向上转换不需要类型信息（位置是编译期常量），向下转换必需类型信息
（非多态类没有，所以标准干脆禁止）。**

### 2.4 minicc 现状：错误被推迟到链接期

实验 `/tmp/np_cast.cpp`（非多态 `A`、`B:public A`，执行 `dynamic_cast<B*>(p)`）：

```text
  sema：    [dynamic_cast] A* → B*    (runtime RTTI check)   ← 放行了
  布局日志：A Total size: 4 bytes、B Total size: 12 bytes    ← 两者均无 _vptr
  link：    undefined reference to '_ZTI1B'                   ← 档案不存在
```

根因：`inferDynamicCast`（src/semantic_analyzer.cpp:2455）只检查
目标类存在、源是类指针、公共祖先可达——**漏了"源类型必须多态"**；
而 `emitRTTI` 只为 `hasVTable` 的类生成 typeinfo，引用自然悬空。

## ③ 有虚函数 ≠ 必转换成功：运行期判定看"实际类型的祖先链"

### 3.1 判定链

```text
  dynamic_cast<目标*>(p)：
  ① 先问出 p 的【实际类型】（_vptr → vtable[-1] → typeinfo）
  ② 从实际类型出发，沿继承链【往上】找：找得到"目标"吗？
     ├─ 找到 → 成功，返回调整偏移后的指针
     └─ 走到头没找到 → 返回 nullptr
```

**①认得出**：有虚函数 → 对象带 `_vptr` → 运行时能回答"我到底是什么类"。
**②转得成**：实际类型还必须能沿继承链走到目标。

### 3.2 实验一：同一个指针，一次成一次败

源码 `/tmp/dc_rt.cpp`：

```cpp
class A { public: virtual int f() { return 1; } };
class B : public A { public: int y; };
class C : public A { public: int z; };

A* p = new B();
B* b = dynamic_cast<B*>(p);   // ?
C* c = dynamic_cast<C*>(p);   // ?
```

```text
        A            ← p 的静态类型
       / \
      B   C          p 实际指向 B

  cast<B*>：实际类型 B，祖先链 B→A 含 B ✓ → 非空
  cast<C*>：祖先链 B→A 不含 C            ✗ → nullptr

  实测退出码 = 1（只有 B 成功）
```

### 3.3 实验二：看的是"实际链"，不是"静态链"

源码 `/tmp/dc_chain.cpp`：

```cpp
class A { public: virtual int f() { return 1; } };
class B : public A { public: int b; };
class C : public B { public: int c; };   // C 是 B 的派生类
class E : public A { public: int e; };   // E 和 B 是兄弟

A* p = new C();            // 实际类型是 C
B* b = dynamic_cast<B*>(p);   // ?
E* e = dynamic_cast<E*>(p);   // ?
```

```text
        A
       / \
      B   E
      |
      C   ← 实际类型

  cast<B*>：C 的祖先链 C→B→A 含 B ✓ 成功
  cast<E*>：链里没有 E               ✗ nullptr    （实测退出码 = 1）
```

**关键对照**：B 和 E 都是静态类型 A 的派生类，静态视角地位相同；
但一个成一个败——运行时只看**实际类型 C 的祖先链**，不看 A 下面有哪些派生类。

### 3.4 三个容易漏的细节

| 细节 | 说明 |
|---|---|
| 虚函数可以**继承** | B、C 自己没写虚函数，但继承 A 的 → 一样是多态类（实验日志：三者均有 `vtable + RTTI`）。判定看类整体，不看类自己有没有声明 |
| 失败方式按类型分 | 指针失败返回 `nullptr`；引用失败抛 `std::bad_cast`（minicc 暂只支持指针版） |
| minicc 实现限制 | `emitRTTI` 只给有 vtable 的类生成 typeinfo，所以**目标类也必须多态**才能 link 成功。真 C++ 更宽松：源多态即可，目标可以完全非多态（`dynamic_cast<纯数据类*>(p)` 合法） |

## ④ 本项目缺口清单（本轮问答暴露）

| # | 缺口 | 现状 | 真 C++ 行为 | 修复思路 |
|---|---|---|---|---|
| 1 | `inferDynamicCast` 不检查源类多态性 | 语义期放行 → 链接期 `undefined reference to '_ZTI*'` | 编译期报 `'X' is not polymorphic` | `inferDynamicCast` 加 `hasVTable` 检查（一行级） |
| 2 | 非多态向上转换不做编译期退化 | 一律走运行期助手 | `dynamic_cast` 向上退化为 `static_cast`（零开销） | 可达性判断为"向上"时改走静态偏移路径（几十行） |
| 3 | 目标类必须多态 | `emitRTTI` 只为 `hasVTable` 类生成档案 | 目标类可以完全非多态 | `emitRTTI` 放宽发射条件，或为 dynamic_cast 目标按需发射 |
| 4 | 助手只做向上 DFS | `__minicc_dynamic_cast` 从实际类型向上找 | Itanium ABI 双向搜索（祖先+后代） | 教学场景单链够用，复杂形态记入 ROADMAP |

## ⑤ 复现步骤

```bash
cd /root/cppproject/mycompiler

# ②.1 向上退化的汇编对照（oracle）
clang++-18 -O2 -S -o /tmp/dc_cmp.s /tmp/dc_cmp.cpp
#   两个函数产物一致：movq %rdi,%rax; retq

# ②.2 非多态向下被拒绝（oracle）
clang++-18 -fsyntax-only /tmp/dc_down.cpp
#   error: 'A' is not polymorphic

# ②.4 minicc 非多态 dynamic_cast 走到链接失败
./build-linux/minicc /tmp/np_cast.cpp
#   [dynamic_cast] A* → B*  ……  undefined reference to '_ZTI1B'

# ③.2 同一指针成败对照
./build-linux/minicc /tmp/dc_rt.cpp -o /tmp/dc_rt && /tmp/dc_rt; echo $?
#   exit=1（B 成功、C 失败）

# ③.3 实际链判定（静态链对照）
./build-linux/minicc /tmp/dc_chain.cpp -o /tmp/dc_chain && /tmp/dc_chain; echo $?
#   exit=1（B 成功、E 失败）
```

实验源文件均在 `/tmp/`（dc_cmp / dc_down / dc_rt / dc_chain / np_cast），
内容已全文收录于本文对应小节，可据此重建。
