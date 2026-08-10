// =============================================================================
// test_pp_01_include.cpp —— P0 正例：#include + #pragma once
// =============================================================================
// 考察理论点：[cpp.include] 文件并合 ——
//   引号形式先搜当前文件所在目录；#pragma once 按 canonical 路径去重。
//   若无去重，square 双重定义会在汇编期报符号重复 —— 执行结果即反证。
//
// 预期行为：
//   Phase 0 日志：第一次 #include pp/math_helper.h → 路径；
//                 第二次 → skipped (pragma once)
//   退出码 = 0（square(6)=36，HELPER_TAG=7，来自头文件宏）
//
// 实验：./build-linux/minicc tests/test_pp_01_include.cpp -E  查看展开后全文
// =============================================================================

#include "pp/math_helper.h"
#include "pp/math_helper.h"   // 第二次：应被 #pragma once 跳过

int main() {
    return square(6) + HELPER_TAG - 43;
}
