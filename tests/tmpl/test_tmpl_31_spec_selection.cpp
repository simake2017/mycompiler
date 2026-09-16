// =============================================================================
// 测试：三路择优对照（主模板 / 偏特化 / 全特化 谁被选中）
// =============================================================================
// 考察理论点：
//   同一份源码里主模板、偏特化、全特化三者共存，四个使用点走完三条路径。
//   择优顺序（[temp.class.spec.match] + [temp.expl.spec]/6）：
//     ① 全特化：逐位类型相等 → 命中即用（最高优先）
//     ② 偏特化：用实参推导模式，全位成功即匹配
//     ③ 都不中 → 主模板 + 默认实参补全
//
//   ┌────────────────────────┬──────────┬──────────────────────────────────┐
//   │ 使用点                 │ 走哪条   │ 为什么                           │
//   ├────────────────────────┼──────────┼──────────────────────────────────┤
//   │ Box<int>               │ ③ 主模板 │ 补全成 <int,void>；偏特化要 T*   │
//   │ Box<int*, void>        │ ③ 主模板 │ 偏特化要求 T*=int* 且 T=void     │
//   │                        │          │ ⇒ T 冲突（int vs void）✗         │
//   │ Box<double*, double>   │ ② 偏特化 │ 模式推导成功 T:=double           │
//   │ Box<int*, int>         │ ① 全特化 │ 偏特化也能匹配（T=int），        │
//   │                        │          │ 但显式特化优先 ↑                 │
//   └────────────────────────┴──────────┴──────────────────────────────────┘
//
//   最后一行是本测试的核心：同一个实参组合被【两条规则】同时命中，
//   靠优先级分出胜负 —— 这正是 [temp.expl.spec]/6 的实际效果。
//
// 预期行为：返回 0 + 0 + 1 + 2 = 3。
//
// 编译过程中的关键日志（四次 select 各一行结论）：
//   [spec:select] ★ selecting class template 'Box' for <int, void>
//     ├─ ② partial specialization not matched: parameter pattern 'T*'
//     │     expects pointer argument, got 'int'
//     └─ ③ falling back to PRIMARY template          → Box_int_void
//   [spec:select] ★ selecting class template 'Box' for <int*, void>
//     ├─ ② partial specialization not matched: conflicting types for
//     │     deduction of 'T': previously deduced 'int', now 'void'
//     └─ ③ falling back to PRIMARY template          → Box_int__void
//   [spec:select] ★ selecting class template 'Box' for <double*, double>
//     ├─ ② partial specialization 'Box<T*, T>' matched by deduction → USING IT
//                                                        → Box_double__double
//   [spec:select] ★ selecting class template 'Box' for <int*, int>
//     ├─ ① explicit specialization matched (exact type equality) → USING IT
//                                                        → Box_int__int
//
// 对照验证（语义 oracle）：
//   $ clang++-18 -std=c++20 test_tmpl_31_spec_selection.cpp -o t && ./t; echo $?
//   → 3      （与本实现一致）
// =============================================================================

template<typename T, typename U = void>
class Box {
public:
    int tag() { return 0; }        // 主模板
};

template<typename T>
class Box<T*, T> {
public:
    int tag() { return 1; }        // 偏特化：只收「指针 + 同类型」
};

template<>
class Box<int*, int> {
public:
    int tag() { return 2; }        // 全特化：同时被偏特化匹配，但优先级更高
};

int main() {
    Box<int> a;
    Box<int*, void> b;
    Box<double*, double> c;
    Box<int*, int> d;
    return a.tag() + b.tag() + c.tag() + d.tag();
}
