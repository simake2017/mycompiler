// ============================================================================
// demo 07 —— 虚函数：同一个调用点，运行期才决定走哪个函数体
// ============================================================================
// 理论：虚函数分派（[class.virtual]，布局见 Itanium ABI 的 vtable 约定）。
//
//   编译期：p->speak() ⇒ 查符号表得知 speak 是 vtable 第 0 项，发射间接调用
//   运行期：[p+0] → vtable → [vtable+0] → 真实函数地址 → 执行
//
//   p 的静态类型是 Animal*，但它指向的对象其实是 Dog —— 所以这里【不能】
//   直接 call Animal_speak，必须绕虚表走一圈。
//
// 看三部曲（生成的汇编里直接可见）：
//   ./build-linux/minicc demos/oop/01_vtable.cpp -S -o /tmp/demo07.s
//   grep -A3 "vptr" /tmp/demo07.s
//     movq (%rax), %rax      # (a) 从对象读出 _vptr
//     movq 0(%rax), %rax     # (b) 取 vtable[0]
//     callq *%rax            # (c) 间接调用
//
// 预期：退出码 0
// ============================================================================

class Animal {
public:
    virtual int speak() { return 1; }
};

class Dog : public Animal {
public:
    int speak() { return 2; }      // 覆写：Dog 的 vtable[0] 换成这个
};

int main() {
    Dog d;
    Animal* p = &d;

    // 经基类指针调用 ⇒ 走虚表 ⇒ 分派到 Dog::speak
    if (p->speak() != 2) return 1;

    // 直接对对象调用 ⇒ 静态绑定，同样得到 Dog::speak
    if (d.speak() != 2) return 2;

    return 0;
}
