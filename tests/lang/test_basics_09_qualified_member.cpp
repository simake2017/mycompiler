// =============================================================================
// 测试：限定名成员访问 `Q::m` / `obj.Q::m`（[expr.prim.id.qual]、[expr.ref]）
// =============================================================================
// 理论点：
//   · 限定名的**唯一用途**是绕过名字隐藏（[class.member.lookup]/3）：派生类一旦
//     声明了 `v`，基类的 `v` 就被隐藏，唯一的取法是把限定者写出来。
//   · `S::v` 出现在成员函数体内不是"限定名查找"，而是 [expr.prim.id.general]/3 的
//     **隐式 this 成员访问**。clang 的 AST 就是这么建的：
//         `-MemberExpr 'int' lvalue ->v   └─CXXThisExpr 'S *' implicit this
//   · 于是它必须落到与裸名 `v` **同一条**查找路径上（字段查找 + 隐式 this），
//     只是查找的**范围**被限定在 Q 之内。
//   · ★ 汇编层的关键：写限定名要求"偏移取 Q 那一条"，而按裸名查会命中派生类
//     自己那一条（findField 的"自身字段优先"）—— 名字是给人看的，**位置**才是
//     机器要的。故 Sema 解析后把权威偏移回填到节点上，CodeGen 优先采用
//     （BUGS.md B21；与 B10/B20/B22 的"符号回填"同一形状）。
// 预期行为：
//   · 类内 `S::v`（当前类自己）、`Base::v`（基类）均可读写
//   · 类外 `obj.Base::v` 亦可读写，且**命中基类那一条**而不是被隐藏的那条
//   · 运行返回 0（下面用 d.Base::v * 10 + d.v 验证两条确实指向不同的槽位）
// 对照 clang：clang++-18 -std=c++20 编译运行同样 rc=0。
// 回归背景（docs/BUGS.md B21）：修复前 ① 报 `Undefined variable 'S::v'`（rc=1），
//   ② `obj.Base::v` 停在**语法期**（`Expected ';' after expression`）——
//   同一个缺口的两半：语义层没通路、语法层压根不认这种写法。
// =============================================================================

struct Base {
public:
    int v;
};

struct Derived : public Base {
public:
    int v;                       // 隐藏 Base::v

    int readBase() { return Base::v; }        // 类内限定名（隐式 this）
    int readOwn()  { return v; }              // 裸名 —— 命中自身那一条
    void writeBase(int x) { Base::v = x; }    // 限定名做赋值目标
};

struct Self {
public:
    int v;
    int readSelf() { return Self::v; }        // 限定者就是当前类
};

int main() {
    Derived d;
    d.v = 4;            // 派生类自己的槽位
    d.Base::v = 3;      // 基类的槽位（类外限定名）
    d.writeBase(3);
    // readBase() = 3、readOwn() = 4、d.Base::v = 3、d.v = 4
    // 3 + 4 + 3 * 10 + 4 - 41 = 0
    Self s;
    s.v = 2;
    return d.readBase() + d.readOwn() + d.Base::v * 10 + d.v - 41
         + (s.readSelf() - 2);
}
