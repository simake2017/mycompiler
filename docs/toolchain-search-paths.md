# Linux 工具链搜索路径完全指南（Clang / ld / ld.so）

本文记录 minicc 项目遇到的 `fatal error: 'format' file not found` 问题的完整始末，
并以此为切入点，讲清 Linux 下一个 C++ 程序从**预处理 → 编译 → 链接 → 运行**四个阶段
各自"去哪里找文件"，所有数据均为本机（Debian, Clang 18.1.8, x86-64）实测。

---

## 目录

- [一、问题始末：`<format>` 为什么找不到](#一问题始末format-为什么找不到)
- [二、阶段 1：预处理/编译 —— 头文件搜索路径](#二阶段-1预处理编译--头文件搜索路径)
- [三、阶段 2：编译 —— Clang 如何挑选 GCC 工具链](#三阶段-2编译--clang-如何挑选-gcc-工具链)
- [四、阶段 3：链接 —— 库文件搜索路径](#四阶段-3链接--库文件搜索路径)
- [五、阶段 4：运行 —— 动态库解析（ld.so）](#五阶段-4运行--动态库解析ldso)
- [六、CMake 中的修复](#六cmake-中的修复)
- [七、常用排查命令速查](#七常用排查命令速查)

---

## 一、问题始末：`<format>` 为什么找不到

### 现象

```text
src/type.cpp:6:10: fatal error: 'format' file not found
    6 | #include <format>
```

`lexer.cpp`、`parser.cpp`、`codegen.cpp` 等全部源文件同时报同一错误。

### 根因链条

| # | 事实 | 说明 |
|---|------|------|
| 1 | 编译器是 Clang 18.1.8 | 本身完全支持 C++20 |
| 2 | Clang **不自带** C++ 标准库 | Linux 上默认借用 GCC 的 libstdc++ |
| 3 | 系统唯一的 libstdc++ 来自 GCC 10.2.1 | Debian 11 自带 |
| 4 | `<format>` 需要 **GCC 13+** 的 libstdc++ | GCC 10 的标准库里根本没有这个头文件 |
| 5 | Debian 的 clang 包**不依赖** libc++ | `libc++-18-dev` 是独立包，默认不装 |

结论：不是代码问题，是**标准库版本太老 + 备选标准库未安装**。

### 修复

1. 安装 LLVM 自家标准库（头文件 + 静态库）：

   ```bash
   sudo apt install libc++-18-dev libc++abi-18-dev
   ```

   安装后头文件落在 `/usr/lib/llvm-18/include/c++/v1/`（`format`、`__format/` 都在其中）。

2. CMakeLists.txt 增加 Linux + Clang 分支（见[第六节](#六cmake-中的修复)）。

3. 重新 `cmake .. && make`，构建通过。

### 另一个容易踩的坑：语言标准

独立命令行直接编译时，Clang 18 默认标准是 **gnu++17**，而 `std::format` 是 C++20 特性：

```bash
$ clang++ -stdlib=libc++ fmt_probe.cpp
error: no member named 'format' in namespace 'std'   # 头文件找到了，但特性被标准版本关掉

$ clang++ -std=c++20 -stdlib=libc++ fmt_probe.cpp    # OK
```

所以完整开关是两个：`-std=c++20`（开特性）+ `-stdlib=libc++`（换标准库）。
项目里前者由 `set(CMAKE_CXX_STANDARD 20)` 提供。

---

## 二、阶段 1：预处理/编译 —— 头文件搜索路径

### 搜索顺序（`#include <...>`）

| 优先级 | 来源 |
|--------|------|
| 1 | `#include "x"`：当前文件所在目录（仅双引号形式） |
| 2 | `-I` 目录，按命令行顺序 |
| 3 | `-isystem` 目录（CMake 的 `include_directories(SYSTEM ...)`） |
| 4 | **标准库 C++ 头**（libstdc++ 或 libc++，见下） |
| 5 | Clang 内置头（`stddef.h`、`stdarg.h`、`__clang_hip_libdevice_declares.h` 等） |
| 6 | 系统默认：`/usr/local/include` → multiarch → `/usr/include` |

先命中谁就用谁，因此标准库头排在系统 C 头之前。

### 实测 A：默认（libstdc++）

`clang++ -E -x c++ -v /dev/null`：

```text
#include <...> search starts here:
 /usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../include/c++/10
 /usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../include/x86_64-linux-gnu/c++/10
 /usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../include/c++/10/backward
 /usr/lib/llvm-18/lib/clang/18/include
 /usr/local/include
 /usr/include/x86_64-linux-gnu
 /usr/include
End of search list.
```

归一化后前三条就是 `/usr/include/c++/10{,/<triple>,/backward}` —— GCC 10 的 libstdc++，
**没有 `<format>`，报错就从这里来**。

### 实测 B：`-stdlib=libc++`

```text
#include <...> search starts here:
 /usr/lib/llvm-18/bin/../include/c++/v1
 /usr/lib/llvm-18/lib/clang/18/include
 /usr/local/include
 /usr/include/x86_64-linux-gnu
 /usr/include
End of search list.
```

三条 libstdc++ 路径被整组替换成一条 `c++/v1`（`libc++-18-dev` 提供）。

---

## 三、阶段 2：编译 —— Clang 如何挑选 GCC 工具链

Clang 自身不带 libstdc++ 和 CRT 启动文件，启动时会做一次 **GCC 探测**。
`clang++ -v` 开头就会打印探测结果（本机实测）：

```text
Found candidate GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/10
Selected GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/10
Candidate multilib: .;@m64
Selected multilib: .;@m64
```

### 挑选规则（按优先级）

1. `--gcc-toolchain=<dir>` 显式指定
2. `--gcc-install-dir=<dir>`（Clang 16+，直接指到版本目录）
3. 自动扫描候选目录：`/usr/lib/gcc/$triple/`、`/usr/lib/gcc/`、PATH 中 gcc 的位置
4. 候选里取**最高版本**

本机 `/usr/lib/gcc/x86_64-linux-gnu/` 下只有 `10`，所以"没得选"——
这就是 Clang 18 配 GCC 10 的原因：不是版本匹配，是唯一候选。

### 探测结果的三个用途

| 用途 | 落点 |
|------|------|
| libstdc++ 头文件 | `$GCC/include/c++/10`（第二节的第 4 优先级） |
| CRT 启动文件 | `$GCC/crtbeginS.o`、`$GCC/crtendS.o`（第四节链接用） |
| GCC 运行时库 | `libgcc.a` / `libgcc_s.so` 所在目录（进 `-L`） |

> 即使加了 `-stdlib=libc++`，GCC 探测**仍然进行**——CRT 文件和 libgcc 还要从那里取。

---

## 四、阶段 3：链接 —— 库文件搜索路径

`clang++` 是驱动，编译出 `.o` 后拼一条 `ld` 命令交给 GNU ld。
实测（`clang++ -std=c++20 -stdlib=libc++ -v fmt_probe.cpp -o fmt_probe`）提取的链接要素：

### 链接输入（按 ld 命令行顺序）

```text
/lib64/ld-linux-x86-64.so.2              ← 指定的动态链接器（写进 ELF 的 PT_INTERP）
/lib/x86_64-linux-gnu/Scrt1.o            ← libc 的 _start 入口（PIE 版）
/lib/x86_64-linux-gnu/crti.o             ← 全局构造/析构 prologue
/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o   ← 来自 GCC 探测结果
-L/usr/bin/../lib/gcc/x86_64-linux-gnu/10      ← ┐
-L.../lib64                                    │
-L/lib/x86_64-linux-gnu                        │
-L/lib/../lib64                                ├ -L 库搜索目录（按此顺序）
-L/usr/lib/x86_64-linux-gnu                    │
-L/usr/lib/../lib64                            │
-L/usr/lib/llvm-18/bin/../lib                  │
-L/lib                                         │
-L/usr/lib                                     ← ┘
-lc++ -lm -lgcc_s -lgcc -lc -lgcc_s -lgcc      ← 要链接的库（顺序敏感）
/usr/lib/gcc/x86_64-linux-gnu/10/crtendS.o     ← 来自 GCC 探测结果
/lib/x86_64-linux-gnu/crtn.o                   ← epilogue
```

### `-L` 与 `-l` 的解析规则

- `-lc++` → 在 `-L` 目录里**按顺序**找 `libc++.so`，找不到再找 `libc++.a`
- `-L` 只对命令行中**排在它后面**的 `-l` 生效
- 库顺序敏感：被依赖者排后面（`-lc` 几乎总是最后）

### ld 的内置兜底目录

`-L` 全部落空后，ld 还有编译进二进制的默认路径（`ld --verbose | grep SEARCH_DIR`，实测）：

```text
SEARCH_DIR("=/usr/local/lib/x86_64-linux-gnu")
SEARCH_DIR("=/lib/x86_64-linux-gnu")
SEARCH_DIR("=/usr/lib/x86_64-linux-gnu")
SEARCH_DIR("=/usr/lib/x86_64-linux-gnu64")
SEARCH_DIR("=/usr/local/lib64")
SEARCH_DIR("=/lib64")
SEARCH_DIR("=/usr/lib64")
SEARCH_DIR("=/usr/local/lib")
SEARCH_DIR("=/lib")
SEARCH_DIR("=/usr/lib")
SEARCH_DIR("=/usr/x86_64-linux-gnu/lib64")
SEARCH_DIR("=/usr/x86_64-linux-gnu/lib")
```

（开头的 `=` 表示相对于 `--sysroot`，默认即 `/`。）

### Clang 驱动自身的搜索目录

`clang++ -print-search-dirs`（实测）：

```text
programs: =/usr/bin:/usr/lib/llvm-18/bin:/usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../x86_64-linux-gnu/bin
libraries: =/usr/lib/llvm-18/lib/clang/18:/usr/bin/../lib/gcc/x86_64-linux-gnu/10:
           =/usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../lib64:/lib/x86_64-linux-gnu:
           /lib/../lib64:/usr/lib/x86_64-linux-gnu:/usr/lib/../lib64:
           /usr/lib/llvm-18/bin/../lib:/lib:/usr/lib
```

---

## 五、阶段 4：运行 —— 动态库解析（ld.so）

链接期只把 `-lc++` 记成 ELF 的 `DT_NEEDED`（**只记 soname，不记路径**），
运行时由动态链接器 `/lib64/ld-linux-x86-64.so.2` 重新解析。

### 实测：minicc 的 NEEDED 项

`readelf -d cmake-build-debug/minicc`：

```text
 (NEEDED)  Shared library: [libc++.so.1]
 (NEEDED)  Shared library: [libc++abi.so.1]
 (NEEDED)  Shared library: [libunwind.so.1]
 (NEEDED)  Shared library: [libm.so.6]
 (NEEDED)  Shared library: [libgcc_s.so.1]
 (NEEDED)  Shared library: [libc.so.6]
```

### 解析顺序

| 优先级 | 来源 |
|--------|------|
| 1 | `LD_LIBRARY_PATH`（setuid 程序忽略） |
| 2 | ELF 的 `DT_RUNPATH` / `DT_RPATH`（本项目无，见下） |
| 3 | `/etc/ld.so.cache`（由 `ldconfig` 从下面的配置生成） |
| 4 | 默认目录 `/lib`、`/usr/lib` |

本机的 cache 来源（`/etc/ld.so.conf` + `/etc/ld.so.conf.d/*.conf`，实测）：

```text
/usr/lib/x86_64-linux-gnu/libfakeroot
/usr/local/lib
/usr/local/lib/x86_64-linux-gnu
/lib/x86_64-linux-gnu
/usr/lib/x86_64-linux-gnu
/lib32
/usr/lib32
```

### 实测：minicc 的最终解析结果

`ldd cmake-build-debug/minicc`：

```text
linux-vdso.so.1 (0x00007ffdf8d21000)
libc++.so.1     => /lib/x86_64-linux-gnu/libc++.so.1
libc++abi.so.1  => /lib/x86_64-linux-gnu/libc++abi.so.1
libunwind.so.1  => /lib/x86_64-linux-gnu/libunwind.so.1
libm.so.6       => /lib/x86_64-linux-gnu/libm.so.6
libgcc_s.so.1   => /lib/x86_64-linux-gnu/libgcc_s.so.1
libc.so.6       => /lib/x86_64-linux-gnu/libc.so.6
libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0
libdl.so.2      => /lib/x86_64-linux-gnu/libdl.so.2
/lib64/ld-linux-x86-64.so.2
```

全部命中 multiarch 目录，无 RUNPATH——所以**换了标准库记得 `ldconfig` 不用管
（apt 装包会自动跑），但手动装 .so 到 `/usr/local/lib` 后必须跑一次**。

---

## 六、CMake 中的修复

本项目 CMakeLists.txt 现状（与原有 macOS 分支并列）：

```cmake
set(CMAKE_CXX_STANDARD 20)          # 开关 1：语言标准（std::format 需要）
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linux + Clang: 系统 libstdc++ (GCC 10) 无 <format>，改用 libc++
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++)   # 开关 2：编译期换头文件
    add_link_options(-stdlib=libc++)      # 开关 3：链接期换 -lc++
endif()
```

要点：

- `-stdlib=libc++` 必须编译、链接**两边都加**，否则链接期仍去找 `libstdc++.so`
- 条件里带 `NOT APPLE` 是因为 macOS 分支已单独处理（Homebrew llvm 自带 `c++/v1`）
- 用 GCC 编译 Linux 版则不受影响（前提是 GCC ≥ 13）

---

## 七、常用排查命令速查

| 目的 | 命令 |
|------|------|
| 看头文件搜索路径 | `clang++ -E -x c++ -v /dev/null` |
| 看完整编译/链接命令 | `clang++ -v a.cpp -o a`（编译与 ld 命令行全打印） |
| 看 GCC 探测结果 | 同上，输出开头的 `Selected GCC installation` |
| 看驱动级搜索目录 | `clang++ -print-search-dirs` |
| 看 ld 内置 SEARCH_DIR | `ld --verbose \| grep SEARCH_DIR` |
| 看 ELF 依赖（静态视角） | `readelf -d minicc \| grep -E "NEEDED\|RUNPATH"` |
| 看运行时解析（动态视角） | `ldd minicc` |
| 看运行时配置 | `cat /etc/ld.so.conf.d/*.conf` |
| 重建运行时缓存 | `sudo ldconfig` |
| 查某个头属于哪个包 | `dpkg -S /usr/include/c++/v1/format` |

配套自检脚本见 [`scripts/check-toolchain.sh`](../scripts/check-toolchain.sh)。
