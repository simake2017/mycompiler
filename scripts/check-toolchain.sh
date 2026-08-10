#!/usr/bin/env bash
# =============================================================================
# check-toolchain.sh —— C++ 工具链自检脚本
# =============================================================================
# 配套文档：docs/toolchain-search-paths.md
#
# 检查项：
#   1. 编译器/链接器版本
#   2. <format> 在各种配置组合下是否可编译、可运行
#   3. 头文件搜索路径（默认 vs -stdlib=libc++）
#   4. 链接命令的 -L / -l 顺序、ld 内置 SEARCH_DIR
#   5. 项目产物 minicc 的运行时动态库解析
#
# 用法：
#   bash scripts/check-toolchain.sh            # 完整自检
#   CXX=g++ bash scripts/check-toolchain.sh    # 换编译器检查
# =============================================================================
set -u

CXX=${CXX:-clang++}
PASS=0; FAIL=0; WARN=0
ok()   { printf '\e[32m[PASS]\e[0m %s\n' "$1"; PASS=$((PASS+1)); }
ng()   { printf '\e[31m[FAIL]\e[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
warn() { printf '\e[33m[WARN]\e[0m %s\n' "$1"; WARN=$((WARN+1)); }
info() { printf '       %s\n' "$1"; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/probe.cpp" <<'EOF'
#include <format>
#include <iostream>
int main() { std::cout << std::format("answer = {}", 42); }
EOF

echo "== 1. 版本 ========================================================"
$CXX --version | head -1
ld --version | head -1

echo
echo "== 2. <format> 可用性（编译 + 运行） ================================"
# 2a. 默认标准库 + C++20
if $CXX -std=c++20 "$tmp/probe.cpp" -o "$tmp/a" >/dev/null 2>&1; then
    out=$("$tmp/a"); [ "$out" = "answer = 42" ] \
        && ok "默认标准库: 编译运行 OK (libstdc++ >= 13 或已有 <format>)" \
        || ng "默认标准库: 编译通过但输出异常: $out"
else
    warn "默认标准库: <format> 不可用（本机即此情况：GCC 10 的 libstdc++）"
fi
# 2b. libc++ + C++20（本项目的修复方案）
if $CXX -std=c++20 -stdlib=libc++ "$tmp/probe.cpp" -o "$tmp/b" >/dev/null 2>&1; then
    out=$("$tmp/b"); [ "$out" = "answer = 42" ] \
        && ok "-stdlib=libc++: 编译运行 OK" \
        || ng "-stdlib=libc++: 编译通过但输出异常: $out"
    info "运行时标准库: $(ldd "$tmp/b" | awk '/lib(c\+\+|stdc\+\+)/{print $1}' | tr '\n' ' ')"
else
    ng "-stdlib=libc++: 编译失败 → 未安装 libc++-dev? (apt install libc++-18-dev libc++abi-18-dev)"
fi
# 2c. 默认语言标准（不加 -std）——验证文档中 gnu++17 的坑
if $CXX -stdlib=libc++ "$tmp/probe.cpp" -o "$tmp/c" >/dev/null 2>&1; then
    warn "不加 -std 也能编译 → 编译器默认标准已 >= C++20"
else
    ok "不加 -std 编译失败 → 证实默认标准 < C++20，必须显式 -std=c++20"
fi

echo
echo "== 3. 头文件搜索路径 ==============================================="
echo "--- 默认 (libstdc++) ---"
$CXX -E -x c++ -v /dev/null 2>&1 | sed -n '/#include <...>/,/End of search list/p' | sed 's/^/  /'
echo "--- -stdlib=libc++ ---"
$CXX -stdlib=libc++ -E -x c++ -v /dev/null 2>&1 | sed -n '/#include <...>/,/End of search list/p' | sed 's/^/  /'
# 自动断言：libc++ 模式下必须出现 c++/v1
if $CXX -stdlib=libc++ -E -x c++ -v /dev/null 2>&1 | grep -q "include/c++/v1"; then
    ok "libc++ 头路径 c++/v1 存在"
else
    ng "libc++ 头路径 c++/v1 缺失"
fi

echo
echo "== 4. 链接阶段 ====================================================="
$CXX -std=c++20 -stdlib=libc++ -v "$tmp/probe.cpp" -o "$tmp/d" 2>"$tmp/v.txt"
echo "--- -L 搜索目录（命令行顺序） ---"
tr ' ' '\n' < "$tmp/v.txt" | grep -E '^-L' | sed 's/^/  /'
echo "--- -l 链接顺序 ---"
tr ' ' '\n' < "$tmp/v.txt" | grep -E '^-l' | paste -sd' ' - | sed 's/^/  /'
echo "--- CRT 启动文件 ---"
tr ' ' '\n' < "$tmp/v.txt" | grep -E 'crt|Scrt1' | sed 's/^/  /'
echo "--- ld 内置 SEARCH_DIR ---"
ld --verbose 2>/dev/null | grep -oE 'SEARCH_DIR\("[^"]*"\)' | head -6 | sed 's/^/  /'
info "（共 $(ld --verbose 2>/dev/null | tr ';' '\n' | grep -c SEARCH_DIR) 条，省略部分见 ld --verbose）"

echo
echo "== 5. 项目产物 minicc 运行时解析 ===================================="
PROJ_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(ls "$PROJ_ROOT"/cmake-build-debug/minicc "$PROJ_ROOT"/build/minicc 2>/dev/null | head -1)
if [ -n "${BIN:-}" ]; then
    info "检查: $BIN"
    if ldd "$BIN" | grep -q "not found"; then
        ng "存在未解析的动态库:"; ldd "$BIN" | grep "not found" | sed 's/^/  /'
    else
        ok "所有动态库均已解析"
    fi
    ldd "$BIN" | grep -E "libc\+\+|libstdc" | sed 's/^/  /'
else
    warn "未找到构建产物，跳过（先 cmake .. && make）"
fi

echo
echo "== 汇总 ============================================================"
printf 'PASS=%d  FAIL=%d  WARN=%d\n' "$PASS" "$FAIL" "$WARN"
[ "$FAIL" -eq 0 ] || exit 1
