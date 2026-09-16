// =============================================================================
// tests/tmpl/test_tmpl_48_class_type_aliases.cpp
// =============================================================================
// 考察理论点：类体内的【类型别名】—— `using X = T;` / `typedef T X;`
//   · [dcl.typedef] / [temp.alias]：别名【不是新类型】，只是一次名字替换
//     （alias 与底层类型在类型系统里【完全等价】，不能靠别名做重载/特化判别）
//   · [basic.lookup.unqual]：类体内直接写 `X x;` 要先在【类作用域】找 X
//   · [basic.lookup.qual]：类外写 `Cls::X` / `Cls<Args>::X` 走限定名查找
//   · [temp.inst]：类模板被实例化时，别名目标里的形参要跟着替换
//     （Box<int>::type 必须是 int，而不是没替换的裸 T）
//
// 三处使用点（逐条对应上面的规则）：
//   ① Plain::Int y;        —— 类外限定名（非模板类）
//   ② Box<int>::type x;    —— 类外限定名（模板实例）
//   ③ 类体内 `type v;`     —— 类作用域内的非限定名
//
// 预期：退出码 173（已用 clang++-18 核对：clang 173）
//   p.a*100 + p.b*10 + bi.v + x*10 + y - 6
//   = 1*100  + 2*10   + 3    + 5*10 + 6 - 6 = 173
//
// 运行：
//   ./minicc tests/tmpl/test_tmpl_48_class_type_aliases.cpp -o /tmp/t48 && /tmp/t48; echo $?
//
// ★ 本用例顺带钉住一个【与别名无关】的 codegen bug（见文件尾注释）：
//   帧大小曾漏算"帧基 + 形参 spill 区"，使最深的几个局部槽位落在 rsp 之下，
//   被 `pushq` 暂存与 `callq` 返回地址踩掉 —— 本用例的 8 个局部
//   （p / bi 占 2 槽 / x / z / q / r / y）正好把帧撑到边界，于是暴露。
// =============================================================================

struct Plain {
    using Int = int;        // [dcl.typedef] C++11 写法
    typedef int Int2;       // 老写法，语义相同

    Int   a;                // ③ 类作用域内用别名
    Int2  b;
};

template <typename T>
struct Box {
    using type = T;         // 别名目标含模板形参 → 实例化时必须替换
    using ptr  = T*;

    type  v;                // ③ 类作用域内用别名
    ptr   p;
};

int main() {
    Plain p;
    p.a = 1;
    p.b = 2;

    Box<int> bi;
    bi.v = 3;

    Box<int>::type x = 5;   // ② Box<int>::type ⇒ int
    int z = 7;
    Box<int>::ptr  q = &z;  // ② Box<int>::ptr  ⇒ int*
    int* r = q;
    Plain::Int y = 6;       // ① Plain::Int     ⇒ int

    r = &z;
    q = r;

    return p.a * 100 + p.b * 10 + bi.v + x * 10 + y - 6;   // 173
}

// ─────────────────────────────────────────────────────────────────────────────
// 附：本用例暴露的 codegen 帧大小 bug（已在 src/codegen.cpp:estimateFrameSize 修）
// ─────────────────────────────────────────────────────────────────────────────
// 症状：变量【单独】读是对的，一旦参与需要压栈暂存的二元表达式、或此前
//       发生过一次函数调用，读到的就是邻居的值。
// 最小复现（与此处的别名无关，纯局部变量就能触发）：
//
//     int main() {
//         int a = 1; int b = 2; int c = 3; int d = 4;
//         int e = 5; int f = 6; int g = 7; int h = 8;
//         return a + h;          // 修复前返回 2，修复后 9（clang 9）
//     }
//
// 根因：局部偏移从 -8 起步（paramOffset），每个 spill 的形参再占 8 字节，
//       而 estimateBlockSize 只数了局部变量的尺寸 ⇒ 帧比最深偏移浅 8 字节。
//       求值 `a + h` 时先 `pushq %rax` 暂存左操作数 —— 它落在 rsp-8，
//       正好就是 h 的槽位；随后的 `movq -N(%rbp), %rax`（读 h）拿到的是
//       刚压进去的 a。同理 `callq` 压入的返回地址也在 rsp-8。
// 对照 clang：帧大小与局部布局出自【同一份】分配器
//       （X86FrameLowering::determineFrameLayout），不存在两处各算一份。
