// =============================================================================
// 测试：次基类里【未被覆写】的虚函数槽，必须原样指向更上游基类的实现
// =============================================================================
// 理论点：
//   - [class.mi] / Itanium ABI：第一个【多态】基类当 primary（共享主表、字段不加前缀），
//     其余多态基类各占一段 secondary vtable（表头带 offset-to-top，槽位覆写时配 thunk）。
//   - 次表槽里的地址同样是一个**符号名**。基类自己没覆写某个虚函数时，那个槽指向的是
//     **更上游**基类的实现 —— 所以这个名字必须从上游【原样透传】，不能按"次基类名"
//     重造：`B_f` 这种符号根本没人定义（除非 B 自己覆写了 f）。
//   - 这正是"拿给人看的字符串当机器用的键"的又一次现形（承 docs/BUGS.md B13~B15、
//     docs/learn/23 的同一句教训）：名字是**路径**（谁提供的实现）而不是**位置**（谁的表）。
// 预期行为：
//   - 菱形 Diamond : P, Q（P 主、Q 次，二者都从 X 继承且都没覆写 f/g）
//     ⇒ 次表槽必须是 X_f / X_g_1（旧实现捏出 Q_f ⇒ 链接期 undefined reference）
//   - 次基类自己【覆写】时仍走 thunk（this 调整）—— 修复不能把这半边带坏
//   - 运行返回 0
// 回归背景（docs/BUGS.md B20 缺陷 b）：
//   - 修复前 `tests/mi/test_mi_04_error.cpp`（菱形）就卡在这里：rc=1，
//     `undefined reference to 'Q_f'`。当时把这句链接错误当成"菱形本该被拒收"的
//     证据写进了测试头 —— 实测 clang rc=0：非虚继承的菱形在标准下**合法**
//     （两个 X 子对象，只有访问歧义才报错），所以那是本实现的缺陷，不是程序的错。
//   - 该文件现已改为真正的错误用例（私有继承），菱形正例搬到这里。
// =============================================================================

class X {
public:
    int x;
    virtual int f() { return 7; }
    virtual int g(int k) { return k; }
};

class P : public X {
};

class Q : public X {
};

// 菱形：标准下合法（两个 X 子对象），P 当主基类、Q 当次基类
class Diamond : public P, public Q {
};

// 次基类【自己覆写】的那半边（thunk 路径）：
// Z 主、B 次；B 覆写了 h ⇒ 次表槽指本类实现 + thunk，C 又是 B 的上游
class C {
public:
    int c;
    virtual int h() { return 1; }
};

class B : public C {
public:
    int b;
    virtual int h() { return 2; }
};

class Z {
public:
    int z;
    virtual int k() { return 9; }
};

class D : public Z, public B {
};

int main() {
    Diamond d;

    Q* pq = &d;                       // 次基类指针上调
    if (pq->f() != 7) return 1;       // 次表槽：X_f（透传，不是 Q_f）
    if (pq->g(5) != 5) return 2;      // 带参：X_g_1（同时钉住 B20 缺陷 a）

    P* pp = &d;                       // 主基类指针
    if (pp->f() != 7) return 3;

    D dd;
    B* pb = &dd;                      // 次基类【覆写】⇒ thunk 跳板
    if (pb->h() != 2) return 4;

    C* pc = &dd;                      // 上游指针：同一槽位也应命中 B::h
    if (pc->h() != 2) return 5;

    Z* pz = &dd;                      // 主基类自身
    if (pz->k() != 9) return 6;

    return 0;
}
