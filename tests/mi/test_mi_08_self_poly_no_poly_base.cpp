// =============================================================================
// 测试：本类自身多态、基类全非多态 ⇒ 无主基类（_vptr 独占 offset 0）
// =============================================================================
// 理论点：
//   - Itanium ABI 的主基类优化：primary base 只在【动态（多态）基类】里选。
//     一个多态基类都没有、而本类自身有虚函数时 ⇒【没有主基类】：
//     vptr 自己占 offset 0，全部基类子对象从 8 起摆。
//   - 对照 clang：sizeof(A)=16、offsetof(A,name)=8（B 非空，不吃空基类优化）。
// 预期行为：
//   - 布局日志：[relocate] 的三条都要说"本类自身多态 ⇒ 无主基类，_vptr 占 0"
//   - 运行返回 111 = (5+3) + (100+1+2)
// 回归背景（docs/BUGS.md B11）：
//   - 修复前 [relocate] 只认"第一个多态基类"，一个都没有时把【首个基类】当成 primary
//     摆到 offset 0，而本类自己的 _vptr 也要占 0 ⇒ 两者重叠。写基类字段即写坏虚表
//     指针，随后虚调用经 [vptr=3] 跳飞 ⇒ 实测 SIGSEGV（exit=139）。
//   - 单继承最朴素的写法就会中招，且编译期全程无警告。
// 注：clang 真实布局 X@0/Y@4；本实现子对象一律 8 字节对齐（教学简化），
//     X@8/Y@16，不重叠即自洽。
// =============================================================================

class B {
public:
    int name;
};

// 场景 1：单个非多态基类 + 本类虚函数（原 bug 现场）
class A : public B {
public:
    virtual int f() { return 5; }
};

class X {
public:
    int a;
};

class Y {
public:
    int b;
};

// 场景 2：两个非多态基类 + 本类虚函数
class Z : public X, public Y {
public:
    virtual int h() { return 100; }
};

int main() {
    A* p = new A();
    p->name = 3;                 // 修复前：写 offset 0 ⇒ 覆盖 _vptr
    int r = p->f() + p->name;    // 修复前：经 [vptr=3] 跳飞 ⇒ SIGSEGV

    Z* q = new Z();
    q->a = 1;
    q->b = 2;
    r = r + q->h() + q->a + q->b;

    return r;                    // 期望 8 + 103 = 111
}
