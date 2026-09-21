// ============================================================================
// demo 03 —— 类模板特化：同一个名字，三种不同的类
// ============================================================================
// 理论：[temp.class.spec] —— 特化是"给同一个模板名提供另一份蓝图"，三路择优：
//
//   主模板    template <class T>       struct Tag        —— 兜底
//   偏特化    template <class T>       struct Tag<T*>    —— 只匹配指针（[temp.class.spec.match]）
//   全特化    template <>              struct Tag<int>   —— 唯一确定，最高优先
//
// ★ 特化的匹配是【结构等价】比较，不是函数调用那样的实参推导 —— 一条铁律：
//   有全特化就选全特化；否则在能匹配的偏特化里挑「最特殊」的那个。
//
// 看三路择优的裁决：
//   ./build-linux/minicc demos/tmpl/03_specialization.cpp -S -o /tmp/demo03.s
// 终端可见（搜 "spec:select"）：
//   [spec:select] ★ selecting class template 'Tag' for <int>
//   [spec:select]   ├─ ① explicit specialization matched (exact type equality) → USING IT
//   [spec:select] ★ selecting class template 'Tag' for <int*>
//   [spec:select]   ├─ ② 候选：'Tag<T*>' 匹配成功
//   [spec:select]   │  唯一候选 → 直接选中 'Tag<T*>'
//   [spec:select]   └─ ③ falling back to PRIMARY template          ← Tag<double>
//
// 预期：退出码 0
// ============================================================================

// ── 主模板：兜底，字段叫 generic ──
template <class T>
struct Tag {
    int generic;
};

// ── 偏特化：只吃指针，字段叫 pointer ──
template <class T>
struct Tag<T*> {
    int pointer;
};

// ── 全特化：唯一确定，字段叫 exact ──
template <>
struct Tag<int> {
    int exact;
};

int main() {
    // 三个实例的字段名各不相同 —— 选错了哪个，编译期就会报
    // "No member 'xxx' in class 'Tag_...'"，因此"能跑通"本身就是证据。
    Tag<double> a;
    a.generic = 1;

    Tag<int*> b;
    b.pointer = 2;

    Tag<int> c;
    c.exact = 3;

    return 0;
}
