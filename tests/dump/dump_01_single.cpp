// =============================================================================
// dump 测试 01：单继承链
// =============================================================================
// 测试目的：验证单继承的类层次和内存布局输出
// 继承关系：A → B → C（三层单继承）
// 运行命令：
//   ./minicc tests/dump/dump_01_single.cpp --dump-hierarchy
//   ./minicc tests/dump/dump_01_single.cpp --dump-layout
// =============================================================================

class A {
public:
    int a;
    virtual int getA() { return a; }
};

class B : public A {
public:
    int b;
    virtual int getB() { return b; }
    virtual int getA() { return a; }  // override
};

class C : public B {
public:
    int c;
    virtual int getC() { return c; }
    virtual int getA() { return a; }  // override
    virtual int getB() { return b; }  // override
};

int main() {
    C* obj = new C();
    obj->a = 1;
    obj->b = 2;
    obj->c = 3;
    return obj->getA() + obj->getB() + obj->getC();
}
