// =============================================================================
// 测试：引用绑定与值类别（[dcl.init.ref]、[expr.unary.op]/1、[expr.sub]/1）
// =============================================================================
// 理论点：
//   · 引用初始化不是"类型相等"就够 —— 还要看**实参的值类别**（value category）：
//       [dcl.init.ref]/5  非 const 左值引用 (T&)  只能绑【左值】
//       [dcl.init.ref]/5.4.2  const 左值引用 (const T&) 可以绑【右值】（延长生存期）
//       [dcl.init.ref]/5.3 右值引用 (T&&) 只能绑【右值】（模板形参的 T&& 除外，
//                           那是转发引用，经引用折叠后左右值皆可，S2 已实现）
//   · ★ 本项目的此前**只在模板推导那条路上**做了这个检查（deducePair 的引用分支），
//     非模板调用一条都没有 —— 因为 typeCompatible / stripRefConst 会把引用剥掉再比
//     类型，"char& 与 char 相等"于是 `void g(int&); g(7);` 静默通过（BUGS.md B25）。
//     两条路现在共用 type.h 的 referenceBindsValueCategory 一个判据。
//   · 值类别本身：本项目简化模型 —— 有确定内存地址的表达式是左值。除变量、成员访问
//     之外，[expr.unary.op]/1（`*p`）与 [expr.sub]/1（`v[i]`）**也**产生左值，
//     漏掉它们会把 `void g(int&); g(*p);` 这类合法程序误拒。
// 预期行为：
//   · `const T&` ← 右值、`const int&` ← 右值、`int&&` ← 右值  全部接受
//   · `int&` ← `*p`（解引用产生的左值）接受，且写回生效
//   · 运行返回 0
// 对照 clang：本文件 clang++-18 -std=c++20 编译运行同样 rc=0。
// 回归背景（docs/BUGS.md B25）：修复前 ① `tmplId(5)` 报
//   `cannot bind non-const lvalue reference ... to an rvalue`（把 const T& 当成了 T&），
//   即"拒收合法程序"。
// =============================================================================

// ① const T& 绑定右值（模板路径 —— deducePair 的引用分支）
template<class T>
int tmplConstRef(const T& x) { return x; }

// ② const int& 绑定右值（非模板路径 —— 同一条判据的另一个调用点）
int plainConstRef(const int& x) { return x; }

// ③ 右值引用绑定右值（[dcl.init.ref]/5.3 允许的那一半）
int takeRvalueRef(int&& x) { return x; }

// ④ 左值引用绑定 `*p`（解引用是左值，[expr.unary.op]/1）—— 且要能写回
int bump(int& x) { x = x + 1; return x; }

int main() {
    int n = 4;
    int* p = &n;
    // 5 + 6 + 7 + 5 = 23
    return tmplConstRef(5) + plainConstRef(6) + takeRvalueRef(7) + bump(*p) - 23;
}
