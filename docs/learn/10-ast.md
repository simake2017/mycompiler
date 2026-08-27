# 10 AST：抽象语法树（编译器的心脏数据结构）

> AST（Abstract Syntax Tree）是编译器管线中承上启下的核心数据结构：
> Parser 把线性 Token 流"折叠"成树，后面所有阶段（语义分析、模板实例化、代码生成）
> 都只是在这棵树上做**遍历 + 变换**。
> 理解 AST 就理解了"编译器到底在操作什么东西"。

---

## ① 理论背景

**标准依据**：C++ 标准本身没有"AST"一词——标准描述的是语法（[gram]）与语义（[basic]/[expr]/[stmt]/[dcl]）。
AST 是编译原理的实现概念，对应龙书（Aho et al.）第 4~5 章的 "Syntax Tree" / "Abstract Syntax"。

### 1.1 为什么需要 AST：从"线性文本"到"结构化程序"

源码是**一维字符串**，程序语义是**嵌套结构**。两者之间隔着两次降维：

```text
源码（一维字符流）
    int c = 1 + 2 * 3;
         │  Lexer（去掉空白/注释，切出词法单元）
         ▼
Token 流（一维，但有类别）
    [int] [c] [=] [1] [+] [2] [*] [3] [;]
         │  Parser（递归下降，把优先级/嵌套编码成层次）
         ▼
AST（二维树形结构）
    VarDeclStmt(c)
    └─ BinaryExpr(+)
       ├─ IntLiteral(1)
       └─ BinaryExpr(*)
          ├─ IntLiteral(2)
          └─ IntLiteral(3)
```

- **Token 流丢掉了优先级信息**：`1 + 2 * 3` 和 `(1 + 2) * 3` 的 Token 序列只差一对括号，
  但语义天差地别。Parser 的职责就是把优先级/结合性**编码进树的嵌套深度**
  （详见 09 号文档：优先级分层递归下降）。
- **AST 是"抽象"的**：相对 CST（Concrete Syntax Tree，具体语法树），
  AST 丢弃了纯语法噪音——括号、分号、逗号都不进树。
  `(1 + 2)` 在 AST 里就是 `BinaryExpr(+, 1, 2)`，
  因为括号的信息已被"树的形状"吸收（见 ② 设计决策）。

### 1.2 AST 的三大节点族

任何命令式语言的 AST 都按"节点在程序中扮演什么角色"分成三族。
本项目的分类与 clang 完全同构：

| 族 | 本项目基类 | 核心特征 | 标准章节 | clang 对应 |
|---|---|---|---|---|
| 表达式 Expression | `Expression` | 有类型、可求值，可嵌套进另一个表达式 | [expr] | `clang::Expr` |
| 语句 Statement | `Statement` | 控制流程、无值、不能作为子表达式 | [stmt] | `clang::Stmt` |
| 声明 Declaration | `Declaration` | 向符号表引入新名字 | [basic.scope]/[dcl] | `clang::Decl` |

判别口诀（ast.h 中三大基类注释的提炼）：

```text
表达式 = "算东西"（有类型有值）        42 / a + b / foo(x)
语句   = "做动作"（控制怎么跑）        return / if / while / x = 5;
声明   = "报户口"（引入名字）          int foo(); class Point; template<...>
```

一个典型的跨族组合：`int x = foo(a, b);`

```text
VarDeclStmt (语句族：声明一个局部变量是"动作")
├─ declaredType → int
└─ initializer → CallExpr (表达式族：右边是要算的值)
    ├─ callee  → VarExpr(foo)   ← foo 这个名字由声明族引入
    └─ arg[0]  → VarExpr(a)
```

### 1.3 NodeKind + 继承：双重类型标签

本项目用"**运行时枚举标签 + C++ 继承**"双轨制（ast.h 的 `NodeKind`）：

- `NodeKind kind`：消费方不必 `dynamic_cast` 就能 switch 分派（热路径更快），
  教学上等价于 clang 的 `Stmt::StmtClass` / `Decl::Kind` 枚举；
- 继承体系：`struct BinaryExpr : Expression`，需要访问子节点字段时再
  `std::static_pointer_cast`（kind 已确认，安全且零开销）。

clang 同样双轨：`Stmt` 有 `StmtClass` 枚举 + 虚析构，访问用 `cast<BinaryOperator>(S)`。

---

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 节点所有权 | `std::shared_ptr<ASTNode>`（ExprPtr/StmtPtr/DeclPtr） | 模板实例化要"克隆子树 + 共享类型节点"；shared_ptr 免手动管理，教学直观 |
| 类型标签 | `NodeKind` 枚举 + 继承双轨 | switch 分派快；字段访问仍走类型系统 |
| 括号 | 不建节点 | 括号语义已被树形吸收；若未来需要保留（如调试显示），才加 `ParenExpr`（clang 有） |
| 赋值 | 独立 `AssignStmt`（语句族） | C++ 里赋值是表达式（`a = b = c` 可链式），本项目简化为语句，牺牲链式赋值换取"表达式无副作用"的简单心智 |
| 源码位置 | 每节点带 `SourceLocation` | 报错定位（"3:7 期望 ';'")；对应 clang 的 `SourceRange` |
| 模板蓝图 | `TemplateDecl` 双槽位（classTemplate / funcTemplate 互斥） | 对照 clang 的 ClassTemplateDecl/FunctionTemplateDecl 共同继承 TemplateDecl；本项目用"判别方法"代替继承，AST 保持扁平 |
| 类型信息 | `Expression::resolvedType` Parser 阶段为空，阶段 3 填充 | 职责分离：Parser 只管结构，类型是语义分析的产物（[expr]：每个表达式有唯一类型） |
| 根节点 | `TranslationUnit { vector<DeclPtr> }` | 对应 clang::TranslationUnitDecl；一个源文件一棵树 |

---

## ③ 例子：一段源码的完整 AST

```cpp
class Point {
public:
    int x;
    Point(int px) : x(px) {}
};

template<typename T> T twice(T v) { return v + v; }

int main() {
    int a = twice(21);
    if (a > 40) { a = 0; }
    while (a) { a = a - 1; }
    return a;
}
```

```text
TranslationUnit
├─ ClassDecl(Point)
│  ├─ fields: [ x : int ]
│  └─ methods: [ ConstructorDecl(Point, initList=[x(px)]) ]
├─ TemplateDecl(function)                 ← 蓝图，暂不做语义
│  └─ funcTemplate: FunctionDecl(twice)
│     ├─ returnType → T (TemplateParam 占位)
│     ├─ parameters → [ v : T ]
│     └─ body → BlockStmt
│                 └─ ReturnStmt
│                     └─ BinaryExpr(+)
│                        ├─ VarExpr(v)
│                        └─ VarExpr(v)
└─ FunctionDecl(main)
   └─ body → BlockStmt(4 条语句)
      ├─ VarDeclStmt(a : int)
      │  └─ CallExpr(1 个实参)
      │     ├─ callee → VarExpr(twice)
      │     └─ arg[0] → IntLiteral(21)
      ├─ IfStmt
      │  ├─ condition  → BinaryExpr(>) [VarExpr(a), IntLiteral(40)]
      │  ├─ thenBranch → BlockStmt[ AssignStmt ]
      │  └─ elseBranch → (空)
      ├─ WhileStmt
      │  ├─ condition → VarExpr(a)
      │  └─ body → BlockStmt[ AssignStmt(a, BinaryExpr(-)) ]
      └─ ReturnStmt
         └─ VarExpr(a)
```

观测方式（全阶段中文日志 + 递归树打印）：

```bash
# 阶段 2 的树形（--dump-ast）；obs_helpers.h 的 dumpExprTree/dumpStmtTree 是同一思想
./cmake-build-debug/minicc tests/test_ctor_01_basic.cpp --dump-ast
```

---

## ④ AST 在管线中的生命周期（各阶段对树做什么）

| 阶段 | 动作 | 代码位置 |
|---|---|---|
| Parser | **建造**：递归下降逐节点构造 | src/parser.cpp（parseTranslationUnit 入口） |
| SemanticAnalyzer | **标注 + 改写**：填 `resolvedType`、绑定符号、把模板调用 callee 原地改写为 mangled 名 | src/semantic_analyzer.cpp（inferCall:1386 改写例） |
| TemplateInstantiator | **克隆 + 替换**：克隆蓝图子树，T → int，产出全新子树挂回函数列表 | src/template_instantiation.cpp（cloneExpr/cloneStmt） |
| CodeGen | **只读遍历**：递归访问节点生成 x86-64 AT&T 汇编 | src/codegen.cpp |

关键认知：**AST 不是只读的**。语义分析会改写它（callee 改名），
实例化会复制它（克隆蓝图）。这正是"模板 = 代码生成器"在数据结构层面的体现。

---

## ⑤ 可复现实验

```bash
# 1. 跑本主题单测（节点族分类 / 树形断言 / 位置信息 / 蓝图冻结）
cmake --build cmake-build-debug --target unit_tests -j$(nproc)
./cmake-build-debug/unit_tests --gtest_filter='Ast*'

# 2. 语义 oracle：clang ast-dump 对照（结构应与本实现同形）
cat > /tmp/ast_demo.cpp <<'EOF'
int add(int a, int b) { return a + b; }
int main() { int x = add(1, 2); return x; }
EOF
clang++-18 -Xclang -ast-dump -fsyntax-only /tmp/ast_demo.cpp
# 预期：FunctionDecl add → CompoundStmt → ReturnStmt → BinaryOperator '+'
#        └─ DeclRefExpr 'a'  DeclRefExpr 'b'，与本项目 AST 一一对应

# 3. 本项目全管线观测
./cmake-build-debug/minicc /tmp/ast_demo.cpp --dump-tokens --dump-ast
# 对比 Token 流（一维）与 AST（嵌套树）：优先级信息只在后者中可见
```

---

## ⑥ clang 源码对照

| clang（AST 基础设施） | 本实现 | 简化了什么 |
|---|---|---|
| `include/clang/AST/Stmt.h`（Stmt + StmtClass 枚举） | include/ast.h `Statement` + `NodeKind` | 无虚函数分派表、无 ASTContext 分配器 |
| `include/clang/AST/Expr.h`（Expr 必有类型） | `Expression::resolvedType` | 无 value category 细分（lvalue/xvalue/prvalue 简化为语义阶段的布尔左值性） |
| `include/clang/AST/Decl.h`（Decl 层次） | `Declaration` 平铺一族 | 不做 DeclContext 嵌套作用域链，符号表用简单 map |
| `RecursiveASTVisitor`（万能遍历器） | 各阶段手写递归访问 | 不用访问者模式泛型框架 |
| `ASTContext`（内存池 + 类型唯一化） | `std::shared_ptr` | 无 arena 分配，教学可读优先 |
| `SourceRange`（begin/end 两个 SourceLocation） | 单 `SourceLocation` | 只记起点，够报错定位 |

---

## ⑦ 关键 ASCII 总图：从字符到汇编，AST 居中

```text
┌────────────┐   Lexer    ┌────────────┐   Parser(递归下降)   ┌────────────┐
│  源码文本   │ ─────────▶ │  Token 流   │ ──────────────────▶ │    AST     │
│  (一维字符) │            │ (一维类别)  │   优先级→嵌套深度    │ (二维树)   │
└────────────┘            └────────────┘                      └─────┬──────┘
                                                                    │
                        ┌───────────────────────────────────────────┼───────────────┐
                        ▼                                           ▼               ▼
                 SemanticAnalyzer                          TemplateInstantiator   CodeGen
                 标注类型/绑定符号/改写 callee              克隆蓝图+类型替换       遍历→x86-64
                        │                                           │               │
                        └─────────────────┬─────────────────────────┘               ▼
                                          ▼                                     汇编 .s
                                     改写/扩充后的 AST ─────────────────────────▶
```

一句话总结：**Lexer 去噪、Parser 塑形、Sema 赋值、实例化分身、CodeGen 翻译——
五个阶段接力传递的，始终是这棵树。**
