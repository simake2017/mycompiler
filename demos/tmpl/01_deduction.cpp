// ============================================================================
// demo 01 —— 函数模板实参推导：从调用实参反推出 T
// ============================================================================
// 理论：[temp.deduct.call] —— 拿调用实参 A 去匹配形参模式 P，合一出模板实参 T。
//
//   形参写法 T    ⇒ A 的原类型直接绑给 T
//   形参写法 T*   ⇒ A 必须是指针，剥掉一层 '*' 后绑给 T
//   多个形参都用 T ⇒ 每个 P/A 对必须推出【同一个】T，否则推导失败（歧义）
//
// 看推导过程（本项目每个 P/A 对都打印一行）：
//   ./build-linux/minicc demos/tmpl/01_deduction.cpp -S -o /tmp/demo01.s
// 终端可见：
//   [deduction] ▶ twice — 模板参数 <T>，实参 1 个
//   [deduction]   P=T            A=int          ⇒ T := int
//   [deduction] ◀ 推导成功: <T=int>
//   [deduction]   P=T            A=int          ⇒ T := int（一致 ✓）   ← pick 的第二个实参
//
// 预期：退出码 0
// ============================================================================

template <class T>
T twice(T x) { return x + x; }

template <class T>
T pick(T a, T b) { return a; }

template <class T>
T* identity(T* p) { return p; }

int main() {
    // ① 最简形态：P = T，A = int
    if (twice(21) != 42) return 1;

    // ② 两个实参推同一个 T —— 必须一致，否则是推导失败而非"随便选一个"
    if (pick(7, 9) != 7) return 2;

    // ③ P = T*：剥掉一层指针后 T := int，返回类型也随之成为 int*
    int v = 5;
    int* pv = &v;
    if (identity(pv) != pv) return 3;

    return 0;
}
