// =============================================================================
// 测试：多继承错误拒绝 (Multiple Inheritance Error Cases)
// =============================================================================
// 理论点：
//   - 私有/保护继承：本实现仅支持 public 继承
//   - 菱形/重复基类：同一基类出现两次 → 报错（无虚继承支持）
// 预期行为：
//   - 编译应报错，不产生汇编
// 期望报错文案：
//   - 私有继承："only public inheritance is supported"
//   - 菱形基类："diamond/duplicate base 'X' detected ... not supported"
// =============================================================================
// 注意：本文件包含两个测试场景，运行测试时分别编译两段代码。
// 场景1（菱形检测）：
class X { public: int x; virtual int f() { return 0; } };
class P : public X {};
class Q : public X {};
class Diamond : public P, public Q { public: int d; };

int main() {
    Diamond* obj = new Diamond();
    return 0;
}
