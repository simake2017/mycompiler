// ============================================================================
// demo 08 —— 控制流：if 与 while 降级成什么形状
// ============================================================================
// 理论：结构化控制流 → 基本块 + 条件跳转；循环再多一条【回边（back edge）】。
//
//   while 的形状是"条件测试在前 + 一条跳回条件测试的回边"：
//     while_begin_N:  <condition> / testq / je while_end_N
//                     <body> / jmp while_begin_N     ← 回边
//     while_end_N:
//
//   这正是流图 reducibility 的经典形状 —— 自然循环的识别就靠回边。
//
// 看生成的汇编：
//   ./build-linux/minicc demos/core/01_control_flow.cpp -S -o /tmp/demo08.s
//   grep -E "while_begin|while_end|jmp" /tmp/demo08.s
//
// 预期：退出码 0
// ============================================================================

int main() {
    // ① while：累加 1+2+3+4+5 = 15
    int sum = 0;
    int i = 1;
    while (i < 6) {
        sum = sum + i;
        i = i + 1;
    }
    if (sum != 15) return 1;

    // ② if / else：条件跳转的两种走向
    int x = 7;
    if (x > 10) {
        return 2;
    } else {
        if (x != 7) return 3;
    }

    return 0;
}
