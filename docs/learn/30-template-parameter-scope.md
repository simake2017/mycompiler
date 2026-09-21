# 30 模板形参作用域：帧、登记时机与依赖默认实参

> 本篇回答一个具体问题：**`template<class T, class U = T>` 为什么编译不过？**
> 答案牵扯三层——作用域的**形状**（扁平表 → 帧链）、形参的**存储**（值 → 指针）、
> 形参的**登记时机**（循环外 → 循环内、默认实参之后）。三层里只有一层是"真 bug"，
> 另外两层是让它能站住的底座。

---

## 1. 问题的三段式

最初的疑问是：「`m_templateParamScope` 是每个 `template` 生效吗？用一个全局的容易有歧义」。查下来，这是个**三合一**的问题，必须分开：

| 层次 | 问的是 | 原实现 | 结论 |
|---|---|---|---|
| **形状** | 形参存在哪、怎么查 | `std::vector<std::string>` 扁平名字表 | 构造上是对的（`scopeBase`/`resize` 配对），但答不出 kind/层级/归属 |
| **存储** | 形参节点住在哪 | `std::vector<TemplateParam>` 值语义 | 解析期 `push_back` 会 reallocate ⇒ 存下的元素**地址**失联（存下标则无恙） |
| **时机** | 形参何时可查 | 整个形参表解析**完**才注册 | ★ **真 bug**：默认实参看不见前一位形参 |

前两层在当时的代码里**没有症状**（栈深最多 1、没人存过元素地址），第三层才是把
`template<class T, class U = T>` 直接打挂的那个。

---

## 2. 理论背景

### 2.1 [basic.scope.pdecl]/9 —— 声明点

> The point of declaration for a template parameter is immediately after its
> complete *declarator*.

翻译成人话：**模板形参名从"它自己的声明写完"那一刻起才可见**。于是：

```cpp
template <class T, class U = T> struct A {};   // ✓ T 是【上一轮】来的，早已可见
template <class U = U>          struct B {};   // ✗ U 此刻还没可见，只能去外层找
```

★ 最容易搞反的一点：`U = T` 能成立，靠的**不是**"把 U 自己提前注册"，
而是 **T 在上一轮循环就已注册完毕**。clang 的注释原文把这个顺序写死了
（见 §3.3）。

### 2.2 [temp.param]/12 —— 默认模板实参

- 实参表可以写不满，缺的由默认值补齐
- 默认值**可以依赖**前面的形参（`class U = T`、`class U = T*`）
- 默认值在**使用点**实例化，不是声明点

第 3 条是「替换 ≠ 实例化」的又一例：蓝图里存依赖形式（`TemplateParam("T")`）是对的，
**使用点必须做那一步替换**，否则半成品会漏进实例。

### 2.3 编译原理对应

- **作用域链（scope chain）/ 环境（environment）**：形参表就是一个环境，
  查名字沿链上行、内层优先。clang 复用统一的 `Scope` 链，minicc 用帧链模拟。
- **声明点与可见性（point of declaration）**：环境不是"整条一起生效"，
  而是**逐个条目按解析顺序生效**——这正好把 §2.1 的规则编码进了数据结构的形状里。

---

## 3. clang 怎么做

### 3.1 存储：指针数组 + arena

```cpp
// clang/include/clang/AST/DeclTemplate.h:72
class TemplateParameterList final
    : private llvm::TrailingObjects<TemplateParameterList, NamedDecl *, Expr *> {
  //                                 ^^^^^^^^^^^^ 内联存的是一【指针】数组
  unsigned NumParams : 29;
  ...
  using iterator = NamedDecl **;                    // :130
  iterator begin() { return getTrailingObjects<NamedDecl *>(); }
};
```

节点本身独立分配：

```cpp
// clang/lib/Sema/SemaTemplate.cpp:1059（ActOnTypeParameter 内）
TemplateTypeParmDecl *Param
  = TemplateTypeParmDecl::Create(Context, Context.getTranslationUnitDecl(), ...);
```

分配器是 **ASTContext 的 `BumpPtrAllocator`**：**永不移动、永不单独释放**，
直到整个 ASTContext 销毁。⇒ clang 缓存 `TemplateTypeParmDecl*` **天然安全**。

> minicc 的坑是自己造的：`vector<TemplateParam>` 值语义 ⇒ 元素住在 vector 的堆块里
> ⇒ 扩容搬家 ⇒ 先前取的指针悬空。
> **指针化让形状向 clang 这套表示靠拢**（数组里存的是指针，节点的命另有归属）；
> 真正终结悬空的是"节点不再住在容器里"这个性质 —— minicc 用 `shared_ptr`
> 拿到它，clang 用 arena 拿到它。详见 §4.2。

### 3.2 作用域：复用统一 Scope 链，不另起容器

```cpp
// clang/include/clang/Sema/Scope.h:81
TemplateParamScope = 0x80,          // 只是 Scope 的一个【种类位】

// clang/lib/Parse/ParseTemplate.cpp:332（ParseTemplateParameters 内）
TemplateScopes.Enter(Scope::TemplateParamScope);   // ★ 在解析形参表【之前】就开
Failed = ParseTemplateParameterList(Depth, TemplateParams);

// clang/lib/Sema/SemaTemplate.cpp:1074（ActOnTypeParameter 末尾）
S->AddDecl(Param);                  // 把形参加进当前 Scope 的声明链
```

进出由 `MultiParseScope`（**RAII**：构造 Enter、析构 Exit）负责。

⇒ **「内层优先」是 Scope 链的天然性质**，clang 不需要手写 parent 指针。
⇒ 注意 `Enter` 在解析形参表**之前**——**作用域开得早是对的**，
   真正决定裸 `T` 能否被认出来的是**形参何时 `AddDecl`**。

### 3.3 ★ 时机：先解析默认实参，后注册自己

```cpp
// clang/lib/Parse/ParseTemplate.cpp:654（ParseTypeParameter 内）
// Grab a default argument (if available).
// Per C++0x [basic.scope.pdecl]p9, we parse the default argument before
// we introduce the type parameter into the local scope.
SourceLocation EqualLoc;
ParsedType DefaultArg;
if (TryConsumeToken(tok::equal, EqualLoc)) {
  ...
  DefaultArg = ParseTypeName(...).get();          // ← 先解析默认实参（:671）
}

NamedDecl *NewDecl = Actions.ActOnTypeParameter(...);   // ← 后建 Decl（:676）
```

实证（`clang++-18 -std=c++20 -fsyntax-only`）：

| 写法 | clang |
|---|---|
| `template<class T, class U = T>` + `A<int> x;` | ✅ rc=0 |
| `template<class T = int, class U = T*>` + `C<> z;` | ✅ rc=0 |
| `template<class U = U>` | ❌ `error: unknown type name 'U'` |

**精确规则：解析第 i 位形参的默认实参时，前 i−1 位在作用域内，第 i 位自己不在。**

### 3.4 使用点的替换

```cpp
// clang/lib/Sema/SemaTemplate.cpp:5476
TemplateArgumentLoc Sema::SubstDefaultTemplateArgumentIfAvailable(
    TemplateDecl *Template, ..., Decl *Param,
    ArrayRef<TemplateArgument> SugaredConverted,
    ArrayRef<TemplateArgument> CanonicalConverted, bool &HasDefaultArg)
```

调用方把**已经填好的实参表**传进来，内部建 `MultiLevelTemplateArgumentList` 再 `SubstType`。
这就是「用前 i 位已绑定实参替换依赖默认值」的对应物。

---

## 4. 本实现的设计决策

### 4.1 形状：帧链（`include/parser.h`）

```cpp
struct TemplateParamFrame {
    const TemplateDecl* owner  = nullptr;  // 归属：这是哪个 template<>
    size_t              count  = 0;        // 已注册形参个数（随解析推进增长）
    TemplateParamFrame* parent = nullptr;  // 外层帧（clang: Scope::getParent()）
    Parser*             parser = nullptr;

    TemplateParamFrame(Parser* p, const TemplateDecl* d)
        : owner(d), parent(p->m_currentFrame), parser(p) {
        p->m_currentFrame = this;                        // ← 入栈
    }
    ~TemplateParamFrame() {
        if (parser) parser->m_currentFrame = parent;     // ← 出栈
    }
};
```

**为什么帧是栈上局部对象**：生命周期 == 该 template 声明的解析范围。
构造即入栈、析构即出栈，**异常路径由栈展开自动保证**。取代了此前的
`scopeBase`/`resize` 手工配对——那种写法在 `error()/errorAt()` 抛异常
（`[[noreturn]]`）时会漏掉恢复动作。对照 clang 的 `MultiParseScope`。

**顺带白捡一条标准语义**：`count` 只覆盖**已注册**的形参，而注册点排在默认实参
解析之后 ⇒ 「第 i 位的默认实参只能看见第 0..i−1 位」这条规则**被 `count` 的
增长顺序自动编码进去了**，不需要额外的检查代码。

查询（`src/parser.cpp`）：

```cpp
const TemplateParam* Parser::lookupTemplateParam(const std::string& name) const {
    for (const TemplateParamFrame* f = m_currentFrame; f; f = f->parent) {  // 内层 → 外层
        for (size_t i = 0; i < f->count; i++) {
            const TemplateParam& p = *f->owner->templateParams[i];
            if (p.name == name) return &p;   // ★ 现取，容器扩容后依然有效
        }
    }
    return nullptr;
}
```

### 4.2 存储：指针化（`include/ast.h`）

```cpp
using TemplateParamPtr = std::shared_ptr<TemplateParam>;
// DeductionGuideDecl::templateParams / TemplateDecl::templateParams
//   都由 vector<TemplateParam> 改为 vector<TemplateParamPtr>
```

#### 4.2.1 为什么值语义会出事：vector 扩容会【搬家所有旧元素】

`push_back` 的"往后放"只在 `size < capacity` 时成立。一旦撞上 `capacity`，
vector 必须申请更大的块、把**全部旧元素搬过去**、`free` 旧块 ——
这是"元素连续存放"的硬约束，后面没位置时没法就地扩容。

实测（探针 `docs/learn/probes/vector_growth_probe.cpp`，跑法见 §7，元素为 `std::string`）：

| 时刻 | size | capacity | `&v[0]` | 说明 |
|---|---|---|---|---|
| 起始 | 0 | 0 | — | |
| push 第 1 个后 | 1 | 1 | `0x…ec0` | 首次分配，无旧元素 |
| push 第 2 个后 | 2 | 2 | `0x…ef0` | ★ 撞 cap，搬家 |
| push 第 3 个后 | 3 | 4 | `0x…f40` | ★ 撞 cap，又搬 |
| push 第 4 个后 | 4 | 4 | `0x…f40` | cap 有余量，原地 |
| push 第 5 个后 | 5 | 8 | `0x…fd0` | ★ 撞 cap，又搬 |

（地址因 ASLR 每次运行不同，要看的是**模式**：容量翻倍那几次地址才变。）

capacity 按 `1→2→4→8` 翻倍，**搬家发生在 push 第 2、3、5、9… 个时**。

这解释了现象里一个刺眼的细节：**为什么两个形参的用例全绿、加到三个才炸**。
第 2 个 `push_back` 是第一个搬家点，而 `template<class T, class U = T>`
里对 `T` 的查询恰好发生在它**之前**（解析 U 的默认实参时 U 自己还没注册）；
只有第 3 位形参的默认实参去查第 1 位时，才会读到那个已被搬走的旧地址。

#### 4.2.2 下标 vs 地址：这才是分水岭

| 帧里存什么 | 扩容后 | 用在 |
|---|---|---|
| `const TemplateParam*`（= `&v[i]`，值语义时代的取法） | ❌ 旧块被 free，失联 | 方案 ①（已废弃） |
| `const TemplateParam*`（= `v[i].get()`，指针化之后） | ✅ 对象在堆上没动 | 指针化之后可以这么存 |
| `size_t count` + 每次现取 `v[i]` | ✅ 下标与扩容无关 | 方案 ②（本实现） |

**关键区分是"存下标"还是"存地址"，不是"存什么类型的指针"。**
存了下标，扩容对查询完全透明；存了地址，扩容即失联。

本实现选**最保守的一条**：帧里只留 `owner + count`（都是下标语义），
查询时现取 `*owner->templateParams[i]`。

> ★ 但要诚实：**自从把元素改成 `shared_ptr`，"防悬空"这一条已经不再需要靠
> "不缓存指针"来实现了** —— 节点住在 vector 之外的稳定地址上，扩容搬的只是
> 指针值本身。保留"只存下标"写法的收益收窄为【少一条必须记住的不变量】，
> 而非【防悬空】。
> 换句话说：**指针化在"解决悬空"上并非必需**，它的真实收益是另两条 ——
> ① 形状对齐 clang（`TemplateParameterList` 就是 `NamedDecl*` 数组 + 长度）；
> ② 让下面那条浅拷贝成立。

> 另一处必须有意为之的语义变化：`parser.cpp` 的
> `decl->guide->templateParams = decl->templateParams;`（推导指引继承外层形参表）
> 由**深拷贝**变成了**浅拷贝**（共享同一批形参节点）。这与 clang 一致——
> 推导指引本来就复用外层模板的 `TemplateParameterList`，不复制。

### 4.3 时机：注册点挪进循环、排在默认实参之后（`src/parser.cpp`）

```cpp
TemplateParamFrame frame(this, decl.get());     // 入栈（对应 clang 的 Enter）

do {
    TemplateParam param;
    ...解析形参名...

    // ★ 先解析默认实参 —— 此刻前 i-1 位已注册，自己还没
    if (match(TokenType::Assign)) {
        param.defaultArg = TemplateArg::ofType(parseType());
        param.hasDefault = true;
    }

    // ★ 后注册自己（对应 clang 的 S->AddDecl）
    decl->typeParams.push_back(param.name);
    decl->templateParams.push_back(std::make_shared<TemplateParam>(std::move(param)));
    frame.count = decl->templateParams.size();   // ← 可见范围同步扩张
} while (match(TokenType::Comma) && ...);
```

**此前是真 bug**：注册写在循环**之后**，连上一轮的 `T` 都没进门 ⇒
`template<class T, class U = T>` 里的 `T` 被建成 `Class("T")`，
报 `unknown type name 'T'`，且报错点看不出根因。

### 4.4 使用点解糖（`src/semantic_analyzer.cpp`）

补全实参表时，用**前 i 位已绑定实参**建 `TypeSubstitution`，对依赖的默认值
调 `substituteType`：

```cpp
TemplateArg filled = p.defaultArg;
if (i > 0 && filled.isType() && filled.type) {
    TemplateInstantiator::TypeSubstitution prior;
    for (size_t j = 0; j < i; j++)
        prior[primary->templateParams[j]->name] = fullArgs[j];
    TypePtr resolved = m_instantiator.substituteType(filled.type, prior);
    if (resolved != filled.type) { ...日志...；filled = TemplateArg::ofType(resolved); }
}
```

不做这一步的后果是**静默算错**（比报错危险）：

```
实例名成 Box_int_T（应 Box_int_int）
字段 b 的类型停在裸的 T（大小 0 字节）
```

---

## 5. 关键过程

### 5.1 `template<class T, class U = T>` 的时间线

```text
parseTemplateDecl
  │
  ├─ decl = make_shared<TemplateDecl>()
  ├─ TemplateParamFrame frame(this, decl.get())        ← 构造：入栈
  │       m_currentFrame: nullptr ──► &frame
  │
  ├─ 第 1 轮：形参 T
  │    ├─ 解析名 "T"
  │    ├─ match(Assign) 失败（无默认值）
  │    └─ ★ 注册：templateParams.push_back(T)   frame.count = 1
  │
  ├─ 第 2 轮：形参 U
  │    ├─ 解析名 "U"
  │    ├─ 解析默认实参 `= T`
  │    │     lookupTemplateParam("T")
  │    │       扫 frame（count == 1）⇒ ★ 命中！返回 templateParams[0]
  │    │     parseType 建 TemplateParam("T")   ✔ 而不是 Class("T")
  │    └─ ★ 注册：templateParams.push_back(U)   frame.count = 2
  │
  └─ 函数返回 ⇒ frame 析构 ⇒ m_currentFrame = parent(nullptr)
```

### 5.2 对照：`template<class U = U>`（负向）

```text
  ├─ 第 1 轮：形参 U
  │    ├─ 解析名 "U"
  │    ├─ 解析默认实参 `= U`
  │    │     lookupTemplateParam("U")
  │    │       count == 0 ⇒ 扫不到任何形参 ⇒ 返回 nullptr
  │    │     parseType 建 Class("U")
  │    └─ 注册：count = 1
  └─ ...语义阶段 resolveType 查不到类型 U
       ⇒ [Semantic Error] unknown type name 'U'    （与 clang 同一句）
```

### 5.3 帧链的嵌套（未来成员模板）

```text
外层帧 frame_outer(owner=Outer, count=1)
  │
  └─ 解析成员模板时再建 frame_inner(owner=Inner, count=1)
        frame_inner.parent ──► &frame_outer
        lookup 从 frame_inner 起扫，找不到才上行 ⇒ 内层优先
```

当前 minicc 不支持成员模板（`KwTemplate` 只在 `parseDeclaration` 处理，
类体成员循环里没有分支），所以栈深最多 1。帧链是为那一步预留的。

---

## 6. clang 源码对照表

| clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|
| `DeclTemplate.h:72` `TemplateParameterList`（`TrailingObjects` 内联 `NamedDecl*` 数组） | `ast.h` 的 `std::vector<TemplateParamPtr>` | 用 `shared_ptr` 代替 arena；没有 `FoldingSet` 去重 |
| `SemaTemplate.cpp:1059` `TemplateTypeParmDecl::Create` | `std::make_shared<TemplateParam>(...)` | 类型形参/NTTP 不分成两类节点，用 `kind` 字段扁平化 |
| `Scope.h:81` `TemplateParamScope` + `MultiParseScope` | `parser.h` 的 `TemplateParamFrame`（RAII 入栈/出栈） | 没有通用 `Scope` 类，用帧链模拟 Scope 链 |
| `SemaTemplate.cpp:1074` `S->AddDecl(Param)` | `templateParams.push_back(...)` + `frame.count = size()` | 名字查找不用 Scope 链，用帧内线性扫描 |
| `ParseTemplate.cpp:654`「默认实参先于作用域引入」 | 注册点排在 `match(Assign)` **之后** | 一致 |
| `ParseTemplate.cpp:353` `ParseTemplateParameterList`（`ParseTemplateParameter` 返回后 `push_back`） | 循环内 `push_back` | 一致 |
| `SemaTemplate.cpp:5476` `SubstDefaultTemplateArgumentIfAvailable` | `semantic_analyzer.cpp`「用默认实参把实参表补全」 | 只做类型替换，不做常量表达式求值 |
| `maybeDiagnoseTemplateParameterShadow`（形参遮蔽警告） | **未实现** | 内层同名遮蔽静默接受 |

---

## 7. 可复现实验

```bash
# ⓪ 独立探针：亲自看 vector 扩容搬家（§4.2 那张表就是它的输出）
clang++-18 -std=c++20 docs/learn/probes/vector_growth_probe.cpp -o /tmp/vgrow && /tmp/vgrow
#   起始         size=0 cap=0
#   push 第 1 个后 size=1 cap=1  &v[0]=0x…ec0  （原地追加，地址没动）
#   push 第 2 个后 size=2 cap=2  &v[0]=0x…ef0  ★ 搬家了 —— 此前元素的地址全变
#   push 第 3 个后 size=3 cap=4  &v[0]=0x…f40  ★ 搬家了 —— 此前元素的地址全变
#   push 第 4 个后 size=4 cap=4  &v[0]=0x…f40  （原地追加，地址没动）
#   push 第 5 个后 size=5 cap=8  &v[0]=0x…fd0  ★ 搬家了 —— 此前元素的地址全变

# ① 正向：依赖默认实参（裸名 + 复合类型）
clang++-18 -std=c++20 -fsyntax-only tests/tmpl/test_tmpl_53_dependent_default_template_arg.cpp  # rc=0
./build-linux/minicc tests/tmpl/test_tmpl_53_dependent_default_template_arg.cpp -o /tmp/t53
/tmp/t53; echo $?                       # → 0

# 关键日志（可见解糖那一步）：
./build-linux/minicc tests/tmpl/test_tmpl_53_dependent_default_template_arg.cpp -S -o /tmp/t53.s 2>&1 \
  | grep -E "default argument|parse:type|register tparam"
#   [parse:template]   ↗ register tparam 'T' into scope    ← ★ 注册在默认实参之后
#   [parse:type] base = T (template param, scope hit)      ← ★ T 被认成形参（不是 Class）
#   [parse:template]   ↗ register tparam 'U' into scope
#   [sema:targ] ⤷ default argument 'U' is dependent: T → int
#   [sema:targ] ⤷ default argument filled: 'U' := int
#   [instantiate:name] 'Box<int,int>' → 汇编符号前缀 'Box_int_int'

# ② 负向：自引用默认实参必须报错（[basic.scope.pdecl]/9）
./build-linux/minicc tests/tmpl/test_tmpl_54_error_tparam_scope_after_declarator.cpp -o /tmp/x
echo $?                                 # → 非 0
# clang 对照：error: unknown type name 'U'（同一句）

# ③ 回归
ctest --test-dir build-linux -j6        # 214/214
./logdiff.sh diff                        # 逐字节零差异
```

---

## 8. 边界与代价

| 项 | 状态 |
|---|---|
| `template<class U = U>` | ✅ 报错，与 clang 同文案 |
| `template<int N> struct A { N x; };` | ⚠️ 报 `unknown type name 'N'`，**不是** clang 的 `err_not_type` |
| 成员模板 / 嵌套类模板 | ❌ 不支持（帧链已就位，但 Parser 无入口） |
| 形参遮蔽警告 | ❌ 未实现 |
| `Self<>` 空实参表 | ❌ 不支持（需写 `Self<int>`） |

**关于第二行**：`parseType` 是**复用**的——「类型位置」和「模板实参位置」
（`parseTemplateArgumentList` 会拿裸名试探 `parseType`）都会进它。
在后者，裸 `N` 是**合法的值实参**：

```cpp
template <int N> struct Buf { ... };
template <int N> using BufA = Buf<N>;    // 这里的 N 是值，不是类型
```

所以不能在 `parseType` 里对"值形参名"一律报错——这条曾经写错过一次，
直接把 `test_tmpl_50` 的 `Buf<N>` 打挂（rc 0→1）。
clang 把"实参位 vs 类型位"的区分放在 `ParseTemplateArgument`，
minicc 没有那层分派，故在此保守处理：**值名一律走旧路径**，
由替换阶段按"替换表里绑的是值"还原。

---

## 9. 顺带修掉的两个 bug

### 9.1 codegen：8 字节字段赋值的宽度（`src/codegen.cpp:1242`）

字段**写入**路径硬编码 `movl`（4 字节），而**读取**路径按宽度分派（`:1960` 一带）。
读写不对称的后果是"写得进、读出来错"：

```asm
    movq -32(%rbp), %rax    # 加载 pv（8 字节指针）
    movl %eax, 0(%rcx)      # ★ 只写 4 字节，高 32 位被截断
```

复现：`PtrBox<int> q; q.p = pv;` 之后 `q.p != pv`
（`tests/tmpl/test_tmpl_53` 的 ②，一度被误判成默认实参的锅）。
此前没暴露是因为**从没有用例给 8 字节字段赋过值**。

修法：按 `field->size` 分派（`<= 4` 用 `movl`、否则 `movq`），
并**保持 `size <= 4` 分支的译文案不动**，避免无谓的汇编/日志漂移。

### 9.2 parser：NTTP 名注册进作用域的副作用

把 NTTP 名也注册进作用域后，`Buf<N>` 的实参位 `N` 会被
`lookupTemplateParam` 命中（`kind == NonType`）。见 §8 的说明——
处理方式是**只在 `kind == Type` 时报/建模板形参节点**。

---

## 10. 一句话总结

> **作用域开的时机**（对应 clang 的 `Scope::TemplateParamScope` + `Enter`）本来就是对的；
> 错的是**形参注册进作用域的时机**——它必须发生在**这位形参自己的默认实参解析完之后**
> （[basic.scope.pdecl]/9），从而让第 i 位的默认实参恰好看得见第 0..i−1 位。
> 帧链 + `count` 的增长顺序把这条规则**编码进了数据结构的形状**里。
