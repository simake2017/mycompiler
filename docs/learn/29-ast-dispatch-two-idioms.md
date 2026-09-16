# 29 · AST 分派的两种手法：访问者 vs 标签

> 这篇讲一个具体的工程判断：**同一个 AST，同一个项目里，为什么有的地方用
> `accept` 虚分派（访问者模式），有的地方用 `switch (node->kind)`（标签分派）**。
> 结论不是"哪个更好"，而是**判据是什么** —— 判据错了，代码会又慢又难读。

## 一、理论背景：把"类型"变成"控制流"

编译器前端把源码变成一棵**节点类型繁多、消费者不断新增**的树。每次处理一个节点，
第一件事都是"这是哪种节点？"——这就是**类型分派**。

在动态类型语言里这件事天然存在（`isinstance` / 鸭子类型）。在 C++ 这种静态类型
语言里，把**运行时才知道的类型**映射到**编译期就固定下来的代码路径**，只有三条路：

| 手法 | 机制 | 成本 | 何时绑定 |
|---|---|---|---|
| ① `dynamic_cast` / RTTI | 运行时类型信息 + 类型图遍历 | 最贵：可能要沿继承链走 | 运行期 |
| ② **虚函数（访问者）** | 对象内 vptr → vtable 槽位 | 一次间接跳转 | 运行期（编译期定槽位） |
| ③ **标签 + switch（本项目 `NodeKind`）** | 枚举成员 → 跳表 | 一次比较 + 跳转（可编译成跳表） | 运行期（编译期定分支） |

**访问者模式（Visitor Pattern）** 是 GoF 23 模式里对**双重分派（double dispatch）**
的标准解法：`node->accept(v)` 是第一次分派（按**节点**类型），`v.visit(*this)`
是第二次分派（按**访问者**类型）。它把一个二维问题（N 种节点 × M 种操作）
从 `N×M` 个函数化成了 `N` 个 `accept` + `M` 个访问者。

> ★ 但访问者的**接口形状是固定的**：`void visit(X&)` —— 不返回值，也不交出所有权。
> 一旦你的 handler 需要**返回一个值**或**拿走 `shared_ptr`**，访问者就套不上了。
> 这不是实现缺陷，是模式的边界。下面第三节展开。

## 二、clang 怎么分的（两种手法它都有）

这一点特别值得看：**clang 自己就同时用了两种**，而且切分线跟本项目一模一样。

| clang 构件 | 手法 | 形状 | 干什么 |
|---|---|---|---|
| `clang::RecursiveASTVisitor` | 标签（宏展开成 `switch (S->getStmtClass())`） | 返回值 `bool`（继续与否） | AST **遍历**骨架 |
| `clang::StmtVisitor` / `DeclVisitor` | 虚函数（`S->visit(v)`） | `void`，收指针 | 按类型**回调** |
| `dyn_cast<X>(S)` / `isa<X>(S)` | 标签（`classof` 查 `getKind()`） | 返回指针/布尔 | 定向**类型测试** |

关键在 `dyn_cast`：**它不是 `dynamic_cast`**。LLVM 给每个类层次定义了
`static bool classof(const Base *B) { return B->getKind() == K; }`，
`dyn_cast` 走的就是这个 —— **零 RTTI**，因为类型标签是对象自己带的一个枚举字段。

对照表：

| 概念 | clang / LLVM | 本实现 |
|---|---|---|
| 节点种类枚举 | `Stmt::StmtClass`、`Decl::Kind` | `NodeKind`（`include/ast.h:86`，32 种） |
| 零 RTTI 的类型测试 | `isa<X>(N)` → `classof` 查 `getKind()` | `node->kind == NodeKind::X` |
| 零 RTTI 的向下转换 | `cast<X>(N)`、`dyn_cast<X>(N)`（失败返回 null） | `std::static_pointer_cast<X>(node)` |
| 虚分派回调 | `S->visit(MyVisitor)` | `node->accept(visitor)` |
| 访问者基类 | `RecursiveASTVisitor`（TableGen 自动生成） | `AstVisitor`（手写，32 个 `visit`） |

★ 本项目的 `static_pointer_cast` 之所以安全，与 `cast<X>` 同理：
**`kind` 由节点的构造函数设定，恒等于自身类型** —— 这是"标签与类型永不失配"的不变量。

## 三、判据：handler 只要引用 → 访问者；还要所有权或返回值 → 标签

这是全项目统一采用的**唯一判据**，写在 `include/semantic_analyzer.h` 的分派说明里。
它的两条理由都很硬：

**① 所有权。** `AstVisitor::visit(X&)` 只给引用。而 Sema 的声明处理链里，
handler 拿到 decl 后要做的第一件事往往是**交给注册表**：

```cpp
m_classDecls[decl->name] = decl;   // ← 存的就是 shared_ptr
m_globalVars.push_back(decl);
```

从引用还原 `shared_ptr` 是**不安全**的 —— 对象未必由 `shared_ptr` 持有，
还原出的控制块是错的（可能二次析构）。硬套访问者就得引入"当前节点暂存槽"
这类隐藏状态，比 switch 难读得多。

**② 返回值。** `inferType` / `cloneExpr` / `estimateBlockSize` 都是**取值型**
递归：每个 handler 要返回 `TypePtr` / `ExprPtr` / `uint32_t`，而 `visit` 返回 `void`。
硬套就得约定"结果写进哪个成员槽"，且要 14 处都遵守这个不成文的约定。

反过来，**语句处理链**（Sema `processStmt`、CodeGen `emitStmt`/`emitExpr`）
的 handler 形状恰好是 `void f(X&)`、不需要所有权 —— 正是访问者。
所以它们用 `accept`。

### 分派路径对比

```text
【访问者】语句链                                   【标签】表达式/声明链
                                                 
processStmt(stmt)                                 inferType(expr)
      │                                                 │
      │ stmt->accept(*this)                             │ switch (expr->kind)
      ▼                                                 ▼
 ┌─────────┐                                     ┌──────────────┐
 │  vptr   │ ──▶ vtable[slot] ──▶ visit(X&)      │  NodeKind 枚举│ ──▶ 跳表/比较
 └─────────┘      一次间接跳转                       └──────────────┘
                                                 
 代价：1 次内存读 + 1 次间接跳转                     代价：1 次比较（+ 跳转）
 好处：新增节点不必改任何分发器                      好处：可返回值、可交出所有权
```

## 四、本次改造的量化结果

口径：**实际调用数**（`grep -c dynamic_pointer_cast` 去掉注释行；头文件里只有注释提及）。

| 文件 | 改造前 | 改造后 |
|---|---|---|
| `src/semantic_analyzer.cpp` | 45 | **0** |
| `src/codegen.cpp` | 43 | **0** |
| `src/main.cpp`（AstDumper） | 32 | **0** |
| `src/template_instantiation.cpp` | 24 | **0** |
| `src/parser.cpp` | 3 | **0** |
| 全项目 | **147** | **0** |

四条 N 路试探链（`Sema::inferType` 14 级、`CodeGen::emitExpr` 13 级、
`CodeGen::emitStmt` 8 级、`main.cpp: dumpExpr` 14 级）全部消失。

一个副产品：**改完才发现有三处结构相同的重复代码**（Pass 2 注册函数、
Pass 3 分析函数体的命名空间递归走查），提取成 `forEachFunctionDecl`
——这是"把 if-else 链换成声明式分派"顺手暴露出来的。

## 五、可复现实验

```bash
# 1) 分派机制确实换了：改造后全项目零 RTTI 转型
grep -rn "dynamic_pointer_cast" src/ include/ | grep -v "//"
#   → 只剩注释里对历史的说明，无任何实际调用

# 2) 分派成本：看汇编里不再有 RTTI 调用
./minicc tests/tmpl/test_tmpl_43_order_three_way.cpp -S -o /tmp/t.s
grep -c "__dynamic_cast" /tmp/t.s      # → 0

# 3) 改了等于没改（重构纪律，见 docs/REFACTOR-ast-visitor.md）
./logdiff.sh diff      # → ✅ 日志逐字节零差异
ctest --test-dir build-linux        # → 159/159

# 4) 分派语义的白盒断言（tests/unit/test_ast_visitor.cpp）
./build-linux/unit_tests --gtest_filter='AstVisitorDispatch.*'
```

## 六、边界：什么时候**不该**动

重构最容易过火的地方。以下三类保留标签分派（但用 `kind` 判断 + `static_pointer_cast`，
不用 RTTI）：

- **定向类型测试**：`if (expr.callee->kind == NodeKind::Member)` ——
  问对象一个具体问题，不是"路由到 N 个 handler"，2~4 路 switch/if 最直白；
- **递归查询走查**：`collectBases` / `emitGlobalVars` —— 递归结构是自定义的、
  还往外部 map 里累积，改访问者要引入状态机；
- **取值型递归**：`estimateBlockSize` —— 见第三节判据②。

★ 一句话记法：**"把节点类型路由到处理函数"用访问者；"问对象一个具体问题"用标签。**

## 七、相关文件

- `include/ast_visitor.h` —— 访问者基类（32 个 `visit` 重载，默认空体）
- `include/ast.h:97` —— `ASTNode::accept` 纯虚挂钩（★ 键函数坑见 REFACTOR 文档第三节）
- `docs/REFACTOR-ast-visitor.md` —— 本次重构的完整记录：病根、边界、护栏、代价
- `include/semantic_analyzer.h` —— 分派判据的正文注释（本项目唯一权威表述）
- `logdiff.sh` —— "日志逐字节不变"的护栏脚本
