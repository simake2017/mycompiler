# 28 · 类模板实参推导（CTAD）与推导指引

> 一句话：`MyPtr m(7);` 没写 `<...>`，编译器**拿构造实参反推类模板形参**。
> 它和函数模板实参推导是**同一套合一算法的反向使用** ——
> 把构造函数形参表当成那个 `f`，被推的是类模板自己的形参。
>
> 验证方法：直接 `git grep deducePair`，会看到 CTAD 与 `deduce()`、
> `matchPattern()` 走的是**同一个函数**。新特性是旧算法的重新布线，
> 不是又一份复制品 —— 这正是本项目的教学主张。

---

## 1. 理论背景

### 1.1 CTAD 的准确触发条件（[dcl.type.class.deduct]/1）

```cpp
MyPtr  m(7);          // ✅ CTAD 触发：裸模板名 + 直接初始化
MyPtr  m = 7;         // ❌ 不触发（这是拷贝初始化；C++17 也不做，那是转换）
MyPtr<int> m(7);      // ❌ 不触发（实参已写全，走老路径）
auto   m = MyPtr(7);  // ✅ C++17 允许，本项目不支持 auto 从纯右值推（见 §5）
```

本项目支持的正是第一行。它也顺带要求一件事：**Parser 得先认得
`Type name(args);` 这条语法** —— 在此之前 minicc 只认 `Type name;`
与 `Type name = expr;`，`MyPtr m(7);` 会直接报
`Expected ';' after variable declaration`。所以 CTAD 的第一步其实是补文法。

### 1.2 为什么说它是"函数模板推导的反向使用"

```
  函数模板                          类模板 + CTAD
  ─────────                         ─────────────
  template<class T>                 template<class T>
  void f(T x);                      struct MyPtr { MyPtr(T q); };

  f(7);                             MyPtr m(7);
   ↑ 已知调用 → 推 T                  ↑ 已知构造实参 → 推 T

  模式 P  = 形参类型 T               模式 P  = 构造函数形参类型 T
  被推项 A = 实参类型 int            被推项 A = 构造实参类型 int
  ⇒ T := int                        ⇒ T := int，于是 MyPtr m 的类型是 MyPtr<int>
```

两边的"模式"都来自一张**形参表**，只是那张表一个属于函数、一个属于构造函数。
合一算法的每一行代码都可以照用 —— 本实现因此直接调
`TemplateDeducer::deducePair`，只是把 `paramNames` 换成**类模板的形参名**。

### 1.3 推导指引：给默认规则打补丁（[temp.deduct.guide]）

"拿构造函数当指引"覆盖不了所有情况。最典型的漏洞是**构造函数推不出的形参**：

```cpp
template <class T, class U>
struct Two { T a; U b;  Two(T x) : a(x) {} };

Two t(5);      // ✗ 隐式指引只能从 `Two(T x)` 推出 T；U 没有任何实参能推它
```

标准给的出口就是**推导指引** —— 一条长得像函数、但**不产生任何代码**的映射规则：

```cpp
Two(int) -> Two<int, int>;               // 非模板指引：写死映射
template<class T> Box(T) -> Box<T>;      // 模板指引：可以含自己的 template<>
```

|  | 隐式指引（构造函数） | 显式指引 |
|---|---|---|
| 来源 | 编译器从构造函数"合成" | 用户写的 `-> ` 那一行 |
| 优先级 | 低 | **高**（用户写了就按用户的来） |
| 有实体吗 | 是真实的构造函数 | **没有** —— 无函数体、无符号、不参与重载决议 |
| 用在哪 | 只在 CTAD 那一刻 | 只在 CTAD 那一刻 |

★ "指引优先于构造函数"是刻意的设计：指引存在的**全部意义**就是改写默认
映射规则；若构造函数还排在前面，用户就没法表达 `MyPtr(T*) -> MyPtr<T>`
这种"我希望剥掉一层指针"的意图了。

### 1.4 顺带暴露的一个真问题：替换 ≠ 实例化

做 CTAD 时踩出一个此前一直存在、但没被触发的洞：

```cpp
template <class T> T readOf(MyPtr<T> p) { return p.value; }
readOf(m);          // 实例化 readOf<int>
```

实例化时 `substituteType(MyPtr<T>, {T:=int})` 产出的是
**`MyPtr<int>` 这个半成品节点**（名字还是模板名 + 实参表），
不是实例类型 `MyPtr_int`。于是函数体里 `p.value` 去类表里查 `"MyPtr"`，
查不到，报 `No member 'value' in class 'MyPtr'` —— 类型其实是对的，
只是"还没被兑现"。

```
   substituteType 的职责边界
   ──────────────────────────
   只做【替换】：T → int
   不做【实例化】：MyPtr<int> → MyPtr_int   ← 那是 resolveType 的活

   所以实例化函数之后必须补一步"签名落地"：
       instance->returnType = resolveType(instance->returnType);
       for (param : instance->parameters) param.type = resolveType(param.type);
```

★ 为什么别名模板的形参位**没**暴露这个问题：别名解糖内部本来就调了一次
`resolveType`（见 docs/learn/27）。直写类模板 id 的参数位才把它顶出来 ——
又一个"两条路径只差一步，结果一边对一边错"的例子。

---

## 2. 数据流（ASCII 图）

```
源码   template<class T> struct MyPtr { T value; MyPtr(T q) : value(q) {} };
       MyPtr m(7);

── 解析 ──────────────────────────────────────────────────────────────
  parseStatement
    └─ 试探：标识符 MyPtr 后跟标识符 m ⇒ 是变量声明
       └─ parseVarDeclStmt
            ├─ 名字 m
            └─ 见到 '(' ⇒ 直接初始化 ⇒ ctorArgs = [IntLiteral(7)]
               （此前这里直接报 "Expected ';'"）

── 注册 ──────────────────────────────────────────────────────────────
  processTemplateDecl → m_classTemplates["MyPtr"] = 蓝图
  （若有 `template<class T> Box(T) -> Box<T>;`
    → m_deductionGuides["Box"] 追加一条）

── 语义（Pass 3，遇到 m 的声明）──────────────────────────────────────
  processVarDecl
    ├─ ctorArgs 非空 ⇒ deduceClassTemplateArgs(decl)
    │    ├─ 实参类型：[int]
    │    ├─ 来源①：m_deductionGuides["MyPtr"] 有条目吗？ → 没有
    │    ├─ 来源②：遍历主模板的构造函数
    │    │     P = T（构造形参）  A = int（构造实参）
    │    │     deducePair(T, int, paramNames=[T]) ⇒ T := int ✓
    │    │     收尾：T 已绑定 ✓
    │    └─ 产出 Class("MyPtr", args=[int])
    ├─ resolveType(MyPtr<int>) ⇒ 实例化 ⇒ MyPtr_int
    └─ 选定构造函数符号：ctor->mangledName = "MyPtr_int_MyPtr_int_1"
         ★ 为什么要回填：mangling 是**有状态**的（同名多参会追加
           `_N` 后缀），"该调哪个重载"只有 Sema 知道；CodeGen 按
           `Name_Name` 硬拼会拼出一个不存在的符号 ⇒ 链接期 undefined

── CodeGen ──────────────────────────────────────────────────────────
  emitVarDecl 路径②（栈对象）
    ├─ 分配 size 字节、零初始化、装 _vptr
    ├─ ctorArgs 非空 ⇒ 逐个求值 pushq 暂存 → 逆序 pop 到 rsi/rdx/rcx/r8/r9
    │                  （rdi 被 this 占用）
    └─ callq MyPtr_int_MyPtr_int_1
```

---

## 3. 实现改动清单

| 位置 | 改动 |
|---|---|
| `include/ast.h` | `VarDeclStmt::ctorArgs` + `ctorSymbol`；新节点 `DeductionGuideDecl`；`NodeKind::DeductionGuide`；`TemplateDecl::guide` + `isDeductionGuide()` |
| `src/parser.cpp` | `parseVarDeclStmt` 接受 `(args...)`；`looksLikeDeductionGuide()` 前瞻 + `parseDeductionGuide()`；两处分派（顶层非模板形态 / `template<>` 外壳形态） |
| `include/template_deduction.h` | `deducePair` 提到 public（CTAD 要直接调）；`Subst` 提到类外 |
| `include/semantic_analyzer.h` / `.cpp` | `m_deductionGuides`；`registerDeductionGuide` / `deduceClassTemplateArgs` / `substituteInType`；`processVarDecl` 开头挂 CTAD；`getOrInstantiateFunction` 补"签名落地" |
| `src/codegen.cpp` | `emitVarDecl` 路径② 带参构造：实参入寄存器 + 按 `ctorSymbol` 调用 |

### 3.1 前瞻：怎么把指引和函数声明分开

```cpp
MyPtr(T) -> MyPtr<T>;    // 推导指引
MyPtr f(T);              // 函数声明（返回 MyPtr）
```

前两个 Token 完全相同（`Identifier` + `(`）。分岔点在**配对右括号之后**：
跟 `->` 才是指引。所以 `looksLikeDeductionGuide()` 必须先做括号配对扫描：

```
  i = m_pos+1                  // 跳过类名
  depth = 0
  扫到 ')' 且 depth 归零 → 看下一个 Token 是不是 '->'
  中途遇到 ';' 或 '{' → 这份声明已经结束，肯定不是指引
```

对照 clang：`Parser::TryParseDeductionGuide` 也是"解析完形参表回头看 `->`"，
只是 clang 真的先把声明解析出来再决定归属，本实现用纯扫描更省事。

### 3.2 三个"必须跳过指引"的地方

指引**不是**类模板也不是函数模板，但借用了 `TemplateDecl` 的外壳。
于是所有"按 kind 分派"的地方都要为它开一条路 —— 漏一处就是**空指针崩溃**：

| 位置 | 漏了会怎样 |
|---|---|
| `parseTemplateDecl` 的蓝图摘要 | `funcTemplate->returnType` 解引用空指针 |
| `main.cpp` Phase 4 演示循环 | `classTemplate->name` 空指针（实测崩溃） |
| `processTemplateDecl` | 落到"函数模板"分支，把指引塞进候选集 |

★ 这已经是**同一类问题第三次出现**了（`Box<T>` 假实例、指向模板参数的空实例、
指引空指针）：**判别依据是"数据看起来像什么"，而不是"它是什么"**。
`TemplateDecl` 用一个外壳装三种东西，每加一种就要把所有分派点过一遍。
clang 这里用三个独立的 Decl 类（`ClassTemplateDecl` / `FunctionTemplateDecl` /
`TypeAliasTemplateDecl`）从类型系统上就杜绝了这类遗漏 —— 值得记住的教训。

---

## 4. 可复现实验

```bash
# ① 端到端（期望 173；clang++-18 -std=c++20 同样 173）
./minicc tests/tmpl/test_tmpl_51_ctad_and_guides.cpp -o /tmp/t51 && /tmp/t51; echo $?

# ② 看 CTAD 三条来源各自的日志
./minicc tests/tmpl/test_tmpl_51_ctad_and_guides.cpp -S 2>&1 \
  | grep -E "parse:guide|deduction guide for|\[ctad\]|\[ctor\]"

# ③ 最小复现：构造函数隐式指引
cat > /tmp/ct1.cpp <<'EOF'
template<class T> struct MyPtr { T value; MyPtr(T q) : value(q) {} };
int main() { MyPtr m(7); return m.value; }
EOF
./minicc /tmp/ct1.cpp -o /tmp/ct1 && /tmp/ct1; echo "期望 7"

# ④ 判别性实验：显式指引补上构造函数推不出的形参
#    去掉 `Two(int) -> Two<int,int>;` 这行，必须编译失败
cat > /tmp/ct2.cpp <<'EOF'
template<class T, class U> struct Two { T a; U b; Two(T x) : a(x) {} };
Two(int) -> Two<int, int>;
int main() { Two t(5); t.b = 9; return t.a + t.b; }
EOF
./minicc /tmp/ct2.cpp -o /tmp/ct2 && /tmp/ct2; echo "期望 14"
printf 'template<class T, class U> struct Two { T a; U b; Two(T x) : a(x) {} };\nint main(){ Two t(5); return 0; }\n' > /tmp/ct3.cpp
./minicc /tmp/ct3.cpp 2>&1 | grep -E "ctad|ERROR"   # 推不出 U ⇒ 必须失败

# ⑤ 全量回归
ctest --test-dir build-linux            # 期望 154/154
python3 /tmp/a21/cmpall.py
python3 /tmp/a21/cmpplang.py            # 期望 0 处不一致
```

对照 clang：

```bash
clang++-18 -std=c++20 -o /tmp/t51c tests/tmpl/test_tmpl_51_ctad_and_guides.cpp
/tmp/t51c; echo $?     # 173
clang++-18 -std=c++20 -fsyntax-only /tmp/ct3.cpp
# error: no viable constructor or deduction guide for deduction of template
#        arguments of 'Two'
```

---

## 5. 边界（本项目**未做**的）

| 未做 | 为什么 | clang 的行为 |
|---|---|---|
| `auto m = MyPtr(7);` | 要把"纯右值 → auto 类型"这条推导接进来；本项目 auto 只从表达式类型取 | 支持 |
| 拷贝推导 `MyPtr m = other;`（C++17） | 需要先有拷贝构造语义（本项目类对象无拷贝） | 支持 |
| 聚合推导（无构造函数的聚合类） | 需要聚合初始化语法 | 支持 |
| 指引参与**重载决议** | 本实现按"找到第一条能推通的就用"，不做候选集排序 | 全部候选一起决议，歧义时报 ambiguous |
| CTAD 用于 `new` / 函数返回类型 | `new MyPtr(7)` 与 `return MyPtr(7);` | 支持 |
| 构造函数**重载**的完整决议 | CTAD 里只按参数个数挑构造函数 | 完整重载决议 |

已验证与 clang 结论一致的四条：`MyPtr m(7)` 推出 `<int>`、
模板指引 `Box(T) -> Box<T>`、
非模板指引补出不可推导形参、
无指引且形参推不出时**必须失败**（`no viable constructor or deduction guide`）。

---

## 6. clang 源码对照表

| clang 位置（文件:函数） | 本实现位置 | 简化了什么 |
|---|---|---|
| `Sema::ActOnVariableDeclarator` → `DeduceTemplateSpecializationFromInitializer` | `SemanticAnalyzer::deduceClassTemplateArgs` | 不建候选集，找到第一条能推通的就用 |
| `CXXDeductionGuideDecl` / `ClassTemplateDecl::getDeductionGuides()` | `DeductionGuideDecl` + `m_deductionGuides` | 指引与模板共用一个 `TemplateDecl` 外壳（代价见 §3.2） |
| 隐式指引：`DeclareImplicitDeductionGuides`（从构造函数合成） | 直接遍历 `classTemplate->methods` 里的 ctor | 不做"按值/按引用构造参数"的形态改写 |
| `Parser::TryParseDeductionGuide` | `looksLikeDeductionGuide()` + `parseDeductionGuide()` | 纯扫描前瞻，不是"先解析再决定归属" |
| `Sema::DeduceTemplateArguments`（CTAD 入口） | 直接调 `TemplateDeducer::deducePair` | 复用函数模板那套合一，不新写 |
| `Sema::BuildCXXConstructExpr`（选构造函数） | `processVarDecl` 末尾按参数个数挑 + 回填 `ctorSymbol` | 不做重载决议，只按个数 |
| `CodeGenFunction::EmitCXXConstructExpr` | `emitVarDecl` 路径② 的带参分支 | 实参上限 5 个（寄存器传参），超出不处理 |

---

## 7. 关键日志片段（`test_tmpl_51` 实跑）

```
  [parse:guide] ★ deduction guide: Box(T) -> Box<T>
  [register] deduction guide for 'Box' (#1) registered
  [register] deduction guide for 'Two' (#1) registered
  [ctad] ▶ m MyPtr(int) —— 未写模板实参，尝试类模板实参推导
  [ctad] ✔ 由构造函数推出 ⇒ MyPtr<int>
  [symbol] ✚ m : MyPtr_int    stack@-8
  [ctad] ▶ b Box(int) —— 未写模板实参，尝试类模板实参推导
  [ctad] ✔ 命中推导指引 ⇒ Box<int>
  [ctad] ▶ t Two(int) —— 未写模板实参，尝试类模板实参推导
  [ctad] ✔ 命中推导指引 ⇒ Two<int, int>
  ...
  [ctor] ★ t : Two_int_int(1 个实参) ⇒ 选定构造函数符号 Two_int_int_Two_int_int_1
  [deduction]   P=MyPtr<T>     A=MyPtr_int    ⇒ 同类模板 MyPtr（出身 MyPtr），逐位合一
  [deduction]   P=T            A=int          ⇒ T := int
```

倒数第二、三行值得单独看一眼：**CTAD 推出来的类型，回头又参与了一次
普通的函数模板推导**（`readOf(m)`）—— 两条推导路径共用同一个 `deducePair`，
`MyPtr<int>` 与 `MyPtr<T>` 能对上，靠的是 docs/learn/27 里补的
"实例出身"记录。三个子阶段（别名模板 / 类模板 id 结构合一 / CTAD）
在这里闭环了。
