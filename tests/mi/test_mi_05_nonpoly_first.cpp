// =============================================================================
// 测试：非多态基类声明在前的多继承布局 (Non-polymorphic First Base Layout)
// =============================================================================
// 理论点：
//   - Itanium ABI：主基类(primary base) = 第一个【多态】基类，与声明顺序无关
//     （[class.mi] + Itanium C++ ABI 2.4 "nonvirtual base allocation"）
//   - 声明在多态基类之前的非多态基类，其子对象必须排在 primary 之后，
//     绝不能与 primary（offset 0 处的 _vptr）重叠
//   - clang oracle（-Xclang -fdump-record-layouts）：
//       D : A(非多态), P(多态) → P 是 primary@0（vptr@0, p@8），
//       A 子对象排在 P 的 nvsize 之后（clang: x@12；本实现简化为 x@16，
//       不做 nvsize 压缩，只要不重叠即自洽），d 在其后
// 预期行为：
//   - 布局日志：[relocate] 'A' → offset 16，A.x 的偏移 ≠ 0（不与 _vptr 重叠）
//   - 运行期：写 obj->x 后再虚调用不崩溃（_vptr 未被覆写），返回 57
// 回归背景：
//   - 修复前 processClassDecl 给 A.offset=0 且 P(primary).offset=0，
//     computeClassLayout 又把 bi==0 的 A 当主基类 → A.x 压在 _vptr 上，
//     任何对 x 的写会毁掉虚表指针，后续虚调用跳飞。
// 注意：沿用 test_mi_02 的调用模式——D 覆写 P::get，经 P*（主表派发）调用。
// =============================================================================

class A {
public:
    int x;
    A() {}
    A(int v) : x(v) {}
};

class P {
public:
    int p;
    virtual int get() { return p; }
    P() {}
    P(int v) : p(v) {}
};

class D : public A, public P {
public:
    int d;
    D() {}
    D(int a, int pv, int dv) : A(a), P(pv), d(dv) {}
    int get() { return p + d; }   // 覆写 P::get（primary 主表派发）
};

int main() {
    D* obj = new D(10, 20, 30);
    obj->x = 7;                   // 修复前：这一写会覆盖 offset 0 的 _vptr 低 4 字节
    P* pp = obj;                  // 向上转型到 primary 基类（offset 0，无需调整）
    int g = pp->get();            // 修复前：虚调用经由被毁的 _vptr → 跳飞/段错误
    // x=7, get()=p+d=20+30=50 → 期望 7 + 50 = 57
    return obj->x + g;
}
