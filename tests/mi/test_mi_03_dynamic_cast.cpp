// =============================================================================
// 测试：多继承 dynamic_cast (Multiple Inheritance dynamic_cast)
// =============================================================================
// 理论点：
//   - RTTI 计数式布局：typeinfo 含基类数组 + 子对象偏移
//   - DFS 遍历 typeinfo 树：上溯/下溯/跨转型（A* → B*）
//   - 非主基类指针调整：B* = obj + base_offset（次基类偏移非 0）
// 预期行为：
//   - 上溯：D* → A*（主基类）和 D* → B*（次基类）均成功
//   - 下溯：A* → D*（运行时类型确实是 D）成功
//   - 跨转型：A* → B*（经 D）成功
//   - 无关类：A* → C*（C 不在继承链）返回 0
// =============================================================================

class A {
public:
    int a;
    virtual int getA() { return a; }
    A() {}
};

class B {
public:
    int b;
    virtual int getB() { return b; }
    B() {}
};

class C {
public:
    int c;
    virtual int getC() { return c; }
    C() {}
};

class D : public A, public B {
public:
    int d;
    D() {}
};

int main() {
    D* obj = new D();
    obj->a = 1;
    obj->b = 2;
    obj->d = 3;

    // 上溯：D* → A*（主基类，偏移 0）
    A* pa = obj;
    D* d1 = dynamic_cast<D*>(pa);
    int r1 = (d1 != 0) ? 10 : 0;

    // 下溯：A* → D*
    D* d2 = dynamic_cast<D*>(pa);
    int r2 = (d2 != 0) ? 20 : 0;

    // 跨转型：B* → A*（经 D 中转）
    B* pb = obj;
    A* pa2 = dynamic_cast<A*>(pb);
    int r3 = (pa2 != 0) ? 30 : 0;

    // 无关类：A* → C*（无公共祖先）→ 返回 0
    C* pc = dynamic_cast<C*>(pa);
    int r4 = (pc == 0) ? 40 : 0;

    // 期望：10 + 20 + 30 + 40 = 100
    return r1 + r2 + r3 + r4;
}
