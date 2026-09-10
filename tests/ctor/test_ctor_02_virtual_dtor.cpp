// =============================================================================
// 测试：虚析构函数与 delete 表达式 (Virtual Destructor & delete Expression)
// =============================================================================
// 理论点：
//   - C++ 析构函数 [class.dtor] 与虚析构多态派发 [class.virtual]
//   - delete 表达式释放对象：(*ptr->vtable[dtor_idx])(ptr) -> free(ptr)
// 预期行为：
//   - Base 拥有虚析构函数，Derived 继承 Base
//   - 通过 Base* 指针 delete 时，经由 vtable 动态派发到派生类析构函数
// =============================================================================

class Base {
public:
    int id;
    virtual ~Base() {}
};

class Derived : public Base {
public:
    int score;
    Derived(int i, int s) {
        id = i;
        score = s;
    }
    virtual ~Derived() {}
};

int main() {
    Base* b = new Derived(1, 100);
    delete b;
    return 0;
}
