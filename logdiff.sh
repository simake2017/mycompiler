#!/bin/bash
# =============================================================================
# logdiff.sh —— 日志基线护栏（重构安全网）
# =============================================================================
# 用途：本项目把「全阶段中文日志」当作交付物之一 —— 单测靠子串匹配日志原文
#       （tests/unit/obs_helpers.h: explainPipelineLine），文档直接粘贴日志片段。
#       因此重构代码时，【日志输出必须逐字节不变】，否则测试和文档一起红。
#
# 这个脚本把「重构前」的全部可观测输出存成基线，重构后逐字节对比。
#
# 用法：
#   ./logdiff.sh save     存基线到 .logbaseline/
#   ./logdiff.sh diff     重跑并与基线对比（有任何差异即退出码 1）
#   ./logdiff.sh clean    删掉基线
# =============================================================================

set -u

cd "$(dirname "$0")"

MINICC=./minicc
BASEDIR=.logbaseline
WORKDIR=/tmp/minicc-logdiff

# 每个测试记录四样东西：
#   ① 编译期完整日志（stdout+stderr 合并）—— 覆盖 Lexer→Parser→Sema→实例化→CodeGen→链接
#   ② 编译器退出码
#   ③ 产物可执行文件的退出码 + 输出
#   ④ 生成的汇编全文（-S）—— CodeGen 改动的最严检查：汇编必须逐字节一致。
#      ①②③ 对 CodeGen 的错漏不一定敏感（日志相同、程序恰好也跑对），
#      ④ 才是把"代码生成没变"钉死的那一层。
run_one() {
    local src="$1"
    local out="$WORKDIR/bin"
    local asm="$WORKDIR/out.s"
    local log
    log=$("$MINICC" "$src" -o "$out" 2>&1)
    local rc=$?

    echo "=== rc=$rc ==="
    echo "$log"

    if [ $rc -eq 0 ] && [ -x "$out" ]; then
        echo "=== run ==="
        "$out" 2>&1
        echo "=== run-rc=$? ==="
    fi

    rm -f "$asm"
    "$MINICC" "$src" -S -o "$asm" > /dev/null 2>&1
    if [ -f "$asm" ]; then
        echo "=== asm ==="
        cat "$asm"
    fi
}

collect() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"

    local n=0
    for src in tests/*/*.cpp; do
        case "$src" in
            tests/unit/*) continue ;;   # 单测归 ctest 管，见下
        esac
        local name
        name=$(echo "$src" | sed 's|tests/||; s|/|__|g; s|\.cpp$||')
        run_one "$src" > "$dest/$name.log" 2>&1
        n=$((n + 1))
    done
    echo "  集成测试 $n 个 → $dest/"

    # 单测：gtest 输出含耗时，不稳定，只存「用例名 → 通过与否」
    # 过滤耗时行：`Passed  0.01 sec` 与 `Total Test time` 每次都不同，留着基线永远对不上。
    ctest --test-dir build-linux -N 2>/dev/null | grep -oE 'Test +#[0-9]+: \S+' | sed 's/.*: //' > "$dest/_unit_cases.txt"
    ctest --test-dir build-linux 2>&1 \
        | grep -E 'tests passed|tests failed|Failed' \
        | sed -E 's/[0-9]+\.[0-9]+ sec//g' > "$dest/_unit_summary.txt"
    echo "  单元测试用例清单 → $dest/_unit_cases.txt"
}

mkdir -p "$WORKDIR"

case "${1:-diff}" in
    save)
        echo "  保存日志基线（重构前必须跑这个）"
        collect "$BASEDIR"
        echo "  ✅ 基线已存；改动后跑 ./logdiff.sh diff"
        ;;
    diff)
        if [ ! -d "$BASEDIR" ]; then
            echo "  ❌ 没有基线，先跑 ./logdiff.sh save"
            exit 1
        fi
        echo "  重跑全部测试并与基线对比"
        collect "$WORKDIR/new"
        if diff -r "$BASEDIR" "$WORKDIR/new" > "$WORKDIR/diff.txt" 2>&1; then
            echo "  ✅ 日志逐字节零差异 —— 重构未改变任何可观测行为"
            exit 0
        else
            echo "  ❌ 检出差异："
            head -60 "$WORKDIR/diff.txt"
            echo "  ...（完整差异见 $WORKDIR/diff.txt）"
            exit 1
        fi
        ;;
    clean)
        rm -rf "$BASEDIR" "$WORKDIR"
        echo "  已清理基线"
        ;;
    *)
        echo "用法: ./logdiff.sh [save|diff|clean]"
        exit 1
        ;;
esac
