// =============================================================================
// 最小 RTTI 演示：minicc 怎么触发 RTTI 和 dynamic_cast
// =============================================================================
// 编译：  ./minicc tests/mini_rtti_demo.cpp -o tests/mini_rtti_demo.s
// 组装：  clang++-18 tests/mini_rtti_demo.s -o /tmp/demo && /tmp/demo
// 看RTTI：grep "_ZTI\|type_name" tests/mini_rtti_demo.s
// 看cast：grep "__minicc_dynamic_cast" tests/mini_rtti_demo.s
//
// 预期：return 0（4 个转型全部成功）
// =============================================================================

class Animal {
public:
    int legs;
    virtual int speak() { return 0; }
};

class Dog : public Animal {
public:
    int speed;
    virtual int speak() { return 1; }
};

int main() {
    Dog* d = new Dog();
    d->speed = 30;

    // ① upcast: Dog* → Animal* (必然成功)
    Animal* a = dynamic_cast<Animal*>(d);
    if (a == nullptr) { return 1; }

    // ② downcast: Animal*(实际是Dog) → Dog* (运行时匹配成功)
    Dog* d2 = dynamic_cast<Dog*>(a);
    if (d2 == nullptr) { return 2; }

    // ③ 转型结果指向同一对象
    if (d2->speed != 30) { return 3; }

    // ④ 虚调用正常
    if (d2->speak() != 1) { return 4; }

    return 0;
}
