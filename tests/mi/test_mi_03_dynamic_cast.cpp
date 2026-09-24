// =============================================================================
// 测试：多继承 dynamic_cast (Multiple Inheritance dynamic_cast)
// =============================================================================
// 理论点：
//   - RTTI 计数式布局：typeinfo 含基类数组 + 子对象偏移
//   - DFS 遍历 typeinfo 树：上溯/下溯/跨转型（B* → A*，经 D 中转）
//   - 非主基类指针调整：B* = obj + base_offset（次基类偏移非 0）
// 四项指标（r1..r4，期望 100 = 10 + 20 + 30 + 40）：
//   r1 上溯  ：D* → A*（D 的【主基类】，偏移 0）
//   r2 下溯  ：A* → D*（运行时类型确实是 D）
//   r3 跨转型：B* → A*（D 的【次基类】转【主基类】，须经 D 中转）
//   r4 无关类：A* → C*（C 不在继承链）⇒ 返回 0
//
// ⚠ 当前状态：编译失败 rc=1，四项【全部未被执行】—— 卡在 r3：
//   minicc 的 dynamic_cast 只认“基类 ↔ 派生类”的直接关系，跨支路报
//     [ERROR] [Semantic Error] 61:14: Cannot dynamic_cast 'B*' to 'A*': unrelated class types
//   实现跨转型后本文件自动变绿（logdiff 基线已固化当前 rc=1）。
//
// 注：原文用 `?:` 三元写指标，而 minicc 尚无三元（ROADMAP 主线 C）⇒ 已改写为 if；
//     否则错误停在【词法期】(Unexpected character '?')，连上面这个真缺口都看不见。
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
    int r1 = 0;
    if (d1 != 0) { r1 = 10; }

    // 下溯：A* → D*
    D* d2 = dynamic_cast<D*>(pa);
    int r2 = 0;
    if (d2 != 0) { r2 = 20; }

    // 跨转型：B* → A*（经 D 中转）
    B* pb = obj;
    A* pa2 = dynamic_cast<A*>(pb);
    int r3 = 0;
    if (pa2 != 0) { r3 = 30; }

    // 无关类：A* → C*（无公共祖先）→ 返回 0
    C* pc = dynamic_cast<C*>(pa);
    int r4 = 0;
    if (pc == 0) { r4 = 40; }

    // 期望：10 + 20 + 30 + 40 = 100
    return r1 + r2 + r3 + r4;
}
