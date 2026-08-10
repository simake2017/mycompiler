# Clang C++ 标准库选择与头文件搜索机制

## 1. 背景

在 Linux 下使用 clang++ 编译 C++ 程序时，经常会遇到：

```bash
clang++ main.cpp
```

或者：

```bash
clang++ -stdlib=libc++ main.cpp
```

两者有什么区别？为什么指定 libc++ 后，头文件搜索路径会发生变化？

例如：

- 默认：`/usr/include/c++/13`
- 指定 libc++：`/usr/lib/llvm-18/include/c++/v1`

---

## 2. C++ 标准库实现

C++ 标准只规定接口和行为，不规定具体实现。Linux 下主要有两个 C++ 标准库实现。

### 2.1 libstdc++

- 来源：GCC 项目
- 头文件目录通常为 `/usr/include/c++/<gcc版本>`，例如 `/usr/include/c++/13`
- 对应动态库：`libstdc++.so.6`

### 2.2 libc++

- 来源：LLVM 项目
- 头文件目录：`include/c++/v1`，例如 `/usr/lib/llvm-18/include/c++/v1`
- 对应库文件：`libc++.so`（LLVM 17+ 默认将 `libc++abi` 合并进 `libc++.so`，旧版本另有独立的 `libc++abi.so`）

---

## 3. 为什么 C++ 标准库依赖头文件？

C 语言中：

```c
#include <stdio.h>
```

头文件主要提供声明（如 `printf()`），真正实现位于 `libc.so`。

但是 C++ 大量使用模板：

```cpp
std::vector<int>
std::map<int,int>
std::optional<int>
```

模板必须在编译阶段看到完整实现：

```cpp
template<class T>
class vector {
public:
    void push_back(const T&);
};

// 实现也在头文件里
template<class T>
void vector<T>::push_back(...) { }
```

因此：**C++ 标准库头文件本身就是实现的一部分。** 所以 libstdc++ 的头文件不能和 libc++ 的库文件随意混用。

---

## 4. clang++ 默认使用什么标准库？

Linux 下 `clang++ main.cpp` 默认通常使用 **libstdc++**。

原因：Linux 长期的默认工具链是 GCC + libstdc++，为了兼容 Linux 生态，Clang 默认也使用 GCC 标准库。

执行：

```bash
clang++ -v -E -x c++ /dev/null
```

可能看到：

```
/usr/include/c++/13
/usr/include/x86_64-linux-gnu/c++/13
```

这些路径来自 GCC。

---

## 5. 指定 libc++ 后发生什么？

执行 `clang++ -stdlib=libc++ main.cpp`，clang++ Driver 会切换标准库。

### 5.1 头文件路径变化

从 libstdc++ 切换到 libc++，例如 `/usr/lib/llvm-18/include/c++/v1`。

### 5.2 链接库变化

默认 `-lstdc++`，变成 `-lc++`。

> 注意：LLVM 16 及更早版本还会额外链接 `-lc++abi`；从 LLVM 17 起，libc++abi 默认合并进 `libc++.so`，一般只需链接 `-lc++`。

因此 `-stdlib=libc++` 实际上同时改变：

1. C++ 标准库头文件
2. C++ 标准库链接目标

---

## 6. 谁决定头文件搜索路径？

不是预处理器，不是 apt，而是 **clang++ Driver**。

流程：

```
clang++
    |
    | 解析参数
    |
    +----------------+
    |                |
 默认              -stdlib=libc++
    |                |
    v                v
libstdc++        libc++
    |                |
    v                v
寻找 GCC          寻找 libc++
安装目录          安装目录
    |                |
    v                v
添加 include     添加 include
搜索路径         搜索路径
```

最后调用 `clang -cc1` 真正执行编译。

---

## 7. 为什么 libc++ 在 /usr/lib/llvm-18？

LLVM 官方工具链结构：

```
llvm/
├── bin
├── include
├── lib
└── share
```

例如安装 `/opt/llvm-20` 会得到：

```
/opt/llvm-20
├── bin
├── include
│   └── c++/v1
└── lib
```

Debian/Ubuntu 保留这种结构（`/usr/lib/llvm-18`），所以 `/usr/lib/llvm-18/include/c++/v1` 就是 LLVM 18 的 libc++。

---

## 8. 为什么有 /usr/include/c++/v1？

查看 `/usr/include/c++` 可能看到：

```
10/
11/
12/
13/
v1 -> ../../lib/llvm-18/include/c++/v1
```

其中 `v1` **不是 LLVM 版本**。它是 libc++ 的 **ABI 版本号**，对应 inline namespace `std::__1`，表示 ABI 版本 1。

---

## 9. 为什么使用软链接？

因为 Linux 需要支持多个 LLVM 版本共存，例如：

```
/usr/lib/llvm-17
/usr/lib/llvm-18
/usr/lib/llvm-19
```

每个版本都有自己的 `include/c++/v1`，这些实际文件装在各自目录下，天然互不冲突。但无版本的统一入口 `/usr/include/c++/v1` 只能指向其中一个版本：

```
/usr/include/c++/v1
        |
        v
/usr/lib/llvm-18/include/c++/v1
```

这个入口指向哪个版本，由发行版的包管理机制（如 alternatives）决定。

---

## 10. apt 安装时会判断 clang 版本吗？

不会。这是一个重要概念。apt 做的事情：

```
下载 deb 包
↓
解压文件
↓
放到 deb 中记录的位置
```

例如 `libc++-18-dev.deb` 里面已经定义好了 `usr/lib/llvm-18/include/c++/v1/vector`，apt 只是把 deb 文件放到 `/usr/lib/llvm-18/include/c++/v1/vector`。它不会：

- 检测当前 clang
- 判断版本
- 动态计算路径

> 补充：deb 中文件的安装路径是打包时固定的；但 `/usr/include/c++/v1` 这个软链接本身是由 `libc++-18-dev` 包的 postinst 维护脚本创建的，不是 deb 里直接解压出来的文件。

---

## 11. 谁决定安装目录？

不是 apt，而是 **Debian/Ubuntu 软件包维护者**，制作 .deb 时就已经决定。例如：

- `libc++-17-dev` → `/usr/lib/llvm-17`
- `libc++-18-dev` → `/usr/lib/llvm-18`

---

## 12. clang 如何找到自己的 libc++？

例如 `/usr/lib/llvm-18/bin/clang++`，Driver 会以**自身可执行文件位置**为基准向上推导安装前缀，查找相对路径 `../include/c++/v1`：

```
/usr/lib/llvm-18/bin/clang++
↓
/usr/lib/llvm-18
↓
include/c++/v1
```

因此 `/usr/lib/llvm-18/include/c++/v1` 可以被自动找到。实测搜索路径即 `/usr/lib/llvm-18/bin/../include/c++/v1`。

### 12.1 多版本共存时：用哪一份，由编译器二进制位置决定

本机 `/usr/lib` 下同时装有 `llvm-11` 和 `llvm-18`。`-stdlib=libc++` 时 Driver 按链式候选尝试（`AddLibCxxIncludePaths`），**二选一、命中即停**：

```
1. <编译器位置>/../include/c++/v1    ← 第一候选，随编译器二进制走
        ↓ 目录不存在才继续
2. /usr/include/c++/v1              ← 发行版无版本兼容入口（见 §8-9，备胎）
```

实测三种组合：

| 调用的编译器 | 第一候选是否存在 | 搜索列表里的标准库行 |
|---|---|---|
| `/usr/lib/llvm-18/bin/clang++` | ✓ 存在 | `/usr/lib/llvm-18/bin/../include/c++/v1` |
| `/usr/lib/llvm-11/bin/clang++`（未装 libc++-11-dev） | ✗ 不存在 | fallback 到 `/usr/include/c++/v1` |
| 默认 libstdc++ 模式 | —— | 两个 v1 候选都不参与，命中 libstdc++ 三件套 |

`<vector>` 等头文件由 **`libc++-18-dev` / `libc++-11-dev` 分包**提供，各装各的 `include/c++/v1`，天然共存。注意 `/usr/lib/llvm-11/include/` 下的其他内容**都不是** C++ 标准库：

| 内容 | 实际身份 |
|---|---|
| `llvm/` → `/usr/include/llvm-11/llvm` | LLVM 库自身的 C++ API 头（开发编译器用），`libllvm11-dev` 提供 |
| `llvm-c/` | LLVM 的 C 语言绑定头 |
| `openmp/`、`polly/`、`ompt-multiplex.h` | OpenMP 运行时 / Polly 优化器头 |

### 12.2 翻车现场：fallback 造成的跨版本混血

第一候选缺失时 fallback 是**静默**的——不会报"缺包"，而是直接吃进软链接指向的另一版本头文件。本机实测（clang-11 + 软链接指向 llvm-18 的头）：

```bash
$ echo '#include <vector>
int main(){ std::vector<int> v; }' | /usr/lib/llvm-11/bin/clang++ -stdlib=libc++ -fsyntax-only -x c++ -

/usr/include/c++/v1/__config:48:8: warning: "Libc++ only supports Clang 16 and later"
/usr/include/c++/v1/__utility/declval.h:18:1: error: expected identifier or '{'
_LIBCPP_BEGIN_NAMESPACE_STD
^
...
fatal error: too many errors emitted
```

编译器 11 + 头文件 18 的混血组合：头文件先自报版本不兼容警告，随后 clang-11 解析不了新宏/新语法，炸出一堆莫名其妙的语法错误。**看到这类错误先查 `clang++ -stdlib=libc++ -v -E -x c++ /dev/null` 的搜索列表第一行是不是你预期的那份**，而不是去怀疑代码。

修复：给对应版本装 dev 包，让第一候选存在、fallback 不再触发：

```bash
apt install libc++-11-dev
readlink -f /usr/include/c++/v1   # 注意：postinst 可能把软链接改指 llvm-11，装完核查
```

---

## 13. GCC 和 LLVM 目录设计区别

**GCC**——传统 Linux 布局：

```
/usr/bin/g++
/usr/include/c++/13
/usr/lib/gcc/x86_64-linux-gnu/13
```

特点：系统级布局，版本通过目录区分。

**LLVM**——工具链布局：

```
/usr/lib/llvm-18
├── bin
├── include
├── lib
└── share
```

特点：一个 LLVM 版本就是一个完整工具链，多版本可以共存。

---

## 14. 最终总结

整体关系：

```
                    clang++
                       |
          +------------+------------+
          |                         |
       默认                    -stdlib=libc++
          |                         |
          v                         v
      libstdc++                 libc++
          |                         |
          v                         v
/usr/include/c++/13       /usr/lib/llvm-18/include/c++/v1
```

职责划分：

| 组件 | 职责 |
|---|---|
| apt | 安装 deb 包，不判断编译器 |
| Debian 包维护者 | 决定安装路径 |
| clang++ Driver | 选择标准库并生成搜索路径 |
| libstdc++ | GCC C++ 标准库 |
| libc++ | LLVM C++ 标准库 |

核心理解：

- apt 负责放文件，clang++ 负责选择和寻找标准库。
- `-stdlib=libc++` 会同时改变头文件搜索路径和链接库。
- `/usr/lib/llvm-18/include/c++/v1` 是 LLVM 工具链的一部分，而 `/usr/include/c++/v1` 通常只是发行版提供的兼容入口。

---

# 第二部分：头文件与 .so 到底是怎么被找到的

以下实验均在 Debian clang 18.1.8 / boost 1.74 环境实测。

## 15. 两套头文件：别把 "clang 的头文件" 和 "C++ 标准库头文件" 混为一谈

clang 的搜索列表里其实有**两套**机制不同的头文件：

| 套 | 例子 | 提供者 | 路径由谁决定 |
|---|---|---|---|
| C++ 标准库 | `<vector>` `<iostream>` | libstdc++ 或 libc++ | `-stdlib`（Linux 默认 libstdc++） |
| 编译器 builtin | `stddef.h` `stdint.h` `emmintrin.h` | clang 自己（resource dir） | 编译器二进制的位置，**无条件加入**，与 `-stdlib` 无关 |

### 实验 15.1：对比两种模式的搜索列表

```bash
clang++ -v -E -x c++ /dev/null
clang++ -stdlib=libc++ -v -E -x c++ /dev/null
```

输出对比（实测，路径有缩写）：

```
=== 默认（libstdc++）===                        === -stdlib=libc++ ===
 /usr/.../include/c++/10                        ┐
 /usr/.../include/x86_64-linux-gnu/c++/10       ├ C++ 标准库组：整组被换掉
 /usr/.../include/c++/10/backward               ┘
 /usr/lib/llvm-18/lib/clang/18/include              /usr/lib/llvm-18/bin/../include/c++/v1  ← 换成它
                                                    /usr/lib/llvm-18/lib/clang/18/include   ← 纹丝不动
 /usr/local/include                             ┐   /usr/local/include                      ┐
 /usr/include/x86_64-linux-gnu                  ├ 不变 /usr/include/x86_64-linux-gnu       ├ 不变
 /usr/include                                   ┘   /usr/include                            ┘
```

结论：`-stdlib` 只拨动最顶上的 C++ 标准库组；resource dir 和系统目录不受影响。

#### 逐目录详解 + 典型代表文件（本机实测）

按"谁提供、装什么、能否被 `-stdlib` 换掉"分四组。**查找顺序就是从列表顶部往下，命中即停**（这也是 `/usr/local/include` 能覆盖 `/usr/include` 同名文件的原因）。

**① C++ 标准库组 · libstdc++（默认模式，整组可被 `-stdlib` 换掉）**

| 目录 | 作用 | 代表文件（实测） |
|---|---|---|
| `/usr/include/c++/10` | GCC 附带的 libstdc++ **主目录**，"10" 是 GCC 版本号。所有无扩展名的标准头都在这 | `vector` `string` `algorithm` `iostream` `any` `atomic` `bits/`（内部实现目录） |
| `/usr/include/x86_64-linux-gnu/c++/10` | libstdc++ 的**架构相关部分**（Debian multiarch：不同架构各装一份） | 只有 `bits/` 和 `ext/` 两个子目录；核心是 `bits/c++config.h`——里面全是 `_GLIBCXX_*` 宏开关（字节序、`long` 宽度、ABI 选择），换架构就换这份文件 |
| `/usr/include/c++/10/backward` | **向后兼容**的老式/废弃头文件，正常代码不该用 | `hash_map` `hash_set` `strstream` `auto_ptr.h` `backward_warning.h`（包含时直接报警告） |

> 为什么标准库头文件没有 `.h` 扩展名（是 `<vector>` 不是 `<vector.h>`）？C++98 标准规定：无扩展名头属于 C++ 标准库，`.h` 形式保留给 C 库兼容头（如 `<string.h>` 对 `<cstring>`）。

**② C++ 标准库组 · libc++（`-stdlib=libc++` 模式，一个目录顶替上面三件套）**

| 目录 | 作用 | 代表文件（实测） |
|---|---|---|
| `/usr/lib/llvm-18/bin/../include/c++/v1` | LLVM 自家的 libc++。`v1` 是 ABI 版本目录（所有架构共用同一份，靠宏区分，不像 libstdc++ 那样按架构拆目录）；`bin/..` 只是 driver 以自身位置推导前缀留下的绕路写法，实际等于 `/usr/lib/llvm-18/include/c++/v1` | `vector` `string`，以及 libc++ 特有的拆分式内部目录：`__algorithm/` `__concepts/` `__chrono/` `__atomic/`（一个算法一个文件，便于模块化） |

**③ 编译器内建组（resource dir，两种模式纹丝不动）**

| 目录 | 作用 | 代表文件（实测） |
|---|---|---|
| `/usr/lib/llvm-18/lib/clang/18/include` | **clang 自带的内建头**，不属于任何标准库。装"只有编译器自己知道"的东西：`size_t` 在 x86-64 上是 `unsigned long`、32 位上是 `unsigned int`，随目标架构变，第三方库给不了 | `stddef.h` `stdint.h` `stdbool.h` `stdarg.h`（基础类型定义）；`immintrin.h` `x86intrin.h`（CPU intrinsics）；`__clang_cuda_*.h` 系列（CUDA 支持） |

> 它排在 `/usr/include` 之前是刻意的：必须抢在 glibc 的同名 `stddef.h` 前面命中。GCC 有完全对应的目录 `/usr/lib/gcc/x86_64-linux-gnu/10/include`。

**④ 系统 C 库 + 第三方（两种模式都不变）**

| 目录 | 作用 | 代表文件（实测） |
|---|---|---|
| `/usr/local/include` | **本机手动安装的第三方库**（`make install` 默认去处），优先级高于 `/usr/include` | `google/`（protobuf/glog 等源码编译安装的库） |
| `/usr/include/x86_64-linux-gnu` | Debian/Ubuntu **multiarch 的 C 库架构相关头**（i386 机器上是 `i386-linux-gnu`） | `bits/`（如 `bits/types.h`，glibc 内部类型）、`asm/`、`curl/`、`ffi.h` |
| `/usr/include` | **C 标准库 + glibc 主目录**，兼作系统级第三方库集散地 | `stdio.h` `stdlib.h` `unistd.h` `pthread.h`、`arpa/`（网络）、`aom/` `armadillo/`（第三方） |

**一张图记住分层与可变性**：

```
C++ 标准库     ← -stdlib 唯一能拨动的开关：libstdc++ 三目录 ↔ libc++ v1 一目录
clang 内建     ← 跟编译器二进制走，换标准库不影响
/usr/local     ← 本机安装的第三方，可覆盖系统同名头
C 库(架构相关) ← Debian multiarch 拆分产物
C 库(通用)     ← glibc + 系统第三方
```

### 实验 15.2：resource dir 在哪、装什么

```bash
$ clang++ -print-resource-dir
/usr/lib/llvm-18/lib/clang/18
```

它的 `include/` 子目录就是列表里那行 `clang/18/include`，路径按编译器二进制自身位置推导（`<prefix>/lib/clang/<版本>/include`，与 §12 找 libc++ 同一机制）。里面装的东西**不属于任何标准库**：

| 内容 | 例子 |
|---|---|
| C 内置头文件 | `stddef.h`（`size_t` 的定义）、`stdint.h`、`stdbool.h`、`stdarg.h` |
| CPU intrinsics | `emmintrin.h`（SSE2）、`immintrin.h`（AVX） |

为什么必须编译器自带：`size_t` 在 x86_64 上是 `unsigned long`、在 32 位上是 `unsigned int`——随目标架构变，**只有编译器自己知道**，不能放在第三方库里。GCC 也有完全对应的目录 `/usr/lib/gcc/x86_64-linux-gnu/10/include`。

### 实验 15.3：证明 `<vector>` 命中的就是列表第一行

`-H` 打印实际被 include 的文件树：

```bash
$ echo '#include <vector>' | clang++ -fsyntax-only -H -x c++ - 2>&1 | head -1
. /usr/.../include/c++/10/vector                       ← 默认模式列表第 1 行

$ echo '#include <vector>' | clang++ -stdlib=libc++ -fsyntax-only -H -x c++ - 2>&1 | head -1
. /usr/lib/llvm-18/bin/../include/c++/v1/vector        ← libc++ 模式列表第 1 行
```

搜索列表就是一份**优先级名单**：从上往下逐目录试字面文件名，命中即停。

两个细节：

1. **切换是"整组换掉"，不是"排到后面"**。libc++ 模式下 GCC 的三行路径直接从列表消失，不存在"回落找 GCC"。driver 保证列表里永远只有一套 C++ 标准库——这正是"头文件不能混用"在机制层的体现：路径顺序决定你拿到谁的 `<vector>` 语义，而链接的库是另一回事，两边错位就是 ODR 违规。
2. **builtin 排中间是刻意的**：它的 `stddef.h` 必须排在 `/usr/include` 之前，才能赢过 glibc 的同名文件；它和标准库没有同名文件，互相之间顺序无所谓。

排查头文件的实用命令：

| 命令 | 看什么 |
|---|---|
| `clang++ -v -E -x c++ /dev/null` | 候选列表（不看命中，只看候选） |
| `clang++ -H -fsyntax-only x.cpp` | 实际 include 树，带缩进层级 |
| `clang++ -E x.cpp \| grep '# 1 "'` | 每个被展开文件的确切路径 |

### 15.4 头文件目录全景（总表）

**表 A：会出现在搜索路径里的目录**（按默认 libstdc++ 模式优先级排序；#8 仅在 `-stdlib=libc++` 模式出现）：

| # | 目录 | 性质 | 具体作用 | 典型文件 | 归属（apt 包） | 备注 |
|---|---|---|---|---|---|---|
| 1 | `/usr/include/c++/10` | libstdc++ 主目录 | C++ 标准库全部标准头（GCC 阵营） | `vector` `string` `algorithm` `iostream` `bits/` | `libstdc++-10-dev` | `-stdlib` 可整组换掉；clang 默认经 `/usr/lib/gcc/…/../../../../` 推导命中 |
| 2 | `/usr/include/x86_64-linux-gnu/c++/10` | libstdc++ 架构相关部分 | 随平台变的配置：`_GLIBCXX_*` 宏开关（类型宽度、ABI、原子实现）；**不是 glibc，是 GCC 针对本平台 configure 期生成的 libstdc++ 目录** | `bits/c++config.h` | `libstdc++-10-dev` | 排在主目录前，保证同名 `bits/` 头命中平台版 |
| 3 | `/usr/include/c++/10/backward` | libstdc++ 废弃兼容头 | 远古非标准头，含即告警 | `hash_map` `auto_ptr.h` `strstream` | `libstdc++-10-dev` | 正常代码不该碰 |
| 4 | `/usr/lib/llvm-18/lib/clang/18/include` | 编译器内建（resource dir） | 编译器"内脏"：只有编译器知道的目标相关类型 + CPU intrinsics | `stddef.h` `stdint.h` `stdarg.h` `immintrin.h` `__clang_cuda_*.h` | `libclang-common-18-dev` | 永不可换，随编译器版本走；必须排在 glibc 同名头之前 |
| 5 | `/usr/local/include` | 本机第三方 | `make install` 默认去处 | `google/`（protobuf/glog） | 手动安装 | 优先级高于 `/usr/include`，可覆盖系统同名头 |
| 6 | `/usr/include/x86_64-linux-gnu` | multiarch 架构相关总集 | 准入标准唯一："换 CPU 架构就得换"的头（与 glibc 无绑定，各家来借住） | `bits/types.h` `asm/` `sys/ucontext.h`、`tiffconf.h` `openssl/`（第三方 config 头） | `libc6-dev` `linux-libc-dev` + 各 `-dev` | i386 机器上另有 `i386-linux-gnu`，可并存 |
| 7 | `/usr/include` | 系统头总杂烩 | glibc 主体 + 内核通用 UAPI + 第三方架构无关头 | `stdio.h` `unistd.h` `arpa/inet.h` `asm-generic/` `boost/` `tiff.h` | `libc6-dev` + 几乎所有 `-dev` | 搜索列表垫底 |
| 8 | `/usr/lib/llvm-18/include/c++/v1` | libc++ 标准库（LLVM 阵营） | 整套 C++ 标准库；`-stdlib=libc++` 模式**以一替三**顶掉 #1-3 | `vector` `string` `__algorithm/` `__concepts/` `__config` | `libc++-18-dev` | 搜索列表显示的 `bin/../include/c++/v1` 是同一目录的 Driver 推导写法（§12），磁盘上只有这个实际路径；所有架构共用一份，靠 `__config` 宏分支适配，不做 multiarch 拆分（对照 §13） |

> 注：#1-3（libstdc++ 三件套）与 #8（libc++）是**替换关系而非共存**——默认模式命中 #1-3，`-stdlib=libc++` 模式命中 #8，其余各行两种模式都不变。

**表 B：不进搜索路径的目录**（"以编译器为库"的 API 头 + 备胎入口）：

| 目录 | 性质 | 具体作用 | 典型文件 | 归属 | 备注 |
|---|---|---|---|---|---|
| `/usr/include/c++/v1` | 软链接（备胎入口） | 无版本统一入口 → 指向某版 libc++ | → `/usr/lib/llvm-18/include/c++/v1` | `libc++-18-dev` 的 postinst 维护 | 仅当编译器自己那份 v1 缺失时 fallback 命中（见 §12.2 翻车源头） |
| `/usr/lib/llvm-11/include/llvm`（→ `/usr/include/llvm-11`） | LLVM 库的 C++ API | 开发编译器/工具用 | `llvm/IR/Module.h` | `libllvm11-dev` | 与编译你的程序用哪套标准库无关 |
| `/usr/lib/llvm-11/include/llvm-c` | LLVM 的 C 绑定 API | 给非 C++ 语言绑定用 | `llvm-c/Core.h` | `libllvm11-dev` | |
| `/usr/lib/llvm-11/include/openmp`、`polly/` | OpenMP 运行时 / Polly 优化器 | 并行/循环优化开发 | `omp.h` `omp-tools.h` | `libomp-11-dev` `libpolly-11-dev` | |
| `/usr/include/clang` | libclang 工具库 API | 写代码分析/重构工具（clang-tidy 那类） | `clang-c/Index.h` | `libclang-18-dev` | "第三种 clang 头文件"，别与内建头、libc++ 混 |
| `/usr/lib/gcc/x86_64-linux-gnu/10/include` | GCC 内建（仅 gcc 模式） | GCC 的 resource dir 对应物 | `stddef.h` `stdint.h` | `gcc-10` | clang 用自己的 #4，不碰这里 |

**三句话总纲**：

1. **可变性只有两档**：`-stdlib` 能拨动的只有最顶上的标准库组（表 A #1-3 ↔ libc++ 行）；其余全组恒定。
2. **multiarch（#6）的准入标准是"架构相关"**：glibc、内核、libstdc++、第三方 config 头都来借住，但没有一个是"因为属于 glibc"而住进来。
3. **三套"看起来像 clang/标准库"的东西别混**：内建头（编译器内脏，#4）／libc++／`/usr/include/clang` + `/usr/lib/llvm-*/include/llvm`（开发工具用的库 API，表 B）。

---

## 16. 链接期：`-l` 是怎么找到 .so 的

### 16.1 规则：字面文件名匹配，没有模糊查找

`-lboost_regex` = 在每个搜索目录里依次试 `libboost_regex.so`，没有再试 `libboost_regex.a`。`libboost_regex.so.1.74.0` 不叫 `libboost_regex.so`，所以不算命中：

```bash
$ g++ re_demo.cpp -lboost_regex -o x
/usr/bin/ld: cannot find -lboost_regex        ← 文件明明在，但名字不是字面匹配的

$ g++ re_demo.cpp -l:libboost_regex.so.1.74.0 -o x && echo OK
OK                                            ← 冒号语法 = 按完整文件名匹配，直接命中
```

（本机恰好没装 `-dev` 包、缺无版本符号链接，反而暴露了这条死规矩。）

### 16.2 搜索顺序

| 顺序 | 来源 | 说明 |
|---|---|---|
| 1 | 命令行 `-L` | 按书写顺序，先写的先找 |
| 2 | g++ 前置的 `-L` | 工具链目录，`g++ -###` 可见 |
| 3 | 环境变量 `LIBRARY_PATH` | **不是** `LD_LIBRARY_PATH`（那个是运行期的） |
| 4 | ld 内置 `SEARCH_DIR` | `ld --verbose` 可见 |

实测数据——`g++ -###` 打出的真实 ld 命令行（用户写的 `-l` 排在这串 `-L` 之后）：

```
-L/usr/lib/gcc/x86_64-linux-gnu/10              ← gcc 自己的库（libstdc++ 在这）
-L/usr/lib/gcc/.../../../../x86_64-linux-gnu    ← 即 /usr/lib/x86_64-linux-gnu
-L/usr/lib/gcc/.../../../../../lib              ← 即 /usr/lib
-L/lib/x86_64-linux-gnu
-L/lib/../lib                                   ← 即 /lib
-L/usr/lib/x86_64-linux-gnu
-L/usr/lib/../lib                               ← 即 /usr/lib
```

`ld --verbose` 的内置列表（去重）：

```
/usr/local/lib/x86_64-linux-gnu    /usr/local/lib64    /usr/local/lib
/lib/x86_64-linux-gnu              /lib64              /lib
/usr/lib/x86_64-linux-gnu          /usr/lib64          /usr/lib
```

Debian 把 multiarch 目录（`x86_64-linux-gnu`）排在通用目录之前，保证 64 位库优先命中。

### 16.3 多版本共存时谁被选中：符号链接说了算

实验：造两个版本的库，看 `-lfoo` 分别链到谁：

```bash
$ gcc -shared -fPIC -Wl,-soname,libfoo.so.1 -o libfoo.so.1 foo.c
$ gcc -shared -fPIC -Wl,-soname,libfoo.so.2 -o libfoo.so.2 foo.c

$ ln -sf libfoo.so.1 libfoo.so
$ gcc main.c -L. -lfoo -o main && readelf -d main | grep 'NEEDED.*foo'
 (NEEDED)  [libfoo.so.1]

$ ln -sf libfoo.so.2 libfoo.so        # 编译命令一字未改，只改符号链接
$ gcc main.c -L. -lfoo -o main && readelf -d main | grep 'NEEDED.*foo'
 (NEEDED)  [libfoo.so.2]

$ gcc main.c -L. -l:libfoo.so.1 -o main && readelf -d main | grep 'NEEDED.*foo'
 (NEEDED)  [libfoo.so.1]               # -l: 可绕开符号链接直接点名
```

结论：**链接器不做版本判断**。无版本符号链接 `libfoo.so`（由 `-dev` 包提供）指向谁，就链谁。本机 `libboost_regex*` 只有 1.74.0 一个文件，"为什么用它"的答案是：没有别的可选。

### 16.4 链哪个组件库：没有任何人自动知道

boost 有几十个组件 .so（regex、thread、filesystem……），工具链**没有**"看你 include 了什么头文件就自动找对应库"的机制。实验——同一份用了 `boost::regex` 的代码：

```bash
$ g++ re_demo.cpp -o x                                                    # 什么都不链
undefined reference to `boost::re_detail_107400::perl_matcher<...>::match()'

$ g++ re_demo.cpp /usr/lib/.../libboost_serialization.so.1.74.0 -o x      # 故意链错的
undefined reference to `boost::re_detail_107400::perl_matcher<...>::match()'   # 一模一样的错

$ g++ re_demo.cpp /usr/lib/.../libboost_regex.so.1.74.0 -o x && ./x       # 链对的
matched: bbb
```

编译器只认头文件和符号，链接器只在**你递进来的清单**里匹配符号，匹配不上就报错，绝不自己去 /usr/lib "找找看"。"用了什么功能 → 该链哪个库"这张映射表只存在于：

- 程序员的脑子里（查 Boost 文档：哪些是 header-only 不用链、哪些要链）
- 构建系统的配置表里（CMake 的 `find_package(Boost COMPONENTS regex)` 也是人肉维护的死表）

顺带注意报错符号里的 `re_detail_107400`——boost 把 `BOOST_VERSION` 嵌进了 inline namespace 名，符号自带版本号，两个版本的 boost 即使同时被链入，符号也永远不会张冠李戴。

---

## 17. 运行期：ld.so 怎么把 NEEDED 变成实际路径

### 17.1 完整链条（boost regex 实测）

```bash
$ objdump -p /usr/lib/.../libboost_regex.so.1.74.0 | grep SONAME
  SONAME  libboost_regex.so.1.74.0        ← 字符串在打包时就烤进了库自己体内

$ g++ re_demo.cpp /usr/lib/.../libboost_regex.so.1.74.0 -o re_demo   # 故意用全路径链

$ readelf -d re_demo | grep NEEDED
 (NEEDED) [libboost_regex.so.1.74.0]      ← 路径丢了！只记录 SONAME 字符串
 (NEEDED) [libstdc++.so.6]
 (NEEDED) [libc.so.6]

$ ldd re_demo | grep regex
 libboost_regex.so.1.74.0 => /lib/x86_64-linux-gnu/libboost_regex.so.1.74.0

$ ./re_demo
matched: bbb
```

决定性证据：用全路径链接，NEEDED 里依然不记路径、只记 SONAME。链接器读库自报的 SONAME 原样抄入；**你用什么路径找到库，与记录什么无关**。

### 17.2 SONAME 的三次转手

| 阶段 | 谁写的 | 写了什么 |
|---|---|---|
| 库打包时 | 构建脚本传 `-Wl,-soname,libboost_regex.so.1.74.0` | 烤进 `.dynamic` 段；版本号来自 `boost/version.hpp` 的 `BOOST_VERSION`（107400 = 1.74.0） |
| 你链接时 | ld 读出目标库的 SONAME，抄进你的 ELF `DT_NEEDED` | 抄字符串，不抄路径 |
| 运行时 | ld.so 拿 NEEDED 字符串查 ldconfig 缓存 | 字符串 → 实际路径 |

好处：库文件可以改名、搬家，只要 SONAME 不变，老程序照跑；ABI 变了就换 SONAME（`.so.1.74.0` → `.so.1.75.0`），新老作为不同文件名共存，互不干扰。

### 17.3 ld.so 的搜索顺序

每个 NEEDED 条目走一遍：

| 顺序 | 来源 | 说明 |
|---|---|---|
| 1 | `DT_RPATH` | 写在 ELF 里，已废弃（有 RUNPATH 时被忽略） |
| 2 | `LD_LIBRARY_PATH` | 环境变量，setuid 程序忽略 |
| 3 | `DT_RUNPATH` | 现代默认 |
| 4 | `/etc/ld.so.cache` | `ldconfig` 扫描 `/etc/ld.so.conf*` 生成 |
| 5 | `/lib`、`/usr/lib`（+multiarch） | 内置兜底 |

实测缓存内容（同名两条，靠架构标记消歧）：

```bash
$ ldconfig -p | grep 'libc\.so\.6'
    libc.so.6 (libc6,x86-64) => /lib/x86_64-linux-gnu/libc.so.6   ← 64 位程序用这条
    libc.so.6 (libc6)        => /lib32/libc.so.6                  ← 32 位程序用这条
```

ld.so 不做任何版本比较：NEEDED 是 `libssl.so.1.1` 就只找这个字符串的文件，系统里同时有 `libssl.so.3` 也不看——这就是多版本共存的原理。

调试工具：`LD_DEBUG=libs ./prog` 打印每个库的查找过程；`LD_DEBUG=bindings ./prog` 打印每个符号绑到哪个库。

---

## 18. macOS 对比：dyld、install_name 与头库劈叉

### 18.1 机制差异总表

| | Linux | macOS |
|---|---|---|
| 库的自报名字 | SONAME：`libboost_regex.so.1.74.0`（绝对文件名） | install_name：`@rpath/libc++.1.0.dylib`（**占位符**） |
| 程序里记录的 | `DT_NEEDED` | `LC_LOAD_DYLIB` |
| 运行期解析者 | ld.so | dyld |
| 运行期顺序 | RPATH → env → ldconfig 缓存 → /lib, /usr/lib | LC_RPATH → env → /usr/local/lib → shared cache |
| 名字形态 | 绝对文件名，查缓存即得 | 含 `@rpath/@executable_path/@loader_path` 占位符，运行时解析 |
| 系统库位置 | /lib /usr/lib 的物理文件 | **磁盘上没有物理文件**（10.15 起在 dyld shared cache；`ls /usr/lib/libc++.dylib` 不存在，但 `-lc++` 照链，ld 对 cache 特判） |
| 验证命令 | `objdump -p` / `readelf -d` / `ldd` / `LD_DEBUG=libs` | `otool -D` / `otool -L` / `otool -l \| grep -A2 LC_RPATH` / `DYLD_PRINT_LIBRARIES=1 ./prog` |

`@rpath` 机制的含义：运行路径不由库自己决定，而是**外包给使用者的 rpath**。这就是 mac 上光链对还不够、必须显式写 rpath 的根源。

### 18.2 劈叉案例：brew clang 的头库版本错位（LLVM 21 真实环境）

clang Darwin driver 有一条硬规则：**如果编译器自己旁边（`<prefix>/include/c++/v1`）有 libc++ 头文件，就用它，不看 SDK**（LLVM 这么设计是为了支持自带全套头文件的独立工具链）。于是：

```
/usr/bin/clang++（Apple）
  自己的 <prefix>/include/c++/v1  → 不存在
  → 回退到 SDK 的 usr/include/c++/v1（Apple libc++ 头文件）
  → -lc++ 命中系统 Apple libc++ 库
  → 头、库同源，自洽 ✅

/opt/homebrew/opt/llvm/bin/clang++（brew，LLVM 21）
  自己的 <prefix>/include/c++/v1  → 存在！LLVM 21（有 C++23 新 API 声明）
  → -lc++ 依然命中系统 Apple 旧 libc++
    （brew 故意把 libc++ 放在 lib/c++ 子目录，避开默认搜索；mac 的 ld 默认只搜 -L 和 /usr/lib）
  → 头新库旧 → undefined symbols ❌
```

与 Linux 对照：Linux 默认 stdlib 是 libstdc++，头文件（`/usr/include/c++/10`）和库（GCC 目录里的 `libstdc++.so`）天然同源，换 clang 默认**不劈叉**；mac 默认 stdlib 就是 libc++，而"头文件就近原则"和"库走系统默认路径"指向两个来源——**默认就裂**。

### 18.3 修复：两行 CMake，各修一侧

```cmake
if(APPLE)
    link_directories("/opt/homebrew/opt/llvm/lib/c++")       # = -L：链接期先命中 brew 的 libc++
    set(CMAKE_BUILD_RPATH "/opt/homebrew/opt/llvm/lib/c++")  # = -Wl,-rpath：写入 LC_RPATH
endif()
```

为什么缺一不可：brew libc++ 的 install_name 是 `@rpath/libc++.1.0.dylib`，这个占位符字符串会被抄进可执行文件的 `LC_LOAD_DYLIB`；运行时 dyld 拿可执行文件的 `LC_RPATH` 列表来解析 `@rpath`——**不写 rpath，程序直接起不来**：

```
dyld: Library not loaded: @rpath/libc++.1.0.dylib
```

第一行修"链接对错库"，第二行修"运行时找不到库"。Linux 的 SONAME 是绝对文件名、ldconfig 缓存直接解析，通常不用补 rpath——两个平台体验差异的根源在此。

### 18.4 mac 上用 brew clang 时四层各归谁

| 层 | 谁提供 | 原因 |
|---|---|---|
| builtin 头文件（`stddef.h`/intrinsics） | brew 自己的 | resource dir 铁律，随编译器二进制走 |
| libc++ 头文件 | brew 自己的 | Darwin driver 就近规则命中自带目录 |
| libc++ 库（`-lc++`） | 系统的（需手动对齐） | 默认 -L 先命中 /usr/lib shared cache |
| C 头文件（`stdio.h`）、libSystem、链接器 ld | 系统/SDK 的 | mac 系统头文件只存在于 SDK，ld 默认调 `/usr/bin/ld` |

---

## 19. 全局总结：一个字符串匹配的世界

整个 C++ 链接体系没有"智能选择"，只有三层精确字符串匹配：

| 层 | 匹配什么 | 候选集由谁给 |
|---|---|---|
| 头文件 | `#include` 的路径字符串 | clang driver 生成的优先级列表（stdlib 组 + resource dir + 系统目录），命中即停 |
| 链接期 | `-l` → 文件名 `libfoo.so`；undefined 符号名 | 程序员给的 `-L` 列表与 `-l` 列表 |
| 运行期 | NEEDED / LC_LOAD_DYLIB 字符串 → 实际路径 | ld.so / dyld 按固定顺序查 |

"用了什么功能 → 该链哪个库"只存在于程序员的脑子里和构建系统的配置表里。版本也不是"选"出来的：链接时由符号链接和 SONAME 钉死，运行时只有查表，没有决策。
