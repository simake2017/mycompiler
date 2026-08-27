# 08 构造/析构与顶层声明体系（主线 A & 语法扩展）

> 主线 A：构造函数 / 成员初始化列表 / 虚析构函数 / new & delete 表达式 / 全局变量 / 枚举 / 命名空间 / 类型别名。
> C++ 对象生命周期 [basic.life] 是面向对象编译实现的核心基石——"分配空间 ≠ 构造对象，析构对象 ≠ 释放空间"。

---

## ① 理论背景

**标准依据**：[class.ctor]、[class.dtor]、[class.base.init]、[expr.new]、[expr.delete]、[dcl.dcl]、[dcl.enum]、[namespace.def]、[dcl.typedef]。

- **对象生命周期与动态内存管理**：
  - `new T(args)`：在编译底层分解为两步：
    1. **内存分配**：`void* p = malloc(sizeof(T));`
    2. **对象构造**：`T::T(p, args);`（将分配所得的内存指针作为隐藏的 `this` 形参传入构造函数）
  - `delete p`：在编译底层分解为两步：
    1. **对象析构**：`p->~T();`（若声明为 `virtual ~T()`，则经由 `_vptr` 索引虚函数表实现多态析构派发）
    2. **内存回收**：`free(p);`
- **成员初始化列表（Member Initializer List）**：
  - 构造函数执行序：**基类构造 → 注入 `_vptr` → 成员初始化列表 → 构造函数体语句**。
  - 为什么要在进入函数体前执行初始化列表？因为 const 成员、引用成员必须在声明时完成绑定，不可在函数体内通过赋值后绑定。
- **虚析构函数（Virtual Destructor）与多态释放**：
  - Itanium C++ ABI 规范：基类指针 `Base* b = new Derived()`，当执行 `delete b;` 时，若 `~Base()` 为虚析构，则析构函数作为虚表（vtable）中的第一个槽位条目被调用，派生类的析构函数会在执行完自身后自动析构基类，最后调用 `free`，避免派生类资源泄漏与切片未完全清理。
- **顶层声明扩展体系**：
  - **全局变量（GlobalVarDecl）**：分配至 `.data` 数据段，采用 x86-64 RIP 相对寻址（`var(%rip)`），在链接阶段完成绝对重定位。
  - **作用域解析与命名空间（NamespaceDecl）**：将符号名修饰为限定名（`Math::add` $\to$ `Math::add` 并在符号表和函数注册表中单向索引）。
  - **枚举声明（EnumDecl / Scoped Enum）**：非限定 `enum` 将枚举项提升至外层全局符号表，`enum class` 将枚举项限定在其命名空间内。
  - **类型别名（TypeAliasDecl）**：`using` / `typedef` 注册为 `SymbolKind::Type`，语义分析阶段通过规范化类型展开（Canonical Type Resolution）透明剥离别名层。

---

## ② 关键流程与内存布局 ASCII 图

### 1. `new` 与 `delete` 对象构造/析构执行链

```
┌─────────────────────────────────────────────────────────┐
│                    new Point(3, 4)                      │
└─────────────────────────────────────────────────────────┘
                            │
                            ▼
           1. call malloc(sizeof(Point)) → %rax
                            │
                            ▼
           2. 移动 %rax 到 %rdi (作为 this 指针)
              传参: %esi = 3, %edx = 4
                            │
                            ▼
           3. call Point_Point (%rdi, %esi, %edx)
              ┌─────────────────────────────────────┐
              │ Point_Point(this, px, py):          │
              │   this->_vptr = Point_vtable        │
              │   this->x = px                      │
              │   this->y = py                      │
              │   body { ... }                      │
              │   return this                       │
              └─────────────────────────────────────┘
                            │
                            ▼
           4. %rax = this (构造完成的完整对象指针)
```

```
┌─────────────────────────────────────────────────────────┐
│                       delete ptr                        │
└─────────────────────────────────────────────────────────┘
                            │
                            ▼
           1. 检查虚析构: class.hasVTable?
              ├─ 是: 经由 vtable[0] 多态分派:
              │     movq (%rdi), %rax     ; 取 _vptr
              │     callq *(%rax)         ; 派发到 Derived_dtor
              └─ 否: 直接调用静态析构标号:
                    callq Point_dtor
                            │
                            ▼
           2. 调用 free(ptr) 归还堆内存
```

---

## ③ 设计决策

| 决策点 | 本编译器设计选择 | 理论权衡与理由 |
|---|---|---|
| **构造函数返回约定** | 构造函数将 `%rdi` (`this`) 作为 `%rax` 返回 | 符合 System V AMD64 ABI 规范，便于链式初始化与 `new` 表达式直接接管指针 |
| **隐式构造/析构函数合成** | 语义分析阶段检测，若类未显式声明，则自动注入默认无参构造与默认析构 | C++ 标准 [class.default.ctor]：保证每个类在 `new` 和 `delete` 下均有明确的生命周期入口 |
| **虚析构在 vtable 中的槽位** | 固定将虚析构函数置于 `vtable[0]` | 统一单继承体系下多态析构派发索引，简化 CodeGen 的动态发射逻辑 |
| **全局变量寻址模式** | 统一采用 `var_name(%rip)` 相对寻址 | 满足 x86-64 现代操作系统的位置无关代码（PIC / PIE）规范 |
| **类型别名展开机制** | 语义分析阶段透明别名规范化 `resolveType()` | 保持 AST 节点原始结构便于诊断，在类型兼容性检查和符号注册时按规范底层类型展开 |

---

## ④ Clang 源码对照表

| 机制 / 阶段 | 本实现位置 | Clang 对应源码实现 | 教学级简化说明 |
|---|---|---|---|
| **顶层声明多路分派** | `src/parser.cpp:parseDeclaration` | `clang/lib/Parse/ParseDeclTop.cpp:ParseTopLevelDecl` | Clang 支持声明说明符复杂的自由组合；本实现使用 LL(1) + 局部试探解析区分全局变量与函数 |
| **构造函数与初始化列表** | `src/parser.cpp:parseConstructorDecl` & `src/codegen.cpp:emitFunction` | `clang/lib/Sema/SemaDeclCXX.cpp:ActOnMemInitializers` | Clang 严格按类成员在类内的声明顺序执行初始化（无论列表顺序如何）；本实现按列表书写顺序依次发射 |
| **虚析构函数与多态 delete** | `src/semantic_analyzer.cpp:processDeleteStmt` & `src/codegen.cpp:emitDelete` | `clang/lib/CodeGen/CGExprCXX.cpp:EmitDeleteExpr` | Clang 包含 deleting destructor 与 complete object destructor 两个变体；本实现简化为单析构槽位 + `free` |
| **全局变量发射** | `src/codegen.cpp:generate` (`.data` 段) | `clang/lib/CodeGen/CodeGenModule.cpp:EmitGlobalVarDefinition` | Clang 支持 BSS 段未初始化零值优化；本实现统一在 `.data` 段发射初始值 |

---

## ⑤ 实验与验证手册

### 1. 运行完整单元测试集
```bash
cd /home/magene/runtime/cppproject/mycompiler/build-linux
cmake --build . && ctest --output-on-failure
```

### 2. 构造函数与初始化列表编译实验
```bash
./minicc ../tests/test_ctor_01_basic.cpp -o /tmp/ctor01.s
# 观察生成的汇编中 Point_Point 构造函数及字段初始化指令：
grep -A 15 "Point_Point:" /tmp/ctor01.s
```

### 3. 虚析构函数多态析构与 delete 实验
```bash
./minicc ../tests/test_ctor_02_virtual_dtor.cpp -o /tmp/dtor02.s
# 观察虚析构函数在 vtable 槽位中的安装与 delete 处的动态派发：
grep -A 10 "delete" /tmp/dtor02.s
```

### 4. 全局变量与命名空间限定名实验
```bash
./minicc ../tests/test_decl_01_global_and_namespace.cpp -o /tmp/decl01.s
# 观察 .data 段全局变量发射与 Math::scale 符号调用：
grep -A 10 ".data" /tmp/decl01.s
```
