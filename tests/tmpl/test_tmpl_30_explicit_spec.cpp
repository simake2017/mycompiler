// =============================================================================
// 测试：全特化（显式特化, Explicit Specialization, [temp.expl.spec]）
// =============================================================================
// 考察理论点：
//   · 全特化用【空形参表】标记：template<> struct Box<int*, int> { ... };
//     形参表为空意味着"没有任何未知量"——特化体里全是具体类型。
//   · 匹配算法与偏特化完全不同：全特化是【逐位类型相等】的直接比对，
//     无需推导（没有未知量可推）。
//   · 优先级最高：Box<int*, int> 同时被偏特化 Box<T*,T>（T=int）和
//     全特化 Box<int*, int> 匹配时，[temp.expl.spec]/6 规定显式特化优先。
//     —— 这正是 test_tmpl_31 三路对照要展示的关键一跳。
//   · ★ 一个易错点：全特化的替换表【本来就是空的】（形参 0 个 ⇒ 无绑定）。
//     若用"空表 == 主模板路径"来判别，全特化会被误判成主模板，
//     进而撞上"expects 0 argument(s), got 2"的个数错位——
//     故路径归属必须由调用方显式告知（见 instantiate 的 substOverride 参数）。
//
// 预期行为：编译通过，命中全特化（tag() 返回 2），程序返回 2。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ empty parameter list — explicit (full) specialization
//     ★ EXPLICIT (full) specialization of 'Box' with pattern <int*, int>
//   Phase 3 [Sema]:
//     [register] ↳ EXPLICIT (full) specialization #1 of 'Box' registered
//     [spec:select]   ├─ ① explicit specialization matched (exact type equality) → USING IT
//   Phase 4 [Instantiation]:
//     ║ Blueprint: Box <?> [EXPLICIT SPECIALIZATION]
//     ║ Pattern:   Box<int*, int>
//     [subst:map] (specialization) substitution supplied by pattern matching:
//                 (none — fully concrete body, verbatim clone)
//     ║ Instance:  Box_int__int
//
// 对照验证（clang 对同一源码）：命中全特化，返回 2
// =============================================================================

template<typename T, typename U = void>
class Box {
public:
    int tag() {
        return 0;
    }
};

template<typename T>
class Box<T*, T> {
public:
    int tag() {
        return 1;
    }
};

template<>
class Box<int*, int> {
public:
    int tag() {
        return 2;
    }
};

int main() {
    Box<int*, int> d;
    return d.tag();
}
