# minicc —— Mini C++ Compiler

一个用现代 C++20 编写的教学级 C++ 编译器，覆盖从源码到 x86-64 汇编的完整编译流水线。

**核心理念**：编译过程中的每一步都会输出详细的中文日志，让你"看见"编译器在做什么。

---

## 目录

- [快速开始](#快速开始)
- [编译编译器本身](#编译编译器本身)
- [使用 minicc 编译测试文件](#使用-minicc-编译测试文件)
- [命令行参数](#命令行参数)
- [编译日志详解](#编译日志详解)
- [模板测试用例一览](#模板测试用例一览)
- [编译器架构（6 大阶段）](#编译器架构6-大阶段)
- [项目结构](#项目结构)
- [核心概念详解](#核心概念详解)
- [支持的语法特性](#支持的语法特性)
- [学习路线建议](#学习路线建议)
- [常见问题](#常见问题)

---

## 快速开始

```bash
# 1. 编译编译器
cd mycompiler
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(sysctl -n hw.ncpu)

# 2. 回到项目根目录，编译一个测试文件
cd ..
./build/minicc tests/test_tmpl_01_basic.cpp

# 3. 终端会打印 6 个阶段的完整编译日志 ✨
```

---

## 编译编译器本身

### 环境要求

| 依赖 | 最低版本 | 说明 |
|------|---------|------|
| CMake | 3.20+ | 构建系统 |
| C++ 编译器 | 支持 C++20 | Apple Clang / GCC / LLVM Clang |

### 编译步骤

```bash
cd mycompiler

# 创建构建目录（推荐用 build/）
mkdir -p build && cd build

# 配置
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 编译（使用所有 CPU 核心加速）
cmake --build . -j$(sysctl -n hw.ncpu)
# Linux 上用: cmake --build . -j$(nproc)
```

编译成功后产出 `build/minicc` 可执行文件。

### 重新编译

修改源码后只需在 `build/` 目录下重新执行：

```bash
cd build
cmake --build . -j$(sysctl -n hw.ncpu)
```

CMake 会自动检测变更，只重编译修改过的文件。

---

## 使用 minicc 编译测试文件

### 基本用法

```bash
# 在项目根目录下执行
./build/minicc <源文件.cpp> [选项]
```

**所有编译日志自动打印到终端**，不需要额外开关。

### 常用命令

```bash
# ── 基本编译：默认直出可执行文件（内部自动 as + 自研链接器）──
./build/minicc tests/test_tmpl_01_basic.cpp -o /tmp/demo && /tmp/demo

# ── 只输出汇编（-S 跳过链接，等价旧行为）──
./build/minicc tests/test_tmpl_01_basic.cpp -S -o output.s

# ── 额外打印 Token 流（词法分析细节）──
./build/minicc tests/test_tmpl_01_basic.cpp --dump-tokens

# ── 额外打印 AST 树（语法分析细节）──
./build/minicc tests/test_tmpl_01_basic.cpp --dump-ast

# ── 全开（Token + AST + 全部日志）──
./build/minicc tests/test_tmpl_01_basic.cpp --dump-tokens --dump-ast -o output.s
```

### 批量运行所有测试

```bash
for f in tests/test_tmpl_*.cpp; do
    echo "========== $(basename $f) =========="
    ./build/minicc "$f" -o /dev/null
    echo ""
done
```

### 只看某个阶段的日志（用 grep 过滤）

```bash
# 只看模板替换过程（Phase 4）
./build/minicc tests/test_tmpl_07_reference_collapse.cpp 2>&1 | grep -A500 "Phase 4"

# 只看内存布局
./build/minicc tests/test_tmpl_09_mixed.cpp 2>&1 | grep -A20 "Memory Layout"

# 只看 auto 推导
./build/minicc tests/test_tmpl_01_basic.cpp 2>&1 | grep "\[auto\]"

# 只看符号表
./build/minicc tests/test_tmpl_09_mixed.cpp 2>&1 | grep -A30 "SYMBOL TABLE"

# 只看引用折叠
./build/minicc tests/test_tmpl_07_reference_collapse.cpp 2>&1 | grep "Reference collapsing"

# 只看 Name Mangling
./build/minicc tests/test_tmpl_06_multi_param.cpp 2>&1 | grep "Mangled:"
```

### 查看生成的汇编代码

```bash
# 编译并输出汇编
./build/minicc tests/test_tmpl_01_basic.cpp -o output.s 2>/dev/null
#                                   注意 2>/dev/null 抑制日志，只看汇编

# 查看汇编
cat output.s
```

---

## 命令行参数

| 参数 | 说明 |
|------|------|
| `<source.cpp>` | **必填**，输入的 C++ 源文件 |
| `-o <output.s>` | 指定输出的汇编文件路径（默认：同名 `.s` 文件） |
| `--dump-tokens` | 额外打印词法分析的 Token 流 |
| `--dump-ast` | 额外打印语法分析的 AST 树结构 |

---

## 编译日志详解

minicc 的一大特色是**每个编译阶段都输出详细的中文日志**，让你看到编译器内部发生了什么。

### 6 个阶段的日志内容

#### Phase 1: 词法分析 (Lexical Analysis)

```
════════════════════════════════════════════════════════════
  Phase 1: Lexical Analysis (词法分析)
════════════════════════════════════════════════════════════
  72 tokens generated
```

加 `--dump-tokens` 可以看到每个 Token：
```
  Token dump:
    17:1   template        'template'
    17:9   <               '<'
    17:10  typename        'typename'
    17:19  Identifier      'T'
    ...
```

#### Phase 2: 语法分析 (Syntax Analysis)

展示模板蓝图的解析过程：

```
  [parse:template] ▶ template declaration at 17:1
  [parse:template]   parsing parameter list <typename T
  [parse:template]   ★ type parameter registered: 'T'
  [parse:template] ◀ template 'Box' with 1 parameter(s) stored as blueprint
  [parse:template]   blueprint summary:
    template <typename T> class Box { ... }
```

加 `--dump-ast` 可以看到 AST 树：
```
  AST dump:
  TemplateDecl: <T>
    ClassDecl: Box
      Field: value : T
      Method: get (normal) T
      Method: set (normal) void
  FunctionDecl: add → int
  FunctionDecl: main → int
```

#### Phase 3: 语义分析 (Semantic Analysis) ★★★ 最核心

这是信息量最大的阶段，包含三遍扫描：

**Pass 1 — 注册类与模板：**
```
  [register] template <typename T> Box (blueprint stored, not analyzed)
  [register] class 'Animal'
    field: age : int
    method: getAge() → int
    ══ Memory Layout of 'Animal' ══
    Total size: 8 bytes
    +0: age : int (4 bytes)
```

**Pass 2 — 注册函数（支持递归调用）：**
```
  [register] add(int a, int b) → int    [mangled: add]
  [register] main() → int    [mangled: main]
  [register] Animal::getAge() → int    [mangled: Animal_getAge]
```

**符号表快照：**
```
  ╔══════════════════════════════════════════════╗
  ║         SYMBOL TABLE (符号表快照)            ║
  ╚══════════════════════════════════════════════╝
    ┌─ Scope[0] 'global' (depth=0) ─────────────────
    │  main         Function   int
    │  add          Function   int
    │  Animal       Type       Animal
    └──────────────────────────────────────
```

**Pass 3 — 函数体分析（auto 推导 + 类型检查）：**
```
  ╔══ Function Body: main ══╗
  [var decl] result : auto =
    [resolve] 'add' → int    (kind=Function, global)
    [call] add(2 args) → int    [symbol resolved]
      [infer] IntLiteral(1) → int
      [infer] IntLiteral(2) → int
    ⟹ inferred: int
  [auto] ★ result : auto ⟹ int   ← auto 被永久替换!
  [symbol] ✚ result : int    stack@-8   ← 加入符号表
```

#### Phase 4: 模板实例化 (Template Instantiation) ★★ 最精彩

每个模板蓝图会自动用多种类型实例化：

**替换映射：**
```
  ╔══ Template Instantiation ═════════════════════════╗
  ║ Blueprint: Box <T>
  ║ Instance:  Box_int
  ║ Substitution map: { 'T' → 'int', }
```

**字段替换过程：**
```
  ║ ── Field Substitution ──
  ║   field 'value' : T → int
  ║     [subst] ★ Class 'T' matches template param → 'int'
```

**嵌套指针的递归替换：**
```
  ║   field 'doublePtr' : T** → int**
  ║     [subst] Pointer(T**) → recursing into pointee...
  ║     [subst] Pointer(T*) → recursing into pointee...
  ║     [subst] ★ Class 'T' matches template param → 'int'
  ║     [subst] ★ Pointer substituted: T* → int*
  ║     [subst] ★ Pointer substituted: T** → int**
```

**引用折叠（万能引用的核心机制）：**
```
  ║   field 'data' : T&& → int&
  ║     [subst] RValueRef(T&&) → recursing into referenced type...
  ║     [subst] ★ Class 'T' matches template param → 'int&'
  ║     [subst] ★ Reference collapsing: int&&& → int& (lvalue ref wins!)
```

**Name Mangling：**
```
  ║ Mangled: Box_int → _Z3BoxIiE
  ╚═══════════════════════════════════════════════════╝
```

#### Phase 5: 代码生成 (Code Generation)

```
  Generating x86-64 assembly...
  Assembly written to: output.s
```

输出的 `.s` 文件就是 x86-64 汇编代码。

#### Phase 6: 链接 (Linking)

```
  To assemble and link:
    gcc -o output output.s -lstdc++
```

---

## 模板测试用例一览

每个文件专注一种模板模式，位于 `tests/` 目录：

| 文件 | 测试场景 | 核心看点 |
|------|---------|---------|
| `test_tmpl_01_basic.cpp` | 基础单参数 `template<typename T>` | T → int/double/int\*/int&/int&&/const int& 六种替换 |
| `test_tmpl_02_pointer.cpp` | 指针类型 `T*`, `T**` | 嵌套指针递归替换过程 |
| `test_tmpl_03_reference.cpp` | 左值引用 `T&` + 右值引用 `T&&` | 引用替换 + 引用折叠 |
| `test_tmpl_04_const_ref.cpp` | 常量引用 `const T&` | 三层递归：Const → LValueRef → TemplateParam |
| `test_tmpl_05_class_keyword.cpp` | `template<class T>` 写法 | class vs typename 关键字差异 |
| `test_tmpl_06_multi_param.cpp` | 双参数 `template<typename T, typename U>` | 两个参数同时替换 |
| `test_tmpl_07_reference_collapse.cpp` | **引用折叠**（万能引用） | T&& 当 T=int& 折叠为 int&，T=int&& 保持 int&& |
| `test_tmpl_08_triple_param.cpp` | 三参数 + class/typename 混合 | `template<class T, typename U, class V>` |
| `test_tmpl_09_mixed.cpp` | 模板 + 普通类 + 普通函数共存 | 模板蓝图不影响普通代码分析 |
| `test_tmpl_10_multi_blueprint.cpp` | 多个模板蓝图共存 | 同一文件两个模板分别实例化 |

### 推荐的学习顺序

```
1. test_tmpl_01_basic.cpp          ← 从这里开始，理解最基本的替换
2. test_tmpl_05_class_keyword.cpp  ← class vs typename 只是语法差异
3. test_tmpl_02_pointer.cpp        ← 看嵌套指针如何递归替换
4. test_tmpl_03_reference.cpp      ← 引用的替换
5. test_tmpl_04_const_ref.cpp      ← const 的层层递归
6. test_tmpl_07_reference_collapse.cpp  ← ★ 最精彩：引用折叠
7. test_tmpl_06_multi_param.cpp    ← 多参数同时替换
8. test_tmpl_08_triple_param.cpp   ← 三参数 + 混合关键字
9. test_tmpl_09_mixed.cpp          ← 模板和普通代码共存
10. test_tmpl_10_multi_blueprint.cpp ← 多蓝图分别实例化
```

---

## 编译器架构（6 大阶段）

```
源码 (.cpp)
  │
  ├─ [阶段 1] 词法分析 (Lexer)             src/lexer.cpp
  │   源码字符串 → Token 流
  │   识别 auto/virtual/template/class 等关键字
  │
  ├─ [阶段 2] 语法分析 (Parser)             src/parser.cpp
  │   Token 流 → 抽象语法树 (AST)
  │   递归下降解析，模板作为"蓝图"暂存
  │
  ├─ [阶段 3] 语义分析 (SemanticAnalyzer)   src/semantic_analyzer.cpp  ★ 核心
  │   三遍扫描：
  │     Pass 1: 注册类和模板蓝图
  │     Pass 2: 注册所有函数（支持递归调用）
  │     Pass 3: 分析函数体（类型推导 + auto 消除 + 符号决议）
  │   产出：符号表、内存布局、vtable 结构
  │
  ├─ [阶段 4] 模板实例化 (TemplateInstantiator)  src/template_instantiation.cpp
  │   AST 蓝图深拷贝 + 模板参数递归替换
  │   引用折叠：T&& 当 T=int& 时 → int&
  │   Name Mangling：MyPtr<int> → _Z5MyPtrIiE
  │
  ├─ [阶段 5] 代码生成 (CodeGen)             src/codegen.cpp
  │   AST → x86-64 汇编 (.s)
  │   字段访问 → [base + offset]（字段名消失，只剩数字）
  │   虚函数调用 → 三部曲 (读vptr → 加偏移 → 间接跳转)
  │
  └─ [阶段 6] 链接 (Linker)
      简化：留给系统链接器 (gcc/ld)
```

### 源码文件对应关系

| 阶段 | 头文件 | 实现 | 核心职责 |
|------|--------|------|---------|
| 1 | `include/token.h` `include/lexer.h` | `src/lexer.cpp` | 源码 → Token 流 |
| 2 | `include/ast.h` `include/parser.h` | `src/parser.cpp` | Token → AST |
| — | `include/type.h` | `src/type.cpp` | 类型系统（贯穿全程） |
| 3 | `include/semantic_analyzer.h` | `src/semantic_analyzer.cpp` | 类型推导 + 内存布局 |
| 4 | `include/template_instantiation.h` | `src/template_instantiation.cpp` | 模板克隆 + 替换 |
| 5 | `include/codegen.h` | `src/codegen.cpp` | AST → x86-64 汇编 |
| — | — | `src/main.cpp` | 编译器驱动（串联6阶段） |

---

## 项目结构

```
mycompiler/
├── CMakeLists.txt                  # 构建配置 (C++20, CMake 3.20+)
├── README.md                       # 本文档
├── include/                        # 头文件
│   ├── token.h                     #   Token 类型定义
│   ├── lexer.h                     #   词法分析器接口
│   ├── ast.h                       #   AST 节点定义
│   ├── parser.h                    #   语法分析器接口
│   ├── type.h                      #   类型系统定义
│   ├── semantic_analyzer.h         #   语义分析器接口
│   ├── template_instantiation.h    #   模板实例化接口
│   └── codegen.h                   #   代码生成器接口
├── src/                            # 实现文件
│   ├── lexer.cpp                   #   阶段 1：词法分析
│   ├── parser.cpp                  #   阶段 2：语法分析
│   ├── type.cpp                    #   类型系统实现
│   ├── semantic_analyzer.cpp       #   阶段 3：语义分析 ★
│   ├── template_instantiation.cpp  #   阶段 4：模板实例化 ★
│   ├── codegen.cpp                 #   阶段 5：代码生成
│   └── main.cpp                    #   编译器主程序
├── tests/                          # 测试用例
│   ├── test_tmpl_01_basic.cpp          # 基础单参数模板
│   ├── test_tmpl_02_pointer.cpp        # 指针类型 T*, T**
│   ├── test_tmpl_03_reference.cpp      # 引用 T&, T&&
│   ├── test_tmpl_04_const_ref.cpp      # 常量引用 const T&
│   ├── test_tmpl_05_class_keyword.cpp  # class vs typename
│   ├── test_tmpl_06_multi_param.cpp    # 双参数模板
│   ├── test_tmpl_07_reference_collapse.cpp  # 引用折叠 ★
│   ├── test_tmpl_08_triple_param.cpp   # 三参数模板
│   ├── test_tmpl_09_mixed.cpp          # 模板 + 普通代码混合
│   └── test_tmpl_10_multi_blueprint.cpp # 多蓝图共存
└── build/                          # 构建输出
    └── minicc                      #   ← 编译器可执行文件
```

---

## 核心概念详解

### 1. auto 类型推导

```
源码:    auto x = 10;
           ↓ 语义分析阶段 (Phase 3)
编译后:  int x = 10;    (auto 被永久替换为 int，从 AST 中彻底消失)
```

**日志中这样看：**
```
  [var decl] result : auto = ...
    ⟹ inferred: int
  [auto] ★ result : auto ⟹ int   ← auto 被永久替换!
```

auto 只是语法糖。编译器推导右边表达式的类型，将 AST 中的 auto 占位符**当场替换**为真实类型。

### 2. 模板实例化

```
蓝图:    template<typename T> class Box { T value; };
           ↓ 模板实例化阶段 (Phase 4)
实例:    class Box_int { int value; };   (T 被替换为 int)
符号:    _Z3BoxIiE                       (Name Mangling)
```

**日志中这样看：**
```
  ╔══ Template Instantiation ═════════════════════════╗
  ║ Blueprint: Box <T>
  ║ Instance:  Box_int
  ║ Substitution map: { 'T' → 'int', }
  ║   field 'value' : T → int
  ║ Mangled: Box_int → _Z3BoxIiE
  ╚═══════════════════════════════════════════════════╝
```

模板实例化是**编译期的"复制粘贴"**——但不是简单的文本替换，而是 AST 层面的结构化克隆与类型递归替换。

### 3. 引用折叠（Reference Collapsing）

C++ 标准规定，当引用嵌套时按以下规则折叠：

```
T&  &   → T&     左值引用 + 左值引用 → 左值引用
T&  &&  → T&     左值引用 + 右值引用 → 左值引用
T&& &   → T&     右值引用 + 左值引用 → 左值引用
T&& &&  → T&&    右值引用 + 右值引用 → 右值引用
```

**简言之：只要有一个是左值引用，结果就是左值引用。**

这就是万能引用 (Forwarding Reference) 的原理：

```cpp
template<typename T>
void foo(T&& x);

foo(42);     // T = int,    T&& = int&&       (右值引用)
foo(var);    // T = int&,   T&& = int& && → int&  (折叠为左值引用)
```

**日志中这样看：**
```
  [subst] RValueRef(T&&) → recursing into referenced type...
  [subst] ★ Class 'T' matches template param → 'int&'
  [subst] ★ Reference collapsing: int&&& → int& (lvalue ref wins!)
```

### 4. 虚函数调用三部曲

```
源码:    ptr->foo();           (多态调用，编译期不知道具体类型)

汇编:    movq (%rdi), %rax     # (a) 从对象偏移量 0 读出 _vptr
         movq 16(%rax), %rax   # (b) 从 vtable[index] 读出函数地址
         callq *%rax           # (c) 间接跳转到该地址执行
```

**编译期看符号**：`ptr->foo()` 查符号表得到 vtable index
**运行期看偏移量**：通过指针运算找到真正的函数地址

### 5. vtable 内存布局

```
vtable (GCC ABI 简化版):
  [-2] = 0                    # offset to top
  [-1] = &type_info           # RTTI 指针
  [0]  = &virtual_func_0      # 第一个虚函数
  [1]  = &virtual_func_1      # 第二个虚函数

对象内存:
  +0:  _vptr → 指向 vtable[0]
  +8:  field_1
  +12: field_2
```

### 6. 字段访问的消除

```
源码:    rect.width = 10;       (使用字段名)
编译期:  width 偏移量 = 16     (查 ClassLayout)
汇编:    movl $10, 16(%rax)     (直接用偏移量，字段名消失)
```

### 7. Name Mangling

C++ 支持函数重载和模板，但汇编器只认唯一的名字。Name Mangling 将完整签名编码为唯一字符串：

```
MyPtr<int>          → _Z5MyPtrIiE       (i = int)
MyPtr<double>       → _Z5MyPtrIdE       (d = double)
MyPtr<int*>         → _Z5MyPtrIPiE      (P = pointer)
MyPtr<int&>         → _Z5MyPtrIRiE      (R = lvalue reference)
MyPtr<int&&>        → _Z5MyPtrIOiE      (O = rvalue reference)
MyPtr<const int&>   → _Z5MyPtrIRKiE     (K = const)
Pair<int, double>   → _Z4PairIidE       (多参数)
MyClass::foo(int)   → _ZN7MyClass3fooEi (类方法)
```

---

## 支持的语法特性

- [x] 基础类型：int, double, bool, void
- [x] auto 类型推导（编译期消除）
- [x] 函数声明与调用
- [x] 类定义（字段 + 方法）
- [x] 继承（public 继承）
- [x] 虚函数与 override
- [x] 多态调用（虚函数调用三部曲）
- [x] new 表达式（内存分配 + vptr 初始化）
- [x] 模板类：`template<typename T>` 和 `template<class T>`
- [x] 模板多参数：`template<typename T, typename U>`
- [x] 模板类型：T\*, T\*\*, T&, T&&, const T&
- [x] 引用折叠（Reference Collapsing）
- [x] 控制流：if/else, while
- [x] 算术/逻辑/比较运算符
- [x] 成员访问（. 和 ->）
- [x] Name Mangling（GCC ABI 风格）
- [x] 函数模板声明解析：`template<typename T> T f(T x)`（S1：解析 + 蓝图注册 + 候选集，文档 docs/learn/01）
- [x] 函数模板实参推导：逐对 P/A 合一，T / T& / T&&（万能引用折叠）/ const T& / T* 形态，多次绑定一致性检查（S2，文档 docs/learn/02）
- [x] 显式模板实参：`mix<int>(1, 2)` 前缀规则 + template-id 歧义消解（S3，文档 docs/learn/03）
- [x] 不可推导上下文诊断：T 只在返回类型等位置时报错并建议显式指定（S4，文档 docs/learn/04）
- [x] 函数模板隐式实例化：调用点驱动、mangled 符号（_Z5twiceIiE）、实例缓存、实例体二次语义分析（S5，文档 docs/learn/05）
- [x] 重载决议：非模板优先 + deduction-based 偏序选最特化（S6，文档 docs/learn/06）
- [x] 隐式转换：左值到右值（引用剥除，[conv.lval]）、int→double 提升（[conv.promo]）
- [x] 预处理器：`#include "..."`/`<...>` 搜索路径、`#define` 对象宏/函数宏（递归展开+涂蓝）、`#ifdef/#if/#elif/#else/#endif`、`#pragma once`、`__LINE__/__FILE__`、`-E` 选项（P0，文档 docs/learn/07）

---

## 学习路线建议

### 第一步：读源码理解架构

```
1. include/token.h + src/lexer.cpp           ← 词法分析（最简单）
2. include/ast.h + src/parser.cpp            ← 语法分析（递归下降）
3. src/semantic_analyzer.cpp                 ← ★ 核心：类型推导 + 内存布局
4. src/template_instantiation.cpp            ← 模板克隆 + 替换
5. src/codegen.cpp                           ← 汇编代码生成
```

### 第二步：运行测试看日志

```bash
# 从最简单的开始
./build/minicc tests/test_tmpl_01_basic.cpp

# 看引用折叠——模板最精彩的部分
./build/minicc tests/test_tmpl_07_reference_collapse.cpp

# 看多参数替换
./build/minicc tests/test_tmpl_06_multi_param.cpp

# 看普通类和模板共存
./build/minicc tests/test_tmpl_09_mixed.cpp
```

### 第三步：修改源码加特性

试着在模板测试文件中添加新的模板类，观察编译器的处理过程。

---

## 常见问题

### Q: `cmake-build-debug/` 目录编译失败？

`cmake-build-debug/` 是 CLion IDE 自动生成的构建目录。如果链接失败，是因为 Homebrew LLVM 版本和系统 C++ 标准库不兼容。

**解决办法**：使用 `build/` 目录手动编译（见上方"编译步骤"）。

### Q: 日志太多了看不清？

用 grep 过滤只看你关心的阶段：

```bash
# 只看 Phase 4 模板替换
./build/minicc tests/test_tmpl_01_basic.cpp 2>&1 | grep -A500 "Phase 4"

# 只看某个关键字
./build/minicc tests/test_tmpl_01_basic.cpp 2>&1 | grep "Mangled:"
```

### Q: 模板类在 main() 中没有使用，为什么也会被实例化？

minicc 的 `main.cpp` 中对每个模板蓝图**自动用 6 种类型实例化**（int, double, int\*, int&, int&&, const int&），用于演示替换过程。实际编译器（如 GCC/Clang）只在代码中真正使用时才实例化。

### Q: 输出的汇编能直接运行吗？

需要系统汇编器和链接器：

```bash
./build/minicc tests/test_tmpl_01_basic.cpp -o output.s
gcc -o output output.s -lstdc++
./output
```

### Q: 如何添加新的测试文件？

在 `tests/` 目录下创建 `.cpp` 文件，遵循以下规则：
- 模板类用 `template<typename T>` 或 `template<class T>` 声明
- 必须有 `int main()` 函数作为入口
- 普通类和普通函数可以和模板共存
