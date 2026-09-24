// =============================================================================
// 测试：继承来的成员方法/字段要查得到（成员访问沿基类链）
// =============================================================================
// 理论点：
//   - [class.member.lookup]：成员名字查找沿【基类链】走，不只是本类的成员表。
//     本实现把字段扁平化进了 classLayout，方法却在各基类的方法表里 ——
//     于是"字段能查到、方法查不到"这种不对称曾经出现过。
//   - 隐藏（[class.member.lookup]/3）：派生类声明同名方法时，基类那个被【隐藏】，
//     查找必须命中派生类的（而不是先到先得）。
//   - 调用点与查找点必须同源：inferMember（取成员）与 inferCall（调用）本是两条
//     通路，谓词收口到 findMethodInClass 后，二者不会各自演化。
// 预期行为：
//   - 多级链 C : B : A，`p->g()` 命中 A::g，日志给出 via 'A'
//   - 同名隐藏：D::g 胜出，返回 2 而不是基类的 1
//   - 运行返回 5 + 2 + 1 = 8
// 回归背景（docs/BUGS.md B12）：
//   - 修复前 inferMember 只扫本类方法表，`D d; d.g()`（g 在基类）报
//     [Semantic Error] No member 'g' in class 'D' —— 合法程序被拒。
//   - 讽刺的是 inferCall 另有一条搜基类的 BFS，但它被更早的 inferType(callee)
//     → inferMember 的 error() 挡死，永远走不到。
// =============================================================================

class A {
public:
    int g() { return 5; }
};

class B : public A {
};

class C : public B {
};

// 同名隐藏：D::g 隐藏 A::g
class Base {
public:
    int g() { return 1; }
};

class D : public Base {
public:
    int g() { return 2; }   // 非虚，仅隐藏
};

int main() {
    C* p = new C();
    int r = p->g();        // 多级链：C → B → A 命中 A::g = 5

    D d;
    r = r + d.g();         // 隐藏：命中 D::g = 2（不是 Base::g 的 1）

    Base b;
    r = r + b.g();         // 基类自身仍解析为 Base::g = 1

    return r;              // 期望 8
}
