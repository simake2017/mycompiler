# 24 · 类体内的类型别名（`using` / `typedef`），与一个被它顺带炸出来的栈帧 bug

> 一句话：别名**不是新类型**，只是一次名字替换（[temp.alias]/[dcl.typedef]）；
> 但它逼着编译器回答两个更根本的问题 ——「这个名字该在哪一层作用域里找」
> （[basic.lookup.unqual]/[basic.lookup.qual]）和「实例化时替换发生在哪些位置」。
> 本文还记录了一个与别名无关、却被它踩出来的 codegen 帧大小 bug：
> **帧大小与局部布局各算一份，必然对不上**。

---

## 1. 理论背景

### 1.1 别名不是类型，是"名字 → 类型"的一次替换

[dcl.typedef]/[temp.alias] 规定：`using X = T;` 引入的名字与 `T` 所指的
**是同一个类型**，不是它的子类型、也不是它的包装。这条看似平淡，但它决定了三件事：

1. **不能靠别名做重载/特化判别**。`using A = int; using B = int;`
   写 `f(A)` 与 `f(B)` 是同一个函数签名 —— 换成 `struct A{};` 就完全不同。
2. **别名不产生实例化**（对类模板别名而言，见本文第 4 节的边界）。
   这里省下的编译期开销，正是 `std::enable_if_t` / `std::decay_t` 的价值来源。
3. **解糖（desugaring）是编译器内部动作**，源码里的别名在类型系统里
   应当已经"消失"。本实现的 `resolveType` 就是那个解糖点。

对照 clang：

| 概念 | clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|---|
| 别名声明 | `Sema::ActOnAliasDeclaration`（SemaDecl.cpp） | `parseClassDecl` 成员循环里的 `KwUsing`/`KwTypedef` 分支（parser.cpp） | 不做模板别名、不做 `using` 引入声明（`using Base::f;`） |
| 别名解糖 | `Type::getUnqualifiedDesugaredType` / `TypedefType::desugar()` | `SemanticAnalyzer::resolveType` 的嵌套名分支 | 只做单级成员查表，不做完整的 DesugaredType 链 |
| 成员名查找 | `LookupQualifiedName` + `CXXRecordDecl::lookup`（SemaLookup.cpp） | `m_classDecls[qual]->typeAliases` 直接查 `unordered_map` | 无作用域链、无 ADL、无 using 引入 |

### 1.2 名字在哪一层作用域里找

`Box<int>::type` 里的 `type` **不在**全局作用域，它在 `Box<int>` 这个类的
作用域里。C++ 的名字查找规则（[basic.lookup.qual]）是：

```
Box<int>::type  x;          Cls<Args>::member   → 限定名查找：去 Box<int> 里找
type            x;          （在 Box 的成员函数体内）→ 非限定查找：类作用域优先
```

于是同一个别名在源码里有**三处使用点**，走**三条不同的查找路径**：

| 使用点 | 写法 | 查找路径 | 本实现的落点 |
|---|---|---|---|
| ① 类外，非模板 | `Plain::Int y;` | 限定名 → 全局符号表里的 `Plain::Int` | `processClassDecl` 登记 `Cls::alias` 符号 |
| ② 类外，模板实例 | `Box<int>::type x;` | 限定名 → 实例化后查 `Box_int` 的别名表 | `resolveType` 的 **nested-name 分支** |
| ③ 类内 | `type v;`（在 `Box` 体内） | 非限定 → 类作用域优先 | `resolveType` 的**类作用域回退** |

三处都需要，缺一处就会在某个使用点上"明明定义了却说没定义"。

### 1.3 为什么它属于"模板机制"

因为别名目标里可以含模板形参：

```cpp
template <typename T>
struct Box {
    using type = T;        // 蓝图里的 type 目标是裸 T
    using ptr  = T*;       // 蓝图里是 T*
    type v;
    ptr  p;
};
```

蓝图阶段 `type` 指向的 `T` 是**未绑定**的。实例化 `Box<int>` 时，
别名目标必须跟着替换（`T → int`、`T* → int*`），否则
`Box<int>::type` 会拿到一个裸 `T` —— 而这个 `T` 在实例化的上下文里
根本没有绑定，使用它就会得到"未知类型"或更糟：**静默地按 T 自己的
占位布局（空布局、size 0）继续编译**。

这与字段/方法走的是同一套替换引擎 `substituteType`，
只是"替换位置"多了一处（`typeAliases` 表）。

---

## 2. 三个使用点的数据流（ASCII 图）

```
源码                                  Parser                       Sema / 实例化
────────────────────────────────────────────────────────────────────────────────
① Plain::Int y;             ┌─ 标识符链拼名 "Plain::Int" ─┐
                            │  parseType → Class("Plain::Int") │
                            └──────────────┬───────────────┘
                                           ▼
                            符号表命中 "Plain::Int"（processClassDecl 登记）
                                           ▼
                                        int

② Box<int>::type x;         ┌ parseType: 名 "Box" + 实参 <int> ┐
                            │ → Class("Box", args=[int])       │
                            │ 后缀循环遇 '::' → 建 nested 节点 │
                            │   Class("type", nestedQualifier=Box<int>)
                            └──────────────┬───────────────────┘
                                           ▼
                     resolveType 的 nested 分支：
                       ① 先 resolveType(Box<int>) ⇒ 【实例化】⇒ Box_int
                       ② 查 m_classDecls["Box_int"].typeAliases["type"]
                          （实例化时已替换：T → int）
                                           ▼
                                        int

③ Box 体内 `type v;`        parseType → Class("type")（不知道是别名）
                                           ▼
                     resolveType 全局查 "type" ✗
                     ⇒ 类作用域回退：m_currentClassName = "Box_int"
                       查它的 typeAliases["type"] ⇒ int
```

---

## 3. 实现改动清单

| 文件 | 改动 | 作用 |
|---|---|---|
| `include/ast.h` | `ClassDecl` 加 `typeAliases`（map）+ `typeAliasOrder`（vector） | 别名的存储；order 只为日志/实例化顺序可观测 |
| `include/type.h` | `Type` 加 `nestedQualifier` + `isNestedName()` | 表示 `Box<int>::type` 这种"限定名型"类型节点 |
| `src/type.cpp` | `equals()` 比较嵌套限定者；`toString()` 打印 `qual::member` | 类型相等性/可读串必须认得出限定名 |
| `src/parser.cpp` | ① `parseClassDecl` 成员循环识别 `using X = T;` / `typedef T X;`；② `parseType` 模板 id 后接 `::` 建嵌套节点；③ `parseStatement` 的**声明前瞻**跳过 `::member` 链 | 语法层认得这三种写法；③ 是关键：不跳 `::` 的话 `Box<int>::type x;` 会被当成表达式语句 |
| `src/semantic_analyzer.cpp` | ① `processClassDecl` 开头登记 `m_classDecls[decl->name]`（提前）② 末尾登记 `Cls::alias` 符号并解析别名目标；③ `resolveType` 的 nested-name 分支；④ `resolveType` 的类作用域回退 | 三个使用点的查找路径 |
| `src/template_instantiation.cpp` | 实例化时克隆 `typeAliases`，目标过 `substituteType` | `Box<int>::type` ⇒ `int` 而不是裸 `T` |

**三个坑**（都是"明明写对了却跑不起来"型）：

1. **声明前瞻不跳 `::`** —— `parseStatement` 里那段"标识符 + 可选的 `<...>` +
   `*`/`&` + 标识符 ⇒ 判定为声明"的试探法，遇到 `Box<int>::type x;` 时会停在
   `::` 上，判定"不是声明"⇒ 走表达式分支 ⇒ `Expected ';' after expression`。
   补法是让前瞻把 `::member` 链也跳过去。对照 clang：
   `isDeclarationSpecifier` 里 `TryAnnotateTypeToken` 把整个模板 id 注解成类型 Token。
2. **`m_classDecls` 登记得太晚** —— 字段/方法类型解析发生在函数中段，
   而 `m_classDecls[decl->name] = decl` 原本在函数末尾。于是类体内
   `Int a;` 的类作用域回退查不到本类 ⇒ 报 `unknown type name 'Int'`。
   补法：进函数就登记（同一指针，无副作用）。
3. **实例化不克隆别名表** —— 不克隆的话，`Box<int>::type` 查到的是蓝图里的裸 `T`。
   这一条只会在"用了别名"的模板上暴露，所以必须配一个端到端用例。

---

## 4. 顺带炸出来的 codegen bug：帧大小与局部布局不能各算一份

### 4.1 症状

```cpp
int main() {
    int a = 1; int b = 2; int c = 3; int d = 4;
    int e = 5; int f = 6; int g = 7; int h = 8;
    return a + h;        // 修复前返回 2（clang 9）
}
```

迷惑点：**把 `h` 单独拿出来读是对的**（`return h;` ⇒ 8）。
只有参与"需要把左操作数压栈暂存"的二元表达式时才是错的。
带函数调用时更凶险：`add(g, h)` 修复前返回 14（clang 15）——
`callq` 压入的**返回地址**也落在同一个位置。

### 4.2 根因

```
        ┌──────────────────────────┐  ← %rbp
   -8   │ 帧基（paramOffset 起点）  │
   -16  │ 形参 spill / 第 1 个局部  │
   ...  │        ...               │
   -72  │ h  ← 最后一个局部         │  ← 帧只减到 -64 ⇒ h 落在 rsp 之下
        ├──────────────────────────┤  ← %rsp = rbp-64
   -80  │  pushq %rax 的暂存位置    │  ← 求值 `a + h` 时压在这里
        │  callq 的返回地址         │  ← 调用时压在这里
        └──────────────────────────┘
```

局部偏移从 `-8` 起步（`emitFunction` 里 `paramOffset = -8`），
每个 spill 的形参再各占 8 字节；而 `estimateBlockSize` **只数了局部变量的尺寸**。
于是帧恒定浅 8~56 字节（形参越多浅得越多），最深的几个槽位落在 `%rsp` 之下。

对照 clang / LLVM：帧大小与局部对象偏移出自**同一份**分配器 ——
`X86FrameLowering::determineFrameLayout` 按 `MFI.getStackSize()` 与
`getMaxCallFrameSize()` 统一算出 `subq` 的量，`PEI::assignFrameObjects`
从同一个指针往后推进。结构上不存在"两处各算一份"的可能。

### 4.3 修法

`estimateFrameSize` = `estimateBlockSize(body)` + **序言区**（帧基 1 槽 +
每个 spill 形参 1 槽 + 成员函数的 `this` 1 槽），槽数算法与
`emitFunction` 里的 spill 循环逐条对应。

修复后：

```bash
$ ./minicc tests/tmpl/test_tmpl_48_class_type_aliases.cpp -o /tmp/t48 && /tmp/t48; echo $?
173            # clang++-18 同样 173
```

回归钉子：`tests/unit/test_codegen_frame.cpp`（4 个 GTest）。
它断言的是**不变量**而不是症状 ——

> 对每个函数：`max{ |X| : 出现 -X(%rbp) }` ≤ `subq $N, %rsp` 的 `N`

这样即使将来换了分配策略（槽位复用、按真实尺寸对齐……），
只要不再让局部落到 `%rsp` 之下，测试就仍然有意义。

---

## 5. 可复现实验

```bash
# ① 端到端：三个使用点各来一次（期望 173）
./minicc tests/tmpl/test_tmpl_48_class_type_aliases.cpp -o /tmp/t48 && /tmp/t48; echo $?
clang++-18 tests/tmpl/test_tmpl_48_class_type_aliases.cpp -o /tmp/t48c && /tmp/t48c; echo $?   # 173

# ② 看别名解糖的日志
./minicc tests/tmpl/test_tmpl_48_class_type_aliases.cpp -S -o /dev/null 2>&1 \
  | grep -E "parse:alias|sema:alias|alias '"

# ③ 白盒：帧不变量
cmake --build build-linux --target unit_tests
./build-linux/unit_tests --gtest_filter='CodegenFrame.*'

# ④ 帧 bug 的最小复现（与别名无关，纯局部变量）
cat > /tmp/frame.cpp <<'EOF'
int main() {
    int a = 1; int b = 2; int c = 3; int d = 4;
    int e = 5; int f = 6; int g = 7; int h = 8;
    return a + h;
}
EOF
./minicc /tmp/frame.cpp -o /tmp/frame && /tmp/frame; echo $?     # 9（修复前 2）

# ⑤ 全量回归
ctest --test-dir build-linux                                    # 154/154
```

---

## 6. 边界：还没做的

| 缺口 | 说明 | 影响 |
|---|---|---|
| **别名模板** `template<typename T> using X = ...` | [temp.alias]，属第二梯队 | `std::enable_if_t` / `decay_t` 惯用法仍然用不了 |
| **`using Base::f;`** 引入声明 | 把基类成员拉进本类作用域 | 派生类里直接写基类重载名仍找不到 |
| **依赖类型名** `typename T::type` | 模板里"依赖的限定名"要 `typename` 消歧 | `void_t<typename T::type>` 探测写不了（第二梯队 #22） |
| **嵌套类** `struct Outer { struct Inner {}; };` | 本实现的 `m_classDecls` 是**扁平**命名 | `Outer::Inner` 尚未支持；别名的限定者只能是"已知类" |
| **别名参与重载/特化判别** | 本实现按解糖后的类型比较，方向是对的 | 但只覆盖了类型位置的解糖；表达式位置（NTTP）未验证 |
| **作用域链** | 本实现只有 `m_currentClassName` 一个"当前类" | 类体内嵌 `if` 块里用别名可以，但在**嵌套类**体内用外层的别名不行 |
