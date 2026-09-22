# 33 · 模板模板参数：形参表里的第三种形态

> 主题：`template <template <class> class C, class T> struct Wrap { C<T> inner; };`
> 对应标准：[temp.param]/4（形参声明）、[temp.arg.template]（实参匹配）。
> 对应编译原理：**高阶类型**（higher-kinded type）—— 形参本身是"类型构造器"而不是类型，
> 实例化时发生的是**二级替换**：先换掉构造器名，再对新构造出的类型做一次普通实例化。

---

## 1. 理论背景

### 1.1 形参的三种形态，这次补齐最后一种

| 形参写法 | kind | 接受的实参 | 本项目支持时间 |
|---|---|---|---|
| `typename T` / `class T` | `Type` | 一个类型（`int` / `Box<int>`） | 起点 |
| `int N` / `bool B` | `NonType` | 一个常量表达式（`4` / `true`） | 主线（docs/learn/18） |
| `template <class> class C` | `Template` | **一个类模板**（`Box`） | 本文 |

第三种的意义：它让"容器"能反过来接受"装什么"的策略。
标准库的 `std::vector<T, Alloc>`、`std::map<K,V,Compare>` 都是这个形状。

### 1.2 `C<T>` 是一次二级替换

模板体里 `C<T> inner;` 在**定义期**是一个依赖类型：`C` 是形参，`T` 是形参。
实例化 `Wrap<Box, int>` 时发生两步：

```text
   C<T>            ← 定义期的依赖类型（模板 id，名字段是形参 C）
     │
     │ ① 换名字：C 在替换表里绑的是【模板】Box（不是类型、不是值）
     ▼
   Box<T>          ← 名字段已落地，实参段也已换过（Case 5.2 干的）
     │
     │ ② 换实参：T → int
     ▼
   Box<int>        ← 半成品：名字与实参都具体了，但还不是"类型"
     │
     │ ③ 落地解析（与模板体里直写 Box<T> inner; 走同一条路）
     ▼
   Box_int         ← 具体类类型，有布局、有方法符号
```

★ ①与②在同一趟 `substituteType` 里完成，③在外部（Sema）—— 这个分层是刻意的：
替换引擎不认识 Sema，若在替换期硬调 `resolveType` 会引入双向依赖
（与 docs/learn/25 的 `MemberTypeResolver` 回调同一个理由）。

### 1.3 实参的"形态"只能由形参表裁定

`Wrap<Box, int>` 里的 `Box` 在 **Parser** 眼里和类型名**完全同形**——裸标识符，
于是被建成 `Class("Box")` 类型实参。它到底是"类型"还是"模板"，
Parser 判不了（它不查符号表），只有 Sema 逐位看形参表才知道。

这与 NTTP 名的处境是同一类问题（docs/learn/27 §3.3 的"实参位裸名还原"），
处理手法也一致：**Parser 一律按最宽的形式建节点，Sema 在拿到形参表后改标形态**。

### 1.4 为什么这一位不能走 `resolveType`

实参位若走常规 `resolveType(Class("Box"))`，会撞上"裸类模板名不能当类型"的检查
（`Box b;` 确实该报错）。但模板模板位上的 `Box` 是**合法实参**，
它不需要被解析成类型——它本身就是答案。

```text
   resolveType(Box)                  ⇒  ✗ 'Box' is a class template; provide template arguments
   TemplateArg::ofTemplate("Box")    ⇒  ✓ 这一位要的就是模板名
```

故在实参循环里**先看形参位**，是 `Template` 就取名字改标，不进 `resolveType`。

---

## 2. 数据流（ASCII 图）

```text
   template <template <class> class C, class T> struct Wrap { C<T> inner; };
                     │                        │
                     │ Parser: 内层 <...> 只数个数   │
                     │         （templateArity = 1）  │
                     ▼                        ▼
              TemplateParam{Template,"C"}   TemplateParam{Type,"T"}

   使用点  Wrap<Box, int> w;
              │
              ▼
   ┌────────────────────────────────────────────────┐
   │ Sema::resolveType（模板 id 分支）              │
   │   for i, arg in 实参表:                        │
   │     params[i].kind == Template ?               │
   │        ├─ 是：检查名字确实是类模板              │
   │        │       ⇒ TemplateArg::ofTemplate(name) │  ← 不进 resolveType
   │        └─ 否：TemplateArg::ofType(resolveType) │
   └──────────────┬─────────────────────────────────┘
                  ▼
   ┌────────────────────────────────────────────────┐
   │ TemplateInstantiator::substituteType           │
   │   Case 5.2: 实参段替换   C<T> → C<int>          │
   │   Case 5.3: 名字段替换   C<int> → Box<int>   ★  │
   └──────────────┬─────────────────────────────────┘
                  ▼
   ┌────────────────────────────────────────────────┐
   │ Sema 落地解析（与嵌套模板 id 字段同路）        │
   │   Box<int> ⇒ getOrInstantiateClass ⇒ Box_int   │
   └──────────────┬─────────────────────────────────┘
                  ▼
        实例 Wrap_Box_int  →  _Z4WrapI3BoxiE
```

---

## 3. 实现改动清单

| # | 文件:位置 | 改动 |
|---|---|---|
| 1 | `include/ast.h` · `TemplateParamKind` | 加 `Template`；`TemplateParam` 加 `templateArity` |
| 2 | `include/type.h` · `TemplateArgKind` | 加 `Template`；`TemplateArg` 加 `templateName` + `ofTemplate` / `isTemplate` |
| 3 | `src/type.cpp` · `toString` / `equals` | 模板实参印名字、按名字判等 |
| 4 | `src/parser.cpp` · 形参循环 | 新增 `KwTemplate` 分支：**单独吃掉内层形参表**（只数个数） |
| 5 | `src/semantic_analyzer.cpp` · `resolveType` 模板 id 分支 | 逐位看形参，`Template` 位取名字改标 + "必须是类模板"校验 |
| 6 | `src/template_instantiation.cpp` · `substituteType` | 新增 **Case 5.3**：模板模板形参改名 |
| 7 | `src/template_instantiation.cpp` · `mangleTemplateInstance` | 模板实参按 `<name>` 编码（`3Box`），**不能落到 NTTP 分支** |
| 8 | `src/main.cpp` · Phase 4 演示 | 含模板模板形参的模板跳过"固定类型演示" |

### 3.1 内层形参表必须单独吃掉

形参循环是这样推进的：每轮解析**一个**形参、然后 `match(Comma)` 决定是否继续。
`template <template <class> class C, class T>` 的第 0 位若把内层的 `class` 当成
"外层形参的 typename 分支"，就会：

1. 把内层 `class` 吃掉当作外层形参关键字；
2. 把紧随其后的 `C` 当成形参名；
3. 下一轮撞上 `,` 之前那个 `class T` 前面已经错位 —— 得到一张**多出一位的形参表**。

故内层表用**独立的 do-while** 吃掉，一个形参都不注册。这也让
`templateArity` 顺手可得（本项目只用它做形态检查，不做逐位签名匹配）。

### 3.2 mangling：模板实参不是值

`mangleTemplateInstance` 原本只有两支（类型 / 值），模板实参落进值分支后
会被编成 `Li0E`（"值为 0 的表达式"）——产出一个**看着像模像样但完全错误**的符号：

```text
   Wrap<Box, int>   错误编码  _Z4WrapILi0EiE    ← 把 Box 当成了值 0
                    正确编码  _Z4WrapI3BoxiE    ← clang++-18 实测
```

★ 这类 bug 的危险在于**不会报错**：符号自洽、链接能过、程序能跑，
只是与全世界其他编译器不兼容。故新增形态时必须同步问一句
"这个 kind 在主流程的每一个 `if/else` 里都有归宿吗"。

---

## 4. 可复现实验

```bash
# ① 端到端（期望两个编译器都返回 0）
./minicc tests/tmpl/test_tmpl_53_template_template_param.cpp -o /tmp/t53 && /tmp/t53; echo $?
clang++-18 -std=c++20 tests/tmpl/test_tmpl_53_template_template_param.cpp -o /tmp/t53c && /tmp/t53c; echo $?

# ② 看二级替换的两步
./minicc tests/tmpl/test_tmpl_53_template_template_param.cpp -S -o /tmp/t53.s 2>&1 \
  | grep -E "template template parameter|期望模板|模板模板形参" | grep -v "\[pp\]"

# ③ 错误用例（期望 exit=1）
./minicc tests/tmpl/test_tmpl_54_error_ttp_not_template.cpp; echo "exit=$?"

# ④ mangling 与 clang 逐字符核对
cat > /tmp/ttp_mng.cpp <<'EOF'
template <template <class> class C, class T> struct Wrap { C<T> inner; };
template <class T> struct Box { T value; };
void f(Wrap<Box, int>*) {}
EOF
clang++-18 -std=c++20 -c /tmp/ttp_mng.cpp -o /tmp/ttp_mng.o && nm /tmp/ttp_mng.o | grep " T "
# ⇒ _Z1fP4WrapI3BoxiE   （类名段：4WrapI3BoxiE —— 本实现逐字符相同）

# ⑤ 全量回归
./build.sh && ctest --test-dir build-linux -j6 && ./logdiff.sh diff
```

---

## 5. 边界（本项目**未做**的）

| 缺口 | 症状 | 卡在哪 |
|---|---|---|
| 逐位签名匹配 [temp.arg.template]/2 | `template<template<class,class> class C>` 收 `Box`（只 1 位）不报错 | 只存了 `templateArity`，没存内层形参的 **kind**；要补齐得让内层表也走完整 `TemplateParam` |
| 默认模板实参 | `template <template <class> class C = Box>` | `TemplateParam::defaultArg` 是 `TemplateArg`，加一个 Template 形态即可，但实参位还没支持 |
| 嵌套模板模板参数 | `template <template <template<class> class> class C>` | Parser 显式报错（depth > 1），不是静默算错 |
| 别名模板作模板模板实参 | `Wrap<Vec, int>`（Vec 是别名模板） | 判定里已放行 `m_aliasTemplates`，但替换期拿到名字后走不到解糖 —— 未验证 |
| 模板模板形参当类型用 | `C c;`（不带实参） | 报 `unknown type name 'C'`，诊断不指向根因 |

---

## 6. clang 源码对照表

| clang（`~/cppproject/llvm-project/`） | 本实现位置 | 简化了什么 |
|---|---|---|
| `TemplateTemplateParmDecl`（`TemplateDecl` 子类，自带形参表） | `TemplateParam{kind=Template, templateArity}` | 不建独立的 Decl 节点，只在外层形参上记元数 |
| `TemplateArgument::Template`（带 `TemplateDecl*`） | `TemplateArg{kind=Template, templateName}` | 存名字而非指针（本项目模板注册表按名字索引） |
| `ParseTemplateParameter` 遇 `kw_template` 递归 `ParseTemplateParameterList` | `parseTemplateDecl` 的 `KwTemplate` 分支 | 内层表只数个数；嵌套深度 > 1 直接报错 |
| `Sema::CheckTemplateArgument` 的 `CheckTemplateTemplateArgument` | `resolveType` 模板 id 分支的逐位检查 | 不做 [temp.arg.template]/2 的逐位形参匹配 |
| `TreeTransform::TransformTemplateSpecializationType` | `substituteType` Case 5.3 | 只换名字段；不做 clang 的 sugar 重建 |
| `ItaniumMangle` 的模板模板实参路径 | `mangleTemplateInstance` 的 `isTemplate()` 分支 | 只支持简单模板名（`3Box`），不支持 `X<...>E` 的复杂形态 |

---

## 7. 关键日志片段（`test_tmpl_53` 实跑）

定义期与使用点：

```text
  [parse:template]   ★ template template parameter registered: 'C' (accepts a template with 1 parameter(s), [temp.param]/4)
  [sema:targ]   ⤷ 第 1 位期望模板 ⇒ 实参 'Box' 按【模板名】处理（[temp.arg.template]）
```

实例化期的二级替换（`C<T>` → `Box<T>` → `Box_int`）：

```text
    [subst] ★ 模板模板形参 'C' → 模板 'Box'：C<int> → Box<int>
  ║ Instance:  Wrap_Box_int
  ║ Mangled: Wrap_Box_int → _Z4WrapI3BoxiE
  ║ Instance:  Box_int
  ║ Mangled: Box_int → _Z3BoxIiE
```

错误用例（`Wrap<int, int>`）：

```text
[ERROR] [Semantic Error] 1:1: template argument 1 for 'Wrap' ('C') must be a class
template, but 'int' is not a template
```

★ 注意 `Wrap_Box_int` 这个名字里出现了模板名 `Box` —— 实例名清洗把实参的
`toString()`（模板实参印模板名）拼了进去，与 `Wrap_int_double` 天然不撞。
