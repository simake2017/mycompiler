// ============================================================================
// demo 10 —— 多继承：对象里并排放着多个基类子对象
// ============================================================================
// 理论：单继承时"基类子对象在偏移 0"是天然的；多继承就不行了 ——
//   第二个基类只能排在第一个之后，于是【同一个对象会有不同的地址】：
//
//     C obj;                        // C : public A, public B
//     A* pa = &obj;   →  pa = &obj              （A 在偏移 0）
//     B* pb = &obj;   →  pb = &obj + sizeof(A)  ★ 指针被调整了
//
//   ★ 这就是"向上转型不是免费的"：把 C* 转成 B* 时要加一个偏移。
//     也正因为如此，B* 上的成员访问、以及 B 的虚函数覆写，
//     都需要 thunk 跳板在运行期把 this 调整回最派生类（见生成的汇编）。
//
// 看布局与 this 调整：
//   ./build-linux/minicc demos/oop/03_multiple_inheritance.cpp -S -o /tmp/demo10.s
//   grep -E "thunk|offset" /tmp/demo10.s
//
// 预期：退出码 0
// ============================================================================

class A {
public:
    int a;
};

class B {
public:
    int b;
};

class C : public A, public B {
public:
    int c;
};

int main() {
    C obj;
    obj.a = 1;
    obj.b = 2;
    obj.c = 3;

    A* pa = &obj;
    B* pb = &obj;

    // 两个基类指针都指回同一个对象，但地址不同 —— B 那一支被调整过
    if (pa->a != 1) return 1;
    if (pb->b != 2) return 2;
    if (obj.c != 3) return 3;

    return 0;
}
