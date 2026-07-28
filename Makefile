# =============================================================================
# minicc 的 Makefile（纯手写版，不依赖 CMake）
# =============================================================================
#
# 本文件来自 docs/make-tutorial.md 第十一章的实战例子。
# 下面每条规则后面都附有实际执行结果、作用和数据的注释。
#
# ═══════════════════════════════════════════════════════════════════════════════
# 【步骤 0】make -n（dry-run，只打印命令不执行）
# ═══════════════════════════════════════════════════════════════════════════════
#
# 作用: 预览 make 会执行哪些命令，但不真正执行。用于调试 Makefile。
#
# 执行: make -n
# 输出:
#   mkdir -p build
#   /opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -Wall -Wextra -pedantic -g \
#       -MMD -MP -Iinclude -isystem /opt/homebrew/opt/llvm/include/c++/v1 \
#       -c src/codegen.cpp -o build/codegen.o
#   ... (对 7 个 .cpp 文件各输出一条编译命令)
#   /opt/homebrew/opt/llvm/bin/clang++ build/codegen.o build/lexer.o \
#       build/main.o build/parser.o build/semantic_analyzer.o \
#       build/template_instantiation.o build/type.o \
#       -o build/minicc -L/opt/homebrew/opt/llvm/lib/c++
#
# 解读: dry-run 揭示了 make 的完整计划——先编译 7 个 .o，再链接成 minicc。
#       因为 @echo 前缀在 -n 模式下也会打印，所以能看到 echo 和实际命令。
# =============================================================================

# ─── 编译器设置 ───────────────────────────────────────────────────────────────
CXX       = /opt/homebrew/opt/llvm/bin/clang++
CXXFLAGS  = -std=c++20 -Wall -Wextra -pedantic -g -MMD -MP
INCLUDES  = -Iinclude -isystem /opt/homebrew/opt/llvm/include/c++/v1
LDFLAGS   = -L/opt/homebrew/opt/llvm/lib/c++
#
# 【变量说明】
#   CXX       — 编译器路径。用 homebrew 的 LLVM 而非系统自带的 Apple Clang，
#               因为 homebrew 版本对 C++20 支持更完整。
#   CXXFLAGS  — 编译选项：
#               -std=c++20     使用 C++20 标准
#               -Wall          开启所有常见警告
#               -Wextra        开启额外警告
#               -pedantic      严格遵守标准，拒绝编译器扩展
#               -g             生成调试符号（可用 lldb/gdb 调试）
#               -MMD           自动生成 .d 依赖文件（记录 .cpp 依赖了哪些 .h）
#               -MP            配合 -MMD，为每个头文件生成空的 phony 目标，
#                              这样删掉某个 .h 后 make 不会报错
#   INCLUDES  — 头文件搜索路径：
#               -Iinclude            项目的 include/ 目录
#               -isystem .../v1      LLVM 的 libc++ 标准库头文件
#   LDFLAGS   — 链接选项：
#               -L.../lib/c++  指定 libc++ 动态库的搜索路径

# ─── 目录设置 ─────────────────────────────────────────────────────────────────
SRCDIR    = src
INCDIR    = include
BUILDDIR  = build

# ─── 文件设置 ─────────────────────────────────────────────────────────────────
TARGET    = $(BUILDDIR)/minicc
SRCS      = $(wildcard $(SRCDIR)/*.cpp)
OBJS      = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS      = $(OBJS:.o=.d)
#
# 【变量展开结果】（make -p 可验证）
#
#   SRCS = src/codegen.cpp src/lexer.cpp src/main.cpp src/parser.cpp \
#          src/semantic_analyzer.cpp src/template_instantiation.cpp src/type.cpp
#          ↑ $(wildcard src/*.cpp) 扫描 src/ 目录，找到 7 个 .cpp 文件
#
#   OBJS = build/codegen.o build/lexer.o build/main.o build/parser.o \
#          build/semantic_analyzer.o build/template_instantiation.o build/type.o
#          ↑ $(patsubst src/%.cpp, build/%.o, ...) 把路径和后缀都替换了
#
#   DEPS = build/codegen.d build/lexer.d build/main.d build/parser.d \
#          build/semantic_analyzer.d build/template_instantiation.d build/type.d
#          ↑ $(OBJS:.o=.d) 后缀替换，每个 .o 对应一个 .d 依赖文件

# ─── 默认目标 ─────────────────────────────────────────────────────────────────
all: $(TARGET)
#
# 作用: make 不带参数时执行第一个目标，即 all → $(TARGET) → build/minicc
#       all 是 phony 目标，不对应真实文件，所以每次都触发对 $(TARGET) 的检查。

# ─── 链接：所有 .o → minicc ──────────────────────────────────────────────────
$(TARGET): $(OBJS) | $(BUILDDIR)
	@echo "  LINK    $@"
	@$(CXX) $(OBJS) -o $@ $(LDFLAGS)
#
# 【规则说明】
#   目标:  build/minicc
#   依赖:  7 个 .o 文件 + build/ 目录（顺序依赖）
#   命令:  把 7 个 .o 链接成一个可执行文件
#
#   $@ 展开为 → build/minicc
#   $^ 展开为 → build/codegen.o build/lexer.o ... build/type.o（全部依赖）
#
#   | $(BUILDDIR) 是"顺序依赖"：只保证 build/ 目录在链接前存在，
#   不参与时间戳比较。如果不加 |，build/ 目录的 mtime 变化会导致
#   不必要的重新链接。
#
# ═══════════════════════════════════════════════════════════════════════════════
# 【步骤 2】make（全量构建，从零开始）
# ═══════════════════════════════════════════════════════════════════════════════
#
# 执行: make
# 输出:
#   CXX     src/codegen.cpp → build/codegen.o
#   src/codegen.cpp:64:28: warning: unused parameter 'unit' [-Wunused-parameter]
#      64 |     const TranslationUnit& unit,
#         |                            ^
#   1 warning generated.
#   CXX     src/lexer.cpp → build/lexer.o
#   CXX     src/main.cpp → build/main.o
#   CXX     src/parser.cpp → build/parser.o
#   src/parser.cpp:393:53: warning: unused parameter 'access' [-Wunused-parameter]
#     393 |                                      AccessModifier access) {
#         |                                                     ^
#   1 warning generated.
#   CXX     src/semantic_analyzer.cpp → build/semantic_analyzer.o
#   CXX     src/template_instantiation.cpp → build/template_instantiation.o
#   CXX     src/type.cpp → build/type.o
#   LINK    build/minicc
#
# 解读:
#   1. 先创建 build/ 目录（mkdir -p build）
#   2. 依次编译 7 个 .cpp → .o，每个都打印 "CXX 源文件 → 目标文件"
#   3. 最后链接所有 .o → build/minicc
#   4. 有 2 个编译警告（unused parameter），因为 -Wall -Wextra 很严格
#   5. 整个流程：7 次编译 + 1 次链接 = 8 条命令
#
# 构建产物清单（ls -lh build/）:
#   codegen.o                  1.9M   目标文件（含调试符号 -g，所以比较大）
#   lexer.o                    1.1M
#   main.o                     2.3M
#   parser.o                   2.4M
#   semantic_analyzer.o        2.8M
#   template_instantiation.o   2.5M
#   type.o                     199K   （最小的源文件，所以 .o 也最小）
#   minicc                     2.2M   最终可执行文件
#   *.d (x7)                   若干   自动生成的依赖文件
#
# 二进制信息（file build/minicc）:
#   build/minicc: Mach-O 64-bit executable arm64
#   ↑ Apple Silicon (M系列) 原生可执行文件

# ─── 编译：每个 .cpp → .o ────────────────────────────────────────────────────
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@echo "  CXX     $< → $@"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
#
# 【规则说明】
#   这是一条"模式规则"——一条规则覆盖所有 .cpp → .o 的编译。
#   % 是通配符，匹配文件名部分（不含目录和后缀）。
#
#   例: build/lexer.o 匹配 src/lexer.cpp
#       % = lexer
#       $< = src/lexer.cpp（第一个依赖）
#       $@ = build/lexer.o（目标）
#
#   -c 表示"只编译不链接"，生成 .o 目标文件。
#
# ═══════════════════════════════════════════════════════════════════════════════
# 【步骤 3】make（增量编译——什么都没改）
# ═══════════════════════════════════════════════════════════════════════════════
#
# 执行: make
# 输出:
#   make: Nothing to be done for `all'.
#
# 解读:
#   make 检查 build/minicc 的时间戳 vs 所有 .o 的时间戳：
#     build/minicc (09:10) 比所有 .o (09:09~09:10) 都新
#   → 没有文件需要重新编译，跳过所有命令。
#   → 这就是 make 的核心价值：避免无意义的重复编译。

# ─── 创建构建目录 ─────────────────────────────────────────────────────────────
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)
#
# 【规则说明】
#   只在 build/ 目录不存在时执行 mkdir -p。
#   -p 参数保证目录已存在时不报错。
#
# ═══════════════════════════════════════════════════════════════════════════════
# 【步骤 4】增量编译——只改了 lexer.cpp
# ═══════════════════════════════════════════════════════════════════════════════
#
# 执行: touch src/lexer.cpp && make
# 输出:
#   CXX     src/lexer.cpp → build/lexer.o
#   LINK    build/minicc
#
# 解读:
#   touch 更新了 lexer.cpp 的修改时间，使其比 build/lexer.o 更新。
#   make 的依赖分析:
#     build/lexer.o 依赖 src/lexer.cpp → lexer.cpp 比 lexer.o 新 → 重新编译
#     build/parser.o 依赖 src/parser.cpp → parser.cpp 没改 → 跳过
#     build/main.o  依赖 src/main.cpp  → main.cpp 没改 → 跳过
#     ... 其余 4 个 .o 同理全部跳过
#     build/minicc 依赖所有 .o → lexer.o 被重新编译了 → 重新链接
#
#   结果: 只执行了 1 次编译 + 1 次链接（而不是 7+1），节省了大量时间。
#   这就是"增量编译"的威力。

# ─── 清理 ─────────────────────────────────────────────────────────────────────
clean:
	@echo "  CLEAN   $(BUILDDIR)/"
	@rm -rf $(BUILDDIR)
#
# 【规则说明】
#   删除整个 build/ 目录，清除所有编译产物。
#
# ═══════════════════════════════════════════════════════════════════════════════
# 【步骤 8】make clean
# ═══════════════════════════════════════════════════════════════════════════════
#
# 执行: make clean
# 输出:
#   CLEAN   build/
#
# 解读: rm -rf build/ 删除了整个构建目录（包括 7 个 .o、7 个 .d、1 个可执行文件）。
#       clean 是 phony 目标，即使有同名文件也会执行。
#       执行后 ls build/ 会报 "No such file or directory"。

# ─── 运行测试 ─────────────────────────────────────────────────────────────────
test: $(TARGET)
	@echo "  TEST    running all template tests..."
	@for f in tests/test_tmpl_*.cpp; do \
	    echo "  RUN     $$f"; \
	    $(TARGET) "$$f" -o /dev/null 2>/dev/null && \
	        echo "  ✅      $$(basename $$f)" || \
	        echo "  ❌      $$(basename $$f)"; \
	done
#
# 【规则说明】
#   依赖 $(TARGET)：先确保 minicc 已编译，然后遍历 tests/test_tmpl_*.cpp，
#   用 minicc 编译每个测试文件，输出 pass/fail。
#   $$f 是 shell 变量（Makefile 里 $ 要写成 $$ 才能传给 shell）。

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
#
# 【关键机制：自动依赖追踪】
#
#   -include 的 "-" 前缀表示：文件不存在时不报错（首次构建时 .d 还没生成）。
#
#   编译器用 -MMD -MP 选项在编译 .cpp 时自动生成同名的 .d 文件。
#   例如 build/lexer.d 的内容:
#
#     build/lexer.o: src/lexer.cpp include/lexer.h include/token.h
#     include/lexer.h:
#     include/token.h:
#
#   第 1 行: 告诉 make "lexer.o 依赖 lexer.cpp、lexer.h、token.h"
#            → 改了任何一个 .h，lexer.o 都会重新编译
#   第 2-3 行: 空的 phony 目标（-MP 生成的），防止删掉 .h 后 make 报错
#            "No rule to make target 'include/lexer.h'"
#
#   更复杂的例子 build/main.d:
#     build/main.o: src/main.cpp include/lexer.h include/parser.h \
#                   include/semantic_analyzer.h include/codegen.h \
#                   include/template_instantiation.h include/ast.h \
#                   include/token.h include/type.h
#     ↑ main.cpp include 了几乎所有头文件，所以改任何 .h 都会触发 main.o 重编译
#
#   这个机制的价值: 你不需要手动维护 "哪个 .o 依赖哪些 .h"，
#   编译器帮你搞定，make 自动读取。改了任何头文件都能正确触发增量编译。
#
# 【步骤 5】查看 .d 文件验证依赖追踪
# ═══════════════════════════════════════════════════════════════════════════════
#
# 执行: cat build/lexer.d
# 输出:
#   build/lexer.o: src/lexer.cpp include/lexer.h include/token.h
#   include/lexer.h:
#   include/token.h:
#
# 解读: lexer.cpp 依赖 lexer.h 和 token.h。
#       如果 touch include/token.h 然后 make，lexer.o 会被重新编译，
#       而 parser.o（不依赖 token.h）不会。

# ─── phony 目标 ───────────────────────────────────────────────────────────────
.PHONY: all clean test help
#
# 【规则说明】
#   声明 all/clean/test/help 为"非文件目标"。
#   即使目录下恰好有个文件叫 clean，make clean 也照样执行，
#   不会说 "clean is up to date"。
