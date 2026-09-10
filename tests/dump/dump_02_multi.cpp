// =============================================================================
// dump 测试 02：多继承
// =============================================================================
// 测试目的：验证多继承的类层次和内存布局输出
// 继承关系：D 同时继承 A 和 B
// 运行命令：
//   ./minicc tests/dump/dump_02_multi.cpp --dump-hierarchy
//   ./minicc tests/dump/dump_02_multi.cpp --dump-layout
// =============================================================================

class A {
public:
    int a;
    virtual int getA() { return a; }
};

class B {
public:
    int b;
    virtual int getB() { return b; }
};

class D : public A, public B {
public:
    int d;
    virtual int getD() { return d; }
    virtual int getA() { return a; }  // override
    virtual int getB() { return b; }  // override
};

int main() {
    D* obj = new D();
    obj->a = 10;
    obj->b = 20;
    obj->d = 30;
    return obj->getA() + obj->getB() + obj->getD();
}
