// =============================================================================
// demos/core/04_decl_kinds.cpp —— 顶层声明的 7 种形态，每支喂一个数据事例
// =============================================================================
// 演示什么：Sema 的声明分发器 processDecl（src/semantic_analyzer.cpp:1178）是一张按
//   NodeKind【一次 switch】的跳表；本文件把每一条 case 都触发一次，编译日志里的
//   `[register] …` / `[namespace] …` 与源码里 case 上方的编号一一对应。
//
// 理论点：声明分派 —— clang 是 Decl::getKind() + dyn_cast 的 switch
//   （lib/Sema/SemaDecl.cpp 的一系列 ActOnXXX），本项目对应 NodeKind + static_pointer_cast。
//
// 看什么日志：[register]（类 / 模板 / 全局变量 / 枚举 / 类型别名）与 [namespace] enter，
//   以及 Pass 1 之后打印的继承关系树。
//
// 预期退出码：0；非 0 即"第几项断言不符预期"（编号见 main 中的 return）。
//
// 复现：
//   ./build-linux/minicc demos/core/04_decl_kinds.cpp -S -o /tmp/dk.s > log.txt 2>&1
//   然后 grep -n "\[register\]\|\[namespace\]" log.txt
// =============================================================================

// ── ③ GlobalVar：顶层全局变量 ──
int g = 3;

// ── ④ Enum：枚举 ──
enum Color { RED, GREEN };

// ── ⑤ TypeAlias：类型别名 ──
using Int = int;

// ── ① Class：普通类 ──
struct Box {
public:
    int v;
    Box() { v = 1; }
};

// ── ② Template：类模板蓝图（Pass 1 只登记，不分析成员）──
template<class T> struct Wrap {
public:
    T x;
};

// ── ⑥ Namespace：命名空间（里面的类与函数随它递归进来）──
namespace N {
    struct S {
    public:
        int w;
    };
    int helper() { return 4; }
}

// ── ② Template 的另一形态：带 template<> 外壳的推导指引 ──
template<class T> Wrap(T) -> Wrap<T>;

// ── ⑦ DeductionGuide：非模板形态的推导指引（顶层独立声明，不带外壳）──
Wrap(int) -> Wrap<int>;

// ── default：函数声明【不经过】processDecl（走 analyze 的三趟流程），
//    故在 switch 里落到 default 分支；这里放一个用来对照。──
int add(int a, int b) { return a + b; }

int main() {
    Box b;                       // Box::v  = 1
    Wrap<int> w;                 // 清零      ⇒ w.x = 0
    N::S s;
    s.w = 2;                     // N::S::w = 2
    int t = add(g, b.v);         // 3 + 1 = 4
    if (t != 4) { return 1; }
    return b.v + w.x + s.w - 3;  // 1 + 0 + 2 - 3 = 0
}
