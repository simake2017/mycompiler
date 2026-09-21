#!/usr/bin/env bash
# ============================================================================
# demos/run.sh —— 一键跑遍 demos/ 下的所有示例
# ============================================================================
# 每个 demo 的约定：编译成功且【运行退出码 = 0】即通过。
# （退出码非 0 表示该 demo 内部的某个断言失败，具体码值写在 demo 源码里。）
#
# 用法：
#   ./demos/run.sh              # 跑全部
#   ./demos/run.sh tmpl         # 只跑某个子目录
#   ./demos/run.sh -v tmpl/01   # 显示完整编译日志
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MINICC="$ROOT/build-linux/minicc"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

if [ ! -x "$MINICC" ]; then
    echo "找不到 $MINICC —— 先跑 ./build.sh" >&2
    exit 1
fi

VERBOSE=0
if [ "${1:-}" = "-v" ]; then VERBOSE=1; shift; fi
FILTER="${1:-}"

pass=0; fail=0; failed_list=""

printf '%-34s %-8s %s\n' "demo" "结果" "备注"
printf -- '------------------------------------------------------------------------\n'

for src in $(find "$ROOT/demos" -name '*.cpp' | sort); do
    rel="${src#$ROOT/demos/}"
    if [ -n "$FILTER" ] && [ "${rel#$FILTER}" = "$rel" ]; then continue; fi

    bin="$OUT/$(echo "$rel" | tr '/' '_' | sed 's/\.cpp$//')"
    log="$OUT/$(basename "$bin").log"

    if ! "$MINICC" "$src" -o "$bin" >"$log" 2>&1; then
        printf '%-34s %-8s %s\n' "$rel" "编译失败" "见 $log"
        [ "$VERBOSE" = 1 ] && tail -20 "$log"
        fail=$((fail+1)); failed_list="$failed_list $rel(compile)"
        continue
    fi

    "$bin" >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        printf '%-34s %-8s %s\n' "$rel" "✅ 通过" ""
        pass=$((pass+1))
    else
        printf '%-34s %-8s %s\n' "$rel" "❌ 失败" "退出码 $rc（demo 内的断言，见源码）"
        fail=$((fail+1)); failed_list="$failed_list $rel(rc=$rc)"
    fi
done

printf -- '------------------------------------------------------------------------\n'
echo "通过 $pass 个，失败 $fail 个"
[ $fail -gt 0 ] && { echo "失败项:$failed_list"; exit 1; }
exit 0
