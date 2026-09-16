# 23 · cv 限定符的位置，与"类型树的形状即身份"

> 一句话：`const int*` 和 `int* const` 是两棵不同的类型树；树建错了，
> 下游按结构匹配的偏特化就会**静默选错**。本文修的是 [dcl.type.cv] 的
> 归属问题，以及它顺带暴露的"人读串必须与类型结构单射"问题。

---

## 1. 理论背景

### 1.1 cv 限定符写在哪儿，就修饰谁（[dcl.type.cv]）

C++ 的声明语法分两半：

```
const  int  *  const  q  =  &a;
└─┬──┘  └┬┘ └┬┘ └─┬──┘
类型说明符  declarator
```

- **类型说明符侧**的 `const` 修饰**基类型**，后续每遇到一个 `*`/`&` 都被包在外层
- **声明符侧**的 `const` 修饰**已建好的那个类型**（指针本身）

于是：

| 源码 | 类型树 | 读法 |
|---|---|---|
| `const int` | `Const(Int)` | const 的 int |
| `const int*` | `Pointer(Const(Int))` | 指针，**指向** const int |
| `int* const` | `Const(Pointer(Int))` | **const 的**指针 |
| `const int* const` | `Const(Pointer(Const(Int)))` | 两者兼有 |
| `const int&` | `LValueRef(Const(Int))` | 引用，指向 const int |

对照 clang：`DeclSpec` 收集类型说明符侧的 cv，`ParseDeclarator` 收集声明符算子，
最后 `GetTypeForDeclarator` 由内向外套。两个 `const` 走的是**两条不同的路**，
本项目对应 `parseType` 的 Step 2.5（说明符侧）与 Step 3（声明符侧）。

### 1.2 为什么这属于"模板机制"而不是"基础语法"

偏特化匹配（[temp.deduct.type] / [temp.class.spec.match]）是**结构化**的：
拿实参类型树与模式类型树逐层合一。所以类型树的形状**就是**身份。

```
C<T*>      只认顶层是 Pointer 的实参
C<const T> 只认顶层是 Const 的实参（按标准还要校验"实参确实有 const"）
```

树建错 → 匹配错 → **选错特化**，而且全程 `rc = 0`。

---

## 2. 修之前：一个结构性错误牵出三类症状

`parseType` 此前的顺序是「后缀循环 → 再套 const」，于是：

| 源码 | 应有结构 | 此前结构 |
|---|---|---|
| `const int` | `Const(Int)` | `Const(Int)` ✓ |
| `const int*` | `Pointer(Const(Int))` | `Const(Pointer(Int))` ✗ |
| `const int&` | `LValueRef(Const(Int))` | `Const(LValueRef(Int))` ✗ |
| `int* const` | `Const(Pointer(Int))` | **解析直接失败** ✗ |

连锁后果（全部 `rc = 0`）：

1. **选错特化**：`C<const int*>` 顶层是 `Const`，模式 `T*` 要的是 `Pointer`
   ⇒ `C<T*>` 失手，反而 `C<const T>` 命中（T := int*）—— 与标准答案**相反**
2. **特化永不匹配**：`C<const T&>` 因实参顶层不是引用，报
   `reference structure mismatch` ⇒ 静默回退主模板
3. **模式过宽**：`const T` 连 `int*` 都能匹配（只要有"剥顶层 const"就放行），
   匹配集被污染，仅靠偏序兜回来

### 可复现实验（修复前 vs clang）

```cpp
template<typename T> struct C { int tag(){ return 0; } };
template<typename T> struct C<T*>      { int tag(){ return 1; } };
template<typename T> struct C<const T> { int tag(){ return 2; } };
// int a = 1; const int* p = &a;   C<decltype(p)>
```

```bash
# 修复前
$ minicc cmp.cpp -o cmp && ./cmp; echo $?     # → 221
$ clang++-18 -std=c++20 cmp.cpp -o c && ./c; echo $?   # → 121
```

---

## 3. 第二个坑：`toString` 不是单射

修完位置，新的红灯出现了 —— `C<const int*>` 与 `C<int* const>` 仍然串味。

原因在 `Type::toString()`：`Const` 一律输出 `"const " + inner`，于是

```
Pointer(Const(Int))  → "const int*"
Const(Pointer(Int))  → "const int*"     ← 同一个字符串
```

**两种不同结构拿到同一个字符串**。危害不在日志难读，而在下游把它当键用：

| 消费点 | 位置 | 后果 |
|---|---|---|
| 实例缓存键 | `getOrInstantiateClass` 的 `key = name + "<" + args.toString() + ">"` | `C<int* const>` 命中 `C<const int*>` 的缓存，返回**错误的实例** |
| 实例名清洗 | `template_instantiation.cpp` Step 2 | 汇编符号撞车 |

这是 ⑩（实例名清洗不是单射）的**同族问题**，但发生在更早的一层：
`toString` 是"类型结构 → 人读串"的映射，它必须单射，否则任何以它为键的表都会串。

修法（对照 clang `TypePrinter` 按 TypeClass 分派）：

```
Const(Int)          → "const int"
Pointer(Const(Int)) → "const int*"
Const(Pointer(Int)) → "int* const"
```

修完，三个使用点各自独立成键：

```
[spec:select] ★ selecting class template 'C' for <const int*>
[spec:select] ★ selecting class template 'C' for <int* const>
[spec:select] ★ selecting class template 'C' for <const int&>
退出码 = 123   （clang 同源 123）
```

---

## 4. 关键过程图

```
源码        const  int  *  const  q  =  &a ;
                 │      │
       ┌─────────┘      │
       │ Step 2.5       │ Step 3
       │ 说明符侧 const  │ 声明符侧 const
       ▼                ▼
   Const(Int) ──*──▶ Pointer(Const(Int)) ──const──▶ Const(Pointer(Const(Int)))
                                                              │
                                                              ▼
                                            偏特化匹配 / 缓存键 / 汇编符号
```

---

## 5. 改动清单

| 文件 | 改动 |
|---|---|
| `src/parser.cpp` | Step 2.5：说明符侧 const 在**后缀循环之前**应用；Step 3 后缀循环新增 `const` 分支（声明符侧）；删除原 Step 4 |
| `src/type.cpp` | `Type::toString()` 的 `Const` 分支按 inner 的 kind 分派，区分 `const T*` 与 `T* const` |
| `tests/tmpl/test_tmpl_47_cv_position.cpp` | 新增回归钉：四棵不同的树 × 四条偏特化 |
| `include/type.h`、`src/parser.cpp` 顶部样例表 | 更正"const 总包在最外层"的过时描述 |

---

## 6. 复现实验

```bash
./build.sh
./minicc tests/tmpl/test_tmpl_47_cv_position.cpp -o /tmp/t47 && /tmp/t47; echo $?   # → 123
./minicc tests/tmpl/test_tmpl_47_cv_position.cpp -S 2>&1 | grep "★ selecting"      # 三个独立键

# 与 clang 对照
clang++-18 -std=c++20 tests/tmpl/test_tmpl_47_cv_position.cpp -o /tmp/c47 && /tmp/c47; echo $?  # → 123

# 回归：46 个编译用例 rc 全同、34 个可运行程序退出码全同、ctest 150/150
```

---

## 7. 仍未做（与本文相邻的已知边界）

| 编号 | 现象 |
|---|---|
| ⑧ | 一元 `*` 解引用未实现（`return *p;` 不解析） |
| ④ | `[stmt.ambig]` 完整裁决（`a < b > c;` 靠"跳过 `<>` + 回滚"近似） |
| ⑥ | 后置 `const` 成员函数（`int f() const;`） |
| — | `const T` 模式匹配**过宽**（`int*` 也能匹配成功），目前靠偏序兜回；标准要求校验实参确实带 const |
