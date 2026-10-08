// =============================================================================
// 测试：成员模板的 static 说明符写在【形参表之后】（[temp.mem] / [class.static]）
// =============================================================================
// 考察理论点：
//   · 语法顺序：`template-head` 必须在声明的最前面（[temp.pre]：template-declaration
//     := template-head declaration），故 static 只能写在 **形参表之后**：
//         template <class U> static U f(U x);     ← 合法（clang rc=0）
//         static template <class U> U f(U x);     ← 非法（clang: expected member name
//                                                    or ';' after declaration specifiers）
//     与 [B19](docs/BUGS.md) 之前的行为正好相反：当时只有"static 在最前"那条路径
//     被解析器认到（1641 起的 match），标准写法反而一路落进 parseMethodDecl ⇒
//     [Parse Error] … at 'static': Expected type name —— **拒收合法程序**。
//   · 与 [B18](docs/BUGS.md) 的关系：这条修的是"识别位置"，B18 修的是"类实例化时
//     把成员模板搬进实例类"。两者在同一个分支上（parser.cpp 的类体循环），
//     一个管声明怎么读进来、一个管实例化怎么带过去。
//   · static 的语义面（[class.static]/2）：没有隐式 this。★ 本项目里它的可观测差别
//     只出现在"不带对象调用"（`O::f(3)`）那种写法上 —— 而那是另一种未实现的语法；
//     带对象调用（`o.f(3)`）与普通成员模板走同一条路，故这里断言的是**结果正确**
//     而不是"this 被省略"。
//
// 预期行为：
//   · 普通类里的静态成员模板：`O o; o.sub(10, 3)` ⇒ 7
//   · 类模板里的静态成员模板（B18 × B19 的交叉点）：`Box<int> b; b.f(3)` ⇒ 3
//   · 同类的非 static 成员模板（msub）保持可用，作对照
//   程序返回 0。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]：与普通成员模板一致
//     [parse:member] ★ 'sub' 是成员模板（1 个模板形参，[temp.mem]）—— 调用点按实参推导实例化
//   Phase 3 [Sema]：
//     ↳ member template 'O::sub' registered (1 template parameter(s))
//     [call] O::sub → O_sub_int (member template instantiated) → int
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_70_member_template_static.cpp -o /tmp/t70 && /tmp/t70; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_70_member_template_static.cpp -o /tmp/t70c && /tmp/t70c; echo $?
// =============================================================================

struct O {
    int pad;

    // ★ 标准写法：static 在【形参表之后】（此前这一步是 Parse Error）
    template <class U>
    static U sub(U a, U b) { return a - b; }

    // 对照：非 static 的成员模板（本来就通）
    template <class U>
    U msub(U a, U b) { return a - b; }
};

template <class T>
struct Box {
    T seed;

    // ★ 交叉点：成员模板住在【类模板】里（B18）× static 写在形参表之后（B19）
    template <class U>
    static U pick(U x) { return x; }
};

int main() {
    int bad = 0;

    O o;
    o.pad = 1;

    // 1. 静态成员模板（标准写法）
    if (o.sub(10, 3) != 7) bad = 1;

    // 2. 非 static 对照：同名的另一种形态不受影响
    if (o.msub(10, 3) != 7) bad = 2;

    // 3. 类模板里的静态成员模板
    Box<int> b;
    b.seed = 5;
    if (b.pick(3) != 3) bad = 3;

    return bad;
}
