# 01 函数模板解析（S1）

> 子阶段 S1：让 Parser 认识 `template<typename T> T twice(T x) {...}`，
> 把函数模板与类模板一样"冻结"成蓝图，并建立同名候选集，为 S2 实参推导铺路。

## ① 理论背景

**标准依据**：C++ [temp.fct]（函数模板）、[temp.decl]（模板声明总则）。

- **函数模板不是函数**，是"造函数的模具"。`template<typename T> T twice(T x)`
  声明了一个参数化的蓝图；只有被调用（`twice(21)`）或显式实例化时才产生真正的函数。
- **两个参数层**：`template<...>` 是**模板参数层**（编译期的类型变量），
  `(...)` 是**函数参数层**。S2 的 P/A 配对推导就发生在函数参数层的类型模式上——
  所以 S1 必须把 `T&`、`const T*` 这些形态解析为可匹配的类型模式。
- **两阶段名称查找**（[temp.dep]）：模板体里依赖 T 的名字（如 `x + x` 的运算符）
  推迟到实例化时检查。这就是为什么 S1 的 sema **只注册蓝图、不分析函数体**——
  和 clang 的行为一致：模板定义在声明时只做"非依赖"部分的检查。
- 对应编译原理概念：参数化语法产物（parameterized production）——
  文法的一个产生式带上了"类型变量"，实例化是对这些变量做替换后展开。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| AST 形态 | `TemplateDecl` 增加 `funcTemplate` 槽位，与 `classTemplate` 互斥 | clang 用继承（`FunctionTemplateDecl : TemplateDecl`）；本项目 AST 扁平无继承体系，双槽位 + `isClassTemplate()/isFunctionTemplate()` 判别法改动最小、最易讲解 |
| 分派依据 | `template<...>` 后的第一个 token：`class` → 类模板；类型关键字/标识符 → 函数模板 | 返回类型可以是模板参数名 `T`（词法上是 Identifier），不能只靠关键字判断 |
| 函数体处理 | sema 只注册蓝图，**不进 Pass 3 函数体分析** | 两阶段查找：依赖名推迟。模板体在 S5 实例化克隆后才走正常函数分析流程 |
| 候选集存放 | sema 新增 `m_functionTemplateCandidates: name → vector<TemplateDeclPtr>`，不进 Scope/SymbolTable | Scope 保持"名字→单符号"的教学模型；clang 的重载集同样挂在 DeclContext 上而非普通查找表。S6 重载决议遍历此候选集 |
| 复用 | 函数模板签名直接复用 `parseFunctionDecl()` | 模板参数名 T 走现有 `parseType()` 的 Identifier→class 分支，类型系统已有 TemplateParam 种类（type.h），S2 推导时再精确区分 |

## ③ clang 对照表

| 本实现 | clang 源（llvm-project/clang） | 简化了什么 |
|---|---|---|
| `parseTemplateDecl` 类/函数分派（src/parser.cpp） | `lib/Parse/ParseTemplate.cpp → Parser::ParseDeclarationStartingWithTemplate`：按后续 token 分派到类模板/函数模板/概念等 6 种 | clang 还要处理 requires 子句、模板模板参数、默认实参、别名模板；本项目只留类与函数两种 |
| `TemplateDecl{classTemplate,funcTemplate}` 双槽位 | `include/clang/AST/DeclTemplate.h`：`TemplateDecl`（基类，持有 TemplateParameterList + templated decl）→ `FunctionTemplateDecl` / `ClassTemplateDecl` | 无继承、无 TemplateParameterList 对象（参数名用 `vector<string>`），不支持非类型模板参数 |
| `processTemplateDecl` 注册候选集（src/semantic_analyzer.cpp） | `lib/Sema/SemaTemplate.cpp → Sema::CheckFunctionTemplate`：创建 FunctionTemplateDecl 并注册进 DeclContext 的重载集 | 无重复声明检查、无链接语义、无 friend/export |
| 蓝图不分析函数体 | `Sema::ActOnFinishFunctionBody` 对模板体只做非依赖名检查，依赖名留给实例化 | 本项目完全不检查模板体（更极端的简化） |

## ④ 实验手册

```bash
cd /home/magene/runtime/cppproject/mycompiler

# 本实现：观察 S1 日志
./build-linux/minicc tests/test_tmpl_11_func_basic.cpp --dump-ast
#   关注：[parse:template] function blueprint / [register] candidate set / TemplateDecl(function)

# 边界形态（多参数、T&、const T&、typename/class 混用）
./build-linux/minicc tests/test_tmpl_12_func_forms.cpp

# 错误用例（模板体是变量声明）→ 期望 [ERROR] Expected '(' after function name，退出码 1
./build-linux/minicc tests/test_tmpl_13_error_not_decl.cpp; echo "exit=$?"

# oracle：clang 里函数模板的 AST 形态
clang++-18 -Xclang -ast-dump -fsyntax-only tests/test_tmpl_11_func_basic.cpp | head -20
#   对照点：FunctionTemplateDecl 包裹 FunctionDecl + TemplateTypeParmDecl，
#   与本项目 TemplateDecl(function) → FunctionTemplate 的两层结构同构
```

## ⑤ 关键过程图

```
源码                        Token 流                         AST
─────────────────────────────────────────────────────────────────────────
template<typename T>    [template < typename T >]
T twice(T x) {          [Ident:T Ident:twice ( ...]
  return x + x;              │
}                            ▼
                    parseTemplateDecl()
                    ├─ 解析模板参数层 → typeParams = [T]
                    └─ 分派：当前 token = T（Identifier）
                         ≠ class → parseFunctionDecl()
                                   ├─ parseType → T（模式！不是具体类型）
                                   ├─ 参数 (T x)
                                   └─ body 冻结不分析
                                     │
                                     ▼
                    TemplateDecl {
                      typeParams: [T]
                      funcTemplate: FunctionDecl {
                        name: twice
                        returnType: T
                        parameters: [T x]      ← S2 推导的"模式 P"
                        body: <冻结>
                      }
                    }
                                     │
                    sema Pass 1      ▼
                    m_functionTemplateCandidates["twice"] = [蓝图]
                                     │
                    S2（下一阶段）：调用 twice(21)
                    遍历候选集 → P=T, A=int ⇒ T := int → 克隆蓝图 → 实例
```

## ⑥ 补注：类模板注册表的对称化（2026-09 修订）

S1 落地时函数模板拿到了名字→候选集（`m_functionTemplateCandidates`），
而类模板只进了公共的 `m_templates` 线性 vector——`resolveType` 判定
`Box<int>` 是否为模板、`getOrInstantiateClass` 找蓝图，都要 O(n) 扫描。
本次修订补齐对称性：

| 决策 | 做法 | 对照 clang |
|---|---|---|
| 类模板注册表 | `m_classTemplates: name → TemplateDeclPtr`，`processTemplateDecl` 类模板分支 `emplace` 注册（重名取先注册者，与旧线性扫描语义一致） | 类模板名经 `DeclContext::lookup` 命中 `ClassTemplateDecl` |
| 裸名诊断 | `resolveType` 中实参为空且命中注册表 → 早期报 `'Box' is a class template; provide template arguments`（[temp.arg.explicit]） | clang：`use of class template 'Box' requires template arguments`；若启用 CTAD 则先报 `no viable constructor or deduction guide for deduction of template arguments`（见 test_tmpl_20） |
| 不动的部分 | `m_templates` 保留有序列表（Phase 4 驱动遍历），符号表不登记蓝图 | 实例化产物是 `ClassTemplateSpecializationDecl`，与模板 Decl 分开 |

```bash
# 复现：裸类模板名错误（错误用例 20）
./build-linux/minicc tests/tmpl/test_tmpl_20_bare_template_error.cpp  # 期望 exit≠0
# 注册日志：Sema Phase 3 可见 "↳ class template 'Box' registered"
./build-linux/minicc tests/tmpl/test_tmpl_01_basic.cpp | grep registered
```
