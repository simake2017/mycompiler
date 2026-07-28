#!/bin/bash
# ============================================================================
# macOS 上 Homebrew LLVM libc++ ABI 不匹配问题 — 完整复现与学习脚本
#
# 用法：bash learn-abi-mismatch.sh
#
# 这个脚本会一步步演示问题发生的全过程，
# 每一步都有注释说明"发生了什么"和"为什么"。
# ============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # 无颜色
BOLD='\033[1m'

PROJECT_DIR="/Users/wangyang/cppproject/mycompiler"
BUILD_DIR="/tmp/learn-abi-mismatch"
LLVM_CLANG="/opt/homebrew/Cellar/llvm/21.1.8/bin/clang++"
LLVM_LIBCXX="/opt/homebrew/opt/llvm/lib/c++/libc++.dylib"
SYSTEM_LIBCXX="/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/usr/lib/libc++.tbd"

step() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}${CYAN}  $1${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

note() {
    echo -e "${YELLOW}📌 说明：${NC}$1"
}

warn() {
    echo -e "${RED}⚠️  问题：${NC}$1"
}

ok() {
    echo -e "${GREEN}✅ $1${NC}"
}

fail() {
    echo -e "${RED}❌ $1${NC}"
}

pause() {
    echo ""
    echo -e "${CYAN}按 Enter 继续下一步...${NC}"
    read -r
}

# ============================================================================
step "第 1 步：确认环境 — 我们用的编译器是谁？"
# ============================================================================

note "检查 Homebrew LLVM 的 clang++ 路径和版本："
echo ""
echo "  $ $LLVM_CLANG --version"
$LLVM_CLANG --version 2>&1 | head -3
echo ""

note "检查系统自带的 c++ 路径："
echo "  $ /usr/bin/c++ --version"
/usr/bin/c++ --version 2>&1 | head -1
echo ""

note "本实验使用 Homebrew LLVM 的 clang++（CLion 默认配置），而不是系统的 /usr/bin/c++"

pause

# ============================================================================
step "第 2 步：查看编译器默认的头文件搜索路径"
# ============================================================================

note "运行以下命令，看 clang++ 找头文件时按什么顺序搜索："
echo "  $ clang++ -E -x c++ -v /dev/null"
echo ""
echo -e "${BOLD}输出：${NC}"
$LLVM_CLANG -E -x c++ -v /dev/null 2>&1 | grep -A8 "search starts here" | sed 's/^/  /'
echo ""

note "注意第 1 行：/opt/homebrew/.../include/c++/v1"
echo "      这是 LLVM 21 自己的 libc++ 头文件，排在最前面，优先被使用。"
echo "      这些头文件是 LLVM 21 新版本，包含 __hash_memory 等新 ABI 符号。"

pause

# ============================================================================
step "第 3 步：干净的 CMake 配置 — 生成构建文件"
# ============================================================================

note "清空临时构建目录，重新用 Homebrew LLVM 配置 CMake："
echo "  $ cmake -DCMAKE_CXX_COMPILER=$LLVM_CLANG -G Ninja $PROJECT_DIR"
echo ""

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake \
    -DCMAKE_CXX_COMPILER="$LLVM_CLANG" \
    -DCMAKE_BUILD_TYPE=Debug \
    -G Ninja \
    "$PROJECT_DIR" 2>&1 | sed 's/^/  /'

echo ""
ok "CMake 配置完成，检测到编译器为 Clang 21.1.8"

pause

# ============================================================================
step "第 4 步：编译阶段 — 把 .cpp 编译成 .o"
# ============================================================================

note "运行 ninja，只观察编译阶段（前 7 步）："
echo "  $ ninja -v"
echo ""

# 编译并捕获输出，忽略链接失败
set +e
ninja -v 2>&1 | while IFS= read -r line; do
    # 编译命令（含 -c）
    if echo "$line" | grep -q "\-c /Users/wangyang"; then
        # 提取文件名
        src=$(echo "$line" | grep -oE "src/[a-z_]+\.cpp")
        echo -e "  ${GREEN}[编译]${NC} $src → .o"
    # 链接命令
    elif echo "$line" | grep -q "CXX_EXECUTABLE_LINKER\|Linking\|^FAILED"; then
        echo -e "  ${RED}[链接]${NC} $line"
    elif echo "$line" | grep -qE "^\[[0-9]+/[0-9]+\]"; then
        echo "  $line"
    fi
done
set -e

echo ""
ok "编译阶段全部成功 — 7 个 .o 文件都正常生成"
echo ""
warn "但每个 .o 文件里已经暗藏了问题：它们引用了 LLVM 21 新增的符号 __hash_memory"
note "编译阶段不检查符号是否真的存在，只是记录'我需要这个符号'，等链接时才验证"

pause

# ============================================================================
step "第 5 步：链接阶段 — 看看 clang++ 给 /usr/bin/ld 传了什么参数"
# ============================================================================

note "用 clang++ -### 参数，可以看到它实际调用 /usr/bin/ld 时的完整命令："
echo "  $ clang++ -### <所有.o文件> -o minicc"
echo ""
echo -e "${BOLD}/usr/bin/ld 收到的关键参数：${NC}"
echo ""

$LLVM_CLANG -### \
    -g -arch arm64 \
    CMakeFiles/minicc.dir/src/lexer.cpp.o \
    CMakeFiles/minicc.dir/src/parser.cpp.o \
    CMakeFiles/minicc.dir/src/type.cpp.o \
    CMakeFiles/minicc.dir/src/semantic_analyzer.cpp.o \
    CMakeFiles/minicc.dir/src/template_instantiation.cpp.o \
    CMakeFiles/minicc.dir/src/codegen.cpp.o \
    CMakeFiles/minicc.dir/src/main.cpp.o \
    -o minicc 2>&1 | grep "/usr/bin/ld" | tr ' ' '\n' | grep -A1 -E "syslibroot|^-lc\+\+$|^-L" | sed 's/^/  /'

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
note "注意看 -syslibroot 的值：指向系统 SDK，不是 LLVM！"
echo "      -lc++ 会在这个路径下找 libc++.dylib"
echo ""
warn "缺失的关键参数：-L/opt/homebrew/opt/llvm/lib/c++"
echo "      没有这个参数，/usr/bin/ld 永远找不到 LLVM 的 libc++"
echo ""
note "原因：Homebrew LLVM 的 clang++ 在 macOS 上不会自动把自己的 lib/ 路径传给链接器"
echo "      这是 Homebrew LLVM 在 macOS 上的已知行为（Linux 上没有这个问题）"

pause

# ============================================================================
step "第 6 步：链接失败 — 实际错误信息"
# ============================================================================

note "执行链接，看错误信息："
echo "  $ ninja"
echo ""

cd "$BUILD_DIR"
set +e
ninja 2>&1 | grep -A10 "Undefined symbols" | sed 's/^/  /'
set -e

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
note "错误解读："
echo "  __do_string_hash[abi:ne210108]  ← LLVM 21 的新 ABI 标签"
echo "  __hash_memory                   ← LLVM 21 libc++ 新增的内部哈希函数"
echo ""
echo "  编译时用的是 LLVM 21 头文件 → .o 里记录了对 __hash_memory 的依赖"
echo "  链接时用的是系统 libc++     → 系统版本没有这个符号 → 链接失败"

pause

# ============================================================================
step "第 7 步：终极验证 — 对比两个 libc++ 里的符号"
# ============================================================================

note "用 nm 命令检查两个 libc++ 文件里有没有 __hash_memory："
echo ""

echo -e "${BOLD}① LLVM 的 libc++.dylib：${NC}"
echo "  $ nm $LLVM_LIBCXX | grep hash_memory"
LLVM_RESULT=$(nm "$LLVM_LIBCXX" 2>/dev/null | grep hash_memory || echo "(无)")
echo "  $LLVM_RESULT"
if echo "$LLVM_RESULT" | grep -q "hash_memory"; then
    ok "LLVM 的 libc++ 里有 __hash_memory 符号"
else
    fail "未找到"
fi

echo ""
echo -e "${BOLD}② 系统的 libc++.tbd：${NC}"
echo "  $ nm $SYSTEM_LIBCXX | grep hash_memory"
SYSTEM_RESULT=$(nm "$SYSTEM_LIBCXX" 2>/dev/null | grep hash_memory || echo "(无结果)")
echo "  $SYSTEM_RESULT"
if echo "$SYSTEM_RESULT" | grep -q "hash_memory"; then
    ok "系统 libc++ 里有"
else
    fail "系统的 libc++ 里没有 __hash_memory 符号 — 这就是链接失败的根本原因"
fi

pause

# ============================================================================
step "第 8 步：触发源头 — 是哪行代码引入了这个问题？"
# ============================================================================

note "找到项目中触发 __hash_memory 的代码："
echo ""
echo "  $ grep -n 'unordered_map.*string_view' include/token.h"
echo ""
grep -n "unordered_map.*string_view" "$PROJECT_DIR/include/token.h" | sed 's/^/  /'
echo ""

note "完整触发链："
echo ""
echo "  token.h: std::unordered_map<std::string_view, TokenType>"
echo "      ↓"
echo "  unordered_map 需要对 key (string_view) 计算哈希"
echo "      ↓"
echo "  std::hash<std::string_view> 被实例化"
echo "      ↓"
echo "  LLVM 21 的 std::hash 内部调用了 __do_string_hash[abi:ne210108]"
echo "      ↓"
echo "  __do_string_hash 内部调用了 __hash_memory"
echo "      ↓"
echo "  链接时找不到 __hash_memory → Undefined symbol"

pause

# ============================================================================
step "第 9 步：两种修复方案"
# ============================================================================

echo -e "${BOLD}方案一：头文件降级，跟链接库对齐（myos 项目的做法）${NC}"
echo ""
echo "  在 CMakeLists.txt 里加："
echo "    target_compile_options(minicc PRIVATE"
echo "        -nostdinc++"
echo "        -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1"
echo "    )"
echo ""
echo "  效果：编译和链接都用系统 libc++，版本一致 ✅"
echo "  代价：无法使用 LLVM 21 新增的 C++ 特性"
echo ""

echo -e "${BOLD}方案二：链接库升级，跟头文件对齐${NC}"
echo ""
echo "  在 CMakeLists.txt 里加："
echo "    if(APPLE)"
echo "        link_directories(\"/opt/homebrew/opt/llvm/lib/c++\")"
echo "        set(CMAKE_BUILD_RPATH \"/opt/homebrew/opt/llvm/lib/c++\")"
echo "    endif()"
echo ""
echo "  效果：编译和链接都用 LLVM 21 的 libc++，版本一致 ✅"
echo "  好处：可以用 LLVM 21 的所有新特性"

echo ""
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BOLD}${GREEN}  演示结束！${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "构建目录保留在：$BUILD_DIR"
echo "可以用 cd $BUILD_DIR && ninja 自己复现"