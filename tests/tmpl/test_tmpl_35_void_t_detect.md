# test_tmpl_35 · 一行 `is_range<Container>::value` 的完整推导过程

> 配套测试：`tests/tmpl/test_tmpl_35_void_t_detect.cpp`
> 相关文档：`docs/learn/20`（decltype / SFINAE）、`docs/learn/19`（类模板特化）、`docs/learn/21`（偏序裁决）

本文只追一行代码：

```cpp
bool has_container = is_range<Container>::value;
```

它看着短，实际串起了本项目最长的两条链：**Parser 的模板 id 歧义消解**、
**Sema 的偏特化匹配 → void_t 探测 → 静态常量折叠**。

行号约定：下文所有行号都指 `tests/tmpl/test_tmpl_35_void_t_detect.cpp`。

---

## 1. 全景：这一行要过五道关

```text
    源码文本  bool has_container = is_range<Container>::value;
       │
       │ ① Parser：拆结构（不查符号表、不求值）
       ▼
    MemberExpr{ .value, isTypeAccess=true }
      └── VarExpr{ "is_range", explicitTemplateArgs=[Container] }
       │
       │ ② Sema Pass 3：foldStaticConst 认出 `X::value` 静态成员形态
       ▼
    resolveType( is_range<Container> )  ──►  getOrInstantiateClass
       │
       │ ③ 补齐默认实参：<Container>  →  <Container, void>
       ▼
    selectClassTemplate  三路择优
       ├─ ① 全特化：逐位 equals          → 未命中
       ├─ ② 偏特化：matchPattern 逐位合一
       │      位 0：P = T          A = Container  ⇒  bind: T := Container
       │      位 1：P = void_t<..> A = void       ⇒  展开探测 → 归约成 void → 恒等 ✓
       │      ⇒ 命中！
       └─ ③ 主模板：兜底（本行走不到）
       │
       │ ④ 实例化：is_range<Container> → 类 is_range_Container_void
       ▼
       │ ⑤ 静态常量折叠：沿继承链查到 true_type::value = 1
       ▼
    BoolLiteralExpr{ true }   ⇒  has_container : bool
```

五道关里，**真正需要"推导"的只有第 ③ 关**：前两关是结构翻译，后两关是执行结果。
下面按阶段拆开：先看 Parser 之后的**结构**（第 2 节），
再看 Sema 的**识别过程**（第 3 节），最后逐步展开七个关键步骤（第 4 节）。

---

## 2. Parser 之后的结构：完整的 AST

Parser 吐出的是一棵 **`TranslationUnit`**，`declarations` 里 5 个顶层节点。
下面是**带真实字段**的结构图（字段名取自 `include/ast.h`）—— dump 打印的是简化版，
这里把指针指向的东西也展开了。

### 2.1 整体骨架

```text
TranslationUnit
└─ declarations : vector<DeclPtr>[5]
   │
   ├─[0] ClassDecl ──────────────────────────────────────────────────────
   │      name           = "Container"
   │      baseClassNames = []
   │      fields         = []
   │      methods        = [ begin(), end() ]        ← 只有源码里写的
   │      staticConsts   = {}
   │      typeAliases    = {}
   │         │
   │         ├─ FuncDecl{ name="begin", returnType=Type{Int},
   │         │            ownerClassName="Container",
   │         │            body = BlockStmt
   │         │                    └─ statements[0] = ReturnStmt
   │         │                                        └─ value = IntLiteral{1} }
   │         └─ FuncDecl{ name="end",   returnType=Type{Int},
   │                      body = BlockStmt
   │                              └─ ReturnStmt → IntLiteral{2} }
   │
   ├─[1] ClassDecl ──────────────────────────────────────────────────────
   │      name = "Empty"    fields=[]    methods=[]      ← 全空
   │
   ├─[2] TemplateDecl ── 主模板 ★ ───────────────────────────────────────
   │      specKind       = TemplateSpecKind::Primary
   │      templateParams = [ {name="T",         kind=Type, hasDefault=false}
   │                         {name="$unnamed0", kind=Type, hasDefault=true,
   │                          defaultArg=TemplateArg{Type: void},
   │                          isUnnamed=true} ]            ← ★ 见 2.2
   │      specPattern    = []                              ← 主模板没有模式
   │      classTemplate  = ClassDecl{ name="is_range",
   │                                  baseClassNames=["std::false_type"] }
   │      funcTemplate   = nullptr
   │
   ├─[3] TemplateDecl ── 偏特化 ★ ───────────────────────────────────────
   │      specKind       = TemplateSpecKind::PartialSpec
   │      templateParams = [ {name="T", kind=Type} ]
   │      specPattern    = vector<TypePtr>[2]              ← ★★ 全篇最关键的结构
   │      classTemplate  = ClassDecl{ name="is_range",
   │                                  baseClassNames=["std::true_type"] }
   │
   └─[4] FunctionDecl ───────────────────────────────────────────────────
          name       = "main"
          returnType = Type{Int}
          body       = BlockStmt
                        └─ statements[7]  ← 见 2.4
```

★ **注意 ctor / dtor 不在这里**。`Container` 的方法只有 `begin` / `end` ——
合成构造/析构是 **Sema Pass 1** 干的（日志里 `[register] class 'Container'`
才列出 `Container()` 和 `~Container()`）。Parser 只管把写出来的东西建树。

### 2.2 放大镜 ①：主模板的形参表

```text
   template <typename T, typename = void>
                            ~~~~~~~~~~~~~~~~
                            没有名字，但有默认值

   templateParams[0] ─┬─ name        = "T"
                      ├─ kind        = Type
                      └─ hasDefault  = false

   templateParams[1] ─┬─ name        = "$unnamed0"    ← Parser 合成的
                      ├─ kind        = Type
                      ├─ hasDefault  = true
                      ├─ defaultArg  = TemplateArg{ kind=Type, type=Type{Void} }
                      └─ isUnnamed   = true
```

> **为什么要有 `$unnamed0` 这个假名字**：替换表（subst）的键只能是字符串，
> 而"无名形参"没有名字 —— 但它的**默认值 void 必须能被查出来**。
> 合成名用 `$` 开头，而 `$` 不是合法标识符字符，用户代码永远写不出同名引用，
> 于是语义上等价于"这个名字不可见"（[temp.param]/3）。

### 2.3 放大镜 ②：偏特化的 `specPattern` —— 一整棵树

`is_range<T, std::void_t<decltype(...), decltype(...)>>` 中
**尖括号里的那串**被存成 `specPattern`，两位：

```text
specPattern[0] = Type{ kind = TemplateParam, templateParamName = "T" }
                 └─ 是形参 ⇒ 合一时当【变量】，可以 bind

specPattern[1] = Type{ kind = Class, name = "std::void_t",
                       templateArgs = vector<TemplateArg>[2] }
                 └─ 不是形参 ⇒ 合一时当【结构】，走 reduce
                     │
                     ├─[0] TemplateArg::ofType( ────────────────────────┐
                     │        Type{ kind = Decltype,                     │
                     │              decltypeParen = false,               │
                     │              decltypeExpr  = ●──────┐             │
                     │        } )                          │             │
                     │                                     ▼             │
                     │            【decltype 里挂着的表达式树】（半成品） │
                     │            CallExpr                                 │
                     │            ├─ callee = MemberExpr{                  │
                     │            │            memberName = "begin",       │
                     │            │            isArrow    = false,         │
                     │            │            object = CallExpr ──┐       │
                     │            │          }                     │       │
                     │            │                                ▼       │
                     │            │                 CallExpr{ callee = VarExpr{ │
                     │            │                            name = "std::declval", │
                     │            │                            explicitTemplateArgs = [ │
                     │            │                              Type{TemplateParam "T"} │
                     │            │                            ] } }                      │
                     │            └─ arguments = []        ← declval<T>() 无实参        │
                     │                                                                   │
                     └─[1] 与 [0] 完全同构，只是 "begin" → "end"  ◀────────────────────┘
```

★★ **这张图是整篇文档的核心**。请注意两件事：

1. `specPattern[1]` **深处藏着一个 `Type{kind=TemplateParam, name="T"}`**
   —— 它是 `declval<T>()` 的模板实参。**P 侧的模式里也有 `T`**，
   只不过埋了三层深（void_t → decltype → CallExpr → VarExpr → TemplateArg）。
   合一时要能**钻进去**把这个 `T` 也 bind 上。
2. `decltype` 节点此刻**只是个壳，里面挂着没求值的表达式树** ——
   日志里那句 `[deferred: 不求值，留待替换阶段]` 说的就是这个。

### 2.4 放大镜 ③：第 70 行那棵子树

```text
   bool has_container = is_range<Container>::value;

   VarDeclStmt ─┬─ name         = "has_container"
                ├─ declaredType = Type{ kind = Bool }
                ├─ initializer  = MemberExpr ──┐
                └─ location                        │
                                                   ▼
                MemberExpr{ memberName   = "value",
                            isArrow      = false,
                            isMethodCall = false,     ← 还没定，Sema 填
                            isTypeAccess = true,      ← ★★ Parser 埋的开关
                            object = VarExpr ──┐
                          }                     │
                                                ▼
                VarExpr{ name = "is_range",
                         explicitTemplateArgs = [ TemplateArg::ofType(
                                                    Type{ kind=Class,
                                                          name="Container" } ) ],
                         resolvedType = nullptr }   ← Sema 阶段才会填
```

两个布尔值决定了后面走哪条路：

| 字段 | 值 | 谁读它 | 作用 |
|---|---|---|---|
| `isTypeAccess` | `true` | `foldStaticConst` | 分流：静态常量折叠 ≠ 字段偏移读取 |
| `explicitTemplateArgs`（非空） | `[Container]` | `foldStaticConst` | 先把它解析成**具体类**才知道查哪个类的静态成员 |

### 2.5 Parser 留下的 4 条线索

Parser 不查符号表、不求值，它只**埋线索**。这 4 条线索就是 Sema 的全部输入：

| # | 线索（存在哪） | 值 | Sema 拿它干什么 |
|---|---|---|---|
| 1 | `MemberExpr.isTypeAccess` | `true` | 走 `foldStaticConst` 而不是字段访问 |
| 2 | `VarExpr.explicitTemplateArgs` | `[Container]` | 拼出模板 id 去实例化 |
| 3 | `TemplateDecl.specPattern` | `[T, void_t<decltype(..)>]` | 当合一算法的 **P 侧模式** |
| 4 | `TemplateParam.hasDefault/defaultArg` | `$unnamed0 → void` | 补齐实参表，让位数对齐 |

---

## 3. 语义分析：怎么识别的（立体视图）

### 3.1 四个平面

把 Sema 想成**四个平面**叠加：A 只读、B 只读、C 可写、D 写回 B。
第 70 行的处理就是一条**从 A 穿到 D** 的竖线。

```text
        ┌──────────────────────────────────────────────────────────────┐
 平面 A │ AST（Parser 产物，全程只读）                                    │
        │                                                              │
        │   MemberExpr{ isTypeAccess=true, memberName="value" }        │
        │     └─ VarExpr{ name="is_range", explicitTemplateArgs=[C] }  │
        │                                                              │
        └───────────────────────────┬──────────────────────────────────┘
                                    │  ① foldStaticConst 读出形态
                                    │  ② resolveType(is_range<Container>)
                                    ▼
        ┌──────────────────────────────────────────────────────────────┐
 平面 B │ 查找表（Pass 1 建好，此后只读）                                 │
        │                                                              │
        │   m_classDecls["Container"]     ──► ClassDecl                 │
        │   m_classTemplates["is_range"]  ──► 主模板 TemplateDecl        │
        │   m_partialSpecs["is_range"]    ──► [ 偏特化 #1 ]   ★ vector  │
        │   m_explicitSpecs["is_range"]   ──► （不存在 → ① 路跳过）      │
        │                                                              │
        └───────────────────────────┬──────────────────────────────────┘
                                    │  ③ 三路择优：拿偏特化当 P 侧
                                    ▼
        ┌──────────────────────────────────────────────────────────────┐
 平面 C │ 推导（可变 —— 替换表在这里长出来）                              │
        │                                                              │
        │   subst = {}                                                 │
        │   第 0 位  bind：subst["T"] = Container                       │
        │   第 1 位  reduce(void_t)                                     │
        │        └─ 把 subst 代进 decltype 操作数 → 真的求值             │
        │              ├─ 成功 ⇒ void_t 归约成 void ⇒ 与 A 恒等 ✓        │
        │              └─ 失败 ⇒ throw SubstitutionFailure ⇒ 候选移出 ✗  │
        │                                                              │
        └───────────────────────────┬──────────────────────────────────┘
                                    │  ④ 用 subst 实例化
                                    ▼
        ┌──────────────────────────────────────────────────────────────┐
 平面 D │ 产物（新类注册【写回平面 B】，供后续复用）                       │
        │                                                              │
        │   新类 is_range_Container_void : public std::true_type        │
        │        └─ 注册进 m_classDecls → 下一次 is_range<Container>     │
        │           直接命中缓存，不再推导                                │
        │                                                              │
        │   第 70 行的结果：foldStaticConst 沿基类链查到 value=1         │
        │        ⇒ BoolLiteral{true} ⇒ has_container : bool            │
        │                                                              │
        └──────────────────────────────────────────────────────────────┘
```

★ **D 写回 B** 这一笔是"按需实例化"的全部含义：模板实例化**不是一次性批处理**，
而是"用到才生成，生成完就进表"。日志里 `Phase 4: Template Instantiation`
只处理了 `is_range<int,double>` 这种**显式**实例化请求，
三个真正用到的实例早在 Pass 3 就被按需造出来了。

### 3.2 调用深度剖面：从第 70 行一路钻到 `int`

把调用栈**竖过来**看：一行一层，**每缩进一格 = 多钻进去一层结构**。
单线缩进画"谁调谁"，不再有并列的竖线 —— 那样分不清哪根线属于谁。

右边两列回答"这一层拿到的**是什么节点**、据此**分派到哪个函数**" ——
`inferType` 本身是个 15 路 `switch (expr->kind)`
（`semantic_analyzer.cpp:3102`），全项目的表达式求值都从这一个漏斗进去，
再按标签散到 `inferXxx` 各家去：

```text
 ── 调用栈（自外向内，每缩进一格 = 深一层）────────────────────────────────────
                                                       实参类型           分派去向
 visit(VarDeclStmt&)                                   VarDeclStmt        accept 虚分派
  └─ foldStaticConst(initializer)                      MemberExpr         按 kind 判定
     └─ resolveType(tid)                               Type{Class,…}      模板 id 分支
        └─ getOrInstantiateClass(tid)                  TypePtr            缓存→择优→实例化
           ├─ checkTemplateArguments                   TemplateDecl       位数/形态校验
           └─ selectClassTemplate                                         【三路择优】
              └─ matchPattern(P, A)                    TypePtr × 2        逐位循环 i=0,1
                 ├─〔位 0〕Sfinae::attempt                                     ← 吸收圈 ①
                 │   └─ reducePattern(P[0])            Type{TemplateParam} 非 void_t⇒原样
                 │      └─ deducePair(P[0], A[0])      ⇒ bind ⇒ subst["T"]=Container
                 │
                 └─〔位 1〕Sfinae::attempt                                     ← 吸收圈 ②
                     └─ reducePattern(P[1])            Type{Class"void_t"}  void_t 分支 ★
                        └─ substituteType(t, subst)    Type{Decltype}     isDecltype 分支
                           └─ evaluateDecltype(e)      ExprPtr            唯一"硬→软"
                              └─ inferType(…)          CallExpr           case Call
                                 └─ inferCall
                                    └─ inferType(…)    MemberExpr         case Member
                                       └─ inferMember
                                          └─ inferType(…)   CallExpr      case Call
                                             └─ inferCall ─→ 内建命中 ⇒ Container&&
                                                    │
                                                    ▼
                                             ⇒ int   ✓ 实参 1（begin）探测通过
                                             ⇒ int   ✓ 实参 2（end）走【同一条路】，不重复画
                                                    │
                                        P=void 与 A=void 恒等 ✓ ⇒ 归约成 void

 ─────────────── 以上是【从第 70 行一路钻到底】，下面是【一层层回来】─────────────

 回到 resolveType 层：此时已选定偏特化 'is_range<T, void_t<...>>'
      └─ lookupStaticConst("is_range_Container_void", "value")
         └─ 沿继承链走 1 层 → std::true_type::value = 1
            └─ 返回 BoolLiteral{true}                  ← 折叠完成
```

这张图分三段读：

| 段 | 从上到下是什么 |
|---|---|
| ① 往下钻 | `visit` → `foldStaticConst` → `resolveType` → `getOrInstantiateClass` → `selectClassTemplate` → `matchPattern` |
| ② 两位探测 | 〔位 0〕bind 出 `T=Container`；〔位 1〕进 `void_t`，一路钻到 `inferType` |
| ③ 往上回 | `selectClassTemplate` 选定偏特化 → `lookupStaticConst` 取到 `value = 1` |

右两列是"这一层拿到的**实参是什么节点**、据此**分派到哪**"。★ 每一层的分派目标
都由节点类型唯一决定 —— `CallExpr` 进 `inferCall`、`MemberExpr` 进 `inferMember`、
`Decltype` 进求值器…… 这就是零 RTTI 的标签分派
（见 `docs/learn/29-ast-dispatch-two-idioms.md`）。

全图**最深、也最关键**的是最底下那四层（`inferType → inferCall → inferType
→ inferMember → inferType`）—— `decltype` 不是被"比一比"，而是**真的下钻去求值**：
真的查成员、真的走调用决议。它内部长什么样、日志按什么顺序印，下一节（3.3）单独放大。

### 3.3 放大镜：深度 10 内部 —— 先递归 `object`，再查表

上一节图里最底下那三层（日志深度 12 / 11 / 10）落到 `inferMember` 里，
而 `inferMember` **只有两半**：

```cpp
TypePtr SemanticAnalyzer::inferMember(MemberExpr& expr) {
    // ── 前半：递归求值 object —— 这一步可以是【任意深】的子树 ──
    m_inferDepth++;
    TypePtr objType = inferType(expr.object);          // 4439-4440
    m_inferDepth--;
    if (expr.isArrow && objType->isPointer()) objType = objType->pointeeType;
    if (objType->isReference()) objType = objType->referencedType;   // 剥 Container&&

    if (!objType || !objType->isClass())
        error("Cannot access member '...' on non-class type '...'");  // ← int  走这里

    // ── 后半：拿 objType 当上下文，查名字 —— 【不再递归】──
    auto fieldInfo = objType->classLayout.findField(expr.memberName); // ① 字段
    if (fieldInfo) return fieldInfo->type;

    for (auto& m : m_classDecls[objType->name]->methods)              // ② 方法
        if (m->name == expr.memberName) return m->returnType;         // ★ 直接给，不求值

    error("No member '...' in class '...'");                          // ← Empty 走这里
}
```

**关键的不对称**：左边**递归**（可任意深），右边**查表**（哈希 + 线性扫方法名，
在表达式树里是叶子行为）。注意后半段是**查表**不是"解析" ——
解析早在 Parser 阶段做完了，Sema 这里只是拿类型当上下文做**名字决议**。

顺序不能反：**查哪个名字，由 `objType` 决定**。这正对应
[basic.lookup.classref] —— 成员名查找以对象类型为语境。

#### 这不是 `inferMember` 独有，是所有复合表达式的通用形状

`inferType` 整体是**自底向上的后序遍历**：先算子节点，再算自己。

| 节点 | 前半：先递归子节点 | 后半：再算自己 |
|---|---|---|
| `BinaryExpr` | `inferType(left)` + `inferType(right)` | 算结果类型 |
| `IndexExpr` | `inferType(object)` + `inferType(index)` | 查 `at()` 的返回类型 |
| `MemberExpr` | `inferType(object)` | 查字段 / 方法 |
| `CallExpr` | `inferType(callee)` + 各实参 | 查函数返回类型 |

所以 `declval<C>().begin()` 能跑通，**不是 Sema 里对它有什么特判** ——
而是 object 位置恰好坐着一个 `CallExpr`，撞上了 declval 内建。

#### `objType` 决定一切：同一个 `MemberExpr` 形状，三份结果

```text
                  同一个 MemberExpr：  .begin()
                             │
              前半段：inferType(object)   ← 三个类型【在这里已经不同了】
                             │
         ┌───────────────────┼───────────────────┐
         ▼                   ▼                   ▼
    objType =           objType =           objType =
    Container&&         Empty&&             int&&
         │                   │                   │
    剥引用 → Container   剥引用 → Empty      剥引用 → int
         │                   │                   │
         ▼                   ▼                   ▼
  ② 扫 methods 命中      ② 扫 methods 落空     ✗ isClass() 就挂了
    return int             error(...)          error(...)
         │                   │                   │
         ▼                   ▼                   ▼
     探测通过              软失败               软失败
```

一句话：**前半段的输出（`objType`）是后半段的输入**。

下一节（3.4）的图说三个类型"深度 0 ~ 9 完全相同"—— 那说的是**调用链**：
进到 `inferMember` 之前，三份调用序列逐帧一致。而分道的**具体位置**就在这里：
`inferType(expr.object)` 返回的那一刻，`objType` 已经不是同一个类型了。
两个说法是同一件事的两种粒度。

#### 这三行日志，是表达式树上的三个节点

原图上 `[member]` 和 `[call]` 并排、都标"深度 10"，看不出它们的关系 ——
因为它们是**日志标签**，不是树上的位置。先把这棵表达式树摆出来：

```text
   CallExpr ⓐ ── 函数调用：.begin()
     │              日志 [call]     begin(0 args) → int              ← 最后印
     │
     ├─ callee ─► MemberExpr ⓑ ── 成员访问：成员名 "begin"
     │              │            日志 [member]   Container.begin() → int
     │              │
     │              └─ object ─► CallExpr ⓒ ── 函数调用：declval<Container>()
     │                             │           日志 [declval] → Container&& ← 最先印
     │                             │
     │                             ├─ callee ─► VarExpr{ name = "std::declval",
     │                             │                     explicitTemplateArgs = [T] }
     │                             │                     ★ 名字引用，不是类型！
     │                             └─ arguments ─► （空）
     │
     └─ arguments ─► （空）
```

**求值顺序 = 树的【后序】：ⓒ → ⓑ → ⓐ**（先算子节点，再算自己）。
日志深度 `12 → 11 → 10` 就是这个顺序的倒影 —— 不是"深度在减小"，
是**最深的那棵子树最先算完**。

★ 这里最容易误会的一点：**`declval` 不是类型，是个函数名**。
它在树里是最底下那个 `VarExpr`（`name = "std::declval"`），
外面套一层 `CallExpr` 才构成"调用"。真 C++ 里它声明在 `<utility>`：

```cpp
template<class T> add_rvalue_reference_t<T> declval() noexcept;   // 只有声明，【没有定义】
```

本项目没有 `<utility>`，于是和 `std::void_t` 同策 ——
`inferCall` 按名字认出它（`semantic_analyzer.cpp:3476`），直接给出 `T&&`，
**不查符号表、不实例化、不产生符号**。所以它才能当整条链的硬叶子。

这也解释了日志那三级缩进（缩进数 = `m_inferDepth`）—— 对应的调用栈是：

```text
inferCall(begin)                                      ← 外层，depth D
  │
  ├─ 3454 预推导 inferType(MemberExpr)                 ← 内层，depth D+1
  │    └─ inferMember
  │         ├─ 4439 inferType(declval<C>())  → [declval]    ← 最内层，depth D+2
  │         └─ 4483 扫 methods 命中          → [member]
  │
  └─ 3489+ 找符号 → [call]                             ← 回到外层才打印
```

★ 注意 `3454` 那次**预推导是单独补的**，不是主流程需要的 ——
它是为 CodeGen 服务的：虚调用要靠 `mem->object` 上的 `resolvedType`
去解引用出类名、查 vtable 条目（见 `semantic_analyzer.cpp:3447-3456` 的注释）。
没有它，`[member]` 这行日志根本不会出现，`[declval]` 也就无从触发。

#### 每一层走的是哪个函数：完整对照表

把 3.2 那张图的两列抽出来单看。**同一个 `inferType` 漏斗，按 `kind` 散到不同方法**：

| 深度 | 调用 | 实参的节点类型 | 分派点 / 代码位置 |
|---|---|---|---|
| 0 | `visit(VarDeclStmt&)` | `VarDeclStmt`（语句，不是表达式） | `accept()` 虚分派 → `Sema::visit` |
| 1 | `foldStaticConst` | `MemberExpr{isTypeAccess=true}` | 按 `kind` 形态判定，**不经过 `inferType`** |
| 2 | `resolveType` | `Type{kind=Class, templateArgs=[Container]}` | `resolveType` 的**模板 id 分支** |
| 3 | `getOrInstantiateClass` | `TypePtr` | 查缓存 → 择优 → 实例化 |
| 3 | `checkTemplateArguments` | `TemplateDecl` + `TypePtr` | 位数 / 形态校验 + 补默认实参 |
| 4 | `selectClassTemplate` | 同上 | 【三路择优】全特化 / 偏特化 / 主模板 |
| 5 | `matchPattern(P, A)` | 两个 `TypePtr` | `template_deduction.cpp:82`，逐位循环 |
| 6 | `reducePattern(P[0])` | `Type{TemplateParam "T"}` | 非 `void_t` ⇒ **原样返回** |
| 6 | `deducePair(P[0], A[0])` | `TypePtr` × 2 | P 是"变量"（在 `paramNames` 里）⇒ `bind` |
| 7 | `reducePattern(P[1])` | `Type{Class "void_t"}` | **`void_t` 分支**（`template_deduction.cpp:244`） |
| 8 | `substituteType(t, subst)` | `Type{kind=Decltype}` | **`isDecltype()` 分支**（`template_instantiation.cpp:602`） |
| 9 | `evaluateDecltype(e)` | `ExprPtr`（替换后的表达式树） | 唯一"硬→软"降级点 |
| 10 | `inferType(...)` | **`CallExpr`**（`.begin()`） | `switch(kind)` → `case Call` → `inferCall` |
| 11 | `inferType(...)`（内层） | **`MemberExpr`** | `case Member` → `inferMember` |
| 12 | `inferType(...)`（最内层） | **`CallExpr`**（`declval<C>()`） | `case Call` → `inferCall` → 命中 declval 内建 |

★ 三处"看类型下菜"的分派点值得记住，它们是断链的关键：
`kind=Decltype`（延迟求值）、`kind=Class && name=="void_t"`（探测归约）、
`CallExpr/MemberExpr`（表达式树按标签散开）。

#### 叶子在哪：`declval` 的 `return` 是整条链的终点

```cpp
// sema.cpp:3476-3490
if (calleeVar && (funcName == "std::declval" || funcName == "declval")) {
    TypePtr t = calleeVar->explicitTemplateArgs[0].type;   // 已经是替换后的 Container
    TypePtr r = Type::makeRValueReference(t);              // Container&&
    return r;        // ★ 硬叶子：不查符号表、不推导、不实例化、不再下钻
}
```

所以从深度 10 往下，**总共只有 2 层**（`[member]` 11、`[declval]` 12），
之后全是回退。`void_t<>` 里 `begin` / `end` 那两次探测是**并列的两条**，
不是套娃 —— 日志里 `[declval]` 出现两次，正是这个意思。

### 3.4 `Empty` / `int` 走的是同一份代码 —— 只在深度 10 分道

```text
                        ┌──────────────┐
                        │  深度 0 ~ 9  │   三个类型走的路径【完全相同】
                        │  逐位合一      │   （拆结构 → 补实参 → 挑候选 → bind T → 进 decltype）
                        └──────┬───────┘
                               │
                        ┌───────▼───────┐
                        │   深度 10     │   inferType( declval<X>().begin() )
                        │  求值 decltype │
                        └───────┬───────┘
                               │
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
     X = Container         X = Empty             X = int
     [member] 命中          ✗ 抛异常              ✗ 抛异常
     → int                 "No member 'begin'   "Cannot access member
                            in class 'Empty'"    'begin' on non-class
                                                 type 'int'"
          │                    │                    │
          │                    └────────┬───────────┘
          │                             ▼
          │                  ┌──────────────────────┐
          │                  │ 深度 9 的 catch 接住  │  evaluateDecltype
          │                  │ Sfinae::demote 降级   │  唯一一处"硬→软"
          │                  │ Sfinae::fail 发信号   │
          │                  └──────────┬───────────┘
          │                             ▼
          │                  ┌──────────────────────┐
          │                  │ 深度 6 的 matchPattern│  Sfinae::attempt
          │                  │ 把该候选移出候选集     │  ← 吸收点
          │                  └──────────┬───────────┘
          │                             ▼
          │                  ┌──────────────────────┐
          │                  │ 深度 4 回退主模板      │  ③ 路
          │                  │ 继承 std::false_type  │
          │                  └──────────────────────┘
          ▼                             ▼
     value = 1                    value = 0
   has_xxx = true              has_xxx = false
```

**"替换失败不是错误"的全部含义就在这张图的左右分叉里**：
右边那条路抛出的异常**不是编译器报错**，只是让一个候选消失。

### 3.5 异常的立体走向：谁抛、谁传、谁接

```text
  ┌─ Sfinae::attempt ─────────────────────────────────────────────┐ ← 吸收点
  │  SfinaeContext ctx;    ← RAII：标记"现在处于直接上下文"         │   (深度 6)
  │                                                               │
  │   try {                                                       │
  │       matchPattern(...)                                       │
  │         └─ reducePattern(void_t)                              │
  │             └─ substituteType(decltype)                       │
  │                 └─ evaluateDecltype                           │
  │                     └─ inferType                              │
  │                         └─ 查不到 begin ✗                      │
  │                             │                                 │
  │                             │ catch(runtime_error)            │ ← 降级点
  │                             │   Sfinae::demote(...)           │   (深度 9)
  │                             │   throw SubstitutionFailure ────┼──┐
  │                             ▼                                 │  │
  │                    （层层上抛，中间层一律不捕获）                 │  │
  │                                                               │  │
  │   } catch (const SubstitutionFailure& e) {                    │◀─┘
  │       rejected(desc, e.what());   ← 日志：⤵ 候选被移出候选集     │
  │       return false;               ← 不 rethrow ⇒ 软失败          │
  │   }                                                           │
  └───────────────────────────────────────────────────────────────┘
```

| 角色 | 谁 | 在哪一层 | 动作 |
|---|---|---|---|
| 产生方 | `evaluateDecltype` | 深度 9 | 把内层 `runtime_error` **降级**成 `SubstitutionFailure` 并抛出 |
| 传播方 | `inferType` / `substituteType` / `reducePattern` | 深度 10→7 | **一律不捕获**，任其冒泡（关键：中间层必须透明） |
| 吸收方 | `Sfinae::attempt` | 深度 6 | 捕获 ⇒ 候选移出 ⇒ **不 rethrow** |

★ 为什么中间层不能捕获：软失败一旦在半路被当成普通错误处理掉，
就变成了"这个偏特化匹配不了"之外的东西 —— 候选集不再可控，
SFINAE 退化成"第一次失败就报错"。

**三方的落点**（全项目收口点仅此三处，改代码时同步此表）：

| 角色 | 代码位置 |
|---|---|
| 信号类型 | `include/sfinae.h` → `class SubstitutionFailure` |
| 直接上下文标记 | `include/sfinae.h` → `SfinaeContext`（RAII） |
| 吸收器 | `include/sfinae.h` → `Sfinae::attempt` |
| ★ 本条链的吸收点 | `src/template_deduction.cpp:106`（`matchPattern` 的模式位归约） |
| 另一处吸收点 | `SemanticAnalyzer::inferCall` 的候选循环（函数模板重载决议） |
| 第三处吸收点 | `SemanticAnalyzer::classSpecAtLeastAsSpecialized`（偏序比较） |

判据（[temp.deduct]/8）：失败发生在**被替换的类型/表达式自身的构成过程**里
⇒ 直接上下文 ⇒ 软失败；失败发生在**被调用函数的函数体**里 ⇒ 不是直接上下文 ⇒ 硬错误。
上面 `No member 'begin'` 属于前者（是在求 `decltype` 操作数的类型时发现的），所以可以降级。

### 3.6 三条路并排看：同一行代码的三次执行

| | `is_range<Container>` | `is_range<Empty>` | `is_range<int>` |
|---|---|---|---|
| 补默认实参 | `<Container, void>` | `<Empty, void>` | `<int, void>` |
| 第 0 位 bind | `T := Container` | `T := Empty` | `T := int` |
| 第 1 位探测 | `begin()`→int、`end()`→int | 抛异常 | 抛异常 |
| 日志关键词 | `实参 1 探测通过` | `No member 'begin'` | `Cannot access member` |
| 候选集 | 偏特化存活 | 偏特化移出 | 偏特化移出 |
| 选中 | ② 偏特化 | ③ 主模板 | ③ 主模板 |
| 实例类 | `is_range_Container_void` | `is_range_Empty_void` | `is_range_int_void` |
| 基类 | `std::true_type` | `std::false_type` | `std::false_type` |
| `::value` | `1` | `0` | `0` |
| 折叠结果 | `BoolLiteral(true)` | `BoolLiteral(false)` | `BoolLiteral(false)` |

```text
      is_range<Container>::value ─┐
      is_range<Empty>::value     ─┼─► 同一个 foldStaticConst
      is_range<int>::value       ─┘   同一个 selectClassTemplate
                                     同一个 matchPattern
                                     同一个 void_t 探测
                                            │
                                     唯一的差异点：
                                     decltype 求值成不成功
```

## 4. 核心：从 `foldStaticConst` 到 `true` 的七步

### 第 1 步 · 认出形态（`foldStaticConst`）

```cpp
// src/semantic_analyzer.cpp:839
ExprPtr SemanticAnalyzer::foldStaticConst(ExprPtr expr) {
    if (expr->kind != NodeKind::Member) return expr;
    auto me = std::static_pointer_cast<MemberExpr>(expr);
    if (!me->isTypeAccess) return expr;               // ← Parser 埋的标记在这里生效
    if (me->object->kind != NodeKind::Var) return expr;
    auto ve = std::static_pointer_cast<VarExpr>(me->object);
    ...
```

三个条件全中 ⇒ 确认是 `X::value` 静态成员形态：

```text
    MemberExpr{ memberName="value", isTypeAccess=true }
      └── VarExpr{ name="is_range", explicitTemplateArgs=[Container] }
                  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
                  非空 ⇒ 第 2 步要先把它解析成具体类
```

### 第 2 步 · 把模板 id 解析成类

```cpp
    std::string clsName = ve->name;                       // "is_range"
    if (!ve->explicitTemplateArgs.empty()) {
        TypePtr tid = Type::makeClass(ve->name);          // Class("is_range") + 实参表
        for (const auto& ta : ve->explicitTemplateArgs)
            tid->templateArgs.push_back(TemplateArg::ofType(resolveType(ta.type)));
        TypePtr inst = resolveType(tid);                  // ★ 递归进 resolveType
        clsName = inst->name;                             // "is_range_Container_void"
    }
```

`resolveType` 认出这是**类模板 id**（`src/semantic_analyzer.cpp:1070`），转交实例化：

```text
  [sema:targ]   ✓ param 1: 'T' (type) ← Container
  [sema:targ] ✓ template arguments OK: is_range<Container>
  [sema:targ] ⤷ default argument filled: '$unnamed0' := void
```

### 第 3 步 · 补齐默认实参：`<Container>` → `<Container, void>`

```text
               调用点写的实参           补齐后参与匹配的实参
               ─────────────           ─────────────────────
  is_range<    Container        >  ⇒   < Container ,  void  >
                                              ↑         ↑
                                         用户写的   主模板的默认实参
```

★ **为什么这一步不能省**：偏特化的模式是**两位**
（`is_range<T, void_t<...>>`），而调用点只写了**一位**。
不补齐的话，第一次"实参个数 == 模式位数"的检查就过不了，
偏特化永远进不了候选集 —— void_t 惯用法整套失效。

补齐后两位对齐，才谈得上逐位合一。

### 第 4 步 · 三路择优（`selectClassTemplate`）

```cpp
// src/semantic_analyzer.cpp:4050
if (allTypeArgs) {
    // ① 全特化：逐位类型相等
    if (auto it = m_explicitSpecs.find(name); ...)  { ... }

    // ② 偏特化：用实参推导模式（两轮：先收集全部候选，再偏序裁决）
    if (auto it = m_partialSpecs.find(name); ...)   { ... }
}
// ③ 主模板兜底
```

本行的实际情况：

| 路 | 查的表 | 结果 |
|---|---|---|
| ① 全特化 | `m_explicitSpecs["is_range"]` | 表里没有这个键 → **跳过**（`is_range<int>` 那种才是全特化） |
| ② 偏特化 | `m_partialSpecs["is_range"]` | 1 条候选 → 进匹配 |
| ③ 主模板 | `m_classTemplates["is_range"]` | 只在 ② 全灭时才走 |

```text
  [spec:select] ★ selecting class template 'is_range' for <Container, void>
```

### 第 5 步 · 逐位合一：第 0 位是"变量"，第 1 位是"结构"

`matchPattern(specPattern, argTypes, paramNames, subst, reason)`
（`src/template_deduction.cpp:82`）把模式与实参**逐位对齐**：

```text
  位置   模式 P（偏特化）                                      实参 A
  ──────────────────────────────────────────────────────────────────────────
   0     T                                                  Container
         └─ 在 paramNames 里 ⇒ 是【可绑定变量】
            bind: T := Container
            [deduction]   P=T            A=Container    ⇒ T := Container

   1     std::void_t< decltype(std::declval<T>().begin()),
                      decltype(std::declval<T>().end()) >   void
         └─ 不在 paramNames 里 ⇒ 是【常量结构】
            走 reducePattern：把 T 代入后归约，看结果是否与 A 相等
```

★ **合一算法的前提**：区分模式里的"变量"与"常量结构"
（对应 `docs/learn/02` 讲的 P/A 对）。变量才进 `bind()`，
常量结构走结构比较。第 1 位不是变量 —— 它是个"要算出来才知道值"的结构。

### 第 6 步 · `void_t` 展开探测（整条链的枢纽）

`reducePattern` 认出第 1 位是 `void_t<...>`（`src/template_deduction.cpp:244`）：

```cpp
// ── void_t<...>：归约为 void（前提是各实参替换后都合法）──
bool isVoidT = P->isClass() && (P->name == "std::void_t" || P->name == "void_t");
if (!isVoidT) return P;
...
for (size_t i = 0; i < P->templateArgs.size(); i++) {
    // ★ 关键：substituteType 内部遇到 decltype 会真的求值；
    //   表达式不合法则抛 SubstitutionFailure，向上冒泡给 matchPattern。
    TypePtr t = inst.substituteType(P->templateArgs[i].type, tagged);
    Sfinae::probeStep(i + 1, t ? t->toString() : "?");
}
return Type::makeVoid();
```

展开第一个探测条件，全过程：

```text
   substituteType( decltype( std::declval<T>().begin() ),  { T → Container } )
        │
        │ ① 先替换操作数（decltype 的"两段式"：替换在前，求值在后）
        ▼
      [subst] ★ TemplateParam 'T' → 'Container' (direct replacement)
        │
        │  表达式树变成： decltype( std::declval<Container>().begin() )
        ▼
        │ ② 真的去求值 —— 不是"假装"
        │      [declval] std::declval<Container>() → Container&&
        │      [member]  Container.begin() → int   (method)
        │      [call]    begin(0 args) → int
        ▼
      [decltype]   ⇒ 声明类型 = int
      [sfinae]   ├─ 实参 1 探测通过 → int
```

第二个条件同理（`end()` → `int`）：

```text
      [sfinae]   ├─ 实参 2 探测通过 → int
      [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓
```

归约结果与实参对齐：

```text
      [deduction]   P=void         A=void         ⇒ 恒等 ✓
      [spec:select]   ├─ ② 候选：'is_range<T, std::void_t<decltype(...), decltype(...)>>' 匹配成功
      [spec:select]   │  唯一候选 → 直接选中 '...'
```

★ **"真的去求值"六个字是整件事的重量所在**。
如果 `decltype` 只是被当作一个占位类型比一比，
`void_t` 的实参永远"看着合法"，探测就变成了恒真 —— 整个惯用法失效。
必须真的去查成员、真的去走调用决议（`[call] begin(0 args) → int`），
查不到才算失败。

#### 追问：`declval<T>` 里的 `T`，到底在哪一行被换掉？

上面日志打的是 `[declval] std::declval<Container>()`。但看 `inferCall` 里那个内建分支，
它只是**朴素地取** `explicitTemplateArgs[0].type`，自己不做任何替换：

```cpp
// src/semantic_analyzer.cpp:3476-3486
if (calleeVar && (funcName == "std::declval" || funcName == "declval")) {
    if (!calleeVar->explicitTemplateArgs.empty() &&
        calleeVar->explicitTemplateArgs[0].type) {
        TypePtr t = calleeVar->explicitTemplateArgs[0].type;   // ← 谁把它从 T 换成了 Container？
        TypePtr r = Type::makeRValueReference(t);              // → Container&&
        std::cout << std::format("{}[declval] std::declval<{}>() → {}\n", ...);
        return r;
    }
}
```

答案：**替换点不在 3476，而在它之前——`cloneExpr` 的 `VarExpr` 分支。**

关键事实：`<T>` 这个显式模板实参**挂在 `VarExpr` 上，不是挂在 `CallExpr` 上** ——
因为 `std::declval<T>()` 里的 `<T>` 属于 callee 名字的一部分。
所以替换必须由 `cloneExpr` 处理 `NodeKind::Var` 的那一段负责：

```cpp
// src/template_instantiation.cpp:1055-1064（cloneExpr 的 NodeKind::Var 分支）
auto cloned = std::make_shared<VarExpr>(e.name);        // ① 克隆名字
cloned->location = e.location;
for (const auto& ta : e.explicitTemplateArgs) {         // ② 逐个处理显式实参
    if (ta.isType() && ta.type) {
        cloned->explicitTemplateArgs.push_back(
            TemplateArg::ofType(substituteType(ta.type, subst)));   // ★ 就是这一行
    } else {
        cloned->explicitTemplateArgs.push_back(ta);     // 值实参（NTTP）原样带过
    }
}
```

真做置换动作的是被它调用的 `substituteType` Case 1（`src/template_instantiation.cpp:555-574`）：

```cpp
if (type->isTemplateParam()) {
    auto it = subst.find(type->templateParamName);   // 查 "T"
    if (it != subst.end()) {
        std::cout << std::format("    [subst] ★ TemplateParam '{}' → '{}' (direct replacement)\n", ...);
        return it->second.type;                      // 返回 Container
    }
    std::cout << std::format("    [subst] TemplateParam '{}' not in substitution map, keep as-is\n", ...);
    return type;
}
```

于是日志里那两行是**紧挨着的一对「写—读」**：

```text
    [subst] ★ TemplateParam 'T' → 'Container' (direct replacement)   ← template_instantiation.cpp:573（写）
      [declval] std::declval<Container>() → Container&&              ← semantic_analyzer.cpp:3482（读）
```

中间只隔了一次函数返回。3482 打印出来的 `Container`，就是 573 行 `return` 的那个 `TypePtr`。

★ **蓝图上那个 `T` 至今还在** —— 替换是**非破坏性**的，
`substituteType` / `cloneExpr` 是纯函数式的：读原节点、返回新节点，
全程没有一次写回 `spec->specPattern`：

```text
原节点（spec->specPattern 里的，永不变）：
  CallExpr
  └── callee: VarExpr{ name="std::declval",
                       explicitTemplateArgs=[ TemplateParam("T") ] }   ← 至今还是 T
                              │
                              │  cloneExpr(1055-1064) → substituteType(555-574)
                              ▼
副本（concrete，栈上局部，只给求值用）：
  CallExpr
  └── callee: VarExpr{ name="std::declval",
                       explicitTemplateArgs=[ Container ] }            ← 3482 读的是这个
```

这解释了为什么实例化日志要把「蓝图」和「替换表」分开列——
匹配全做完之后，`║ Pattern:` 那一行印的仍然是 `is_range<T, ...>`：

```text
  [spec:select]   │  唯一候选 → 直接选中 'is_range<T, std::void_t<decltype(...), ...>>'
  [instantiate:class] ★ on-demand instantiation: is_range<Container>
  ║ Pattern:   is_range<T, std::void_t<decltype(...), decltype(...)>>   ← T 还在
  ║ Substitution map: { 'T' → 'Container', }
```

★ **两层替换的分工**：`explicitTemplateArgs` 里装的是 `TemplateArg::type`（`TypePtr`），
属于**类型位置** ⇒ 走 `substituteType`；若是值位置（NTTP）⇒ 走 `cloneExpr`。
这就是项目里"两层替换"的来源（见 `docs/learn/18`）。

### 第 7 步 · 实例化 + 静态常量折叠

选中偏特化后，用它当蓝图实例化：

```text
  [instantiate:class] ★ on-demand instantiation: is_range<Container>
  [subst:map] (specialization) substitution supplied by pattern matching: T := Container

  ╔══ Template Instantiation ═════════════════════════╗
  ║ Blueprint: is_range <typename T> [PARTIAL SPECIALIZATION]
  ║ Pattern:   is_range<T, std::void_t<decltype(...), decltype(...)>>
  ║ Instance:  is_range_Container_void
  ║ Substitution map: { 'T' → 'Container', }
  ║ Mangled: is_range_Container_void → _Z8is_rangeI9ContainervE
  ╚═══════════════════════════════════════════════════╝
```

注意 `[PARTIAL SPECIALIZATION]` 这个标记 —— 它说明用的是偏特化蓝图，
而它的基类是 `std::true_type`（不是主模板的 `std::false_type`）：

```text
  [register] class 'is_range_Container_void' : public std::true_type
```

最后 `lookupStaticConst` **沿继承链**找到 `value`：

```text
  [static] ★ 静态常量命中：std::true_type::value = 1 : bool（沿继承链第 1 层）
  [static] 折叠为字面量：is_range_Container_void::value(is_range) → 1
  [var decl] has_container : bool =   [infer] BoolLiteral(true) → bool
  [symbol] ✚ has_container : bool    stack@-8   ← 加入符号表
```

★ 为什么 `is_range_Container_void` 自己没有 `value` 也能查到：
`value` 来自基类 `std::true_type`（[class.member.lookup] 沿基类链查找）。
"偏特化继承 `true_type`，`::value` 就是真值"—— 这个惯用法的全部魔法就在这里。

最终这一行在 CodeGen 阶段等价于：

```asm
    movq $1, %rax        # BoolLiteral(true) —— 编译期已算完，运行时零开销
```

---

### 七步之后 · 收束：第 6 步 vs 第 7 步，同一个 `substituteType` 的两种语义

这是全文最容易看漏的一处——**替换在整条链上发生了两次，用的是同一张表、同一个引擎，
但目的完全不同**：

| | 第 6 步（探测） | 第 7 步（落地） |
|---|---|---|
| 调用者 | `reducePattern` 里**当场 new 的临时** `TemplateInstantiator`（`template_deduction.cpp:263-266`） | `m_instantiator`（Sema 成员，长驻） |
| 目的 | 回答一个问题：「这个模式位替换后合法吗？」 | 造出真类型 |
| 产物 | 栈上的 `concrete`，函数返回即销毁 | 注册进符号表的实例类 `is_range_Container_void` |
| 替换表来源 | 同一次 `matchPattern` 里刚推出来的 `subst` | 同一个表（`selectClassTemplate` 返回的 `taggedSubst`） |
| 失败时 | 抛 `SubstitutionFailure` → 候选出局（**SFINAE**） | 硬错误，编译中断 |
| 蓝图 | 只读，分毫未动 | 深拷贝一份再改 |

```text
  第 5 步 bind 写下的 T := Container
        │
        ├─► 第 6 步：临时 instantiator.substituteType(..., tagged)   ← 试探
        │      产物：concrete（栈上）  →  evaluateDecltype  →  合法/不合法
        │
        └─► 第 7 步：m_instantiator.instantiate(blueprint, args, &specSubst)   ← 兑现
               产物：is_range_Container_void（进符号表）
```

★ 一句话：**第 6 步的替换是"试探"，第 7 步的替换是"兑现"**。
同一张表从第 5 步的 `bind` 里写下，被这两处先后消费 ——
这正是 `void_t` 探测能"先假装试一次，成了再真做一遍"的全部机制。

---

## 5. 结构总览：哪些字段在哪个阶段被填上

轨迹：**Parser 埋线索 → Pass 1 建表 → Pass 3 消费**。

| 结构 / 字段 | 谁写 | 写的是什么 | 谁读 |
|---|---|---|---|
| `TemplateDecl.templateParams` | Parser | `[T, $unnamed0]`；后者带 `defaultArg=void` | Sema 补默认实参（第 3 步） |
| `TemplateDecl.specPattern` | Parser | `[T, void_t<decltype(..),decltype(..)>]` | `matchPattern`（第 5 步） |
| `Type{DecltypeT}` 内的表达式树 | Parser | `std::declval<T>().begin()`，**T 仍是形参** | `substituteType` 替换 + `evaluateDecltype` 求值（第 6 步） |
| `VarExpr.explicitTemplateArgs` | Parser | `[Container]` | `foldStaticConst` 拼模板 id（第 2 步） |
| `MemberExpr.isTypeAccess` | Parser | `true` | `foldStaticConst` 的分流开关（第 1 步） |
| `m_partialSpecs["is_range"]` | Pass 1 | `vector`，1 条 | `selectClassTemplate`（第 4 步） |
| `m_classTemplates["is_range"]` | Pass 1 | 主模板 | 兜底（第 4 步 ③） |
| `is_range_Container_void` 类 | 第 7 步实例化 | 继承 `std::true_type` | `lookupStaticConst` |
| `has_container` 符号 | 最后一步 | `bool, stack@-8` | CodeGen |

---

## 6. 可复现实验

```bash
# 一、完整日志（845 行，含预处理）
./minicc tests/tmpl/test_tmpl_35_void_t_detect.cpp -S -o /tmp/t35.s > /tmp/t35.log 2>&1

# 二、只看 Parser 阶段
awk '/Phase 2: Syntax/,/Phase 3/' /tmp/t35.log | grep -vE '^\s*$'

# 三、只看第 70 行（has_container）的推导
awk '/Function Body: main/,0' /tmp/t35.log | grep -vE '^\s*$'

# 四、看 AST 结构
./minicc tests/tmpl/test_tmpl_35_void_t_detect.cpp --dump-ast

# 五、真跑一遍（退出码 0 = 三个判定都对）
./minicc tests/tmpl/test_tmpl_35_void_t_detect.cpp -o /tmp/t35 && /tmp/t35; echo "rc=$?"

# 六、语义 oracle：与 clang 对照
clang++-18 -std=c++20 tests/tmpl/test_tmpl_35_void_t_detect.cpp -o /tmp/t35_clang && /tmp/t35_clang; echo "rc=$?"
```

---

## 7. 与 clang 对照

| 环节 | clang | 本实现 |
|---|---|---|
| 模板 id 歧义消解 | `Sema::isTemplateName` + `TryAnnotateTypeOrScopeToken` 试探/回滚 | Parser 的 `m_pos = saved` 回滚 |
| 无名形参 | `TemplateTypeParmDecl` 名字为空 | 合成名 `$unnamed0` |
| 补默认实参 | `Sema::CheckTemplateArgumentList` 尾部补 `TemplateArgument` | `getOrInstantiateClass` 里的补全分支 |
| `void_t` 语义 | `std::void_t` 是**别名模板**，走正常的别名解糖 | **按名识别**，在 `resolveType` 里直接归约（`src/semantic_analyzer.cpp:959`） |
| 特化选择 | `Sema::CheckClassTemplatePartialSpecializationArgs` + `DeduceTemplateArguments` | `selectClassTemplate` + `TemplateDeducer::matchPattern` |
| 偏序裁决 | `isMoreSpecializedThan`（[temp.class.order]） | `classSpecAtLeastAsSpecialized` + dominance 循环 |
| 替换失败 | `SFINAETrap` + `TDK_SubstitutionFailure` 返回码 | `SfinaeContext`（RAII）+ `SubstitutionFailure` 异常 |
| 静态常量 | `DeclRefExpr` 绑到 `VarDecl`，`EvaluateAsInt` 求值 | `foldStaticConst` + `lookupStaticConst`（按名查） |

★ 最大的简化：**`void_t` 不是别名模板，而是按名字识别的内建**。
真 clang 里 `void_t` 走的是完整的别名解糖路径（`docs/learn/27` 讲过别名模板）；
本项目因为别名模板支持得晚，把 `void_t` 做成了"看到这个名字就归约"的特例。
语义等价，但少了一层解糖 —— 这也是 `std::enable_if_t` 至今做不了的原因
（它需要**类型级条件选择**，按名识别表达不了 `cond ? X : Y`）。

---

## 8. 一句话总结

```text
  Parser 把 `is_range<Container>::value` 拆成
      MemberExpr(isTypeAccess) ← VarExpr(is_range, [Container])
  并且把偏特化的模式 `is_range<T, void_t<decltype(..), decltype(..)>>` 原样存下（decltype 不求值）。

  Sema 在第 70 行做的事是：
      补上默认实参 void  →  拿 <Container, void> 去逐位匹配模式
      →  第 0 位 bind: T := Container
      →  第 1 位把 T 代进去【真的求值】两个 decltype
      →  都成立 ⇒ void_t 归约成 void ⇒ 与实参 void 恒等 ⇒ 偏特化命中
      →  实例化出的类继承 true_type ⇒ 查到 value = 1 ⇒ 折叠成 BoolLiteral(true)

  而 X = Empty / int 时，第 6 步求值报错被 Sfinae::attempt 吸收成"该候选移出"，
  回退主模板（继承 false_type）⇒ value = 0。
  这就是 SFINAE —— 替换失败不是错误。
```
