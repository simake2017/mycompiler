cmake_minimum_required(VERSION 3.20)
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

# ─── 源文件 ────────────────────────────────────────────
set(SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/type.cpp
    src/semantic_analyzer.cpp
    src/template_instantiation.cpp
    src/codegen.cpp
    src/main.cpp
)

add_executable(minicc ${SOURCES})
target_include_directories(minicc PRIVATE include) ## 添加include文件件

# 编译选项
target_compile_options(minicc PRIVATE -Wall -Wextra -pedantic)

