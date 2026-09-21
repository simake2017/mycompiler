#!/bin/bash
# =============================================================================
# build.sh —— 编译 minicc，二进制输出到项目根目录
# =============================================================================
# 用法：
#   ./build.sh          编译
#   ./build.sh clean    清理
#   ./build.sh test     编译 + 跑全量测试
# =============================================================================

set -e

cd "$(dirname "$0")"

CXX=clang++-18
SRCDIR=src
INCDIR=include
BUILDDIR=.build
TARGET=minicc

SRCS=(
    preprocessor.cpp
    sfinae.cpp
    lexer.cpp
    parser.cpp
    type.cpp
    semantic_analyzer.cpp
    template_instantiation.cpp
    template_deduction.cpp
    linker.cpp
    codegen.cpp
    main.cpp
)

CXXFLAGS="-std=c++20 -stdlib=libc++ -Wall -Wextra -pedantic -g -MMD -MP"
INCLUDES="-I${INCDIR}"
LDFLAGS="-stdlib=libc++"

do_clean() {
    echo "  CLEAN   ${BUILDDIR}/"
    rm -rf "${BUILDDIR}"
    rm -f "${TARGET}"
}

do_build() {
    mkdir -p "${BUILDDIR}"

    OBJS=()
    for src in "${SRCS[@]}"; do
        obj="${BUILDDIR}/${src%.cpp}.o"
        OBJS+=("${obj}")

        if [ -f "${obj}" ] && [ "${SRCDIR}/${src}" -ot "${obj}" ]; then
            # 检查头文件依赖
            dep_file="${BUILDDIR}/${src%.cpp}.d"
            needs_rebuild=false
            if [ -f "${dep_file}" ]; then
                while IFS= read -r line; do
                    for dep in $line; do
                        dep="${dep#:}"
                        dep="${dep%\\}"
                        [ -z "$dep" ] && continue
                        if [ -f "$dep" ] && [ "$dep" -nt "${obj}" ]; then
                            needs_rebuild=true
                            break 2
                        fi
                    done
                done < "${dep_file}"
            fi
            if ! $needs_rebuild; then
                continue
            fi
        fi

        echo "  CXX     ${SRCDIR}/${src} → ${obj}"
        ${CXX} ${CXXFLAGS} ${INCLUDES} -c "${SRCDIR}/${src}" -o "${obj}"
    done

    echo "  LINK    ${TARGET}"
    ${CXX} "${OBJS[@]}" -o "${TARGET}" ${LDFLAGS}
    echo "  ✅      ${TARGET} 构建完成 ($(du -h "${TARGET}" | cut -f1))"
}

# ── 并行度：★ 默认 -j2，不要改成 -j$(nproc) ──
# 【为什么封顶】本机 8 核 / 15Gi 内存，但常驻着别的负载（clangd×3 就占 ~3GB、
#   多开 IDE 与其它会话），实测可用内存常只剩 3GB 上下。
#   而单个重量级 TU（semantic_analyzer.cpp）峰值 RSS ≈ 326MB ——
#   -j8 就是 8 × 326MB ≈ 2.6GB，正好顶穿可用内存 ⇒ 进 swap ⇒ 整机卡死十几分钟。
#   （2026-09-21 实测：一次 `cmake --build build-linux -j$(nproc)` 把机器拖死到
#     load average 87，命令被超时中断。）
# 需要更快时用环境变量覆盖，并先 `free -h` 看一眼：
#     JOBS=4 ./build.sh test
JOBS="${JOBS:-2}"

do_test() {
    if [ ! -f "${TARGET}" ]; then
        do_build
    fi
    echo "  TEST    全量回归测试（并行度 -j${JOBS}，覆盖：JOBS=N ./build.sh test）"
    if [ ! -d build-linux ]; then
        echo "  ⚠️       build-linux 不存在，跳过"
        return 0
    fi
    if ! cmake --build build-linux -j"${JOBS}"; then
        echo "  ❌      构建失败，跳过测试"
        return 1
    fi
    ctest --test-dir build-linux --output-on-failure
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    test)
        do_build
        do_test
        ;;
    build|"")
        do_build
        ;;
    *)
        echo "用法: ./build.sh [build|clean|test]"
        exit 1
        ;;
esac
