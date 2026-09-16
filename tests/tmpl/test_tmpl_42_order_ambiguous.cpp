// =============================================================================
// 测试：偏序歧义 —— 互不更特化时必须【报错】，不能静默选一个
// =============================================================================
// 考察理论点：
//   偏序不是全序，存在【不可比】的偏特化对。此时标准要求程序 ill-formed，
//   而不是"随便挑一个能用就行"。
//
//   本测试的两条偏特化：
//     #1  template <typename T, typename U> struct P<T*, U> { };   // 第一个位置是指针
//     #2  template <typename T, typename U> struct P<T, U*> { };   // 第二个位置是指针
//
//   对 P<int*, int*> 而言，两者都匹配：
//     #1: T := int,  U := int*
//     #2: T := int*, U := int
//
//   做偏序：
//     ① #1 ≥ #2？把 #1 的形参换成合成类型 X、Y，得 P<X*, Y>，去推 #2 的 P<T, U*>
//          T := X*，U* := Y  ⇒ 要求 Y 是指针，但 Y 是【凭空造的合成类型】
//          —— 它不是指针 ⇒ 不成立 ⇒ #1 不 ≥ #2
//     ② #2 ≥ #1？对称地同样不成立 ⇒ #2 不 ≥ #1
//     ③ 互不更特化 ⇒ 歧义 ⇒ 必须报错
//
//   ★ 第 ① 步里"合成的 Y 不是指针"正是关键：这解释了为什么必须用
//     【唯一合成类型】而非真实类型 —— 合成类型除了"是个独一无二的新类型"
//     以外不带任何结构，才能如实反映"模式里这个位置有没有约束"。
//
//   ★ 这也是 [temp.class.order] 与函数模板重载偏序共享的坑：
//     很多"看起来能跑"的实现其实是按声明顺序取第一个，
//     遇到本测试会静默选中 #1（返回 1），而不是报错 —— 那是错的。
//     一个完整的偏序实现必须把"不可比"这一支单独处理成错误。
//
// ── 预期行为（本文件属于【必须编译失败】的负向测试）──
//   期望退出码：minicc 非 0（编译失败）
//   期望报错文案（关键片段）：
//     [Semantic Error] ...: ambiguous partial specializations of 'P' for <int*, int*>
//     : T*, U / T, U* are equally specialized, none is more specialized than the others
//
//   对照 clang：clang 报
//     error: ambiguous partial specializations of 'P<int *, int *>'
//   两者诊断一致（措辞不同、结论相同）。
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_42_order_ambiguous.cpp -o /tmp/x
//     echo $?                          # → 非 0
//     ... 2>&1 | grep "ambiguous partial"   # → 命中上式文案
//
//   ★ 本文件不参与"退出码对齐 clang"的常规回归（两边都失败，
//     要比的是【报错文案】而非退出码）。
// =============================================================================

template <typename T, typename U>
struct P {
    int tag() { return 0; }   // 主模板
};

// #1：第一个位置要求是指针
template <typename T, typename U>
struct P<T*, U> {
    int tag() { return 1; }
};

// #2：第二个位置要求是指针
template <typename T, typename U>
struct P<T, U*> {
    int tag() { return 2; }
};

int main() {
    // <int*, int*> 同时满足 #1 和 #2，且两者互不更特化
    // → 语言要求在此报歧义，绝不能静默挑一个
    P<int*, int*> b;
    return b.tag();
}
