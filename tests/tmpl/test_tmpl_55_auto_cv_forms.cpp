// =============================================================================
// 测试：auto 占位符的六种"带壳"形态（auto Placeholder with cv/pointer/ref Shells）
// =============================================================================
// 考察理论点（[dcl.spec.auto]/7、[dcl.type.cv]/1、[temp.deduct.call]）：
//   auto 占位符可以被 cv 限定符 / 指针 / 引用包住，推导规则等价于把 auto 当作
//   模板形参跑一次函数模板实参推导 —— 外壳照抄到结果上，A 同步剥掉对应层：
//
//     const auto   ⇒ const int    （cv 是【声明的一部分】，不参与反推）
//     auto const   ⇒ const int    （[dcl.type.cv]/1：cv 与类型名的相对位置可互换）
//     auto*        ⇒ int*         （要求实参确实是指针，A 剥掉一层）
//     auto&        ⇒ int&
//     const auto&  ⇒ const int&
//     auto&&       ⇒ int&&
//
//   修复前（BUGS.md B1）：判据写成裸 `type->isAuto()`，被壳包住就看不见占位符
//   ⇒ 六种形态里除裸 auto 外【全部】落到 else 分支被误报 "Type mismatch"。
//
// 预期行为：
//   - 编译通过，main 返回 32（clang++-18 同值）。
//     五个拷贝/引用各读 5，加上 v6 读右值 7 ⇒ 5+5+5+5+7+5 = 32。
//   - 日志逐条给出推导结果（外壳原文可见，而非统一印成 "auto"）：
//       [auto] ★ v1 : const auto ⟹ const int
//       [auto] ★ v3 : auto* ⟹ int*
//       [auto] ★ v5 : const auto& ⟹ const int&
//       [auto] ★ v6 : auto&& ⟹ int&&
//
// ★ 本用例【不】验证引用的别名语义：`int& r = a; r = 30;` 目前不会改到 a ——
//   那是独立缺口（BUGS.md B6：引用只当拷贝，没做取址+间接访问），
//   且【不带 auto 的裸引用也一样】，与本修复无关。此处只钉"推导正确 + 能编过"。
// =============================================================================

int main() {
    int a = 5;

    const auto  v1 = a;   // ⇒ const int
    auto const  v2 = a;   // ⇒ const int
    auto*       v3 = &a;  // ⇒ int*
    auto&       v4 = a;   // ⇒ int&
    const auto& v5 = a;   // ⇒ const int&
    auto&&      v6 = 7;   // ⇒ int&&
    auto        v7 = a;   // ⇒ int

    return v1 + v2 + v4 + v5 + v6 + v7;
}
