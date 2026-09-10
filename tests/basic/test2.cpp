// =============================================================================
// 精简测试：聚焦展示符号表构建、符号决议、类型推导的完整过程
// =============================================================================

// ─── 1. 基础函数（展示参数注册和符号决议）───
int add(int a, int b) {
    auto result = a + b;
    return result;
}

// ─── 2. 递归函数（展示函数符号的前向注册）───
int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

// ─── 3. auto 推导链（展示 auto 被逐步抹去）───
int testAutoChain() {
    auto x = 10;
    auto y = x + 20;
    auto z = y * x;
    auto flag = z > 100;
    return z;
}

// ─── 4. 类定义（展示内存布局计算、vtable 注入）───
class Animal {
public:
    int age;

    virtual int speak() {
        return 0;
    }
};

class Dog : public Animal {
public:
    int tricks;

    virtual int speak() override {
        return 1;
    }
};

// ─── 5. 多态调用（展示虚函数决议）───
int testPoly() {
    Animal* pet = new Dog();
    auto voice = pet->speak();
    return voice;
}

// ─── 6. 字段偏移量（展示符号→偏移量的转换）───
int testFields() {
    Dog d;
    d.age = 5;
    d.tricks = 3;
    auto total = d.age + d.tricks;
    return total;
}

// ─── 7. 主函数 ───
int main() {
    auto r1 = add(3, 4);
    auto r2 = factorial(5);
    auto r3 = testAutoChain();
    auto r4 = testPoly();
    auto r5 = testFields();
    return 0;
}
