# 35 · NTTP 的类型域：从「只认 int」到整型家族 + 字符 + auto

> 前置：[18-non-type-template-parameters](18-non-type-template-parameters.md)（NTTP 的
> tagged 实参与两层替换）、[32-nttp-spec-pattern](32-nttp-spec-pattern.md)（值位进特化模式）。
> 本批**不新增语法**，只把 [temp.param]/6 允许的**形参类型**从 `int` 一个点
> 扩到整型家族 + 字符 + `auto`，并把实参侧的**字面量形态**与**转换规则**补齐。
>
> ★ 本批同时修掉一个**拒收合法程序**的 bug（[BUGS.md B17](../BUGS.md)）与一个
> **静默撞键**的 bug（`template<auto V>` 下 `K<4>` 与 `K<4L>` 复用同一实例）。

---

## 1. 理论背景

### 1.1 一句话：形参位要的是「类型」，类型有一整个家族

[temp.param]/6 规定非类型模板形参的类型只能是**结构化类型**（structural type）：
整型/枚举、指针、左值引用、`std::nullptr_t`、以及满足条件的字面量类类型。
其中最基本、也最常被使用者碰到的一档是**整型家族**。

本项目此前只认字面关键字 `int` —— `template<unsigned N>`、`template<char C>`
一律解析失败。这不是"少写几个分支"，而是三个独立的知识点各自缺一块：

| 知识点 | 标准位置 | 本项目缺口 |
|---|---|---|
| 形参类型怎么写 | [dcl.type.simple] 的 type-specifier-seq | 只认单关键字；`unsigned long` / `long unsigned` 不认 |
| 实参字面量的**类型**是什么 | [lex.icon]/2 的类型表、[lex.ccon] | 只认裸十进制，后缀/进制/字符字面量全丢 |
| 实参能不能填进形参 | [temp.arg.nontype]/1 → [expr.const]/10 → [dcl.init]/7 | 判据写错（见 §1.4） |

### 1.2 type-specifier-seq：写法有穷举，类型只有 12 种

[dcl.type.simple] 允许 `signed` / `unsigned` / `short` / `long` / `int` / `char` / `bool`
以**任意顺序、任意重复**组合（`unsigned long long int` ≡ `long long unsigned`）。
朴素的"逐 token 建类型"会爆炸；正确做法是 clang 的做法：

> **先把关键字收集成计数器，最后一次性归一**（clang 的 `Sema::GetTypeFromParser`
> 背后是 `DeclSpec` 的四组位掩码 `TypeSpecifier` / `TypeSpecWidth` /
> `TypeSpecSign` / `TypeSpecComplex`）。

本项目对应物是 `Parser::parseBuiltinTypeSpecifierSeq()`：收 `char/short/long/signed/unsigned/int`
六个计数器 → 组合非法就报错（`long long long`、`signed unsigned`、`char int`…）→
否则映射到 12 个工厂之一。

```
                  ┌──────── 收集计数 ────────┐
  "unsigned long long int"                            
   unsigned:1  long:2  int:1  char:0  short:0  signed:0
                         │
                         ▼
              ┌──── 归一：先定宽度，再定符号 ────┐
              long>0 ∧ short>0            ⇒ ✗ 报错
              long≥3                      ⇒ ✗ 报错
              short>1 / char>1 / int>1    ⇒ ✗ 报错
              long==2                     ⇒ 64 位
              long==1                     ⇒ 64 位（LP64！）
              short==1                    ⇒ 16 位
              else                        ⇒ 32 位
                         │
                         ▼ 叠符号
              unsigned ∧ !char            ⇒ ULong
              else                        ⇒ Long
```

★ 易错点：**LP64 下 `long` 与 `long long` 都是 8 字节**，但它们是**不同的类型**
（`TypeKind::Long` ≠ `TypeKind::LongLong`，mangling `l` ≠ `x`）。
本项目按本机 x86-64 System V ABI 走 LP64。

### 1.3 字面量的类型：Lexer 只切片，Parser 才解释

这是本批一个**刻意的分层**，对应 clang 的 `Lexer::LexNumericConstant` 与
`NumericLiteralParser` 的分工：

| 层 | 干什么 | 不干什么 |
|---|---|---|
| `Lexer::scanNumber` | 吃完前缀/数字/后缀/`'` 分隔符，**产出一个 token 的原文** | ❌ 不算值、❌ 不判类型 |
| `Parser::parseIntLiteral` | 按 [lex.icon]/2 的表把原文读成 `(value, type)` 二元组 | ❌ 不打日志（见下） |

[lex.icon]/2 的类型表（十进制这一列，完整表在 §5）. 规则是"第一个装得下的候选"：

| 后缀 | 候选类型序列（十进制） |
|---|---|
| 无 | `int` → `long` → `long long` |
| `u` | `unsigned int` → `unsigned long` → `unsigned long long` |
| `l` | `long` → `long long` |
| `ul` | `unsigned long` → `unsigned long long` |

★ 本项目的简化：**不做"值溢出候选表就换下一档"的自动升格**（要 `stoull` 的
溢出检测 + 逐档比较，属常量折叠的地盘）。`std::stoull` 抛 `out_of_range` 即报错。
对 `Buf<4>` 这种教学场景够用；边界在 §5。

**为什么 `parseIntLiteral` 一行日志都不打**：它被**每一条整数表达式**调用
（`return 0;` 里的 `0` 也走它）。逐字面量打点会把日志淹掉（实测加了之后
全量 logdiff 多出几百行纯噪声）。而"字数形态"这件事有两个更好的观测点：

- NTTP 实参位 —— 调用点 `[parse:targ] … (形态 int)`；
- 普通表达式 —— `[infer] IntLiteral(4) → long` 已经说明类型。

> **经验**：日志该打在**决策点**（"这一位现在定成什么"），不该打在
> **被高频复用的纯函数**里。

字符字面量同理：`[lex.ccon]` 的转义在**词法期**翻译完毕，`'\n'` 的 token 正文
是真正的 `0x0A`。回吐日志时必须**重新转义**（`Parser::escapeCharText`），
否则一行日志会被裸换行拦腰截断 —— 既没法读，也让 logdiff 的逐行对比失去意义。

### 1.4 判据：可表示性，不是形态相等（★ 本批的 bug）

旧实现的值位检查是：

```cpp
else if (!a.valueType->equals(p.nonType)) { error("… cannot be narrowed …"); }
```

要求"实参字面量的形态与形参类型**精确相等**"。标准要的不是这个：

| 标准位置 | 说的是什么 |
|---|---|
| [temp.arg.nontype]/1 | 实参须是形参类型的 **converted constant expression** |
| [expr.const]/10 | 该术语的定义直接引用 [dcl.init]/7 的初始化规则 |
| [dcl.init]/7 | 窄化禁令，但**常量表达式豁免"值恰好装得下"的那部分** |

于是：

| 用法 | 形态 | 值 | 旧判据 | 正确判据 |
|---|---|---|---|---|
| `Flag<1>`（bool ← int） | int ≠ bool | 1 ∈ {0,1} | ❌ 拒 | ✅ 收 |
| `A<4L>`（int ← long） | long ≠ int | 4 ∈ int | ❌ 拒 | ✅ 收 |
| `A<true>`（int ← bool 提升） | bool ≠ int | 恒不窄化 | ❌ 拒 | ✅ 收 |
| `F<2>`（bool ← int） | int ≠ bool | 2 ∉ {0,1} | ✅ 拒 | ✅ 拒 |
| `U<-1>`（unsigned ← int） | int ≠ unsigned | -1 ∉ [0,2³²) | ✅ 拒 | ✅ 拒 |
| `D<300>`（char ← int） | int ≠ char | 300 ∉ [-128,127] | ✅ 拒 | ✅ 拒 |

★ **前三行旧版全拒** —— 后三行"拒对了"掩盖了它。这是"**负向测试全绿 ≠ 判据正确**"
的又一例：只有拿**正向用例**去撞才会露馅。详见 [BUGS.md B17](../BUGS.md)。

新判据收口成 `Type::canRepresentValue(int64_t)`（[dcl.init]/7 + [expr.const]/10 的合体）：

```
                    canRepresentValue(v)
                            │
              ┌─────────────┼──────────────┐
              ▼             ▼              ▼
          !isInteger()   isBool()      按符号分两路
              ⇒ false    v∈{0,1}     ┌────────┴────────┐
                                    ▼                 ▼
                              无符号 [0,2^N-1]   有符号 [-2^(N-1), 2^(N-1)-1]
                              （v<0 直接 false）  （N==64 时即 int64_t 全域）
```

★ 三条设计约束：

1. **判据单点**：`canRepresentValue` 只定义一处（`src/type.cpp`），
   `semantic_analyzer` 与 `main.cpp` 的 Phase 4 演示路径共用。
   本项目在 B10 / B12 上已经吃过两次"同一判据写两份"的亏（见 [BUGS.md](../BUGS.md)）。
2. **归一 = 语义必需，不是修饰**：通过判据后把 `a.valueType` 改写成 `p.nonType`
   （"③-c 形态归一"）。否则 `Buf<4L>` 与 `Buf<4>` 的可读串 / 缓存键 / mangling
   会对不上，各建一份 `Buf_4`（撞汇编符号）。
   clang 也认它们是同一实例（`template struct A<4>; template struct A<4L>;` 报 duplicate）。
3. **`bool` 单独一条**：它的 `integerBitWidth()` 是 1，但"1 位量"描述不了值域 ——
   bool 的可表示集合是 `{0,1}`。同样地 `isUnsignedInteger()` **不包含 bool**
   （见单测 `NttpTypeDomain.IntegerFamilyWidthsAndSignedness` 里那格"既非 signed 也非 unsigned"）。

### 1.5 `template<auto V>`：类型由实参反推

[temp.param]/6 的 placeholder 形参：`auto V` 不是"某个整型"，而是**一整族**整型，
类型在推导时由实参定。这带来一个与 `template<int N>` 的本质差别：

| 形参 | `X<4>` 与 `X<4L>` | 理由 |
|---|---|---|
| `template<int N>` | **同一实例** | long→int 转换后形态都是 int |
| `template<auto V>` | **两个实例** | V 的类型分别是 int / long，形态即类型 |

★ 而这条差别要求**实例缓存键与汇编符号名必须能区分形态** —— 曾经的实现
用 `TemplateArg::toString()` 生成键，它对 `4`(int) 与 `4`(long) 都产 `"4"`，
于是第二个**静默复用**第一个实例。既没报错、也没算错（值恰好都是 4），
**连"看起来不对"的症状都没有**。这与 [23](23-cv-position-and-type-identity.md)
以及 B13~B15 是同一个坑：**拿"给人看的字符串"当机器用的键，早晚出事。**

修法：键与实例名同源于一对函数（`NameMangler::losslessArgumentsKey` /
`renderArgLossless`），**形态只在 `auto` 形参位才写入** —— 这样既修了 auto 位的
撞键，又对所有既有模板（无 auto 形参）保持零日志漂移。

```
       实参元组 + 形参表
              │
              ▼  renderArgLossless(a, p)   ← 逐位
   p 是 auto 位？ ──是──▶ 渲染成 "4:long"（值 + 形参反推出的类型）
        │否
        ▼
   渲染成 "4"（只值）
              │
      ┌───────┴────────┐
      ▼                ▼
  缓存键 "K<4:long>"   实例名 "K_4Clong"
                        （`:` 经 sanitizeSymbolChars 转义成 `C`）
```

★ 为什么用 `:` 而不是别的分隔符：它在 C++ 类型里不会出现（除非嵌套名，
而嵌套名的 `::` 已被更早的清洗规则处理），且**在符号名里非法** ——
所以必须过 `sanitizeSymbolChars`。第一次实现忘了给它加分支，
汇编期直接吐出非法符号（`.globl K_4:long`）。

### 1.6 全特化只认「转换后类型」

`template<> struct K<4>` 只匹配 **int 形态**的 `K<4>`，不吃 `K<4L>`：
[temp.expl.spec] 的实参等同性按**类型**判定，`4` 与 `4L` 的转换后类型
分别是 int / long，不相等。日志里两条 `[spec:select]` 一行
`① explicit specialization matched`、一行 `① no explicit specialization matched`
—— 见 `test_tmpl_68`。

### 1.7 mangling：`<builtin-type>` 与 `TnDa`

Itanium C++ ABI §5.1.2 的 `<builtin-type>` 单字符编码（本批新增的 10 个）：

| 编码 | 类型 | 编码 | 类型 |
|---|---|---|---|
| `b` | `bool` | `j` | `unsigned int` |
| `c` | `char` | `l` | `long` |
| `a` | `signed char` | `m` | `unsigned long` |
| `h` | `unsigned char` | `x` | `long long` |
| `s` | `short` | `y` | `unsigned long long` |
| `t` | `unsigned short` | `Da` | `auto` |

实参编码是 `<expr-primary>` = `L <type> <value> E`，负数写成 `n<绝对值>`：

```
template<unsigned N> struct U { int f(); };   U<4u>::f  ⇒ _ZN1UILj4EE1fEv
                 ▲                                          ▲▲▲▲▲▲▲
                 │                                          │││  └─ E  实参结束
                 └─ 4 = 名字长度                            ││└──── 4  值
                                                            │└───── j  unsigned int
                                                            └────── L  字面量开始
```

★ 一个**只出现在函数模板**里的东西 —— `<template-param-decl>`（`TnDa`）：

```
template<auto V> struct K { int f(); };   K<4>::f   ⇒ _ZN1KILi4EE1fEv      ← 无 TnDa
template<auto V> int getv();              getv<4>() ⇒ _Z4getvITnDaLi4EEiv ← 有 TnDa
                                        ─────────────────────┬────
                                     用 V 的【推导类型】给 auto 形参做消歧，
                                     否则 template<auto V> f() 与 template<int V> f()
                                     的实例会撞同一个符号
```

★ **本批不写死代码实现 `TnDa`**：函数模板的**显式** NTTP 实参目前直接报
`explicit non-type template argument '4' for function template 'f' is not supported yet`
（函数模板推导那一批有意留的口子）—— 没有能到达该编码的路径，写了就是死代码。
类模板**不**发射 `TnDa`（类模板的实参表里没有"模板形参声明"这一节），
故本批的类模板全量对照是**逐字符相同**的（§4 有 12/12 的实测）。

---

## 2. 数据流（ASCII 图）

```
 源码 ──▶ ┌──────────────┐
          │  Lexer       │  '4L' → IntLiteral("4L")   'a' → CharLiteral("\x61")
          │  scanNumber  │  ★ 只切片，不算值不判类型
          │  scanChar    │
          └──────┬───────┘
                 ▼
          ┌──────────────────────────────────────────────────┐
          │  Parser::parseTemplateArgumentList               │
          │    ├─ 负数分支：'-' + IntLiteral                 │
          │    ├─ bool 分支：kw_true / kw_false              │
          │    ├─ 字符分支：CharLiteral → 折叠多字符         │
          │    └─ 整数分支：parseIntLiteral                  │
          │           原文 ──▶ (value:int64, 形态:TypePtr)   │
          │           [lex.icon]/2 的类型表                  │
          └──────┬───────────────────────────────────────────┘
                 ▼  TemplateArg{kind=Integral, value, valueType}
          ┌──────────────────────────────────────────────────┐
          │  Sema::checkTemplateArguments（逐位）            │
          │    ③   形参类型受支持？ 整型家族 ∨ auto          │
          │    ③-b 可表示性 canRepresentValue(v)             │
          │         ✗ ⇒ "cannot be narrowed to type 'T'"     │
          │    ✅ ⇒ ③-c 形态【归一】a.valueType = p.nonType  │
          │    auto 位：反推 p 的类型 = a.valueType          │
          └──────┬───────────────────────────────────────────┘
                 ▼  归一后的实参元组
          ┌──────────────────────────────────────────────────┐
          │  losslessArgumentsKey / renderArgLossless        │
          │    ├─ auto 位 ⇒ 写形态  ⇒ "K<4:long>"            │
          │    └─ 非 auto ⇒ 只写值  ⇒ "K<4>"                 │
          └──────┬───────────────────────────┬───────────────┘
                 ▼                           ▼
          实例缓存键                    实例名 / 汇编符号前缀
          （撞键 ⇒ 静默复用）           "K_4Clong"（':' → 'C'）
```

---

## 3. 实现改动清单

| 层 | 文件 | 改了什么 |
|---|---|---|
| 词法 | `include/token.h` | 新增 `KwChar/KwShort/KwSigned/KwUnsigned/KwLong` + `CharLiteral`；全部进 `isTypeKeyword()` |
| | `src/lexer.cpp` | `scanNumber` 重写（`0x`/`0b`/前导 `0`、`u/U/l/L` 后缀、`'` 分隔符）；新增 `scanChar` |
| 语法 | `include/parser.h` / `src/parser.cpp` | `parseBuiltinTypeSpecifierSeq()`（收集→归一）；`parseType` 在 `KwInt` 分支**之前**插入新分支（保证既有单关键字日志逐字节不变）；`IntLiteralValue parseIntLiteral()`；三处实参分流新增字符字面量分支；`escapeCharText()` |
| AST | `include/ast.h` | `NodeKind::CharLiteral` + `CharLiteralExpr`；`IntLiteralExpr` 增 `literalType`（2 参构造，1 参版本保留以零漂移） |
| | `include/ast_visitor.h` | `visit(CharLiteralExpr&)` 默认空体（★ 类内 inline 定义，避免成为键函数） |
| 语义 | `src/semantic_analyzer.cpp` | `inferIntLiteral` 返回 `literalType`；新增 `inferCharLiteral`；`typeCompatible` 放开整型↔整型；**checkTemplateArguments ③-b/③-c 重写**（可表示性 + 归一 + auto 反推） |
| 类型 | `include/type.h` / `src/type.cpp` | 10 个新 `TypeKind` + 工厂 + 谓词（`isInteger` / `isChar` / `isUnsignedInteger` / `integerBitWidth`）；`canRepresentValue`；`sizeInBytes` 改为从位宽推导 |
| 实例化 | `include/template_instantiation.h` / `src/template_instantiation.cpp` | `encodeType` 新增 10 个整型分支；`renderArgLossless` / `losslessArgumentsKey`；`sanitizeSymbolChars` 补 `:` → `C` |
| 代码生成 | `src/codegen.cpp` | `visit(CharLiteralExpr&)` → `movq $<value>, %rax`（★ CodeGen 全程**按宽度**分派 1/≤4/8 字节，零 `TypeKind` 引用 ⇒ 新标量类型的 codegen 成本为 0） |

★ 最后一格值得单独说：**新类型在 CodeGen 侧一行都不用加**。
`codegen.cpp` 里没有任何 `TypeKind::` 的引用，只有 `sizeInBytes()` 的分档。
这是"类型系统与代码生成解耦"的一次红利。

---

## 4. 可复现实验

```bash
# ① 端到端（期望两个编译器都返回 7）
./minicc tests/tmpl/test_tmpl_61_nttp_integer_family.cpp -o /tmp/t61 && /tmp/t61; echo $?
clang++-18 -std=c++20 tests/tmpl/test_tmpl_61_nttp_integer_family.cpp -o /tmp/t61c && /tmp/t61c; echo $?

# ② 整型家族的 mangling：本实现 vs clang，逐字符对照
cat > /tmp/probe.cpp <<'EOF'
template<bool V>               struct Tb { int f() { return 0; } };
template<char V>               struct Tc { int f() { return 0; } };
template<signed char V>        struct Ta { int f() { return 0; } };
template<unsigned char V>      struct Th { int f() { return 0; } };
template<short V>              struct Ts { int f() { return 0; } };
template<unsigned short V>     struct Tt { int f() { return 0; } };
template<int V>                struct Ti { int f() { return 0; } };
template<unsigned int V>       struct Tj { int f() { return 0; } };
template<long V>               struct Tl { int f() { return 0; } };
template<unsigned long V>      struct Tm { int f() { return 0; } };
template<long long V>          struct Tx { int f() { return 0; } };
template<unsigned long long V> struct Ty { int f() { return 0; } };
int main() {
    Tb<true> a0; Tc<'a'> a1; Ta<'a'> a2; Th<'a'> a3;
    Ts<1> a4; Tt<1> a5; Ti<1> a6; Tj<1> a7;
    Tl<1> a8; Tm<1> a9; Tx<1> a10; Ty<1> a11;
    return a0.f()+a1.f()+a2.f()+a3.f()+a4.f()+a5.f()+a6.f()+a7.f()+a8.f()+a9.f()+a10.f()+a11.f();
}
EOF
clang++-18 -std=c++20 -c /tmp/probe.cpp -o /tmp/probe.o && \
  llvm-nm-18 /tmp/probe.o | grep -oE '_ZN2T.IL[a-zA-Z0-9]+EE1fEv' | sort -u
# minicc 侧：`(1|97)EE$` 是为了滤掉 Phase 4 演示路径自动实例化的那份（值都是 4）
./minicc /tmp/probe.cpp -o /tmp/probe 2>/dev/null \
  | grep -oE '_Z2T.IL[a-zA-Z0-9]+EE' | sort -u | grep -E '(1|97)EE$'
# ⇒ 12/12 逐字符相同（`_Z2TaILa97EE` 与 clang 的 `_ZN2TaILa97EE1fEv`：
#     `<expr-primary>` 段 ILa97EE 完全一致）：b c a h s t i j l m x y
#   （a = signed char，h = unsigned char，t = unsigned short，y = unsigned long long）
```

```
# ⇒ clang 侧（12 条，本实现逐字符相同）
_ZN2TaILa97EE1fEv   signed char 97     _ZN2TbILb1EE1fEv    bool 1
_ZN2TcILc97EE1fEv   char 97            _ZN2ThILh97EE1fEv   unsigned char 97
_ZN2TiILi1EE1fEv    int 1              _ZN2TjILj1EE1fEv    unsigned int 1
_ZN2TlILl1EE1fEv    long 1             _ZN2TmILm1EE1fEv    unsigned long 1
_ZN2TsILs1EE1fEv    short 1            _ZN2TtILt1EE1fEv    unsigned short 1
_ZN2TxILx1EE1fEv    long long 1        _ZN2TyILy1EE1fEv    unsigned long long 1
```

```bash
# ③ 字面量形态（后缀 / 进制 / 分隔符）
./minicc tests/tmpl/test_tmpl_62_nttp_literal_forms.cpp -o /tmp/t62 && /tmp/t62; echo $?

# ④ 字符字面量与「日志不被截断」
./minicc tests/tmpl/test_tmpl_63_nttp_char_literal.cpp -o /tmp/t63 && /tmp/t63; echo $?

# ⑤ ★ 回归 B17：三条【应当被接受】的合法程序
./minicc tests/tmpl/test_tmpl_64_nttp_integral_conversion.cpp -o /tmp/t64 && /tmp/t64; echo $?
clang++-18 -std=c++20 -fsyntax-only tests/tmpl/test_tmpl_64_nttp_integral_conversion.cpp; echo "clang rc=$?"

# ⑥ 三条窄化负例（期望 exit=1，文案与 clang 逐字同）
for n in 65 66 67; do
  f=$(ls tests/tmpl/test_tmpl_${n}_*.cpp)
  ./minicc "$f" -o /tmp/n$n 2>&1 | grep '\[ERROR\]'
  ./minicc "$f" -o /tmp/n$n >/dev/null 2>&1; echo "  minicc rc=$?"
  clang++-18 -std=c++20 -fsyntax-only "$f" 2>&1 | head -1
done

# ⑦ template<auto V>：形态即类型 ⇒ 各自成实例 + 全特化只吃 int 形态
./minicc tests/tmpl/test_tmpl_68_nttp_auto_param.cpp -o /tmp/t68 && /tmp/t68; echo $?

# ⑧ 单测（10 例：四条判据分支 + 三条不变量）
./build-linux/unit_tests --gtest_filter='NttpTypeDomain.*'

# ⑨ 全量回归
ctest --test-dir build-linux -j6
./logdiff.sh diff
```

---

## 5. 边界（本项目**未做**的）

| 边界 | 现状 | 说明 |
|---|---|---|
| **任意常量表达式** `Buf<2+2>` / `Buf<k>` | ❌ Parse Error | 仍是 ROADMAP 主线 D 的全部剩余工作。★ 下游（归一 / 可表示性 / 缓存键 / mangling）**已全部就绪**，主线 D 只需在 `parseTemplateArgumentList` 里把"字面量分支"换成"完整常量表达式分支" |
| 字面量的**溢出自动升格** | ❌ | [lex.icon]/2 规定十进制无后缀 `3000000000` 的类型是 `long`（int 装不下就往下一档）。本实现按 `stoull` 直读，只报 `out_of_range` |
| `\x41` / `\101` 转义 | ❌ | `scanChar` 只认 simple-escape-sequence（`\n \t \r \0 \\ \' \"`），十六进制/八进制转义未做 |
| 通用字符名 `é` / `U'x'` / `u8'x'` | ❌ | 需要 UTF 码点支持，属字符集整条线 |
| 非整型 NTTP（指针 / 引用 / 枚举 / 字面量类） | ❌ 报错 | ③ 那道检查明确拒绝，文案 `unsupported type … (only integral types and 'auto' are supported)` |
| `template<auto V>` 的 **NTTP 特化模式** | 部分 | 全特化 `K<4>` 可用；**偏特化**模式 `K<V>` 里的 `V` 尚不参与模式匹配 |
| 函数模板的 `auto` NTTP + `TnDa` | ❌ | 函数模板的显式 NTTP 实参一律报 `not supported yet`，故无 `TnDa` 路径（§1.7） |
| `unsigned long` 的字面量后缀 `ul` / `llu` 组合 | ✅ | 已支持大小写任意组合 |
| 形参 `wchar_t` / `char16_t` / `char32_t` / `char8_t` | ❌ | 需要对应的 `KwWChar…` 关键字与 TypeKind |

---

## 6. clang 源码对照表

| clang 位置 | 干什么 | 本项目对应物 | 简化了什么 |
|---|---|---|---|
| `clang/lib/Lex/Lexer.cpp: LexNumericConstant` | 吃完整数字面量，产 `tok::numeric_constant` | `src/lexer.cpp: scanNumber` | 不做 PPMinWidth/最大 munch 的细节；只切原文 |
| `clang/lib/Lex/LiteralSupport.cpp: NumericLiteralParser` | 解析进制/后缀，定 `(value, type)` | `src/parser.cpp: parseIntLiteral` | 不实现溢出升格候选表；`stoull` + 异常 |
| `clang/lib/Lex/LiteralSupport.cpp: ParseCharLiteral` | 字符字面量求值（含多字符折叠） | `src/lexer.cpp: scanChar` + `parsePrimaryExpr` 的折叠 | 只认 simple-escape；不处理 `\x`/`\u` |
| `clang/lib/Parse/ParseExprCXX.cpp: ParseTemplateArgument` | 实参分流（`tok::char_constant` / `tok::kw_true` / 数值） | `Parser::parseTemplateArgumentList` | 只认字面量 + 一元负号，不解析表达式 |
| `clang/lib/Sema/DeclSpec.cpp: SetTypeSpecType` / `TypeSpecifierType` | type-specifier-seq 的收集与归一（四组位掩码） | `Parser::parseBuiltinTypeSpecifierSeq` | 用六个计数器代替位掩码；不支持 `_Complex` 等 |
| `clang/lib/Sema/SemaTemplate.cpp: CheckTemplateArgument` | 值位实参的合法性（含 `CheckConvertedConstantExpression`） | `checkTemplateArguments` ③-b | 只覆盖整型；不实现用户定义转换与 `constexpr` 求值 |
| `clang/lib/AST/ExprConstant.cpp: CheckConvertedConstantExpression` / `EvaluateAsInt` | converted constant expression 的求值 | `Type::canRepresentValue` | ❌ **不做到这里** —— 本实现只做"值是否装得下"，不做表达式求值（那是主线 D） |
| `clang/lib/AST/ItaniumMangle.cpp: mangleTemplateArg` / `mangleType` | `<expr-primary>` 与 `<builtin-type>` | `NameMangler::encodeType` / `encodeValueArg` | 无替换表（B8）；不发射函数模板的 `TnDa` |
| `clang/lib/Sema/SemaTemplateInstantiate.cpp` + `ASTContext` 的 specialization 缓存 | 实例的唯一化 | `getOrInstantiateClass` + `losslessArgumentsKey` | 用字符串键而非 `TemplateArgument` 的等价类 |

★ 最关键的一条对照：clang 的 converted constant expression 检查
**会真的求值**（`Buf<2+2>` 走 `ExprConstant` 求成 4），本实现只做"读字面量 + 可表示性"
两件事 —— **中间那一层（求值）就是主线 D**。清楚了这一点，主线 D 的边界也就清楚了：
它要在 `parseIntLiteral` 与 `canRepresentValue` **之间**插一层求值器，
两端的接口都已经稳定。

---

## 7. 关键日志片段（`test_tmpl_68` 实跑）

```
  [parse:targ] ★ non-type argument (NTTP): integer literal 4 (形态 int)      ← K<4>
  [parse:targ] ★ non-type argument (NTTP): integer literal 4 (形态 long)     ← K<4L>
  [parse:targ] ★ non-type argument (NTTP): char literal 'x' (120)            ← K<'x'>
  [parse:targ] ★ non-type argument (NTTP): bool literal true (1)             ← K<true>

  [sema:targ]   ⤷ auto 形参推导（[temp.param]/6）：'V' := int （类型由实参反推）
  [sema:targ]   ✓ param 1: 'V' (non-type) ← 4
  [sema:targ] ✓ template arguments OK: K<4>
  [spec:select] ★ selecting class template 'K' for <4>
  [spec:select]   ├─ ① explicit specialization matched (exact argument equality) → USING IT
  [instantiate:name] 'K<4>' → 汇编符号前缀 'K_4'
  ║ Instance:  K_4
  ║ Mangled: K_4 → _Z1KILi4EE

  [sema:targ]   ⤷ auto 形参推导（[temp.param]/6）：'V' := long （类型由实参反推）
  [sema:targ]   ✓ param 1: 'V' (non-type) ← 4
  [sema:targ] ✓ template arguments OK: K<4>            ← ★ 可读串仍是 K<4>！
  [spec:select]   ├─ ① no explicit specialization matched      ← 不吃全特化（形态是 long）
  [spec:select]   └─ ③ falling back to PRIMARY template
  [instantiate:name] 'K<4:long>' → 汇编符号前缀 'K_4Clong'     ← ★ 这里才带形态
  ║ Instance:  K_4Clong
  ║ Mangled: K_4Clong → _Z1KILl4EE
```

★ 上面那两行 `✓ template arguments OK: K<4>` 一模一样 —— 这正是"可读串不是单射"
的现场：**决策已经在缓存键那一层做完了，人看的日志仍然可以不含形态**。
把 `TemplateArg::toString()` 改成全局带形态，会让所有既有模板的日志漂移
（比如 `OnlyTrue<1,int>` 的键串）；本批的选择是**只在真正需要区分的地方
（缓存键 / 实例名）携带形态**，日志保持人类可读。

对照转换用例（`test_tmpl_64`，回归 B17）：

```
  [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'1'(int)  ⇒ 'bool'（转换后常量表达式）
  [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'4'(long) ⇒ 'int' （转换后常量表达式）
  [sema:targ]   ⤷ 值位整型转换（[temp.arg.nontype]/1）：'1'(bool) ⇒ 'int' （转换后常量表达式）
```

对照窄化用例（`test_tmpl_65`，文案与 clang 逐字相同）：

```
[ERROR] [Semantic Error] 1:1: non-type template argument evaluates to 2,
  which cannot be narrowed to type 'bool'

$ clang++-18 -std=c++20 -fsyntax-only tests/tmpl/test_tmpl_65_error_nttp_bool_narrowing.cpp
test_tmpl_65_error_nttp_bool_narrowing.cpp:37:7: error: non-type template argument
  evaluates to 2, which cannot be narrowed to type 'bool' [-Wc++11-narrowing]
```

---

## 8. 一句话总结

本批把 NTTP 的**类型域**从 `int` 一个点扩成"整型家族 + 字符 + `auto`"，
但真正的收获是**三个判据的归位**：

1. **形态由形参表裁定**（承 [33](33-template-template-params.md)）——
   实参写成 `4L` 还是 `4` 不重要，重要的是**形参要什么**；
2. **合法性的判据是"可表示性"而不是"形态相等"**（修 B17）——
   这一条只能靠**正向用例**撞出来，负向全绿说明不了任何事；
3. **缓存键与实例名必须是单射**，而"给人看的字符串"从来不是 ——
   这个坑本项目已经踩过三次（[23](23-cv-position-and-type-identity.md) / B13~B15 / 本批的 auto 位）。
