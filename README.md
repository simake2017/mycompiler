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
for f in tests/*/*.cpp; do
    echo "========== $(basename $f) =========="
    ./minicc "$f" -o /dev/null
    echo ""
done

# 单测（GoogleTest，159 个用例）
ctest --test-dir build-linux --output-on-failure
```

### 重构安全网：日志基线对比

本项目的「全阶段中文日志」是交付物之一 —— 单测靠子串匹配日志原文，
`docs/learn/` 里的文档直接粘贴日志片段。所以**改内部实现时，
日志必须逐字节不变**。`logdiff.sh` 就是守这条线的：

```bash
./logdiff.sh save     # 改动【前】存基线（84 个集成测试的
                      #   编译日志 + 退出码 + 程序输出 + 生成的汇编全文）
#   ... 改代码 ...
./logdiff.sh diff     # 改动【后】逐字节对比，零差异才放行
```

基线存在 `.logbaseline/`（已 gitignore）。详见 `docs/REFACTOR-ast-visitor.md`。

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
| `test_tmpl_21_nttp_basic.cpp` | **非类型模板参数** `template<int N>` + `Buf<4>` | 值替换：`VarExpr{N}` → `IntLiteral{4}`；符号 `_Z3BufILi4EE` |
| `test_tmpl_22_nttp_mixed.cpp` | 类型 + 非类型混排 `template<class T, int N>` | 逐位按形参 kind 分派，非按实参长相 |
| `test_tmpl_23_nttp_value_distinct.cpp` | 不同值 → 不同实例 `Buf<4>` vs `Buf<8>` | 缓存键必须区分类型实参与值实参 |
| `test_tmpl_24_nttp_negative.cpp` | 负数实参 `Buf<-3>` | Itanium 负数编码 `Lin3E`（实例名清洗为 `Buf__3`） |
| `test_tmpl_25_error_nttp_got_type.cpp` | 错误：NTTP 位收到类型 | `Buf<int>` → must be a non-type argument |
| `test_tmpl_26_error_type_got_value.cpp` | 错误：类型位收到值 | `Box<4>` → must be a type argument |
| `test_tmpl_27_struct_template.cpp` | **`struct` 写模板体** | 与 `class` 模板机制相同，仅默认访问级别不同 |
| `test_tmpl_28_default_arg.cpp` | **默认模板实参** `template<class T, class U = void>` + `Box<int>` | 使用点少写实参 → 补全为 `Box<int, void>` |
| `test_tmpl_29_partial_spec.cpp` | **偏特化** `template<class T> class Box<T*, T>` | 用实参对模式跑合一：`Box<double*,double>` ⇒ `T := double` |
| `test_tmpl_30_explicit_spec.cpp` | **全特化** `template<> class Box<int*, int>` | 空形参表 + 逐位类型相等直接比对，无推导 |
| `test_tmpl_31_spec_selection.cpp` | **三路择优对照** | 四个使用点走完 ①全特化 ②偏特化 ③主模板 三条路径 |
| `test_tmpl_32_error_default_gap.cpp` | 错误：默认实参空洞 | 前位有默认值时后位必须有（[temp.param]/12） |
| `test_tmpl_33_decltype_basic.cpp` | **decltype 基础** `decltype(a)` / `decltype(f(1))` / `decltype(&a)` | 表达式→类型的查询算子；依赖上下文延迟求值 |
| `test_tmpl_34_decltype_paren.cpp` | **decltype 括号规则**（[dcl.type.decltype]） | 多一对括号就变：`decltype(a)`=int vs `decltype((a))`=int&，经 `Probe<T&>` 偏特化观测 |
| `test_tmpl_35_void_t_detect.cpp` | **void_t 探测命中** | `void_t<decltype(declval<T>().begin()), ...>` 全合法 ⇒ 归约为 void ⇒ 命中偏特化 |
| `test_tmpl_36_void_t_fallback.cpp` | **SFINAE 软失败** | 替换失败被候选集消化，静默回退主模板，**不报错** |
| `test_tmpl_37_void_t_partial.cpp` | **全有或全无** | 半吊子类型（只有 begin 没有 end）⇒ 第 1 个探测通过也整条作废 |
| `test_tmpl_38_error_no_fallback.cpp` | 错误：兜底不存在 | 同一替换失败，主模板接不住 ⇒ **升级为硬错误**（与 36 成对） |
| `test_tmpl_39_is_range_demo.cpp` | **用户原用例的 is_range + Box 半边** | `decltype(numbers)` / `decltype(1)` 探测 + `Box<decltype(&a)>` 择优，返回码对齐 clang |
| `test_tmpl_40_order_pointer_depth.cpp` | **偏序裁决** T\* vs T\*\* | [temp.class.order] 互相推导，更特化者胜 |
| `test_tmpl_41_order_decl_independent.cpp` | **偏序与声明顺序无关** | 与 40 构成判别性实验：证伪"取第一个/最后一个匹配者" |
| `test_tmpl_42_order_ambiguous.cpp` | 错误：偏序歧义 | 互不更特化 ⇒ 必须报 ambiguous partial specializations |
| `test_tmpl_43_order_three_way.cpp` | **三路偏序** T\* ⊂ T\*\* ⊂ T\*\*\* | dominance 循环 + 传递性，从 3 个同时匹配的候选里选出唯一赢家 |
| `test_tmpl_44_error_extraneous_template.cpp` | 错误：`template<>` 却没有 `<...>` | 写了空形参表却不点明实参 ⇒ 全特化无从谈起，必须报错（此前**静默当主模板**） |
| `test_tmpl_45_error_undeclared_in_explicit_spec.cpp` | 错误：全特化模式里出现未声明名 | `template <>` 形参表为空 ⇒ 模式里每个名字都得是真实类型；此前**静默永不匹配** |
| `test_tmpl_46_ptr_vs_ref_instance.cpp` | **指针版与引用版是两条独立实例** | `Box<int*>`/`Box<int&>`/`Box<int**>`/`Box<int*&>` 各走各的偏特化；实例命名必须单射，否则撞汇编符号 |
| `test_tmpl_47_cv_position.cpp` | **cv 限定符的位置即语义** | `const int*`=Pointer(Const(Int)) 与 `int* const`=Const(Pointer(Int)) 是两棵不同的树；建错树 ⇒ 偏特化**静默选错**。顺带钉住 `toString` 必须单射（否则实例缓存键串味） |
| `test_tmpl_48_class_type_aliases.cpp` | **类内类型别名** `using X = T;` / `typedef T X;` | 别名不是新类型，只是一次名字替换（[temp.alias]）；三个使用点走三条查找路径：类外限定名（非模板）/ 类外限定名（模板实例）/ 类体内非限定名。顺带钉住实例化时别名目标必须跟着替换、以及一个被它踩出来的栈帧 bug |
| `test_tmpl_49_dependent_type_name.cpp` | **依赖类型名** `typename T::type` | [temp.res]/5 的悬案：定义期不知道 `T::x` 是类型还是值，`typename` 消歧、替换期才兑现（[temp.inst]）。★ 探测惯用法 `void_t<typename T::type>` 靠的正是 [temp.deduct]/8 的**直接上下文**：查不到必须**当场软失败** |
| `test_tmpl_50_alias_templates.cpp` | **别名模板** `template<class T> using Vec = MyPtr<T>;` | [temp.alias]/1：别名**不是新类型**，`Vec<int>` 与 `MyPtr<int>` 就是同一个类型 ⇒ 没有"实例化"只有"解糖"，不产生新符号。三级落点：① 使用点 `Vec<int> v;` 由 resolveType 解糖；② 模板体内 `Vec<T>` 保持依赖，在替换的**直接上下文**里解（substituteType Case 5.8）；③ ★ 推导侧也要解 —— `T f(Vec<T>)` 的 P 侧不解糖就与 A 侧 `MyPtr_int` 合不上，候选被**静默剔除**。顺带补齐类模板 id 的结构合一（[temp.deduct.type]/8 的 `T<T1...>` 情形）与实例"出身"记录 |
| `test_tmpl_51_ctad_and_guides.cpp` | **CTAD + 推导指引** `MyPtr m(7);` / `Two(int) -> Two<int,int>;` | [dcl.type.class.deduct]：没写 `<...>` 时拿**构造实参**反推类模板形参 —— 与函数模板推导是**同一套合一算法的反向使用**（模式来自构造函数形参表，实现上直接复用 `deducePair`）。只在直接初始化触发，故顺带补上 `Type name(args);` 文法。[temp.deduct.guide]：显式指引**优先于**构造函数（指引存在的意义就是改写默认规则），非模板指引用来补构造函数根本推不出的形参。★ 顺带暴露一个真问题：**替换 ≠ 实例化** —— 实例化函数后签名里残留的 `MyPtr<int>` 半成品必须再过一次 resolveType 才成 `MyPtr_int` |
| `tests/decl/test_decl_02_adl_and_qualified_lookup.cpp` | **ADL + 限定名查找** | 三条路：限定名（只在 N 里找）/ 命名空间内非限定名 / [basic.lookup.argdep] ADL。★ ADL 不是兜底而是**补进同一候选集**：`measure(s)` 里 `N::measure(S)` 与全局 `measure(int)` 同场竞争，实现成“先到先得”会静默调错函数 |

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
11. test_tmpl_21_nttp_basic.cpp    ← 非类型模板参数：值也能当实参
12. test_tmpl_27_struct_template.cpp ← struct 写模板
13. test_tmpl_28_default_arg.cpp    ← 默认模板实参：少写实参怎么补
14. test_tmpl_29_partial_spec.cpp   ← 偏特化：模式匹配 = 合一算法复用
15. test_tmpl_30_explicit_spec.cpp  ← 全特化：空形参表 + 类型相等
16. test_tmpl_31_spec_selection.cpp ← ★ 三路择优：谁被选中
17. test_tmpl_33_decltype_basic.cpp ← decltype：表达式→类型
18. test_tmpl_34_decltype_paren.cpp ← ★ 多一对括号就变：int vs int&
19. test_tmpl_35_void_t_detect.cpp  ← void_t 探测：能力查询
20. test_tmpl_36_void_t_fallback.cpp ← ★ SFINAE 软失败：失败不是错误
21. test_tmpl_38_error_no_fallback.cpp ← 对照 20：接不住才是错误
22. test_tmpl_39_is_range_demo.cpp  ← 用户原用例的 is_range 半边
23. test_tmpl_40_order_pointer_depth.cpp ← 偏序：更特化者胜
24. test_tmpl_41_order_decl_independent.cpp ← ★ 与 23 成对：证伪顺序依赖
25. test_tmpl_43_order_three_way.cpp ← 三路偏序 + 传递性
26. test_tmpl_44_error_extraneous_template.cpp ← ★ 反例：写错的 `template<>` 长什么样
27. test_tmpl_45_error_undeclared_in_explicit_spec.cpp ← ★ 最阴的一种：静默选错
28. test_tmpl_47_cv_position.cpp ← const 写在哪一侧：`const int*` vs `int* const`
29. test_tmpl_48_class_type_aliases.cpp ← 类内别名 + 三个使用点（含一个 codegen 栈帧 bug）
30. test_tmpl_49_dependent_type_name.cpp ← `typename T::type`：查不到要当场软失败
31. tests/decl/test_decl_02_adl_and_qualified_lookup.cpp ← ADL：名字查找的第三个入口
32. test_tmpl_50_alias_templates.cpp ← ★ 别名模板：只是名字，不是类型（推导侧也要解糖）
33. test_tmpl_51_ctad_and_guides.cpp ← ★ CTAD：把构造函数当指引，反向用一次合一算法
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
│   ├── ast.h                       #   AST 节点定义（各节点 accept 挂钩）
│   ├── ast_visitor.h               #   ★ AST 访问者基类（Visitor 模式）
│   ├── parser.h                    #   语法分析器接口
│   ├── type.h                      #   类型系统定义
│   ├── semantic_analyzer.h         #   语义分析器接口
│   ├── sfinae.h                    #   ★ SFINAE 协议（信号 + 吸收器 + 直接上下文）
│   ├── template_instantiation.h    #   模板实例化接口
│   └── codegen.h                   #   代码生成器接口
├── src/                            # 实现文件
│   ├── lexer.cpp                   #   阶段 1：词法分析
│   ├── parser.cpp                  #   阶段 2：语法分析
│   ├── type.cpp                    #   类型系统实现
│   ├── semantic_analyzer.cpp       #   阶段 3：语义分析 ★
│   ├── template_instantiation.cpp  #   阶段 4：模板实例化 ★
│   ├── template_deduction.cpp      #   实参推导（合一算法）★
│   ├── preprocessor.cpp            #   阶段 0：预处理器
│   ├── sfinae.cpp                  #   SFINAE 协议：信号 / 吸收器 / 统一日志
│   ├── codegen.cpp                 #   阶段 5：代码生成
│   ├── linker.cpp                  #   阶段 6：自研链接器
│   └── main.cpp                    #   编译器主程序
├── tests/                          # 测试用例
│   ├── test_tmpl_01..10_*.cpp      # 基础替换 / 指针 / 引用 / 折叠 / 多参 / 多蓝图
│   ├── test_tmpl_11..19_*.cpp      # 函数模板实参推导 S1~S6
│   ├── test_tmpl_21..26_*.cpp      # 非类型模板参数 NTTP
│   ├── test_tmpl_27..32_*.cpp      # struct 模板 / 默认实参 / 偏特化 / 全特化 / 三路择优
│   ├── test_tmpl_33..39_*.cpp      # decltype / 括号规则 / void_t 探测 / SFINAE 软失败与硬错误
│   ├── test_tmpl_40..43_*.cpp      # 偏序裁决：T*vs T** / 顺序无关 / 歧义 / 三路
│   ├── test_tmpl_44..45_*.cpp      # 负向：错误 template<> / 全特化模式里的未声明名
│   ├── test_tmpl_46_*.cpp          # 指针版与引用版是两条独立实例（实例名单射性）
│   ├── test_tmpl_47_*.cpp          # cv 限定符的位置：const int* vs int* const
│   ├── test_tmpl_48_*.cpp          # 类内类型别名 using/typedef 与三个使用点
│   ├── test_tmpl_49_*.cpp          # 依赖类型名 typename T::type 与探测惯用法
│   ├── test_tmpl_50_*.cpp          # 别名模板 template<T> using（解糖而非实例化）
│   ├── test_tmpl_51_*.cpp          # CTAD 类模板实参推导 + 推导指引
│   └── unit/                       # 单元测试（ctest 驱动，159 个用例）
│       ├── test_template_deduction.cpp  # 推导 / 替换 / 偏特化匹配 / NTTP
│       ├── test_decltype_sfinae.cpp     # Decltype.* / Sfinae.* / PartialOrder.* / SfinaeProtocol.*
│       ├── test_codegen_frame.cpp       # CodegenFrame.*：帧大小 ≥ 最深局部偏移（不变量）
│       └── test_ast_visitor.cpp         # AstVisitorDispatch.*：分派路由 + kind≡类型不变量
├── docs/learn/                     # 分主题学习文档（01..29，与测试一一对应）
├── docs/REFACTOR-ast-visitor.md    # AST 分派重构（五批次）：动机 / 边界 / 验证方法
├── docs/NOTES-阅读笔记.md          # 通读源码时的理解要点（按模块整理）
├── logdiff.sh                      # 重构安全网：日志 + 汇编基线对比
└── build-linux/                    # 构建输出
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
- [x] **非类型模板参数（NTTP）**：`template<int N>` + `Buf<4>`，含类型/值混排
      `template<class T, int N>`、负数实参 `Buf<-3>`、形态校验与 ITanium
      `L...E` 编码（文档 docs/learn/18）
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
- [x] 类模板特化：`struct` 写模板体、默认模板实参、偏特化、全特化、三路择优（文档 docs/learn/19）
- [x] **decltype**：`decltype(e)` 两套规则（声明类型 / 表达式类型+左值引用）、
      依赖上下文的延迟求值、`decltype(&a)` 取地址（文档 docs/learn/20）
- [x] **SFINAE**：替换失败软处理（候选移出而非报错）、`std::void_t` 探测惯例
      （CWG 1558）、`std::declval`、`std::false_type`/`std::true_type` 内建垫片；
      **独立模块 `include/sfinae.h`**（信号 / 吸收器 / 直接上下文 / 统一日志出口，
      收口点全项目仅三处）（文档 docs/learn/20）
- [x] **类模板偏序裁决**：`[temp.class.order]` 互相推导 + 唯一合成类型、
      dominance 循环选唯一最特化者、互不更特化时报歧义（文档 docs/learn/21）
- [x] 类型别名（`using`/`typedef`）、枚举、命名空间、全局变量（文档 docs/learn/08）
- [x] **名字在哪一层当真**：语句层声明/表达式前瞻（`S& r`、模板 id + 引用、`const` 开头）、
      未声明类型名兜底校验、实例命名单射性（`Box<int*>` 与 `Box<int&>` 不再撞符号）
      （文档 docs/learn/22）
- [x] **类内类型别名**：`using X = T;` / `typedef T X;`，三个使用点（类外限定名 /
      模板实例限定名 `/ 类体内非限定名）（文档 docs/learn/24）
- [x] **依赖类型名**：`typename T::type` 与 `void_t<typename T::type>` 探测惯例
      （文档 docs/learn/25）
- [x] **ADL + 限定名查找**：普通查找与 ADL 候选并入**同一候选集**再决议
      （文档 docs/learn/26）
- [x] **别名模板**：`template<class T> using Vec = MyPtr<T>;` —— 解糖（不是实例化），
      使用点 / 替换期 / 推导期三级落点；NTTP 别名 `template<int N> using BufA = Buf<N>;`
      （文档 docs/learn/27）
- [x] **CTAD + 推导指引**：`MyPtr m(7);` 由构造实参反推类模板形参、
      直接初始化 `Type name(args);` 文法、`X(T) -> X<T>;` 模板/非模板指引
      （文档 docs/learn/28）

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
