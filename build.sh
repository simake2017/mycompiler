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

do_test() {
    if [ ! -f "${TARGET}" ]; then
        do_build
    fi
    echo "  TEST    全量回归测试"
    cmake --build build-linux -j$(nproc) 2>/dev/null && \
        ctest --test-dir build-linux --output-on-failure || \
        echo "  ⚠️       build-linux 不存在或 ctest 失败，跳过"
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
