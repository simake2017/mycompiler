// =============================================================================
// 测试：P4 内建外部函数 —— malloc / free / memcpy / realloc
// =============================================================================
// 理论点：
//   语义层只登记"原型"（名字 + 形参 + 返回类型，body 为空），
//   CodeGen::generate() 因 `if (func->body)` 守卫跳过发射——
//   .s 文件里不存在这四个函数的汇编体，调用点发射普通 `callq`，
//   链接期须由【外部实现】兑现 —— 内建原型只是“声明”，不是“定义”。
//   对照 clang：Builtins::Info 表 + 内建语义。
//
// 校验点（minicc 无解引用/取地址，故用返回值语义断言）：
//   ① malloc(8) 返回非空指针（!= nullptr）          —— 分配成功
//   ② memcpy(dst, src, n) 返回 dst 本身              —— libc 返回语义
//   ③ realloc(p, n) 返回新块首址（非空）             —— 扩容不失败
//   ④ free 无返回、不崩溃                            —— 释放闭环
//
// 预期：编译通过，运行退出码 0
//   ok = 1 + 1 = 2 → return 2 - 2 = 0
//
// ⚠ 当前状态：编译通过、【链接失败】rc=1（logdiff 基线已把失败固化）
//   自研链接器只内置 malloc/free，不链 libc（docs/learn/15）⇒
//     ① malloc ✓   ④ free ✓     ② memcpy ✗   ③ realloc ✗
//   报 "undefined reference to 'memcpy'" / "... 'realloc'"。
//   补上这两个运行时刻桩后本文件自动变绿。
// =============================================================================

int main() {
    int ok = 0;

    int* p = malloc(8);             // ① 语义层认识原型 → callq malloc
    if (p != nullptr) { ok = ok + 1; }

    int* q = malloc(8);
    int* r = memcpy(p, q, 8);       // ② 返回 dst 本身
    if (r == p) { ok = ok + 1; }

    int* p2 = realloc(p, 16);       // ③ 扩容（原地或搬家，返回新首址）
    free(p2);                       // ④ 释放新块（注意：realloc 后旧指针不可再 free）
    free(q);

    return ok - 2;                  // 0
}
