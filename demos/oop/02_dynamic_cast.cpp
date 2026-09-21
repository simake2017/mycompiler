// ============================================================================
// demo 09 —— RTTI：dynamic_cast 在运行期沿继承链找目标类型
// ============================================================================
// 理论：[expr.dynamic.cast] —— dynamic_cast 不做编译期静态变换，而是把问题
//   推迟到运行期：取对象 vptr → vtable[-1] 的 typeinfo → 沿继承链上行比对。
//
//   本编译器的落地方式：翻译成一次运行时助手调用
//     %rdi = 对象指针，%rsi = 目标类的 typeinfo 地址
//     callq __minicc_dynamic_cast
//   返回：成功 = 调整后的指针，失败 = 0
//
//   ★ 这个"0 表示失败"的约定与真 C++ 一致 —— 对指针类型的 dynamic_cast，
//     失败返回空指针，而不是抛异常。
//
// 看降级结果：
//   ./build-linux/minicc demos/oop/02_dynamic_cast.cpp -S -o /tmp/demo09.s
//   grep -B2 -A2 "__minicc_dynamic_cast" /tmp/demo09.s
//
// 预期：退出码 0
// ============================================================================

class Animal {
public:
    virtual int speak() { return 1; }
};

class Dog : public Animal {
public:
    int speak() { return 2; }
};

class Cat : public Animal {
public:
    int speak() { return 3; }
};

int main() {
    Dog d;
    Cat c;
    Animal* pd = &d;
    Animal* pc = &c;

    // ① 向下转型命中：pd 实际指向 Dog ⇒ 成功
    Dog* asDog = dynamic_cast<Dog*>(pd);
    if (asDog == 0) return 1;
    if (asDog->speak() != 2) return 2;

    // ② 向下转型落空：pc 实际指向 Cat，不是 Dog ⇒ 返回 0
    //    （真 C++ 里这里返回空指针，不抛异常）
    Dog* wrong = dynamic_cast<Dog*>(pc);
    if (wrong != 0) return 3;

    return 0;
}
