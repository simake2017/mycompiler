# 07 预处理器（P0）

> 主线 P0：#include / #define / 条件编译 / #pragma once。
> 预处理是"编译之前的编译"——C++ 标准 [cpp.phase] 把它定义为独立的翻译阶段 2~4。

## ① 理论背景

**标准依据**：[cpp.phase]（翻译阶段）、[cpp.include]、[cpp.cond]、[cpp.replace]/[cpp.subst]/[cpp.rescan]。

- **翻译阶段视角**：阶段 2 行拼接（`\`+换行）→ 阶段 3 拆分为预处理记号、
  识别指令行 → 阶段 4 宏展开 + include 并合。预处理完成后，
  后续阶段看到的才是"干净"的程序文本。
- **include 搜索算法**（HeaderSearch）：
  `"..."` 先搜**包含者所在目录**（自包含友好），再搜 `-I` 序列；
  `<...>` 直接搜 `-I` → 系统目录。顺序即优先级，全部落空报错并列出搜索过的路径。
- **#pragma once vs include guard**：按文件（canonical 路径）而非宏名去重，
  无需手写 `#ifndef X / #define X`。clang 对传统 guard 也有"多次包含检测"
  优化（识别出 guard 模式后等价于 pragma once）。
- **宏展开三定律**：
  1. **实参先展开**（除非用于 `#`/`##`）——`add(1, ANSWER)` 中 `ANSWER` 先变 42；
  2. **替换按词边界**——参数名在串字面量内、作为更长词的一部分都不替换；
  3. **重扫描 + 涂蓝**（[cpp.rescan]）：展开结果继续扫描找宏，
     但正在展开的宏名被"涂蓝"（hide 集），自引用不再展开——
     这就是 `#define A A` 不会死循环的原因。
- **条件编译语义**：`#elif`/`#else` 互斥靠 `takenBranch` 标志；
  `defined(X)` 必须在宏展开**之前**求值（否则 `defined` 本身可能被展开掉）；
  `#if` 中未定义标识符按 **0** 处理（所以 `#if 某平台宏` 是安全写法）。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| **文本级 vs token 级** | 行/文本级：预处理产出展开后的纯文本，再交给既有 Lexer | clang 是 token 级（更精确，能保留记号边界）；文本级实现量小一个数量级，教学上"看得见全文"（`-E`） |
| 行号保持 | 指令行输出空行占位 | include 插入会使主文件后续行号偏移——已知简化，clang 用 linemarker（`# 12 "file"`）解决 |
| hide 集传值 | `expand(text, hide)` 按值传递再 insert | 天然实现作用域式涂蓝，无需手动进出栈 |
| #if 求值 | 三步：defined → 宏展开 → 递归下降（‖/&&/比较/加减乘除/一元!-） | 对照 PPEpressions.cpp 的运算符优先级表裁剪 |
| 不支持项 | `#` 字符串化、`##` 粘贴、变参宏、`_Pragma`、宏内注释语义 | 都是记号级操作，文本级实现代价高、理论增量低，明确写进文档 |

## ③ clang 对照表

| 本实现 | clang 源 | 简化了什么 |
|---|---|---|
| `processText` 指令循环 + 条件栈 | `lib/Lex/PPDirectives.cpp`（指令分派）+ `PPConditionalDirectiveRecord` | 无 `#line`/`#assert`、无诊断 pragma、条件栈只记活跃性 |
| `resolveInclude` | `lib/Basic/HeaderSearch.cpp → LookupFile` | 无 `#include_next`、无 module map、无 framework 搜索、无 guard 宏识别优化 |
| `expand` + `substituteParams` | `lib/Lex/PPMacroExpansion.cpp + TokenConcatenation` | 无 `#`/`##`、无变参 `__VA_ARGS__`、无延迟展开（delayed expansion） |
| `evalConstantExpr` | `lib/Lex/PPExpressions.cpp → EvaluateDirective` | 无字符常量、无枚举/sizeof（标准上 #if 不允许 sizeof，但允许真常量表达式） |
| `#pragma once` 按 canonical 路径 | `HeaderSearch` 的 `ShouldEnterIncludeFile` + 文件 ID 去重 | 无硬链接/符号链接同文件识别（weakly_canonical 已覆盖大部分） |

## ④ 实验手册

```bash
cd /home/magene/runtime/cppproject/mycompiler

# -E：只看预处理结果（同 gcc -E），最直观
./build-linux/minicc tests/test_pp_01_include.cpp -E | head -30

# include + pragma once（日志：skipped (pragma once)）
./build-linux/minicc tests/test_pp_01_include.cpp -o /tmp/pp01.s
gcc /tmp/pp01.s -o /tmp/pp01 && /tmp/pp01; echo $?          # 0

# 宏展开 trace（[pp] expand 逐条）
./build-linux/minicc tests/test_pp_02_macros.cpp 2>&1 | grep "\[pp\] .* expand"

# 条件分支决策（branch taken/skipped）
./build-linux/minicc tests/test_pp_03_conditional.cpp 2>&1 | grep "branch"

# 错误用例：#ifdef 无 #endif
./build-linux/minicc tests/test_pp_04_error_unterminated.cpp; echo $?   # 1

# oracle：与真预处理器对比
clang++-18 -E tests/test_pp_02_macros.cpp | tail -8     # 对照宏展开形态
gcc -E tests/test_pp_03_conditional.cpp | grep -A2 main # 对照条件结果
```

## ⑤ 关键过程图

```
源文本（主文件）
  │ 阶段2：行拼接（'\'+换行）
  ▼
逐行扫描 ──'#'开头?──是──→ 指令分派
  │ 否                        ├─ ifdef/if：条件栈 push（parentActive && eval）
  │                          ├─ define ：MacroDef{name, params?, body} 入表
  │                          ├─ include：resolveInclude 搜索路径
  │                          │           ├─ canonical ∈ pragmaOnce? → skip
  │                          │           └─ 递归 processText(头文件) 并合
  │                          └─ endif：pop（栈空检查在 EOF）
  ▼ 活跃分支的普通行
expand(line, hide={})
  ├─ 词 == 宏名？
  │   ├─ 对象宏：expand(body, hide∪{名})        ← 涂蓝防自引用
  │   └─ 函数宏：收实参(括号配平) → 实参各自 expand
  │             → substituteParams(词边界替换) → 整体重扫描
  └─ __LINE__/__FILE__ → 动态替换
  ▼
展开后纯文本 → Lexer（阶段5 起）
```
