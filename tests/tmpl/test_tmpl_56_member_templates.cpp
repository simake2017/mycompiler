// =============================================================================
// 测试：成员模板 (Member Templates, [temp.mem])
// =============================================================================
// 考察理论点：
//   · 成员模板是"定义在类体内的函数模板"。标准 [temp.mem]/1 明说：它的实参推导
//     规则与 [temp.deduct] 【完全一致】—— 形参表就是形参表，不为"成员"另立一套
//     合一算法。本实现因此直接复用 TemplateDeducer，只多两样东西：
//       ① ownerClassName（隐式 this，CodeGen 靠它决定参数寄存器偏不偏一位）；
//       ② 符号是 `类名_方法名_实参后缀`（S_id_int / S_id_intP），不是 Itanium
//          模板实例名 —— 同一个方法名的多个实例必须各有符号。
//   · 与【类模板的成员函数】的区别：那是"类被实例化 ⇒ 成员跟着被实例化"，
//     本测试是反过来的——类是普通类，成员自己带模板形参、按调用点实参推导。
//   · 调用点查找顺序：[over.match.best] 里非模板候选优先于模板候选，
//     故成员模板的查找刻意排在普通方法表【之后】。
//
// 预期行为：
//   · a.add(5)     ⇒ T := int，实例符号 Acc_add_int
//   · a.add(&v)    ⇒ T := int*，另一个实例符号 Acc_add_intP（★ 与上面不撞）
//   · 成员模板体内访问成员变量 base ⇒ 隐式 this 正确传递
//   程序返回 0。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:member] ★ 'add' 是成员模板（1 个模板形参，[temp.mem]）—— 调用点按实参推导实例化
//   Phase 3 [Sema]:
//     ↳ member template 'Acc::add' registered (1 template parameter(s))
//     [member] Acc.add —— 成员模板（待实参推导，[temp.mem]）
//     [call] Acc::add — 成员模板候选，开始推导（[temp.mem]）
//     [call] Acc::add → Acc_add_int (member template instantiated) → int
//   Phase 4：
//     ║ Blueprint: add <T>    Substitution: { T → int, }
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_56_member_templates.cpp -o /tmp/t56 && /tmp/t56; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_56_member_templates.cpp -o /tmp/t56c && /tmp/t56c; echo $?
// =============================================================================

struct Acc {
    int base;

    Acc(int b) : base(b) {}

    // ★ 成员模板：类本身不是模板，是【这个成员】带模板形参
    template <class T>
    T add(T x) {
        return base + x;          // 访问成员 ⇒ 实例必须带隐式 this
    }

    template <class T>
    T identity(T x) {
        return x;
    }
};

int main() {
    int bad = 0;

    // 1. T := int（成员访问 + 推导）
    Acc a(10);
    int r1 = a.add(5);
    if (r1 != 15) bad = 1;

    // 2. 同一个成员模板的另一个【实例】：T := int* ⇒ 符号必须是 Acc_identity_intP
    //    （与下面的 Acc_identity_int 是两个不同符号 —— 这正是成员模板调用的
    //     符号不能按"类名_方法名"硬拼的原因）
    int v = 2;
    int* p = a.identity(&v);
    if (p != &v) bad = 2;

    // 3. 另一个对象：隐式 this 必须跟着变（不同的 base）
    Acc b(100);
    int r2 = b.add(7);
    if (r2 != 107) bad = 3;

    // 4. 同一个成员模板回到 int 实例（应与第 1 步命中同一份实例）
    int r3 = a.identity(9);
    if (r3 != 9) bad = 4;

    return bad;
}
