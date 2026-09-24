// =============================================================================
// 测试：多继承错误拒绝 (Multiple Inheritance Error Cases)
// =============================================================================
// 理论点：
//   - 私有/保护继承：本实现仅支持 public 继承
//   - 菱形/重复基类：同一基类出现两次 → 报错（无虚继承支持）
// 设计意图（本该拒收）：
//   - 私有/保护继承 ⇒ "only public inheritance is supported"
//   - 菱形/重复基类 ⇒ "diamond/duplicate base 'X' detected ... not supported"
//
// ⚠ 当前状态：与上面的设计意图【不符】，而且暴露了一个真缺陷 ——
//   菱形（重复基类 X）被【静默接受】，两道检测都没触发，编译一路成功，
//   直到链接期才失败（rc=1）：
//     undefined reference to 'Q_f'
//   Q 从 X 继承了虚函数 f，vtable 条目引用 Q_f，却没有任何地方发射 Q_f ——
//   f 的实现只存在于 X（符号 X_f），这条【继承链上的符号转发】是缺的。
//
// 复现：./minicc tests/mi/test_mi_04_error.cpp -o /tmp/m4   # rc=1，报 Q_f 未定义
// （原注释称“本文件包含两个测试场景”，实际只有场景 1：私有继承那段从未写进来）
// =============================================================================
// 场景1（菱形检测）：
class X { public: int x; virtual int f() { return 0; } };
class P : public X {};
class Q : public X {};
class Diamond : public P, public Q { public: int d; };

int main() {
    Diamond* obj = new Diamond();
    return 0;
}
