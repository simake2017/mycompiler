# AST 访问者重构（Visitor 模式）

> 把四个消费者各自的 `if-else + dynamic_pointer_cast` 分派链收进类型系统。
> 本文记录：为什么改、改成什么、边界在哪、怎么保证"改了等于没改"。

## 一、理论背景

**访问者模式（Visitor Pattern）** 解决的是"在不修改类层次的前提下，
为一组类型增加新操作"—— GoF 23 模式里对**双重分派（double dispatch）**的标准解法。

编译器的 AST 是它最经典的用武之地：节点种类固定且多（本项目 32 种），
而消费者会不断新增（类型检查、代码生成、常量折叠、AST 打印、克隆替换……）。
若每个消费者都自己写一遍类型分派，就退化成 O(n) 的类型试探链。

**对照 C++ 标准与 clang**：

| 概念 | 标准 / clang | 本实现 |
|---|---|---|
| 节点类型标签 | `Stmt::StmtClass` / `Decl::Kind`（枚举） | `NodeKind`（`include/ast.h:86`） |
| 访问者基类 | `clang::RecursiveASTVisitor`（TableGen 生成） | `minicc::AstVisitor`（`include/ast_visitor.h`） |
| 节点入口 | 各节点 `void accept(StmtVisitor&)` | 各节点 `void accept(AstVisitor&) override` |
| 默认行为 | `bool VisitX(X*) { return true; }` | `virtual void visit(X&) {}` |

★ clang 的 `RecursiveASTVisitor`/`StmtVisitor` 由 `StmtNodes.td`、`DeclNodes.td`
经 TableGen **自动生成**，本文件是那份生成结果的**手写版** —— 手写的原因正是
教学目的：生成器藏起来的东西，恰好是这里要讲清楚的。

## 二、改造前的病根

全项目 **147 处** `dynamic_pointer_cast`（实际调用数，不含注释提及），其中四条是纯粹的
N 路分派链 —— 结构相同、只为"按动态类型路由"而存在：

```text
  Sema::inferType        ← 14 级
CodeGen::emitExpr        ← 13 级
CodeGen::emitStmt        ←  8 级
main.cpp: dumpExpr       ← 14 级
```

两条毛病：

1. **性能 O(n)**：一个 `CallExpr` 要试穿前面 7 个 cast 才轮到；
2. **结构脆弱**：新增节点类型时四条链都要手动补，漏一条**不报错**，只是静默走 `else`
   （`main.cpp` 那两条链的 `else` 打印 `"UnknownExpr"`，从来没人见过 —— 因为不可达，
   但也意味着它永远不会提醒你漏了分支）。

讽刺的是 `NodeKind` 枚举早就存在，注释白纸黑字写着"让消费者不必 dynamic_cast 就能快速分派"
（`include/ast.h:83`）—— 设计与实现背离了。

## 三、改造后的结构

```text
                       ┌──────────────────────────┐
   node->accept(*this) │  accept 是虚函数          │
   ──────────────────▶ │  一次虚表跳转 = O(1)      │
                       └────────────┬─────────────┘
                                    │ v.visit(*this)
                                    │  ★ 重载决议在【编译期】完成：
                                    │    *this 的静态类型就是节点自己
                                    ▼
        ┌───────────────────────────────────────────────┐
        │  class AstVisitor {                           │
        │      virtual void visit(BinaryExpr&) {}       │  ← 32 个重载
        │      virtual void visit(CallExpr&) {}         │     默认实现为空
        │      ...                                      │
        │  };                                           │
        └───────────────────────────────────────────────┘
                    ▲                    ▲
        ┌───────────┴──────┐   ┌─────────┴──────────┐
        │ AstDumper        │   │ CodeGen            │   ← 各自只重写关心的
        │ (main.cpp 打印)  │   │ (emit 系列)        │
        └──────────────────┘   └────────────────────┘
```

一次 `accept` 调用里没有**任何**类型试探 —— `v.visit(*this)` 的重载在
`accept` 被编译时（静态类型已确定）就绑好了。

### 文件布局与依赖方向

```text
   ast.h ──include──▶ ast_visitor.h      （节点实现 accept 需要访问者完整）
   ast_visitor.h ──✗──▶ ast.h            （只需前置声明，因为 visit 收的是【引用】）
```

引用参数只要求类型**被声明**、不要求**被定义** —— 这是打破循环依赖的关键，
也让依赖保持单向无环。

★ **踩过的坑（值得单记）**：最初把 `accept` 的声明留在 `ast.h`、定义挪到
`ast_visitor.h`（写成 `inline void IntLiteralExpr::accept(...)`）。结果链接期满屏
`undefined reference to 'vtable for minicc::DeleteExpr'`。原因是 Itanium ABI 的
**键函数（key function）** 规则：类内声明、类外定义的虚函数会成为该类的键函数，
而 **vtable 只在定义键函数的那个 TU 里发射**。本项目的 `.cpp` 大多只 `include ast.h`，
于是没有 TU 发射 vtable。改成**类内 inline 定义**（隐含 `inline` ⇒ 不构成键函数）后，
vtable 在每个用到它的 TU 里以弱符号发射，问题消失。

## 四、边界：哪些**不**是分派链，不该改

这是本次重构最需要克制的部分。以下三类**不用访问者**（但也不再走 RTTI，
一律 `node->kind` 判断 + `static_pointer_cast`）：

| 类别 | 例子 | 为什么用标签而非访问者 |
|---|---|---|
| **递归查询走查** | `CodeGen::collectBases` / `emitGlobalVars` | 各只关心 2-3 种节点、递归结构是自定义的、还要往外部 map 里累积。改成访问者要引入状态机，纯属绕远 |
| **返回值语义** | `estimateBlockSize` / `cloneExpr` / `inferType` | 需要"算出结果"而不是"做点副作用"，`visit` 返回 `void` 表达不了 |
| **需要 shared_ptr 所有权** | Sema 的声明链（注册表存的就是 `shared_ptr`） | 从引用还原 `shared_ptr` 不安全（对象未必由 `shared_ptr` 持有） |
| **二~四路定向类型测试** | "赋值目标是 `IndexExpr` / `VarExpr` / `MemberExpr` 中的哪种？" | 分支少，标签 + `static_cast` 比访问者更直白 |

★ 判据一句话：**"把节点类型路由到处理函数"用访问者；"问对象一个具体问题"用标签。**

### 五批次推进记录

改造按"先立护栏、再逐层搬"的顺序推进，每批结束都必须
`./build.sh`（0 警告）+ `ctest`（全绿）+ `./logdiff.sh diff`（零差异）三绿才允许进下一批。
（新增用例见 `tests/unit/test_ast_visitor.cpp` 的 `AstVisitorDispatch.*`：精确重载路由、
默认空体、递归由调用方驱动、标签不变式 `kind ≡ 实际类型`、忘记下钻则静默漏访问。）

| 批次 | 范围 | 结果 |
|---|---|---|
| 0 | `logdiff.sh` 基线护栏（84 个集成测试的四类可观测输出） | 护栏本身 |
| 1 | `include/ast_visitor.h` + `ast.h` 的 `accept` 挂钩 + `main.cpp` 的 `AstDumper` | main.cpp 32 → 0 |
| 2 | `src/codegen.cpp` 的 `emitStmt` / `emitExpr` 两条链 | codegen 43 → 5 |
| 3 | `src/semantic_analyzer.cpp`：语句链改访问者，声明链 / `inferType` / `isLValueExpr` 改标签 | sema 45 → 6 |
| 4 | `src/template_instantiation.cpp` 的 `cloneExpr` / `cloneStmt` | inst 24 → 5 |
| 5 | 三个文件的残余定向测试（含 `parser.cpp`）+ 提取 `forEachFunctionDecl` | **全项目 0** |

★ 批次 3 有过一次**回退**：最初把 Sema 的**声明链**也改成 `visit`，编译期就炸了
（`no viable conversion from 'ClassDecl' to 'ClassDeclPtr'`）—— 因为注册表存的是
`shared_ptr`，而 `visit` 只给引用。这次失败直接催生了第三节那条判据。
**教训：先想清楚 handler 的形状（只要引用？还要所有权？要返回值？），再选分派手法。**

改造后的分布（五个批次全部完成后）：

| 文件 | 改造前 | 改造后 | 手段 |
|---|---|---|---|
| `src/main.cpp` | 32 | **0** | 访问者（`AstDumper`） |
| `src/codegen.cpp` | 43 | **0** | 语句链访问者；走查/取值链标签 |
| `src/semantic_analyzer.cpp` | 45 | **0** | 语句链访问者；声明链/infer 链标签 |
| `src/template_instantiation.cpp` | 24 | **0** | clone 链标签分派 |
| `src/parser.cpp` | 3 | **0** | 定向测试改 `kind` 判断 |
| 全项目 | **147** | **0** | —— |

★ **零 RTTI**：改造后全项目不再有任何 `dynamic_pointer_cast` 调用，
所有按类型的向下转换都走 `node->kind` + `static_pointer_cast`
（`static_cast` 是编译期零成本）—— 与 LLVM 的 `cast<>`/`dyn_cast<>`
用 `classof()` 查 `getKind()` 是同一个思路，见 `docs/learn/29`。

## 五、可复现实验

### 1. 看分派确实换了机制

```bash
# 改前：一个 CallExpr 要试穿 7 个 cast
grep -n "dynamic_pointer_cast" src/codegen.cpp | head

# 改后：只有一次 accept
./minicc tests/tmpl/test_tmpl_43_order_three_way.cpp -S -o /tmp/t.s
```

### 2. 验证"改了等于没改"（重构的核心纪律）

```bash
./logdiff.sh save     # 重构【前】存基线：84 个集成测试的
                      #   编译日志 + 退出码 + 程序输出 + 生成的汇编全文
#   ... 改代码 ...
./logdiff.sh diff     # 重构【后】逐字节对比，要求零差异
```

输出 `✅ 日志逐字节零差异` 才允许继续。这条纪律不是洁癖 ——
本项目把**全阶段中文日志当交付物**：单测靠子串匹配日志原文
（`tests/unit/obs_helpers.h: explainPipelineLine`），`docs/learn/01..28` 直接粘贴日志片段。
改一个 `std::cout` 字符串，测试和文档一起红。

### 3. 单独验代码生成（最严的一层）

```bash
# 用「仅 codegen 回退」的参照二进制逐文件比汇编
for f in tests/*/*.cpp; do
  ./minicc "$f" -S -o /tmp/new.s
  ./ref_minicc "$f" -S -o /tmp/old.s
  diff -q /tmp/old.s /tmp/new.s || echo "差异: $f"
done
```

结果：**70 个成功用例汇编逐字节一致；14 个负向用例报错文案逐字节一致**。

## 六、收益与代价

| | 说明 |
|---|---|
| ✅ 分派 O(n) → O(1) | 一次虚表跳转 |
| ✅ 新增节点类型 | 由"四处手动补链、漏了不报错"变成"改 `NodeKind` + 一处 `accept`" |
| ✅ 可读性 | `visit(BinaryExpr&)` 的函数名即分派表，不再需要读完 14 级 if-else 才知道支持哪些节点 |
| ⚠️ `accept` 纯虚 | 32 个节点各加一行；好处是漏了**编译期**就报错 |
| ⚠️ visit 默认体为空 | 与 clang 一致；代价是新增节点时访问者不会强制补分支 —— **但这与改造前完全一致**（原来也是静默走 else），没有变差 |
| ⚠️ 上下文参数 | `prefix`/`isLast` 这类"每节点不同"的状态只能存成访问者成员，且**递归前必须先取进局部变量**，否则子调用会覆写 —— 已在 `AstDumper` 注释中写明 |

## 七、相关文件

- `include/ast_visitor.h` —— 访问者基类 + 32 个 `visit` 重载
- `include/ast.h:97` —— `ASTNode::accept` 纯虚挂钩；各节点类内 `inline` 定义
- `src/main.cpp` —— `AstDumper`（`--dump-ast` 的树形打印）
- `include/codegen.h:76` —— `class CodeGen : public AstVisitor`
- `logdiff.sh` —— 基线护栏
- `docs/learn/29-ast-dispatch-two-idioms.md` —— 两种分派手法的判据、clang 对照、零 RTTI
- `docs/NOTES-阅读笔记.md` —— 通读源码时的笔记（原先散落在源码里的 `// wangyang`）
