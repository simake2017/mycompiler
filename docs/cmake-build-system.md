# CMake 构建系统详解

本文档解释 minicc 项目的构建过程：CMake、Make、Ninja 三者的关系，以及 `make` 如何自动追踪 `CMakeLists.txt` 的变更。

---

## 目录

- [核心问题：make 会读 CMakeLists.txt 吗？](#核心问题make-会读-cmakeliststxt-吗)
- [构建全流程](#构建全流程)
- [CMakeLists.txt 每一行的实际效果](#cmakeliststxt-每一行的实际效果)
- [为什么注释 link_directories 就编译失败](#为什么注释-link_directories-就编译失败)
- [CMake 生成的文件清单](#cmake-生成的文件清单)
- [make 如何自动检测 CMakeLists.txt 变更](#make-如何自动检测-cmakeliststxt-变更)
- [make vs ninja 对比](#make-vs-ninja-对比)
- [实际编译和链接命令](#实际编译和链接命令)

---

## 核心问题：make 会读 CMakeLists.txt 吗？

**不会直接读。** 但 CMake 在生成的 Makefile 里埋了一个"检查点"机制：

```
你改了 CMakeLists.txt
     ↓
你执行 make
     ↓
make 第一个目标是 all
     ↓
all 依赖 cmake_check_build_system
     ↓
cmake_check_build_system 运行:
  cmake --check-build-system CMakeFiles/Makefile.cmake 0
     ↓
cmake 检查 ../CMakeLists.txt 的修改时间
     ↓
发现比 Makefile 新 → 自动重新跑 cmake 配置
     ↓
重新生成 Makefile（你的改动已生效）
     ↓
make 用新 Makefile 编译/链接
```

所以你不需要每次手动 `cmake ..`，CMake 的依赖追踪帮你做了。

---

## 构建全流程

```
项目根目录: mycompiler/
  └── CMakeLists.txt          ← 你写的构建配置

第一步: cmake 配置（只需一次，或 CMakeLists.txt 改动后自动触发）
  cd build
  cmake ..                     ← '..' 告诉 cmake: CMakeLists.txt 在上一级
  cmake 读取 CMakeLists.txt，生成:
    build/Makefile             ← make 用的构建规则
    build/CMakeCache.txt       ← 缓存所有配置变量
    build/CMakeFiles/          ← 编译/链接的具体命令
    build/compile_commands.json

第二步: make 构建
  make                         ← 读取 build/Makefile，编译+链接
  产出: build/minicc
```

### 关键理解

```
CMakeLists.txt   →   cmake 翻译   →   Makefile
 (你写的配置)                        (make 实际读的)

你改 CMakeLists.txt → make 自动检测到 → 自动重新跑 cmake → 重新生成 Makefile
```

`make` 只认 `Makefile`，不认 `CMakeLists.txt`。但 CMake 在 `Makefile` 里设了一个"哨兵"目标 `cmake_check_build_system`，每次 `make` 都先检查 `CMakeLists.txt` 有没有被改过。

---

## CMakeLists.txt 每一行的实际效果

### 你的 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(MiniCppCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    set(CXX_STDLIB_PATH "/opt/homebrew/opt/llvm/include/c++/v1")
    if(EXISTS ${CXX_STDLIB_PATH})
        include_directories(SYSTEM ${CXX_STDLIB_PATH})
        link_directories("/opt/homebrew/opt/llvm/lib/c++")
    endif()
endif()

set(SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/type.cpp
    src/semantic_analyzer.cpp
    src/template_instantiation.cpp
    src/codegen.cpp
    src/main.cpp
)

add_executable(minicc ${SOURCES})
target_include_directories(minicc PRIVATE include)
target_compile_options(minicc PRIVATE -Wall -Wextra -pedantic)
```

### 每行对应到实际的编译/链接命令

| CMakeLists.txt 写的 | 做什么 | 变成什么 |
|---|---|---|
| `set(CMAKE_CXX_STANDARD 20)` | C++ 版本 | `-std=gnu++20` |
| `include_directories(SYSTEM ...)` | 编译时头文件搜索路径 | `-isystem /opt/homebrew/opt/llvm/include/c++/v1` |
| `target_include_directories(minicc PRIVATE include)` | 项目头文件路径 | `-I.../mycompiler/include` |
| `link_directories("...")` | **链接时**库文件搜索路径 | `-L/opt/homebrew/opt/llvm/lib/c++` |
| `target_compile_options(...)` | 编译警告选项 | `-Wall -Wextra -pedantic` |
| `add_executable(minicc ${SOURCES})` | 编译哪些源文件、输出什么 | 所有 `.cpp` → `.o` → `minicc` |

### 两条命令的区别

```
include_directories  →  编译阶段  →  #include <xxx> 去哪找头文件
link_directories     →  链接阶段  →  -lxxx 去哪找库文件
```

---

## 为什么注释 link_directories 就编译失败

### 你机器上有两套 C++ 环境

```
① Homebrew LLVM 21（你自己装的）
   编译器:  /opt/homebrew/opt/llvm/bin/clang++      版本 21.1.8
   头文件:  /opt/homebrew/opt/llvm/include/c++/v1
   库文件:  /opt/homebrew/opt/llvm/lib/c++/libc++.dylib

② Apple 系统自带
   编译器:  /usr/bin/clang++                        版本 17.0.0
   库文件:  /usr/lib/libc++.dylib                   ← 旧版
```

### 有 link_directories 时（正常）

```
编译: clang++ (LLVM 21) + LLVM 21 的头文件 → .o    ✅ 匹配
链接: clang++ (LLVM 21) + LLVM 21 的 libc++ → minicc  ✅ 匹配
```

### 注释掉 link_directories 后（失败）

```
编译: clang++ (LLVM 21) + LLVM 21 的头文件 → .o    ✅ 匹配
链接: clang++ (LLVM 21) + 系统的旧版 libc++ → 失败  ❌ ABI 不匹配！

  原因: LLVM 21 的头文件里用了一些新符号
        (比如 std::__1::__hash_memory)
        但 Apple Clang 17 的旧 libc++ 里没有这个函数
        → Undefined symbols → 链接失败
```

### 一句话

**头文件用谁家的，库文件也得用谁家的。`include_directories` 和 `link_directories` 必须配套。**

---

## CMake 生成的文件清单

```
build/
├── Makefile                        ← make 的入口文件
├── CMakeCache.txt                  ← 所有配置变量的缓存
├── compile_commands.json           ← 完整编译命令（IDE 用）
├── cmake_install.cmake             ← install 规则
│
├── CMakeFiles/
│   ├── Makefile.cmake              ← 追踪 CMakeLists.txt 依赖
│   │                                 （记录 "../CMakeLists.txt" 是依赖源）
│   │
│   ├── Makefile2                   ← 二级 Makefile（递归目标）
│   │
│   └── minicc.dir/
│       ├── build.make              ← 每个 .cpp 的编译命令 + 链接命令
│       ├── flags.make              ← 编译选项（-I, -std, -Wall...）
│       ├── depend.make             ← 头文件依赖
│       ├── link.txt                ← 最终的链接命令（一行）
│       ├── compiler_depend.ts      ← 编译器依赖时间戳
│       └── src/
│           ├── lexer.cpp.o         ← 编译产物
│           ├── parser.cpp.o
│           ├── ...
│           └── main.cpp.o
│
└── minicc                          ← 最终产物：编译器可执行文件
```

### 各文件的职责

| 文件 | 谁生成的 | 做什么 |
|------|---------|--------|
| `Makefile` | cmake | make 的入口，定义 all/clean/minicc 等目标 |
| `Makefile2` | cmake | 二级调度，递归调用 build.make |
| `minicc.dir/build.make` | cmake | 每个 .cpp → .o 的具体编译命令 |
| `minicc.dir/flags.make` | cmake | 编译选项（从 CMakeLists.txt 翻译来） |
| `minicc.dir/link.txt` | cmake | 最终链接命令（一行，包含所有 .o 和 -L） |
| `Makefile.cmake` | cmake | 记录 CMakeLists.txt 是依赖源 |
| `CMakeCache.txt` | cmake | 缓存编译器路径、构建类型等变量 |

---

## make 如何自动检测 CMakeLists.txt 变更

### 机制：Makefile 里的"哨兵"目标

CMake 在生成的 `Makefile` 中写了这个规则：

```makefile
# 每次 make 都先跑这个
all: cmake_check_build_system
    $(MAKE) -f CMakeFiles/Makefile2 all

# 检查构建系统是否需要重新生成
cmake_check_build_system:
    $(CMAKE_COMMAND) -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR) \
        --check-build-system CMakeFiles/Makefile.cmake 0
```

### 这条命令做了什么

```
cmake -S.. -B. --check-build-system CMakeFiles/Makefile.cmake 0

  -S..            源码目录（CMakeLists.txt 在这里）
  -B.             构建目录（Makefile 在这里）
  --check-build-system
                  检查 Makefile.cmake 中记录的所有依赖文件
                  的修改时间，如果有任何一个比 Makefile 新，
                  就重新运行 cmake 配置

Makefile.cmake 里记录:
  "../CMakeLists.txt"    ← 这个文件被追踪为依赖
```

### 完整链路图

```
你执行 make
    │
    ▼
Makefile 的第一个目标是 all
    │
    ▼
all 依赖 cmake_check_build_system
    │
    ▼
cmake --check-build-system 检查 ../CMakeLists.txt 时间戳
    │
    ├── CMakeLists.txt 没改 → 什么都不做，继续编译
    │
    └── CMakeLists.txt 改了 → 自动重新跑 cmake 配置
        │
        ├── 重新生成 Makefile
        ├── 重新生成 flags.make（编译选项可能变了）
        ├── 重新生成 link.txt（链接选项可能变了）
        └── 然后继续用新 Makefile 编译/链接
```

---

## make vs ninja 对比

### 你的项目里两套构建目录

```
build/               ← 手动创建，用 make（Unix Makefiles 生成器）
cmake-build-debug/   ← CLion 自动创建，用 ninja（Ninja 生成器）
```

### 使用方式几乎一样

```bash
# 用 make
mkdir build && cd build
cmake ..                        # 生成 Makefile
make -j$(sysctl -n hw.ncpu)     # 编译

# 用 ninja
mkdir build && cd build
cmake .. -G Ninja               # 生成 build.ninja（注意 -G Ninja）
ninja                            # 编译
```

### 详细对比

| | make | ninja |
|---|---|---|
| **生成器** | `Unix Makefiles`（默认） | `Ninja`（需要 `-G Ninja`） |
| **生成文件** | `Makefile` + `CMakeFiles/` | `build.ninja` + `.ninja_deps` |
| **可读性** | Makefile 人能看懂 | build.ninja 是纯机器格式，很难读 |
| **速度** | 较慢（大项目明显） | 更快（专为速度设计，C++ 写的） |
| **并行编译** | 手动加 `-j8` | 默认就并行（自动检测 CPU 核心数） |
| **手写规则** | 支持（Makefile 可以手写） | 不支持（只接受工具生成） |
| **增量编译** | 基于文件时间戳 | 基于文件时间戳 + 内容哈希 |
| **谁在用** | 你手动 `build/` 目录 | CLion 的 `cmake-build-debug/` |

### 选哪个？

- **小项目**（如 minicc）：随便，感知不到差异
- **大项目**（如 Chromium、LLVM）：用 ninja，快很多
- **CLion 用户**：CLion 默认用 ninja，不用管
- **想调试构建过程**：用 make，Makefile 能看懂

### 核心结论

**因为你用 CMake，用哪个都一样。** CMake 帮你生成对应的构建文件，你只管 `cmake ..` 然后 `make` 或 `ninja` 就行。CMakeLists.txt 只写一份，两种生成器都能用。

---

## 实际编译和链接命令

### 编译命令（每个 .cpp 文件）

以 `lexer.cpp` 为例：

```bash
/opt/homebrew/opt/llvm/bin/clang++        \   ← 编译器: Homebrew LLVM 21
    -I.../mycompiler/include              \   ← target_include_directories
    -isystem .../llvm/include/c++/v1      \   ← include_directories(SYSTEM ...)
    -g -arch arm64                        \   ← Debug 模式 + ARM64
    -Wall -Wextra -pedantic               \   ← target_compile_options
    -std=gnu++20                          \   ← CMAKE_CXX_STANDARD 20
    -c src/lexer.cpp                      \   ← 编译（不链接）
    -o CMakeFiles/minicc.dir/src/lexer.cpp.o  ← 输出 .o 文件
```

### 链接命令（所有 .o → minicc）

```bash
/opt/homebrew/opt/llvm/bin/clang++        \   ← 编译器（也做链接器）
    -g -arch arm64                        \
    -L/opt/homebrew/opt/llvm/lib          \   ← Homebrew LLVM 的库路径
    CMakeFiles/minicc.dir/src/lexer.cpp.o \   ← 所有 .o 文件
    CMakeFiles/minicc.dir/src/parser.cpp.o    \
    CMakeFiles/minicc.dir/src/type.cpp.o      \
    CMakeFiles/minicc.dir/src/semantic_analyzer.cpp.o \
    CMakeFiles/minicc.dir/src/template_instantiation.cpp.o \
    CMakeFiles/minicc.dir/src/codegen.cpp.o   \
    CMakeFiles/minicc.dir/src/main.cpp.o      \
    -o minicc                             \   ← 输出可执行文件
    -L/opt/homebrew/opt/llvm/lib/c++          ← link_directories() 的效果！
```

注意最后一行 `-L/opt/homebrew/opt/llvm/lib/c++`——这就是 `link_directories()` 在 CMakeLists.txt 中的效果。注释掉它，这行就消失，链接器就找不到 LLVM 21 的 libc++，导致链接失败。
