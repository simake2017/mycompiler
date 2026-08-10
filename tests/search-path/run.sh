#!/usr/bin/env bash
# =============================================================================
# 头文件搜索路径实证测试
# =============================================================================
# 配套文档：docs/toolchain-search-paths.md 第二节
#
# 原理：同名头文件 myhdr.h 放在不同优先级位置（内容仅 HDR_SOURCE 宏不同），
#       通过改变 include 形式与编译参数，断言每次"谁命中 / 何时落空"：
#
#   src/myhdr.h     —— 当前文件目录（仅 "..." 形式可达）
#   dir_a/ dir_b/   —— -I 目录（验证顺序敏感性）
#   sys/            —— -isystem 目录（验证 -I 优先于 -isystem）
#   （不设任何 flag）—— 全链落空 → 编译失败
#
# 用法：bash tests/search-path/run.sh
# =============================================================================
set -u
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
PASS=0; FAIL=0
ok() { printf '\e[32m[PASS]\e[0m %s\n' "$1"; PASS=$((PASS+1)); }
ng() { printf '\e[31m[FAIL]\e[0m %s\n' "$1"; FAIL=$((FAIL+1)); }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# 编译+运行，断言 stdout == 期望值
expect_output() { # label expected_src flags...
    local label=$1 expected=$2; shift 2
    if $CXX "$@" src/"$label".cpp -o "$tmp/out" 2>"$tmp/err" && \
       [ "$("$tmp/out")" = "$expected" ]; then
        ok "$label → 命中 $expected"
    else
        ng "$label → 期望 $expected，实际: $("$tmp/out" 2>/dev/null || cat "$tmp/err" | head -2)"
    fi
}

# 断言编译失败（全链落空）
expect_fail() { # label flags...
    local label=$1; shift
    if $CXX "$@" src/"$label".cpp -o "$tmp/out" 2>"$tmp/err"; then
        ng "$label → 应当编译失败，居然成功了"
    elif grep -q "file not found" "$tmp/err"; then
        ok "$label → 全链落空，报 'file not found'（预期行为）"
    else
        ng "$label → 失败但错误不符: $(head -2 "$tmp/err")"
    fi
}

echo "== A. \"...\" 双引号形式：当前文件目录优先级最高 ==============="
expect_output probe_quote src_samedir
expect_output probe_quote src_samedir -Idir_a        # 有 -I 也不该抢走

echo
echo "== B. <...> 尖括号形式：跳过当前目录，-I 顺序敏感 ============="
expect_output probe_angle dir_a     -Idir_a
expect_output probe_angle dir_b     -Idir_b -Idir_a  # 先声明者先命中
expect_output probe_angle dir_a     -Idir_a -Idir_b  # 换序 → 换结果

echo
echo "== C. -I 永远优先于 -isystem（与命令行先后无关） =============="
expect_output probe_angle dir_a       -isystem sys -Idir_a
expect_output probe_angle isystem_dir -isystem sys

echo
echo "== D. 所有档都落空 → 编译失败 ================================"
expect_fail probe_angle            # 无 -I/-isystem，myhdr.h 无处可寻

echo
echo "== E. 真实头文件的落空下沉 =================================="
# <stdio.h>：第 4 档（标准库）和第 5 档（内置）都没有 → 沉到 /usr/include
resolved=$($CXX -H -fsyntax-only src/probe_stdio.cpp 2>&1 \
           | grep -m1 -oE '/[^ ]*/stdio\.h')
if [ "$resolved" = "/usr/include/stdio.h" ]; then
    ok "<stdio.h> 下沉到第 6 档命中: $resolved"
else
    ng "<stdio.h> 解析路径异常: ${resolved:-<无输出>}"
fi
# <format>：本项目原始案例
expect_fail probe_format                              # 默认 libstdc++(GCC10) → 落空
if $CXX -std=c++20 -stdlib=libc++ src/probe_format.cpp -o "$tmp/out" 2>"$tmp/err" \
   && [ "$("$tmp/out")" = "libc++" ]; then
    ok "<format> 换 -stdlib=libc++ 后在第 4 档 c++/v1 命中"
else
    ng "<format> libc++ 模式失败: $(head -2 "$tmp/err")"
fi

echo
printf '汇总: PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
