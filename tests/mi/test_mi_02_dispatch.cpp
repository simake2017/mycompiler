// =============================================================================
// 测试：多继承虚函数派发 (Multiple Inheritance Virtual Dispatch)
// =============================================================================
// 理论点：
//   - 主表虚调用：直接经主 vptr 派发（offset-to-top=0，无需 this 调整）
//   - 次表虚调用：经次 vptr 派发，thunk 跳板读 vptr[-2]=offset-to-top 归顶 this
//   - 覆写函数读派生类字段验证 this 已正确归顶
// 预期行为：
//   - D 覆写 A::getA() 和 B::getB()，各自读 D.d（偏移 32）
//   - 经 A* 和 B* 调用虚函数，均返回 D.d 的值
// =============================================================================

class A {
public:
    int a;
    virtual int getA() { return a; }
    virtual int whoAmI() { return 1; }
    A() {}
};

class B {
public:
    int b;
    virtual int getB() { return b; }
    virtual int whoAmI() { return 2; }
    B() {}
};

class D : public A, public B {
public:
    int d;
    D() {}
    int getA() { return d; }   // 覆写 A::getA，读 D.d
    int getB() { return d; }   // 覆写 B::getB，读 D.d
    int whoAmI() { return 3; } // 覆写
};

int main() {
    D* obj = new D();
    obj->a = 100;
    obj->b = 200;
    obj->d = 300;

    // 主表派发：A::getA 被 D 覆写，经 A* 调用应返回 300
    A* pa = obj;
    int via_a = pa->getA();

    // 次表派发：B::getB 被 D 覆写，经 B* 调用（thunk 归顶 this）应返回 300
    B* pb = obj;
    int via_b = pb->getB();

    // whoAmI 也覆写
    int who_a = pa->whoAmI();
    int who_b = pb->whoAmI();

    // 期望：300 + 300 + 3 + 3 = 606
    return via_a + via_b + who_a + who_b;
}
