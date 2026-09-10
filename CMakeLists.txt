cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(MiniCppCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS: 确保使用正确的 SDK 和 C++ 标准库路径
if(APPLE)
#    execute_process(COMMAND xcrun --show-sdk-path
#        OUTPUT_VARIABLE SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
#    set(CMAKE_OSX_SYSROOT ${SDK_PATH})
#
#    # 显式添加 C++ 标准库头文件路径
#    set(CXX_STDLIB_PATH "${SDK_PATH}/usr/include/c++/v1")

    set(CXX_STDLIB_PATH "/opt/homebrew/opt/llvm/include/c++/v1")
    if(EXISTS ${CXX_STDLIB_PATH})
        include_directories(SYSTEM ${CXX_STDLIB_PATH})

        link_directories("/opt/homebrew/opt/llvm/lib/c++")
#        set(CMAKE_BUILD_RPATH "/opt/homebrew/opt/llvm/lib/c++")
    endif()

endif()

# Linux + Clang: 系统 libstdc++ (GCC 10) 无 <format>，改用 libc++
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

# ─── 源文件 ────────────────────────────────────────────
set(SOURCES
    src/preprocessor.cpp
    src/lexer.cpp
    src/parser.cpp
    src/type.cpp
    src/semantic_analyzer.cpp
    src/template_instantiation.cpp
    src/template_deduction.cpp
    src/codegen.cpp
    src/linker.cpp
    src/main.cpp
)

add_executable(minicc ${SOURCES})
target_include_directories(minicc PRIVATE include) ## 添加include文件件

# 编译选项
target_compile_options(minicc PRIVATE -Wall -Wextra -pedantic)

# ─── 单元测试（GoogleTest，CLion gutter 可点击单测）─────────────────────
# CLion 用法：Reload CMake 后 tests/unit/*.cpp 里每个 TEST() 左侧出现绿色三角，
# 点击即单跑该用例；底部 Test 面板树状查看全部结果。
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(unit_tests
    tests/unit/test_preprocessor.cpp
    tests/unit/test_template_deduction.cpp
    tests/unit/test_decl_and_ctor.cpp
    tests/unit/test_expr_parser.cpp
    tests/unit/test_expressions.cpp
    tests/unit/test_ast_nodes.cpp
    tests/unit/test_rtti_layout.cpp
    tests/unit/test_typeinfo_suite.cpp
    src/preprocessor.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic_analyzer.cpp
    src/codegen.cpp
    src/template_deduction.cpp
    src/template_instantiation.cpp
    src/type.cpp
    src/linker.cpp
)
target_include_directories(unit_tests PRIVATE include)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)
target_link_libraries(unit_tests GTest::gtest GTest::gtest_main)
# -rdynamic：把弱符号（vtable/typeinfo）导出进动态符号表，
# 供 test_rtti_layout.cpp 用 dladdr 把地址解析成符号名打印内存图形；
# 老 glibc 的 dladdr 在独立 libdl 里，需显式链接
target_link_options(unit_tests PRIVATE -rdynamic)
target_link_libraries(unit_tests dl)

include(GoogleTest)
gtest_discover_tests(unit_tests)

