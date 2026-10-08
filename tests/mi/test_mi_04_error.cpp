// =============================================================================
// 测试：继承说明符只支持 public（错误拒绝用例）
// =============================================================================
// 理论点：
//   - [class.derived]/2：`class` 的默认继承是 private、`struct` 的默认继承是 public；
//     本实现【只做 public 继承】，非 public 说明符一律响亮报错（不静默当 public 用）。
//   - 私有继承在标准下是合法 C++（只是把基类子对象设为不可访问），这里拒收是
//     **本实现的功能边界**，不是程序错误 —— 报错文案里必须说清楚这一点，
//     否则用户会以为自己的程序写错了。
// 预期行为：
//   - rc=1，[Parse Error] 35:14 at 'private'：
//     "'private' inheritance is not implemented (only public inheritance is);
//      omit the specifier on a struct to get public by default"
//   - 对照 clang：同一份源码 clang++-18 -std=c++20 rc=0（标准认可，本实现拒收）
//
// ⚠ 本文件此前的形态（已改）—— 记在这里因为它是 BUGS.md B20 的现场：
//   原来写的是菱形继承 `class Diamond : public P, public Q;`（P/Q 都从 X 公有继承），
//   期望"重复基类被拒收"。实测两件事都不成立：
//     ① clang rc=0 —— 非虚继承的菱形在标准下**合法**（两个 X 子对象，
//        只有成员访问歧义才报错），"本该拒收"是误判；
//     ② 本实现当时 rc=1 的原因是链接期 `undefined reference to 'Q_f'`：
//        Q 从 X 继承了虚函数 f 却没人覆写，次表槽被按"次基类名"重造出 Q_f ——
//        真缺陷（B20 缺陷 b），不是"检测到菱形"。
//   修好 B20 后该菱形 rc=0 且运行正确，正例已搬到
//   tests/mi/test_mi_12_secondary_inherited_slot.cpp（配套的 .s 快照同时撤下：
//   本文件现在是解析期报错，永不产出汇编）。
// =============================================================================

class X {
public:
    int x;
    virtual int f() { return 0; }
};

class Priv : private X {      // ← rc=1：本实现只支持 public 继承
public:
    int p;
};

int main() {
    Priv p;
    return 0;
}
