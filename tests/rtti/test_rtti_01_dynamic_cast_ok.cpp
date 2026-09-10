// =============================================================================
// 测试：dynamic_cast 成功路径（downcast + upcast）
// =============================================================================
// 理论点：
//   dynamic_cast<T*>(p) 在运行时检查对象实际类型能否到达目标类型
//   （[expr.dynamic.cast]）。实现依据 Itanium ABI：对象 → vptr →
//   vtable[-1] 的 typeinfo → 沿 typeinfo 第三槽（基类指针，__si 风格）
//   逐级上溯，与目标 typeinfo 地址比较。
// 预期行为：
//   1. Dog* → Animal*（upcast）：成功，非 nullptr
//   2. Animal*（实际是 Dog）→ Dog*（downcast）：成功，非 nullptr
//   3. 转型结果指向同一对象：读得到原字段、虚调用行为不变
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

    // ① upcast：Dog* → Animal*，必然成功
    Animal* a1 = dynamic_cast<Animal*>(d);
    if (a1 == nullptr) { return 1; }

    // ② downcast：Animal*（动态类型是 Dog）→ Dog*，运行时匹配成功
    Animal* a2 = dynamic_cast<Animal*>(d);
    Dog* d2 = dynamic_cast<Dog*>(a2);
    if (d2 == nullptr) { return 2; }

    // ③ 转型结果指向同一对象：读得到原字段
    if (d2->speed != 30) { return 3; }

    // ④ 转型结果可正常发起虚调用
    if (d2->speak() != 1) { return 4; }

    return 0;
}
