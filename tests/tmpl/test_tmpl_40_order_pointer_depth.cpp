// =============================================================================
// 测试：偏特化偏序裁决 —— T* 与 T** 的"谁更特化"
// =============================================================================
// 考察理论点：
//   当【多个偏特化同时匹配】时，靠什么决定用哪一个？
//   答案不是"先声明者胜"（那是实现偷懒），而是 [temp.class.order] 的
//   【部分排序（partial ordering）】：
//
//     A 至少与 B 同样特化  ⟺  用 A 的模式（把形参换成唯一合成类型）
//                              能推导出 B 的模式
//
//   拿本测试的两条规则算一遍：
//     A = Box<T*>    B = Box<T**>
//
//     ① A 是否 ≥ B？（把 A 的 T 换成合成类型 X，得 X*，去推 B 的模式 T**）
//          T** = X*  → 要求 X* 是"指针的指针" ⇒ 不成立 ⇒ A 不 ≥ B
//     ② B 是否 ≥ A？（把 B 的 T 换成合成类型 Y，得 Y**，去推 A 的模式 T*）
//          T* = Y**  → T := Y*  ⇒ 成立 ⇒ B ≥ A
//     ③ B ≥ A 且 A 不 ≥ B  ⇒  B 更特化  ⇒  Box<int**> 选 Box<T**>
//
//   ★ 直观口诀：模式里的"结构"越多越具体（T** 比 T* 多一层指针），
//     越具体的越优先。这与函数模板重载的偏序是同一套算法
//     （见 isAtLeastAsSpecialized / classSpecAtLeastAsSpecialized）。
//
//   ★ "唯一合成类型"（clang 里叫 UniqueSynthesizedType）是这套算法的关键道具：
//     必须把形参换成【凭空造的、不与任何真实类型相同】的类型，
//     否则合成类型可能恰好撞上真实类型，推出错误的结论。
//     本项目用 "$ord_<模板名>_<形参名>" 作前缀来保证唯一性。
//
// 预期行为：Box<int> → 主模板(0)，Box<int*> → 1，Box<int**> → 2，返回 0。
//
// 编译过程中的关键日志：
//   [spec:select] ★ N 个偏特化候选同时匹配 —— 进入 [temp.class.order] 偏序裁决
//   [order] 合成类型：T := $ord_Box_T
//   [order] ✓ 'Box<T**>' 比 'Box<T*>' 更特化
//
// 对照 clang：clang++-18 -std=c++20 编译本文件，退出码一致。
// =============================================================================

template <typename T>
struct Box {
    int tag() { return 0; }   // 主模板
};

template <typename T>
struct Box<T*> {
    int tag() { return 1; }   // 一级指针
};

template <typename T>
struct Box<T**> {
    int tag() { return 2; }   // 二级指针：比 T* 更特化
};

int main() {
    Box<int>   a;
    Box<int*>  b;
    Box<int**> c;

    if (a.tag() != 0) return 90;   // 完全没匹配上偏特化 → 主模板
    if (b.tag() != 1) return 91;   // 只有 T* 匹配
    if (c.tag() != 2) return 92;   // T* 和 T** 都匹配 → 偏序裁决出 T**

    return 0;
}
