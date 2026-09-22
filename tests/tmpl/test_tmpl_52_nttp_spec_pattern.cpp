// =============================================================================
// 测试：特化模式里的【非类型位】—— NTTP pattern argument ([temp.class.spec])
// =============================================================================
// 考察理论点：
//   · 偏特化模式 `enable_if<true, T>` 的第 1 位是【值】不是类型。此前本项目的
//     specPattern 是 vector<TypePtr>，值位根本进不了模式表 ⇒ Parser 直接报
//     "non-type argument is not supported in a class template specialization pattern"。
//   · 现在 specPattern 与实参表同型（vector<TemplateArg>，对应 clang 的
//     TemplateArgument 数组），值位在 matchPattern 里【不参与合一、不绑定形参】，
//     只与实参位做同形态同值比较 —— 这是 enable_if 惯用法的地基：
//       主模板 + `enable_if<true, T>` 有 type 成员 / `enable_if<false, T>` 没有，
//       探测 typename enable_if<B,T>::type 是否存在 ⇒ 分支选择。
//       位不比值，两个分支会同时匹配 ⇒ 偏序裁决只能报歧义。
//   · 全特化 `template <> struct Flag<0>` 同理：模式位是值 0，全特化匹配走
//     TemplateArg::equals（类型位比类型、值位比值 + 形态）。
//
// 预期行为：
//   · enable_if<true, int>  ⇒ 命中 <true, T> 偏特化，有成员 value
//   · enable_if<false, int> ⇒ 命中 <false, T> 偏特化，有成员 other
//   · OnlyTrue<true, int>   ⇒ 命中 <true, T> 偏特化，有成员 value
//   · OnlyTrue<false, int>  ⇒ 无匹配偏特化 ⇒ 回落主模板，有成员 generic
//   · Flag<0>               ⇒ 命中全特化，有成员 zero
//   · Flag<3>               ⇒ 无匹配全特化 ⇒ 主模板，有成员 generic
//   程序返回 0（bad = 第一个失败的用例编号）。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:template] ★ PARTIAL specialization of 'enable_if' with pattern <1, T>
//   Phase 3 [Sema]:
//     [spec:select] ★ selecting class template 'enable_if' for <1, int>
//     [deduction]   P=1  A=1  ⇒ 值位相等 ✓（NTTP 模式位，无绑定）
//     [deduction]   P=T            A=int          ⇒ T := int
//     [spec:select]   ├─ ② 候选：'enable_if<1, T>' 匹配成功
//     [deduction]   ✗ 值位不匹配：non-type pattern '0' does not match argument '1'
//     [spec:select]   │  唯一候选 → 直接选中 'enable_if<1, T>'
//   Phase 4 [Instantiation]:
//     ║ Pattern:   enable_if<1, T>
//     ║ Instance:  enable_if_1_int
//     ║ Mangled: enable_if_1_int → _Z9enable_ifILb1EiE
//
// mangling 已用 clang++-18 核对（nm -C /tmp/mng2.o）：
//   enable_if<true, int>::m()  ⇒ _ZN9enable_ifILb1EiE1mEv   ← 类名段 ILb1EiE
//   enable_if<false, int>::m() ⇒ _ZN9enable_ifILb0EiE1mEv   ← 值位编码 Lb1E / Lb0E
//   Flag<0>::m()               ⇒ _ZN4FlagILi0EE1mEv        ← int 形态编码 Li0E
//   本实现只编码到类名一层（不含成员函数段），前缀逐字符相同。
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp -o /tmp/t52 && /tmp/t52; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_52_nttp_spec_pattern.cpp -o /tmp/t52c && /tmp/t52c; echo $?
// =============================================================================

// ── ① enable_if：值位 + 类型位混排，true/false 各一个偏特化 ──
template <bool B, class T>
struct enable_if { };                  // 主模板：B 为 true/false 时都已被偏特化覆盖

template <class T>
struct enable_if<true, T> {            // ★ 模式第 1 位是【值】true
    T value;
};

template <class T>
struct enable_if<false, T> {           // ★ 模式第 1 位是【值】false
    T other;
};

// ── ② 只有一个值位偏特化 ⇒ 另一位必须回落主模板 ──
template <bool B, class T>
struct OnlyTrue { int generic; };

template <class T>
struct OnlyTrue<true, T> {
    T value;
};

// ── ③ 全特化里的值位：template <> struct Flag<0> ──
template <int N>
struct Flag { int generic; };

template <>
struct Flag<0> {                       // ★ 空形参表 + 值位模式
    int zero;
};

int main() {
    int bad = 0;                       // 第一个失败的用例编号，全通过则返回 0

    // 1. enable_if<true, int> → 偏特化 <true, T>
    enable_if<true, int> e;
    e.value = 5;
    if (e.value != 5) bad = 1;

    // 2. enable_if<false, int> → 偏特化 <false, T>
    enable_if<false, int> f;
    f.other = 7;
    if (f.other != 7) bad = 2;

    // 3. OnlyTrue<true, int> → 偏特化 <true, T>
    OnlyTrue<true, int> t;
    t.value = 3;
    if (t.value != 3) bad = 3;

    // 4. OnlyTrue<false, int> → 值位不匹配 ⇒ 回落主模板
    OnlyTrue<false, int> u;
    u.generic = 4;
    if (u.generic != 4) bad = 4;

    // 5. Flag<0> → 全特化（模式位是值 0）
    Flag<0> z;
    z.zero = 9;
    if (z.zero != 9) bad = 5;

    // 6. Flag<3> → 全特化不匹配 ⇒ 主模板
    Flag<3> g;
    g.generic = 2;
    if (g.generic != 2) bad = 6;

    return bad;
}
