# 13 - C++ 名字修饰（Name Mangling）

## 13.1 什么是名字修饰？

名字修饰（Name Mangling）是编译器将 C++ 源代码中的标识符（函数名、类名、变量名等）转换为链接器能识别的唯一符号名的过程。

### 为什么需要名字修饰？

C 语言不允许函数重载，所以函数名就是符号名：

```c
int add(int a, int b) { return a + b; }
// 编译后符号表：add
```

C++ 允许函数重载，但汇编器和链接器只认唯一的名字：

```cpp
int add(int, int);      // 两个都叫 add
double add(double, double);  // 链接器怎么区分？

// 编译后符号表：
// _Z3addii    ← int add(int, int)
// _Z3addd      ← double add(double, double)
```

## 13.2 Itanium C++ ABI Mangling

GCC 和 Clang 使用 **Itanium C++ ABI** 的名字修饰方案。这是 Linux 和 macOS 上的事实标准（Windows MSVC 使用不同的方案）。

### 13.2.1 基本前缀

所有 C++ 符号以 `_Z` 开头：

| 前缀 | 含义 | 示例 |
|------|------|------|
| `_Z` | 普通函数/变量 | `_Z3foov` |
| `_ZN` | 嵌套名（类成员、命名空间） | `_ZN3Dog5speakEv` |
| `_ZTV` | vtable（虚函数表） | `_ZTV3Dog` |
| `_ZTI` | typeinfo（RTTI 类型信息） | `_ZTI3Dog` |
| `_ZTS` | typeinfo name（类型名字符串） | `_ZTS3Dog` |

### 13.2.2 编码规则

**长度前缀编码**：标识符长度 + 标识符本身

```
Dog       → 3Dog
speak     → 5speak
Animal    → 6Animal
my_func   → 7my_func
```

**基本类型编码**：

| 类型 | 编码 | 类型 | 编码 |
|------|------|------|------|
| `void` | `v` | `int` | `i` |
| `bool` | `b` | `long` | `l` |
| `char` | `c` | `long long` | `x` |
| `short` | `s` | `unsigned` | `j` |
| `float` | `f` | `unsigned long` | `m` |
| `double` | `d` | `unsigned long long` | `y` |

**指针和引用**：

| 类型修饰 | 编码 | 示例 |
|---------|------|------|
| `T*` | `P` + T | `int*` → `Pi` |
| `T&` | `R` + T | `int&` → `Ri` |
| `const T` | `K` + T | `const int` → `Ki` |

### 13.2.3 函数签名编码

格式：`_Z` + [嵌套] + 函数名 + 参数类型列表

```
void foo()
→ _Z3foov
  │  │  └─ v = void（无参数）
  │  └─ 3foo = 函数名长度+名字
  └─ _Z = C++ 符号前缀

int add(int, int)
→ _Z3addii
  │  │  └─ ii = int, int
  │  └─ 3add = 函数名
  └─ _Z

double compute(float, double)
→ _Z7computedf
       │  └─ fd = float, double（注意：参数顺序是从左到右）
       └─ 7compute = 函数名
```

### 13.2.4 嵌套名（类成员函数）

格式：`_ZN` + 类名 + 方法名 + `E` + 参数类型

```
Dog::speak()
→ _ZN3Dog5speakEv
  │ │  │   │    │  └─ v = void 参数
  │ │  │   │    └─ E = 嵌套结束标记
  │ │  │   └─ 5speak = 方法名
  │ │  └─ 3Dog = 类名
  │ └─ N = 嵌套名开始
  └─ _Z

Animal::speak(int)
→ _ZN6Animal5speakEi
  │ │    │   │    │ └─ i = int 参数
  │ │    │   │    └─ E = 嵌套结束
  │ │    │   └─ 5speak
  │ │    └─ 6Animal
  │ └─ N
  └─ _Z
```

**多层嵌套**（命名空间或嵌套类）：

```
std::vector<int>::push_back(int)
→ _ZNSt6vectorIiSaIiEE9push_backEi
  │  │  │      │  │     │ │       └─ i = int 参数
  │  │  │      │  │     │ └─ E = push_back 结束
  │  │  │      │  │     └─ 9push_back
  │  │  │      │  └─ E = vector<int> 结束
  │  │  │      └─ I...E = 模板参数 int
  │  │  └─ 6vector
  │  └─ 2std = std 命名空间
  └─ _ZN
```

### 13.2.5 模板实例化

格式：`_Z` + 模板名 + `I` + 模板参数 + `E`

```
template<typename T> T twice(T x);

twice<int>
→ _Z5twiceIiE
  │ │    │ │ └─ E = 模板参数结束
  │ │    │ └─ i = int
  │ │    └─ I = 模板参数开始
  │ └─ 5twice
  └─ _Z

twice<double>
→ _Z5twiceIdE
            └─ d = double
```

**模板参数编码**：

| 模板参数 | 编码 | 示例 |
|---------|------|------|
| 类型 | 类型编码 | `int` → `i` |
| 非类型值 | `L` + 类型 + 值 + `E` | `42` → `Li42E` |
| 模板模板参数 | 完整编码 | `std::vector` → `St6vector` |

### 13.2.6 特殊符号

**构造函数/析构函数**：

```
Dog::Dog()
→ _ZN3DogC1Ev    // C1 = 完整对象构造
→ _ZN3DogC2Ev    // C2 = 基类子对象构造

Dog::~Dog()
→ _ZN3DogD1Ev    // D1 = 完整对象析构
→ _ZN3DogD2Ev    // D2 = 基类子对象析构
→ _ZN3DogD0Ev    // D0 = 删除析构（调用 delete）
```

**虚函数表（vtable）**：

```
Dog 的 vtable
→ _ZTV3Dog
  │  │  └─ 3Dog = 类名
  │  └─ TV = vtable
  └─ _Z
```

**类型信息（typeinfo）**：

```
Dog 的 typeinfo
→ _ZTI3Dog
  │  │  └─ 3Dog = 类名
  │  └─ TI = typeinfo
  └─ _Z
```

**类型信息名（typeinfo name）**：

```
Dog 的 typeinfo name（字符串 "Dog"）
→ _ZTS3Dog
  │  │   └─ 3Dog = 类名
  │  └─ TS = typeinfo name
  └─ _Z
```

## 13.3 实际示例

### 13.3.1 简单函数

```cpp
void hello();
int add(int, int);
double compute(float, double, char);
```

```
_Z5hellov
_Z3addii
_Z7computedfdc
```

### 13.3.2 类成员函数

```cpp
class Animal {
public:
    void speak();
    int eat(int);
};
```

```
_ZN6Animal5speakEv
_ZN6Animal3eatEi
```

### 13.3.3 重载函数

```cpp
void print(int);
void print(double);
void print(const char*);
```

```
_Z5printi
_Z5printd
_Z5printPKc    // P = 指针, K = const, c = char
```

### 13.3.4 继承与虚函数

```cpp
class Base {
public:
    virtual void foo();
};

class Derived : public Base {
public:
    void foo() override;
};
```

```
Base:
  _ZTV4Base      // vtable
  _ZTI4Base      // typeinfo
  _ZN4Base3fooEv // Base::foo()

Derived:
  _ZTV7Derived   // vtable
  _ZTI7Derived   // typeinfo
  _ZN7Derived3fooEv // Derived::foo()
```

### 13.3.5 模板

```cpp
template<typename T>
T max(T a, T b) { return a > b ? a : b; }

// 实例化
int x = max(3, 5);
double y = max(3.14, 2.71);
```

```
_Z3maxIiET_T_      // max<int>(int, int)
_Z3maxIdET_T_      // max<double>(double, double)
```

## 13.4 minicc 的实现

minicc 对 **类符号**（vtable、typeinfo）使用真实的 Itanium ABI mangling，但对 **虚函数名** 使用简化格式以便阅读。

### 13.4.1 类符号（符合标准）

```cpp
// semantic_analyzer.cpp:1048
std::string typeinfoName = "_ZTI" + std::to_string(name.size()) + name;
// Animal → _ZTI6Animal

// semantic_analyzer.cpp:2798
std::string vtable_name = "_ZTV" + std::to_string(name.size()) + name;
// Dog → _ZTV3Dog
```

### 13.4.2 虚函数名（简化版）

```cpp
// semantic_analyzer.cpp:1104-1112
// 成员函数：类名_方法名
decl->mangledName = decl->ownerClassName + "_" + decl->name;
// Dog::speak → Dog_speak（而非 _ZN3Dog5speakEv）
```

**为什么简化？**

- 学习目的：`Dog_speak` 比 `_ZN3Dog5speakEv` 更易读
- 避免复杂性：真实 mangling 需要处理重载、参数类型、嵌套等
- 自包含：minicc 生成的汇编用这些简化名，不需要与外部 C++ 代码链接

### 13.4.3 在 dump 输出中的应用

**类层次图（`--dump-hierarchy`）**：

```
┌─ _ZTI6Animal ─────────────────────────────────────┐
│  typeinfo form: 'C' (__class_type_info)          │
│  ...                                              │
└───────────────────────────────────────────────────┘

┌─ _ZTI3Dog ────────────────────────────────────────┐
│  typeinfo form: 'S' (__si_class_type_info)       │
│  ...                                              │
│  RTTI chain:                                      │
│    _ZTI3Dog ['S'] ──base──→ _ZTI6Animal ['C']    │
└───────────────────────────────────────────────────┘
```

**内存布局（`--dump-layout`）**：

```
_ZTV3Dog:
  [-2] offset-to-top: 0
  [-1] typeinfo: _ZTI3Dog
  [0]  Dog_speak  (override)
  [1]  Dog_eat
```

**注意**：`_ZTV3Dog` 和 `_ZTI3Dog` 是真实符号，`Dog_speak` 是简化名。

## 13.5 名字修饰的意义

### 13.5.1 链接器视角

链接器只看到符号名，看不到源码：

```cpp
// file1.cpp
int add(int a, int b) { return a + b; }

// file2.cpp
int add(int, int);  // 声明
int x = add(1, 2);  // 调用
```

编译后：

```
file1.o: 定义符号 _Z3addii
file2.o: 引用符号 _Z3addii
链接器：匹配成功 ✓
```

如果没有 mangling，所有 `add` 都会冲突。

### 13.5.2 调试视角

调试器用 mangling 反向解析符号：

```
Breakpoint 1 at 0x401234: _ZN3Dog5speakEv
→ 显示为: Dog::speak()
```

`c++filt` 工具可以反修饰：

```bash
$ echo "_ZN3Dog5speakEv" | c++filt
Dog::speak()
```

### 13.5.3 性能视角

Mangling 增加符号长度，但对性能无影响：

- 符号名只在链接和调试时使用
- 运行时只关心地址，不关心名字
- 现代链接器用哈希表，符号长度不影响速度

## 13.6 与 C 的对比

C 语言不使用 mangling（或极简化）：

```c
// C 代码
int add(int, int);
// 编译后：add（或 _add，取决于平台）
```

这就是为什么 C++ 调用 C 函数需要 `extern "C"`：

```cpp
extern "C" {
    int c_function(int);  // 不修饰，保持 C 链接
}
```

## 13.7 总结

| 特性 | 说明 |
|------|------|
| **目的** | 将 C++ 标识符转换为链接器能识别的唯一符号 |
| **标准** | Itanium C++ ABI（GCC/Clang 使用） |
| **前缀** | `_Z`（普通）、`_ZN`（嵌套）、`_ZTV`（vtable）、`_ZTI`（typeinfo） |
| **编码** | 长度前缀 + 标识符（如 `3Dog`、`5speak`） |
| **minicc** | 类符号用真实 mangling，虚函数用简化名 |

名字修饰是 C++ 支持函数重载、类成员、模板等特性的基础设施。理解 mangling 有助于调试链接错误和理解编译器的内部工作。

---

**下一步**：
- 运行 `./minicc tests/mini_rtti_demo.cpp --dump-hierarchy` 观察真实的 `_ZTI*` 符号
- 使用 `nm` 或 `readelf` 查看编译后目标文件的符号表
- 对比 `c++filt` 的输出，理解反修饰过程
