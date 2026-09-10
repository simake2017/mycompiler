// =============================================================================
// test_pp_03_conditional.cpp —— P0 正例：条件编译全家桶
// =============================================================================
// 考察理论点（[cpp.cond]）：
//   1. 条件栈与 #elif/#else 互斥（takenBranch 语义）
//   2. defined(X) 必须在宏展开之前处理
//   3. #if 常量表达式：MODE 先展开为 2 再求值
//   4. 未定义标识符按 0 处理（UNDEFINED_THING + 1 == 1 为真）
//
// 预期行为：Phase 0 打印各分支 taken/skipped；退出码 = 0
// =============================================================================

#define DEBUG
#define MODE 2

#ifdef DEBUG
#define A 7
#else
#define A 999
#endif

#ifndef RELEASE
#define B 35
#endif

#if defined(MODE) && MODE == 2
#define C 0
#elif MODE == 3
#define C 999
#else
#define C 998
#endif

#if UNDEFINED_THING + 1 == 1
#define D 0
#endif

int main() {
    return A + B + C + D - 42;   // 7 + 35 + 0 + 0 - 42 = 0
}
