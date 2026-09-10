// =============================================================================
// 测试：dynamic_cast 失败路径（返回 0）+ 继承图打印
// =============================================================================
// 理论点：
//   运行时类型不匹配时，指针形式的 dynamic_cast 返回空指针（[expr.dynamic.cast]/7）。
//   本项目 __minicc_dynamic_cast 沿 typeinfo 基类链上溯未命中 → 返回 0。
//   同时本测试触发语义阶段的"继承图打印"：Pass 1 之后输出整棵继承树，
//   验证 4 层继承链（Animal → Dog → Puppy → Puppy2）完整可见。
// 预期行为：
//   1. Animal*（实际是 Cat）→ Dog*：失败，结果为 nullptr
//   2. Animal*（实际是 Dog）→ Puppy*：失败（实际类型不是 Puppy 及其子类）
//   3. 成功的转型不受影响（Dog → Puppy2 的 upcast）
// =============================================================================

class Animal {
public:
    virtual int speak() { return 0; }
};

class Dog : public Animal {
public:
    virtual int speak() { return 1; }
};

class Cat : public Animal {
public:
    virtual int speak() { return 2; }
};

class Puppy : public Dog {
public:
    int age;
    virtual int speak() { return 3; }
};

class Puppy2 : public Puppy {
public:
    virtual int speak() { return 4; }
};

int main() {
    Cat* c = new Cat();
    Dog* dog = new Dog();
    Puppy2* p2 = new Puppy2();

    // ① 兄弟分支转型失败：Cat 与 Dog 无祖先关系（语义期静态可达性检查
    //    只要求"同一族"，Cat→Dog 属同族但运行时失败）
    Animal* ac = dynamic_cast<Animal*>(c);
    Dog* fail1 = dynamic_cast<Dog*>(ac);
    if (fail1 != nullptr) { return 1; }

    // ② downcast 目标过深失败：Dog 不是 Puppy
    Animal* ad = dynamic_cast<Animal*>(dog);
    Puppy* fail2 = dynamic_cast<Puppy*>(ad);
    if (fail2 != nullptr) { return 2; }

    // ③ 深层继承上的 upcast 仍然成功：Puppy2 → Animal
    Animal* ok = dynamic_cast<Animal*>(p2);
    if (ok == nullptr) { return 3; }

    // ④ 从最深派生类 upcast 到中间层也成功：Puppy2 → Puppy
    Puppy* okMid = dynamic_cast<Puppy*>(p2);
    if (okMid == nullptr) { return 4; }

    return 0;
}
