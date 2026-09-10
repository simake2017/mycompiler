// =============================================================================
// 测试：全非多态基类的多继承布局 (All Non-Polymorphic Bases Layout)
// =============================================================================
// 理论点：
//   - Itanium ABI：全部基类都非多态时，第一个基类视作 primary（占 offset 0）
//   - 其余基类子对象必须从 primary 尾部依次对齐摆放，不得重叠
// 预期行为：
//   - 布局日志：[non-poly] 'A' / [non-poly] 'B' / [relocate] 'B' → offset 8
//   - A.x、B.y、D.d 三者互不重叠，构造后读取正确，返回 60
// 回归背景：
//   - 修复前 [relocate] 的 place 计算被 if(anyPoly) 守卫：全非多态时 place
//     停在 0 → B 被 alignTo(0,8)=0 放到 offset 0，B.y 与 A.x 重叠，
//     构造 B(20) 把 x 覆盖成 20，运行返回 70 而非 60。
// 注意：clang 真实布局 x@0/y@4/d@8/size=12（4 字节对齐）；本实现子对象
//   一律 8 字节对齐（教学简化），x@0/y@8/d@16/size=20，不重叠即自洽。
// =============================================================================

class A {
public:
    int x;
    A() {}
    A(int v) : x(v) {}
};

class B {
public:
    int y;
    B() {}
    B(int v) : y(v) {}
};

class D : public A, public B {
public:
    int d;
    D() {}
    D(int a, int b, int dv) : A(a), B(b), d(dv) {}
};

int main() {
    D* obj = new D(10, 20, 30);
    // 期望：x=10, y=20, d=30 → 返回 60（修复前 B 构造踩掉 x → 70）
    return obj->x + obj->y + obj->d;
}
