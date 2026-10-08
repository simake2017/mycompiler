// =============================================================================
// demos/core/03_infer_branches.cpp —— inferType 的 15 条分支，逐支一个数据事例
// =============================================================================
// 演示什么：Sema 的表达式定型函数 inferType（src/semantic_analyzer.cpp:2943）
//   是一张按 NodeKind【一次 switch】的跳表；本文件给每一条 case 喂一个真实表达式，
//   编译日志里的 `[infer] … → T` 与之一一对应（源码里的编号 ①..⑮ = 分支序）。
//
// 理论点：语法制导翻译 —— 表达式的类型是【综合属性】，由子节点自底向上合成
//   （[expr] 各条款的类型规则即属性文法）。clang 的对应物是 SemaExpr.cpp 的
//   BuildXXX + Expr::setType；本项目是各 inferXxx + expr->resolvedType。
//
// 看什么日志：前缀就是分支名 ——
//   [infer]（定型） [resolve]（名字决议） [member] [index] [new] [this] [dynamic_cast]
//
// 预期退出码：0；非 0 即"第几项断言不符预期"（编号见 main 中的 return）。
//
// 复现（本文件全部日志）：
//   ./build-linux/minicc demos/core/03_infer_branches.cpp -S -o /tmp/ib.s > log.txt 2>&1
//   然后 grep -n "\[infer\]\|\[resolve\]\|\[member\]\|\[index\]\|\[new\]" log.txt
// =============================================================================

// ── ⑨ Call 分支之② 的素材：普通自由函数 ──
int add(int a, int b) { return a + b; }

// ── ⑨ Call 分支之③ 的素材：函数模板（候选集 → 推导 → 实例化）──
template<class T> T twice(T x) { return x + x; }

// ── ⑩ Member / ⑪ New / ⑫ This / ⑬ DynamicCast 的素材 ──
struct Base {
public:
    int b;                            // 字段：⑩ Member 的"字段"路径
    Base() { b = 5; }
    int value() { return b; }         // 方法：⑩ Member 的"方法"路径
    int self() { return this->b; }    // ⑫ This：this 是隐式形参，类型 Base*
    virtual int kind() { return 1; }  // 虚方法：⑨ 走 vtable
};

struct Derived : public Base {
public:
    int d;
    // ⚠ 边界（本项目当前口径）：派生类的用户构造函数【不会隐式调用基类的默认构造】——
    //   基类构造只在初始化列表里显式写出时才发射（CodeGen 扫 ctor->initList 找
    //   与基类同名的条目）。真 C++ 中此处省略 `: Base()` 也会自动调用（[class.base.init]/8）。
    //   本 demo 写全 `: Base()`，让 Base::b 真的等于 5。
    Derived() : Base() { d = 7; }
    virtual int kind() { return 2; }
};

// ── ⑭ Index 的素材：下标糖 v[i] ≡ v.at(i)，类须提供 at()/set() 约定方法 ──
class IntVec {
public:
    int a0;
    int a1;
    int at(int i) {
        if (i == 0) { return a0; }
        return a1;
    }
    int set(int i, int v) {
        if (i == 0) { a0 = v; }
        if (i == 1) { a1 = v; }
        return 0;
    }
};

int main() {
    // ── ① 字面量五分支：类型完全由字面量自身决定，不看上下文 ──
    int   li = 42;        // IntLiteral     → int
    char  lc = 'A';       // CharLiteral    → char   （[lex.ccon]/1，不是 int）
    bool  lb = true;      // BoolLiteral    → bool
    int*  ls = "hi";      // StringLiteral  → char*  （本项目无 char 类型，以 int* 承载地址）
    int*  ln = nullptr;   // NullptrLiteral → void*  （随后走 [poly] 隐式转 int*）

    // ── ② Var ③ Binary ④ Unary ──
    int  x = 3;
    int  y = 4;
    int  sum   = x + y;   // ③ Binary 算术 → int
    bool cmp   = x < y;   // ③ Binary 比较 → bool
    bool nf    = !cmp;    // ④ Unary ! → bool
    int  neg   = -x;      // ④ Unary - → 保持操作数类型
    int* addr  = &x;      // ④ Unary & → int*（[expr.unary.op]/3）
    int  deref = *addr;   // ④ Unary * → int （解引用是左值，[expr.unary.op]/1）

    // ── ⑨ Call 的三条瀑布：成员方法 / 普通函数 / 函数模板 ──
    Derived dr;                     // dr.b = 5（基类构造），dr.d = 7
    int c1 = add(x, y);             // ② 普通函数：add(2 args) → int
    int c2 = twice(x);              // ③ 函数模板：P=T A=int ⇒ T:=int → _Z5twiceIiE
    int c3 = dr.kind();             // ① 成员方法（虚）：经 vtable → int

    // ── ⑩ Member：字段 / 箭头解引用后的字段 / 方法 ──
    int m1 = dr.d;                  // 字段：Derived.d (offset=..., size=4)
    Base* pb = &dr;                 // [poly] Derived* → Base*
    int m2 = pb->b;                 // 箭头：先解指针再查字段
    int m3 = dr.value();            // 方法：按类作用域查（不是 m_functionMap 裸名）

    // ── ⑪ New ⑮ Delete ──
    Base* nb = new Base();          // ⑪ NewExpr → Base*（分配 totalSize 字节）
    delete nb;                      // ⑮ DeleteExpr → void（唯一不返回自身类型的分支）

    // ── ⑫ This：只能出现在成员函数体内，类型 = 属主类指针 ──
    int th = dr.self();             // self() 体内 this->b ⇒ 5

    // ── ⑬ DynamicCast：编译期只查"同一继承体系"，成败留到运行期查 RTTI ──
    Derived* pd = dynamic_cast<Derived*>(pb);

    // ── ⑭ Index：v[i] 是 at()/set() 的糖 ──
    IntVec v;                       // 栈对象：清零 + 合成构造
    v[0] = 10;                      // 写形态 → IntVec_set(this, 0, 10)
    int ix = v[0] + v[1];           // 读形态 → IntVec_at(this, 0) + at(this, 1)

    // ───────────────────────────── 断言（退出码 = 项号）─────────────────────────
    if (li    != 42)   { return 1;  }
    if (lc    != 'A')  { return 2;  }
    if (lb    != true) { return 3;  }
    if (ls    == nullptr) { return 4; }
    if (ln    != nullptr) { return 5; }
    if (sum   != 7)    { return 6;  }   // 3 + 4
    if (nf    != false){ return 7;  }   // !(3 < 4)
    if (neg   != -3)   { return 8;  }
    if (deref != 3)    { return 9;  }
    if (c1    != 7)    { return 10; }   // add(3, 4)
    if (c2    != 6)    { return 11; }   // twice(3)
    if (c3    != 2)    { return 12; }   // Derived::kind（虚调用）
    if (m1    != 7)    { return 13; }   // dr.d
    if (m2    != 5)    { return 14; }   // pb->b
    if (m3    != 5)    { return 15; }   // dr.value()
    if (th    != 5)    { return 16; }   // dr.self()
    if (pd    == nullptr) { return 17; }  // dynamic_cast 成功
    if (pd->d != 7)    { return 18; }   // 转型结果指向同一对象
    if (ix    != 10)   { return 19; }   // v[0] + v[1] = 10 + 0

    return 0;
}
