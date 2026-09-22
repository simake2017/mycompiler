// =============================================================================
// tests/lang/test_basics_01.cpp —— 语言基础补齐批（第一、二梯队）
// =============================================================================
// 四项一次性跑通，每项对应一条此前缺失或**错误拒绝合法程序**的能力：
//
//   ① [class.derived]/2   struct 的默认继承级别是 public
//                         （此前一律强制写 'public'，把 `struct D : Base` 拒掉）
//   ② [expr.unary.op]/1   一元 * 解引用：读 / 写（解引用是【左值】）/ 链式
//   ③ [dcl.fct]/7         后置 const 成员函数（`int get() const`）
//   ④ [class.static]/2    类内 static 成员函数：**没有隐式 this**，
//                         参数寄存器从 rdi 起算（有参时必须验证，否则整体错位一格）
//
// 观测约定：本项目不链接 libc，没有 printf，故用【退出码】汇总：
//     bad 累加"哪一项不符预期"，run-rc = 0 表示四项全过，
//     非 0 时其值就是失败项的编号（1..4），便于定位。
//
// 期望（clang++-18 -std=c++20 与本编译器必须一致）：
//     编译 rc = 0，运行 rc = 0
//
// 语义 oracle：
//     clang++-18 -std=c++20 tests/lang/test_basics_01.cpp -o /tmp/b1 && /tmp/b1; echo $?
// =============================================================================

// ── ① struct 默认 public 继承 ────────────────────────────────────────────────
// 不写 public：C++ 里 struct 的默认继承级别就是 public（class 才是 private）
struct Base1 {
    int v;
};
struct Derived1 : Base1 {   // ← 此前在此报「仅支持 public 继承（每个基类前需写 'public'）」
    int w;
};

// ── ② 一元 * 解引用 ──────────────────────────────────────────────────────────
int derefRead() {
    int  a = 5;
    int* p = &a;            // p 指向 a
    return *p;              // ⇒ 5
}

int derefWrite() {
    int  a = 5;
    int* p = &a;
    *p = 30;                // 解引用产生【左值】，故可作赋值目标（[expr.ass]/3）
    return a;               // ⇒ 30（证明真的写回了 a，不是写了个临时）
}

int derefChain() {
    int   a  = 7;
    int*  p  = &a;
    int** pp = &p;
    return **pp;            // 两次解引用 ⇒ 7
}

// ── ③ 后置 const 成员函数 ────────────────────────────────────────────────────
struct ConstHolder {
    int v;
    int get() const { return v + 1; }   // ← 此前在此报「Expected '{' or ';'」
    int put()       { return v + 2; }   // 非 const 版本仍正常
};

// ── ④ static 成员函数（无隐式 this）─────────────────────────────────────────
struct StaticHolder {
    int v;
    static int zero() { return 100; }              // 无参：寄存器错位在这里看不出来
    static int add(int a, int b) { return a + b; } // ★ 有参：形参必须从 rdi 起，
                                                   //   若仍按"rdi=this"右移一格，
                                                   //   a 会取到 rsi、b 取到 rdx 的垃圾
                                                   //   （曾返回 4 而非 7）
    int inst() { return v; }                       // static 与实例方法共存
};

int main() {
    int bad = 0;   // 0 = 全过；否则值即失败项编号

    // ① struct 默认继承：子对象按偏移摆放，字段可读可写 ⇒ 3 + 4 = 7
    Derived1 d;
    d.v = 3;
    d.w = 4;
    if (d.v + d.w != 7) bad = 1;

    // ② 一元 *：读 5、写回 30、链式 7
    if (derefRead()  != 5)  bad = 2;
    if (derefWrite() != 30) bad = 2;
    if (derefChain() != 7)  bad = 2;

    // ③ 后置 const：11 + 12 = 23
    ConstHolder h;
    h.v = 10;
    if (h.get() + h.put() != 23) bad = 3;

    // ④ static 成员：100 + 7（3+4）+ 实例字段 5 = 112
    StaticHolder s;
    s.v = 5;
    if (StaticHolder::zero() + StaticHolder::add(3, 4) + s.inst() != 112) bad = 4;

    return bad;
}
