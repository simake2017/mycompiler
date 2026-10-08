// =============================================================================
// 测试：同名同个数的成员重载（方法身份 = 名字 + 形参类型）
// =============================================================================
// 理论点：
//   - [class.member.lookup] 只产出**候选声明集**，不筛类型；筛类型是
//     [overload.best.viable] 的事：候选集 → 可行集（个数 + 隐式转换序列）→ 最优。
//   - 于是"身份"必须包含**参数类型**，不能只到"名字 + 个数"：
//       `f()` / `f(int)`   —— 名字 + 个数就够了
//       `f(int)` / `f(S)`  —— 名字 + **个数**也一样（都是 1），必须比类型
//   - 同一件事在汇编层的后果：符号名。符号若只编码"类名_方法名_个数"，
//     两个同签名个数的 f 都叫 `C_f_1` ⇒ as 报 `symbol 'C_f_1' is already defined`
//     （错误落在汇编期，离根因很远 —— 这是 BUGS.md B22 的原症状）。
//   - 对照 clang：`_ZN1C1fEi` / `_ZN1C1fE1S` —— 形参类型进 mangling，
//     同时声明是 Decl* 句柄，查找/匹配/发射三处**天然同一个东西**。
// 预期行为：
//   - `f(int)` 与 `f(S)` 各自可用，各自命中正确的那个（返回 1 / 2）
//   - 三元混排 `f(int,int)` / `f(int,S)` / `f(S,int)` 也各自命中
//   - 构造函数的同签名个数重载（`C(int)` / `C(S)`）同样分得开
//   - 运行返回 0
// 回归背景（docs/BUGS.md B22）：修复前 rc=1，`[ERROR] 汇编失败` +
//   `minicc_o4.s:43: Error: symbol 'C_f_1' is already defined`。
// =============================================================================

struct S {
public:
    int v;
};

class C {
public:
    int f(int x) { return 1; }        // 与下一个：同名、同个数、类型不同
    int f(S s) { return 2; }

    int g(int a, int b) { return 10; }  // 三元混排
    int g(int a, S s) { return 20; }
    int g(S s, int b) { return 30; }
};

class D {
public:
    int tag;
    D(int x) { tag = 1; }              // 构造函数同样按类型分
    D(S s) { tag = 2; }
};

int main() {
    C c;
    S s;
    s.v = 0;

    if (c.f(1) != 1) return 1;
    if (c.f(s) != 2) return 2;

    if (c.g(1, 2) != 10) return 3;
    if (c.g(1, s) != 20) return 4;
    if (c.g(s, 1) != 30) return 5;

    D d1(1);
    D d2(s);
    if (d1.tag != 1) return 6;
    if (d2.tag != 2) return 7;

    return 0;
}
