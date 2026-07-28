# Make 完全教程

本教程从零开始讲解 Make，结合 minicc 项目的实际构建过程。

---

## 目录

- [第一章：Make 是什么](#第一章make-是什么)
- [第二章：Makefile 基本语法](#第二章makefile-基本语法)
- [第三章：变量](#第三章变量)
- [第四章：自动变量](#第四章自动变量)
- [第五章：模式规则](#第五章模式规则)
- [第六章：内置函数](#第六章内置函数)
- [第七章：条件判断](#第七章条件判断)
- [第八章：phony 目标](#第八章phony-目标)
- [第九章：依赖与增量编译](#第九章依赖与增量编译)
- [第十章：多目录项目](#第十章多目录项目)
- [第十一章：实战——手写一个 minicc 的 Makefile](#第十一章实战手写一个-minicc-的-makefile)
- [第十二章：make 命令行技巧](#第十二章make-命令行技巧)
- [速查表](#速查表)

---

## 第一章：Make 是什么

Make 是一个**构建自动化工具**。你告诉它：

- **目标**（target）：你想得到什么（比如 `minicc` 可执行文件）
- **依赖**（dependencies）：得到它需要什么（比如 `.o` 文件）
- **命令**（recipe）：怎么从依赖得到目标（比如 `clang++ -o minicc *.o`）

Make 会**自动判断哪些文件需要重新编译**——只重建过期的部分，不浪费时间去碰没改过的文件。

### 为什么需要 Make？

```
没有 Make 的世界：
  每次都要手动敲：
    clang++ -std=c++20 -Iinclude -c src/lexer.cpp -o build/lexer.o
    clang++ -std=c++20 -Iinclude -c src/parser.cpp -o build/parser.o
    clang++ -std=c++20 -Iinclude -c src/type.cpp -o build/type.o
    clang++ -std=c++20 -Iinclude -c src/semantic_analyzer.cpp -o build/sema.o
    clang++ -std=c++20 -Iinclude -c src/template_instantiation.cpp -o build/tmpl.o
    clang++ -std=c++20 -Iinclude -c src/codegen.cpp -o build/codegen.o
    clang++ -std=c++20 -Iinclude -c src/main.cpp -o build/main.o
    clang++ build/*.o -o build/minicc

  改了 lexer.cpp 一个文件，上面 8 条命令全部重跑一遍……

有了 Make：
  敲一个词: make
  Make 自动检测只改了 lexer.cpp，只编译那一个文件 + 重新链接
```

---

## 第二章：Makefile 基本语法

### 最小的 Makefile

```makefile
# 目标: 依赖
# ←Tab→命令

hello: hello.cpp
	clang++ hello.cpp -o hello
```

**注意：命令前必须是 Tab 字符，不能是空格！** 这是 Make 最经典的坑。

### 运行

```bash
make hello    # 构建 hello 目标
make          # 构建 Makefile 中的第一个目标
```

### 多个目标

```makefile
# 默认目标（make 不带参数时执行这个）
all: hello goodbye

hello: hello.cpp
	clang++ hello.cpp -o hello

goodbye: goodbye.cpp
	clang++ goodbye.cpp -o goodbye

clean:
	rm -f hello goodbye
```

```bash
make           # 构建 all → 构建 hello + goodbye
make hello     # 只构建 hello
make clean     # 删除编译产物
```

### 规则的结构

```
目标(target): 依赖(dependencies)
←Tab→命令(recipe)
←Tab→命令(recipe)
```

- **目标**：要生成的文件名，或者一个动作的名字（如 `clean`）
- **依赖**：目标依赖的文件列表（空格分隔）
- **命令**：如何从依赖生成目标的 shell 命令（每行一个，必须以 Tab 开头）

### 执行逻辑

```
make 执行一个目标时：
  1. 检查目标文件是否存在
  2. 如果不存在 → 执行命令
  3. 如果存在 → 比较目标文件和依赖文件的修改时间
     ├── 目标比所有依赖都新 → "已经是最新的"，不执行
     └── 任何一个依赖比目标新 → 执行命令（重新构建）
```

---

## 第三章：变量

### 定义和使用

```makefile
# 定义变量（= 或 :=）
CXX = clang++
CXXFLAGS = -std=c++20 -Wall
SOURCES = src/lexer.cpp src/parser.cpp src/main.cpp

# 使用变量 $(变量名) 或 ${变量名}
hello: hello.cpp
	$(CXX) $(CXXFLAGS) hello.cpp -o hello
```

### 两种赋值方式

```makefile
# =  延迟赋值：使用时才展开（可以引用后面定义的变量）
CXXFLAGS = -Wall $(OPTIMIZE)
OPTIMIZE = -O2
# 使用时 CXXFLAGS 展开为 -Wall -O2

# := 立即赋值：定义时就展开
CXXFLAGS := -Wall $(OPTIMIZE)
OPTIMIZE = -O2
# 使用时 CXXFLAGS 展开为 -Wall（因为定义时 OPTIMIZE 还没值）

# ?= 条件赋值：只在变量未定义时赋值
CXX ?= clang++
# 如果环境变量已经设了 CXX，就不覆盖

# += 追加
CXXFLAGS = -Wall
CXXFLAGS += -Wextra
# CXXFLAGS 现在是 -Wall -Wextra
```

### 实战：minicc 的变量

```makefile
CXX       = clang++
CXXFLAGS  = -std=c++20 -Wall -Wextra -pedantic -g
INCLUDES  = -Iinclude
LDFLAGS   = -L/opt/homebrew/opt/llvm/lib/c++

SOURCES   = src/lexer.cpp src/parser.cpp src/type.cpp \
            src/semantic_analyzer.cpp src/template_instantiation.cpp \
            src/codegen.cpp src/main.cpp
OBJECTS   = $(SOURCES:.cpp=.o)    # 把 .cpp 替换成 .o
TARGET    = minicc
```

### 环境变量

```bash
# Make 会自动读取环境变量
CXX=g++ make              # 用 g++ 而不是 clang++
CXXFLAGS="-O3" make       # 覆盖编译选项

# 也可以在 Makefile 中读取环境变量
ifdef DEBUG
    CXXFLAGS += -DDEBUG -g
endif
```

---

## 第四章：自动变量

Make 在命令执行时自动设置一些特殊变量：

| 变量 | 含义 | 例子 |
|------|------|------|
| `$@` | 目标文件名 | `minicc` |
| `$<` | 第一个依赖文件 | `src/main.cpp` |
| `$^` | 所有依赖文件（去重） | `a.o b.o c.o` |
| `$?` | 比目标新的依赖 | `a.o`（只有 a.o 改了） |
| `$*` | 匹配的模式部分 | 见模式规则 |

### 实际使用

```makefile
# 不用自动变量（啰嗦）
minicc: lexer.o parser.o main.o
	clang++ lexer.o parser.o main.o -o minicc

# 用 $@ 和 $^（简洁）
minicc: lexer.o parser.o main.o
	clang++ $^ -o $@
	# 展开为: clang++ lexer.o parser.o main.o -o minicc

# 用 $< 编译单个文件
lexer.o: src/lexer.cpp
	clang++ -c $< -o $@
	# 展开为: clang++ -c src/lexer.cpp -o lexer.o
```

---

## 第五章：模式规则

### 后缀规则（简单）

```makefile
# 所有 .cpp → .o 的通用规则
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

这条规则的含义：
- `%` 是通配符，匹配任意字符串
- `%.o: %.cpp` 表示：任何 `.o` 文件依赖同名的 `.cpp` 文件
- `$<` 是对应的 `.cpp` 文件
- `$@` 是要生成的 `.o` 文件

### 带路径的模式规则

```makefile
# build/ 目录下的 .o 依赖 src/ 目录下的 .cpp
build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

```bash
make build/lexer.o
# 匹配: build/lexer.o 依赖 src/lexer.cpp
# 执行: clang++ -std=c++20 -Wall -Iinclude -c src/lexer.cpp -o build/lexer.o
```

### 静态模式规则（限定范围）

```makefile
OBJECTS = build/lexer.o build/parser.o build/main.o

$(OBJECTS): build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

只对 `$(OBJECTS)` 列表中的文件应用这个规则。

---

## 第六章：内置函数

### 字符串替换

```makefile
SOURCES = src/lexer.cpp src/parser.cpp src/main.cpp

# $(变量:旧后缀=新后缀)
OBJECTS = $(SOURCES:.cpp=.o)
# 结果: src/lexer.o src/parser.o src/main.o

# $(patsubst 模式,替换,文本)
OBJECTS = $(patsubst src/%.cpp,build/%.o,$(SOURCES))
# 结果: build/lexer.o build/parser.o build/main.o
```

### 文件名操作

```makefile
SOURCES = src/lexer.cpp src/parser.cpp src/main.cpp

# $(wildcard 模式) —— 匹配文件
SRCS = $(wildcard src/*.cpp)
# 结果: src/lexer.cpp src/parser.cpp src/type.cpp ...

# $(notdir 路径) —— 去掉目录部分
$(notdir src/lexer.cpp)    # 结果: lexer.cpp

# $(dir 路径) —— 只保留目录部分
$(dir src/lexer.cpp)       # 结果: src/

# $(basename 文件名) —— 去掉后缀
$(basename src/lexer.cpp)  # 结果: src/lexer

# $(suffix 文件名) —— 只保留后缀
$(suffix src/lexer.cpp)    # 结果: .cpp
```

### 列表操作

```makefile
# $(sort 列表) —— 排序并去重
$(sort c b a c)            # 结果: a b c

# $(words 列表) —— 计数
$(words src/lexer.cpp src/parser.cpp)  # 结果: 2

# $(firstword 列表) —— 第一个
$(firstword a b c)         # 结果: a

# $(filter 模式,列表) —— 过滤
$(filter %.cpp,$(SOURCES)) # 只保留 .cpp 文件
```

### Shell 命令

```makefile
# $(shell 命令) —— 执行 shell 命令
CORES = $(shell sysctl -n hw.ncpu)
DATE  = $(shell date +%Y%m%d)
```

---

## 第七章：条件判断

```makefile
DEBUG = 1

# ifeq / ifneq —— 字符串比较
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -DDEBUG
else
    CXXFLAGS += -O2
endif

# ifdef / ifndef —— 变量是否定义
ifdef VERBOSE
    $(info 详细模式已开启)
endif

ifndef CXX
    CXX = clang++
endif
```

### 实战：跨平台

```makefile
UNAME = $(shell uname)

ifeq ($(UNAME), Darwin)
    # macOS
    LDFLAGS = -L/opt/homebrew/opt/llvm/lib/c++
    CXXFLAGS += -arch arm64
else ifeq ($(UNAME), Linux)
    # Linux
    LDFLAGS =
    CXXFLAGS +=
endif
```

---

## 第八章：phony 目标

### 问题

```makefile
clean:
	rm -f *.o minicc
```

如果当前目录恰好有个文件叫 `clean`，Make 会说：

```
make: 'clean' is up to date.
```

因为它以为 `clean` 是一个文件目标，而且这个"文件"没有依赖，所以是"最新的"。

### 解决

```makefile
.PHONY: clean all install

clean:
	rm -f *.o minicc

all: minicc

install: minicc
	cp minicc /usr/local/bin/
```

`.PHONY` 告诉 Make：这些目标不是文件，是动作，每次都执行。

### 常见的 phony 目标

```makefile
.PHONY: all clean install uninstall test help

all: minicc              # 默认目标
clean:                   # 清理编译产物
	rm -rf build/*.o build/minicc
install: minicc          # 安装
	cp minicc /usr/local/bin/
test: minicc             # 运行测试
	./run_tests.sh
help:                    # 显示帮助
	@echo "可用目标:"
	@echo "  make          - 编译 minicc"
	@echo "  make clean    - 清理"
	@echo "  make test     - 运行测试"
	@echo "  make install  - 安装"
```

---

## 第九章：依赖与增量编译

### Make 的核心逻辑

```
目标: 依赖1 依赖2 依赖3
	命令

Make 的判断流程:
  1. 目标文件不存在？ → 执行命令
  2. 目标文件存在，但依赖1比目标新？ → 执行命令
  3. 目标文件存在，所有依赖都比目标旧？ → 不执行（"已经是最新的"）
```

### 增量编译示例

```makefile
minicc: lexer.o parser.o main.o
	clang++ $^ -o $@

lexer.o: src/lexer.cpp include/lexer.h include/token.h
	clang++ -c $< -o $@

parser.o: src/parser.cpp include/parser.h include/ast.h
	clang++ -c $< -o $@

main.o: src/main.cpp include/lexer.h include/parser.h
	clang++ -c $< -o $@
```

```bash
# 第一次 make
make
# 编译 lexer.o parser.o main.o + 链接 minicc（全部重新编译）

# 只改了 src/lexer.cpp
make
# 只编译 lexer.o + 重新链接 minicc（parser.o main.o 不动）

# 只改了 include/token.h
make
# 只编译 lexer.o + 重新链接 minicc（因为 lexer.o 依赖 token.h）

# 什么都没改
make
# "make: 'minicc' is up to date."
```

### 自动生成头文件依赖

手动写每个 `.o` 依赖哪些 `.h` 太痛苦了。编译器可以帮你生成：

```makefile
# -MMD 让编译器自动生成 .d 依赖文件
CXXFLAGS = -std=c++20 -Wall -MMD -MP

%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 包含自动生成的依赖文件（- 表示文件不存在也不报错）
-include $(OBJECTS:.o=.d)
```

编译器生成的 `.d` 文件长这样：

```makefile
# build/lexer.d
build/lexer.o: src/lexer.cpp include/lexer.h include/token.h
```

这样改了任何头文件，Make 都能自动检测到。

---

## 第十章：多目录项目

### 目录结构

```
mycompiler/
├── Makefile
├── include/
│   ├── lexer.h
│   └── parser.h
├── src/
│   ├── lexer.cpp
│   ├── parser.cpp
│   └── main.cpp
└── build/         ← 编译产物放这里
```

### Makefile

```makefile
CXX      = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -g -MMD
INCLUDES = -Iinclude

SRCDIR   = src
BUILDDIR = build
TARGET   = $(BUILDDIR)/minicc

# 自动扫描所有 .cpp 文件
SRCS = $(wildcard $(SRCDIR)/*.cpp)
# src/lexer.cpp src/parser.cpp src/main.cpp

# 转换成 build/ 目录下的 .o 文件
OBJS = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
# build/lexer.o build/parser.o build/main.o

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# 编译（build 目录下的 .o 依赖 src 目录下的 .cpp）
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 创建 build 目录（| 表示"顺序依赖"，只保证目录存在）
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# 清理
clean:
	rm -rf $(BUILDDIR)

# 包含自动生成的依赖
-include $(OBJS:.o=.d)

.PHONY: all clean
```

### 顺序依赖（Order-Only Prerequisites）

```makefile
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
                                   ↑
                              这个是顺序依赖
```

`|` 后面的依赖只保证在命令执行前存在，**不参与时间戳比较**。

如果不加 `|`：每次 `build/` 目录的修改时间变了（比如新增了文件），所有 `.o` 都会重新编译——因为 `build/` 比 `.o` 新了。

---

## 第十一章：实战——手写一个 minicc 的 Makefile

不用 CMake，纯手写一个完整功能的 Makefile：

```makefile
# =============================================================================
# minicc 的 Makefile（纯手写版，不依赖 CMake）
# =============================================================================

# ─── 编译器设置 ───────────────────────────────────────────────────────────────
CXX       = /opt/homebrew/opt/llvm/bin/clang++
CXXFLAGS  = -std=c++20 -Wall -Wextra -pedantic -g -MMD -MP
INCLUDES  = -Iinclude -isystem /opt/homebrew/opt/llvm/include/c++/v1
LDFLAGS   = -L/opt/homebrew/opt/llvm/lib/c++

# ─── 目录设置 ─────────────────────────────────────────────────────────────────
SRCDIR    = src
INCDIR    = include
BUILDDIR  = build

# ─── 文件设置 ─────────────────────────────────────────────────────────────────
TARGET    = $(BUILDDIR)/minicc
SRCS      = $(wildcard $(SRCDIR)/*.cpp)
OBJS      = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS      = $(OBJS:.o=.d)

# ─── 默认目标 ─────────────────────────────────────────────────────────────────
all: $(TARGET)

# ─── 链接：所有 .o → minicc ──────────────────────────────────────────────────
$(TARGET): $(OBJS) | $(BUILDDIR)
	@echo "  LINK    $@"
	@$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# ─── 编译：每个 .cpp → .o ────────────────────────────────────────────────────
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@echo "  CXX     $< → $@"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ─── 创建构建目录 ─────────────────────────────────────────────────────────────
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# ─── 清理 ─────────────────────────────────────────────────────────────────────
clean:
	@echo "  CLEAN   $(BUILDDIR)/"
	@rm -rf $(BUILDDIR)

# ─── 运行测试 ─────────────────────────────────────────────────────────────────
test: $(TARGET)
	@echo "  TEST    running all template tests..."
	@for f in tests/test_tmpl_*.cpp; do \
	    echo "  RUN     $$f"; \
	    $(TARGET) "$$f" -o /dev/null 2>/dev/null && \
	        echo "  ✅      $$(basename $$f)" || \
	        echo "  ❌      $$(basename $$f)"; \
	done

# ─── 帮助 ─────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  minicc 构建系统"
	@echo "  ════════════════════════════════════"
	@echo "  make          编译 minicc 编译器"
	@echo "  make clean    清理编译产物"
	@echo "  make test     运行所有模板测试"
	@echo "  make help     显示本帮助"
	@echo ""

# ─── 包含自动生成的头文件依赖 ─────────────────────────────────────────────────
-include $(DEPS)

# ─── phony 目标 ───────────────────────────────────────────────────────────────
.PHONY: all clean test help
```

### 使用

```bash
make              # 编译
make test         # 跑测试
make clean        # 清理
make help         # 看帮助
make VERBOSE=1    # 显示完整命令（需要去掉 @ 前缀）
```

### 显示完整命令

上面用了 `@echo` 来美化输出。想看完整命令：

```bash
make -n           # 只打印命令，不执行（dry-run）
make V=1          # 需要自己在 Makefile 里支持（见下方）
```

支持 `V=1` 显示完整命令：

```makefile
# 在文件开头加:
ifeq ($(V),1)
    Q =
else
    Q = @
endif

# 然后把所有 @ 换成 $(Q)
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(Q)echo "  CXX     $< → $@"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

```bash
make         # 简洁输出: "  CXX     src/lexer.cpp → build/lexer.o"
make V=1     # 完整输出: "/opt/homebrew/opt/llvm/bin/clang++ -std=c++20 ..."
```

---

## 第十二章：make 命令行技巧

### 常用选项

```bash
make                  # 构建默认目标
make target           # 构建指定目标
make -j8              # 8 个并行任务（加速编译）
make -j$(sysctl -n hw.ncpu)  # 自动检测 CPU 核心数

make -n               # dry-run：只打印命令，不执行
make -B               # 强制重建所有目标（忽略时间戳）
make -k               # 遇到错误继续（尽量多编译）

make -C build         # 切换到 build/ 目录执行 make
make -f MyMakefile    # 使用指定的 Makefile（默认用 Makefile 或 makefile）

make VAR=value        # 覆盖变量
make CXX=g++          # 用 g++ 编译
make CXXFLAGS="-O3"   # 覆盖编译选项
```

### 调试 Makefile

```bash
# 查看变量的值
make -p 2>/dev/null | grep "^CXXFLAGS"

# 打印所有变量
make -p

# 查看 make 会执行什么（不真正执行）
make -n

# 查看依赖关系图
make -d              # 极详细的调试输出

# 在 Makefile 中打印变量值
$(info CXXFLAGS = $(CXXFLAGS))
$(warning 这是一个警告)
$(error 这是一个错误，会停止构建)
```

### 常见陷阱

```
❌ 错误: 命令前用了空格而不是 Tab
   Makefile:
       target:
           echo "hello"     ← 4个空格，报错！

   ✅ 正确:
       target:
    ←Tab→echo "hello"       ← 必须是 Tab

   检查方法: cat -A Makefile | grep "    "  （Tab 显示为 ^I）
```

```
❌ 错误: 变量赋值多了空格
   CXX = clang++       ← 正确
   CXX = clang++       ← 也正确（= 两边的空格被忽略）
   CXX=clang++         ← 也正确

   但注意:
   CXXFLAGS = -Wall    ← CXXFLAGS 的值是 "-Wall"（没有前导空格）
   CXXFLAGS =  -Wall   ← CXXFLAGS 的值是 " -Wall"（有前导空格！）
```

```
❌ 错误: 依赖写错了
   main.o: main.cpp header.h    ← header.h 不存在
   # make 报错: *** No rule to make target 'header.h'

   ✅ 用通配符更安全:
   HEADERS = $(wildcard include/*.h)
   main.o: main.cpp $(HEADERS)
```

---

## 速查表

### 变量

| 语法 | 含义 |
|------|------|
| `VAR = value` | 延迟赋值 |
| `VAR := value` | 立即赋值 |
| `VAR ?= value` | 条件赋值（未定义时） |
| `VAR += value` | 追加 |
| `$(VAR)` | 使用变量 |

### 自动变量

| 变量 | 含义 |
|------|------|
| `$@` | 目标文件 |
| `$<` | 第一个依赖 |
| `$^` | 所有依赖（去重） |
| `$?` | 比目标新的依赖 |
| `$*` | 模式匹配的部分 |

### 函数

| 函数 | 用途 |
|------|------|
| `$(wildcard pattern)` | 匹配文件 |
| `$(patsubst from,to,text)` | 模式替换 |
| `$(var:old=new)` | 后缀替换 |
| `$(dir path)` | 取目录部分 |
| `$(notdir path)` | 取文件名部分 |
| `$(basename file)` | 去后缀 |
| `$(suffix file)` | 取后缀 |
| `$(sort list)` | 排序去重 |
| `$(words list)` | 计数 |
| `$(shell cmd)` | 执行 shell |
| `$(info msg)` | 打印信息 |

### 命令行

| 命令 | 用途 |
|------|------|
| `make` | 构建默认目标 |
| `make target` | 构建指定目标 |
| `make -j8` | 8 并行 |
| `make -n` | dry-run |
| `make -B` | 强制重建 |
| `make clean` | 清理 |
| `make V=1` | 详细输出（需 Makefile 支持） |
| `make -p` | 打印所有变量 |

### 特殊目标

| 目标 | 含义 |
|------|------|
| `.PHONY` | 声明非文件目标 |
| `.SUFFIXES` | 清空/设置后缀规则 |
| `.DEFAULT` | 默认规则 |
| `.DELETE_ON_ERROR` | 命令失败时删除目标文件 |
