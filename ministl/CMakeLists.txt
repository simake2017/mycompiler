cmake_minimum_required(VERSION 3.16)
project(ministl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────────────────────
#  ministl 是纯头文件库：用 INTERFACE 目标把 include 路径传播给使用者
# ─────────────────────────────────────────────────────────────────────────────
add_library(ministl INTERFACE)
target_include_directories(ministl INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ministl INTERFACE cxx_std_20)

# ─────────────────────────────────────────────────────────────────────────────
#  测试（GoogleTest）
# ─────────────────────────────────────────────────────────────────────────────
# CLion 用法：Reload CMake 后 tests/test_*.cpp 里每个 TEST() 左侧出现绿色三角，
# 点击即单跑该用例；底部 Test 面板树状查看全部结果。
# 命令行：ctest --test-dir build --output-on-failure
#         ./build/test_ranges --gtest_filter='Ranges.TakeShortCircuits'
#
# 这里刻意用与顶层 mycompiler/CMakeLists.txt 里 unit_tests 完全相同的
# googletest 声明（同 URL、同 tag），这样当 ministl 被 add_subdirectory
# 进顶层时，FetchContent 会识别为同一个依赖、复用已拉取的源码，不会重复下载。
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
enable_testing()

# 每个 tests/test_*.cpp 编译成一个独立可执行文件：
# 单文件失败不影响其它文件，CI 里也更容易并行与定位。
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)

foreach(src ${TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)

    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ministl GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra -pedantic)

    # 把每个 TEST() 注册成独立的 ctest 用例，而不是整个二进制一个条目
    gtest_discover_tests(${name})
endforeach()

list(LENGTH TEST_SOURCES _n_tests)
message(STATUS "ministl: 注册了 ${_n_tests} 个测试可执行文件")
