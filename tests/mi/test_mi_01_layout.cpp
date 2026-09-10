// =============================================================================
// 测试：多继承对象布局 (Multiple Inheritance Object Layout)
// =============================================================================
// 理论点：
//   - Itanium ABI 主基类优化 [class.mi]：第一个多态基类共享主表
//   - 次基类子对象独立偏移，各自带 _vptr 指向次表段
//   - 字段名限定（"BaseName.field"）避免多基类同名冲突
// 预期行为：
//   - A 和 B 各有独立虚函数和字段
//   - D 继承 A、B，总大小 = sizeof(A) + sizeof(B) + sizeof(D自身)
//   - 通过 D 对象访问 A.a 和 B.b 得到正确值
// =============================================================================

class A {
public:
    int a;
    virtual int getA() { return a; }
    A() {}
    A(int x) : a(x) {}
};

class B {
public:
    int b;
    virtual int getB() { return b; }
    B() {}
    B(int x) : b(x) {}
};

class D : public A, public B {
public:
    int d;
    D() {}
    D(int x, int y, int z) : A(x), B(y), d(z) {}
};

int main() {
    D* obj = new D(10, 20, 30);
    // 验证字段访问
    int a_val = obj->a;
    int b_val = obj->b;
    int d_val = obj->d;
    // 期望：a=10, b=20, d=30 → 返回 10+20+30 = 60
    return a_val + b_val + d_val;
}
