// =============================================================================
// 测试：同一实参类型的「指针版」与「引用版」必须是【两条独立实例】
// =============================================================================
// 考察理论点：
//   ① 模板选择是【结构化】的，不看名字
//   ② 实例命名这一层的键必须【单射】，否则两条不同实例抢同一个汇编符号
//
// ── 背景：实例名是怎么来的 ──
//   实例化时（template_instantiation.cpp Step 2）会为实例类生成一个
//   「人读名」，它同时充当汇编符号前缀（方法符号 = <实例名>_<方法名>）：
//
//       Box<int>   → Box_int        （实参 toString "int" 无特殊字符，原样）
//       Box<int*>  → Box_intP       （'*' → 'P'，Pointer）
//       Box<int&>  → Box_intR       （'&' → 'R'，Reference）
//
//   ★ 这里曾经有个坑：清洗规则把 '*' 和 '&' 都换成下划线，于是
//       Box<int*>  → Box_int_
//       Box<int&>  → Box_int_      ← 撞成同一个名字
//     两条实例于是产出同一批汇编符号，as 报
//       Error: symbol `Box_int__dtor' is already defined
//     —— 而裁决本身是【对的】（结构匹配把 T* 和 T& 分得很清楚），
//     错的只是命名这一层。诊断还指向汇编期，看不出根因。
//     这正是"名字不是裁决依据，但名字是另一层的键"的活标本。
//
//   ② 的对应物：C++ 标准里 Box<int*> 与 Box<int&> 是两个不同类型
//      （clang 接受二者共存于一个 TU），必须实例化出两份代码。
//
// ── 本文件验证 ──
//   四条偏特化同时在场，四个使用点各走各的：
//       Box<int*>   → Box<T*>   tag=1   ⇒ Box_intP
//       Box<int&>   → Box<T&>   tag=2   ⇒ Box_intR
//       Box<int**>  → Box<T**>  tag=3   ⇒ Box_intPP    （比 Box<T*> 更特化）
//       Box<int*&>  → Box<T&>   tag=2   ⇒ Box_intPR    （T := int*）
//   返回值 1*100 + 2*10 + 3 + 2 = 125，clang 与 minicc 必须一致。
//
//   注意 Box<int**> 的归属：它同时匹配 Box<T*>（T := int*）与 Box<T**>
//   （T := int），由 [temp.class.order] 偏序裁决，更特化的 Box<T**> 胜出。
//
// ── 预期行为 ──
//   编译成功，退出码 = 125
//   与 clang 对照：clang++-18 -std=c++20 同源编译运行亦为 125
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_46_ptr_vs_ref_instance.cpp -o /tmp/t46
//     /tmp/t46; echo $?                       # → 125
//     编译日志里搜实例命名那一行，可见四条各自的符号前缀
//
//   （本用例是 ⑩ 实例名撞车的回归钉：修复前此处根本编不过，
//     as 报 duplicate symbol。）
// =============================================================================

template <typename T> struct Box { int tag() { return 0; } };   // 主模板
template <typename T> struct Box<T*>  { int tag() { return 1; } };
template <typename T> struct Box<T&>  { int tag() { return 2; } };
template <typename T> struct Box<T**> { int tag() { return 3; } };

int main() {
    Box<int*>  a;   // Box<T*>   → 1
    Box<int&>  b;   // Box<T&>   → 2
    Box<int**> c;   // Box<T**>  → 3（比 Box<T*> 更特化）
    Box<int*&> d;   // Box<T&>   → 2（T := int*）

    return a.tag() * 100 + b.tag() * 10 + c.tag() + d.tag();   // 125
}
