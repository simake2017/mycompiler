// =============================================================================
// 测试（错误用例）：名字找得到、个数也对得上，但没有一个候选能接受实参类型
// =============================================================================
// 理论点：
//   · [overload.best.viable]：可行候选 = 逐个实参都能形成隐式转换序列的候选。
//     个数相符不算可行 —— `f(int)` 收不下 `int*`，`f(S)` 也收不下。
//   · 本项目此前在"候选集里挑第一个"（pickBestByArgs 的旧版③）—— 个数对得上
//     就往回退，于是 `c.f(p)`（p 是 int*）静默调用 `f(int)`，**编译期全绿、
//     运行期算错**（BUGS.md B27）。现在没有可行候选 ⇒ 当场报错。
// 预期行为：rc=1，报错文案（前缀；括号内是本实现的补充说明）
//   no matching member function for call to 'f' in class 'C' (1 argument(s) given; ...)
// ★ 这一条**不是**个数问题：`f(int)` 与 `f(S)` 都是 1 个形参，个数落在区间内，
//   淘汰它们的是类型（[overload.best.viable] 的隐式转换序列）—— 故文案是
//   "no matching member function"，与 07 的 "too many arguments" 各说各的错。
// 对照 clang：clang++-18 -std=c++20 同样 rc=1（`no viable conversion from 'int *'
//   to 'int'` / `no matching member function`）。
// =============================================================================

struct S {
public:
    int v;
};

class C {
public:
    int f(int x) { return x; }
    int f(S s) { return s.v; }
};

int main() {
    C c;
    int a = 1;
    int* p = &a;
    return c.f(p);   // ✗ int* 既不是 int 也不是 S
}
