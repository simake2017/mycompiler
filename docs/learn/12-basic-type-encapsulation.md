# 12 基础类型封装：从栈对象到内建函数

> 一个能写 `Vector<int> v; v[i] = x;` 的编译器才算有了"类型封装"的雏形。
> 本文串起 minicc 的四个里程碑（P1~P4）：类类型栈对象与块作用域析构（RAII）、
> 下标糖 `v[i]`、类模板按需实例化闭环、libc 内建函数发射。
> 每一步都给出标准条款依据、clang 对照与可复现的观测命令。

---

## ① 理论背景

### 1.1 四个能力与标准条款的对应

| 能力 | 标准条款 | 一句话语义 |
|---|---|---|
| P1 栈对象 + 块尾析构 | [class.ctor] / [stmt.dcl] / [class.dtor] | 类类型局部变量进入作用域构造、离开即析构（RAII） |
| P2 下标运算 | [expr.sub] / [over.sub] | `v[i]` 即 `operator[](i)`，本项目糖化为约定方法 `at`/`set` |
| P3 类模板实例化 | [temp] / [temp.inst] / [temp.names] | `Box<int>` 是"模板 id"，首次使用才实例化为具体类 |
| P4 内建函数 | [support.runtime]（libc 接口） | `malloc` 等只有语义原型，实现由链接期提供 |

### 1.2 为什么顺序是固定的

P1 是地基：**没有"栈上的类对象"，一切封装无从谈起**。
P2 依赖 P1 的对象布局（方法调用要喂 `this`）。
P3 依赖 P1/P2 对普通类成立的一切——实例化出的类必须与普通类
在 CodeGen 眼中**一视同仁**（同样的布局、同样的方法调用约定）。
P4 收尾：容器类最终要调 `malloc` 管理堆内存，闭环才算完整。

---

## ② P1：类类型局部变量与块作用域析构（RAII）

### 2.1 语义要求

```cpp
int main() {
    Point p;          // ① 零初始化 → ② 调用构造函数
    {
        Point q;      // 进入内层块：构造
    }                 // 离开块：立刻析构（[class.dtor] 的确定性销毁）
    return 0;
}
```

两个关键决策：

- **零初始化 + 构造**：栈槽先清零再调构造，未显式初始化的字段有
  确定值（P2 的 Map 用 `key==0` 判空槽正是依赖这一点）；
- **块尾析构**：语义阶段记录"本块声明了哪些类类型变量"，CodeGen
  在块的每条出口路径发射 `callq <类>_dtor`。栈对象无需 `free`——
  帧空间随函数返回自动回收（对照 `new` 的堆对象：析构后还要
  `callq free`，见 emitDelete）。

### 2.2 clang 对照

clang 的 `CodeGenFunction::EmitAutoVarDecl` + `Destroyer` 机制：
每个自动变量登记一个销毁动作，作用域退出时按**声明逆序**发射。
minicc 教学版简化为"块尾集中析构"，顺序同样是逆序。

---

## ③ P2：下标糖 `v[i]` → `at` / `set`

### 2.1 约定代替重载

真 `operator[]` 是运算符重载（[over.sub]），要引入运算符优先级与
左/右值上下文。教学版降级为**方法约定**：

```text
读：v[i]          →  v.at(i)        （类必须提供 at 方法）
写：v[i] = x      →  v.set(i, x)    （类必须提供 set 方法）
```

- **语义阶段**：`inferIndex` 在对象类里查 `at`，取其返回类型；
  赋值目标位置的 `v[i]` 走 `set` 的形参检查；
- **代码生成**：IndexExpr 降级为"对象地址进 `rdi` + 下标进 `rsi` +
  `callq <类>_at`"；赋值版发射 `callq <类>_set`。

### 2.2 约定为何成立

实例类与普通类共用同一套方法调用约定，所以**模板实例自动获得
下标能力**——`Map<int, int> m; m[10] = 100;` 零额外代码（P3 测试
test_stl_04 验证）。代价：`at` 的名字是硬编码的，不像真重载那样
可自定义行为。

---

## ④ P3：类模板实例化闭环

### 4.1 四个必修点

1. **类型位置解析**（Parser）：`parseType` 见标识符后跟 `<` 即递归
   解析实参表 → `Box<int>` 记为 `Class(name="Box", templateArgs=[int])`。
   嵌套 `Box<Box<int>>` 的 `>>` 由词法保证是两个 `Greater` token，
   天然避开 C++11 前的著名歧义；
2. **语句前瞻**（Parser）：`Box<int> bi;` 开头与表达式 `a < b` 同形，
   `parseStatement` 必须深度配对跳过 `<...>` 才能判对"这是声明"；
3. **按需实例化**（Sema）：`resolveType` 见非空 `templateArgs` →
   `getOrInstantiateClass`：查缓存 → `instantiate()` 深拷贝蓝图、
   替换形参 → `processClassDecl` 完整注册实例类；
4. **实例命名**：实例名 = 模板名 + 实参串净化（`Box<int>` → `Box_int`），
   空格剔除、`,*<>&` → `_`。实例名**即符号名**——方法符号
   `Box_int_get`、构造符号 `Box_int_Box_int` 全部由它派生。

### 4.2 两个关键设计

**缓存先行写入**：`instantiate()` 产出的实例类型在分析方法体**之前**
就写入 `m_classInstanceCache`，支持方法体内递归与互用引用
（[temp.inst] 的"同一实参组合只实例化一次"由缓存天然保证）。

**构造/析构改名**：克隆蓝图时构造函数名仍是蓝图名 `Box`，若不改为
实例名 `Box_int`，mangled 名会变成 `Box_int_Box` 而调用符号是
`Box_int_Box_int` → 链接期 undefined reference。这是实现时真实踩过的坑。

### 4.3 顺手修复的既有缺陷：类作用域方法查找

`Box_int.get` 与 `Box_double.get` 同名，`registerFunction` 以裸名做
`m_functionMap` 键会互相覆盖，导致返回类型误判。修复：`inferCall`
增加**按对象类型查 `m_classDecls` 方法表**的分支，命中即返回——
对照 clang `BuildMemberCallExpr` 的类作用域限定查找。

### 4.4 堆上的模板

`new Box<int>()` 的模板 id 在 `inferNew` 中原地把 `className`
改写为实例名，CodeGen 只见实例名。注意：**堆对象不走块尾析构**，
需显式 `delete`（析构 + `callq free`），与栈对象形成对照。

---

## ⑤ P4：内建外部函数原型

### 5.1 原理：无 body 的原型

```cpp
// registerBuiltins()：analyze() 入口第一件事
declare("malloc", intPtr, {intTy});                 // malloc(int) → int*
declare("free",   voidTy, {intPtr});                // free(int*)
declare("memcpy", intPtr, {intPtr, intPtr, intTy}); // memcpy(dst,src,n) → dst
declare("realloc",intPtr, {intPtr, intTy});         // realloc(p,n) → 新块
```

三处登记缺一不可：

| 登记点 | 作用 |
|---|---|
| `m_functionMap` | 调用点按裸名查找命中（与自由函数同一路径） |
| `m_functions` | CodeGen 遍历它，但 `if (func->body)` 守卫**自动跳过发射** |
| 全局作用域符号表 | 任何函数体内可见 |

于是 `.s` 里**不存在**这四个函数的汇编体，调用点发射普通
`callq malloc`，链接期由 libc 提供实现——与 `emitDelete` 里
`callq free` 的先例完全同构。

### 5.2 签名取舍

minicc 没有 `void*`/`size_t` 语义，一律用 `int*` 表示"一块内存"、
`int` 表示字节数。对照 clang：内建也走"先有声明再使用"——
`Builtins::Info` 大表给出类型串，语义检查照常；区别只在真 clang
多数内建在 IR 层替换为 `llvm.memcpy` 等 intrinsic，本项目直接调
libc 符号，链接器兜底。

---

## ⑥ 实现中顺手修复的历史缺陷：汇编符号净化

全量回归暴露 test_decl_01（命名空间测试）汇编失败：

```asm
.globl Math::scale      # gas 报错：junk at end of line
Math::scale:
```

根因：gas 标签只允许 `[A-Za-z0-9_.$]`，而 Sema 的
`processNamespaceDecl` 把命名空间函数改名为 `Math::scale`。
修复：Codegen 增加 `asmSymbol()` 净化器，**标签发射端与使用端
（`callq` / `%rip` 读写）成对一致**地把非法字符替换为下划线
（`Math::scale` → `Math__scale`）。对照真 mangling：
Itanium ABI 的 `_ZN4Math5scaleEi` 可编码任意类型签名且双向可逆，
是同一问题的完备解。

---

## ⑦ 复现步骤

```bash
cd /root/cppproject/mycompiler
cmake --build cmake-build-debug

# P4 内建：观察"无 body 却可调"
./cmake-build-debug/minicc tests/test_stl_06_builtins.cpp -o /tmp/t6.s
grep -c "callq malloc\|callq free\|callq memcpy\|callq realloc" /tmp/t6.s  # 6
grep -c "^malloc:\|^free:\|^memcpy:\|^realloc:" /tmp/t6.s                  # 0（无汇编体）
g++ -no-pie /tmp/t6.s -o /tmp/t6 && /tmp/t6; echo "exit=$?"                # exit=0

# P3 模板闭环：观察按需实例化日志
./cmake-build-debug/minicc tests/test_stl_03_template_class.cpp | grep instantiate:class
./cmake-build-debug/minicc tests/test_stl_04_map.cpp          | grep instantiate:class
./cmake-build-debug/minicc tests/test_stl_05_heap_template.cpp | grep "\[new\]"
```

### 测试矩阵

| 测试 | 验证点 | 预期退出码 |
|---|---|---|
| test_stl_01_unique_ptr | P1：栈对象 + 块尾析构（RAII） | 0 |
| test_stl_02_subscript | P2：`v[i]` → `at`/`set` 糖 | 0 |
| test_stl_03_template_class | P3：`Box<int>`/`Box<double>` 按需实例化 + 缓存命中 | 0 |
| test_stl_04_map | P2×P3：`Map<int,int> m; m[10]=100` | 0 |
| test_stl_05_heap_template | P3：`new Box<int>()` 堆上模板 + 显式 delete | 0 |
| test_stl_06_builtins | P4：malloc/memcpy/realloc/free 全链路 | 0 |
| test_decl_01_global_and_namespace | 符号净化回归（`Math::scale` → `Math__scale`） | 20 |
| test_tmpl_01..19 | 函数模板红线（必须始终全绿） | 见各测试头注释 |

回归命令（38 个端到端测试全量核对）：`unit_tests` 98/98 + 上表全部。

---

## ⑧ 一图总结

```text
源码  Box<int> bi; bi.set(7);   m[10]=100;   int* p=malloc(8);
        │            │              │               │
Parser  │ 模板 id 解析│              │               │
        ├─parseType 吃 <...>        │               │
        ├─语句前瞻（声明 vs 表达式） │               │
        ▼            ▼              ▼               ▼
Sema    resolveType  inferCall      inferIndex      inferCall
        │ 带实参→    │ 类作用域查    │ 读→at 写→set   │ m_functionMap
        ▼ 实例化     ▼ 方法表        ▼               ▼ 命中内建原型
        getOrInstantiateClass                       （body=nullptr）
        │ 缓存先行+方法改名+完整注册                 │
        ▼            ▼              ▼               ▼
CodeGen emitVarDecl  callq          callq           callq malloc
        零初始化     Box_int_set    Map_int_int_set （无函数体发射！
        +构造+块尾析构               ────同一套约定──── if(func->body)
                                                 跳过）
        │
        ▼
链接    ld ← libc 提供 malloc/free/memcpy/realloc
```

四个阶段落地后，minicc 的"类型封装"雏形闭环：**栈对象有生死
（P1），容器有下标（P2），类型可泛化（P3），内存有内建（P4）**。
