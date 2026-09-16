// =============================================================================
// 测试：类型形参与非类型形参混排 (Mixed Type & Non-Type Template Params)
// =============================================================================
// 考察理论点：
//   · template<class T, int N> 这类混排模板，形参形态【逐位独立】判定：
//       第 1 位是 Type    → 必须收类型实参
//       第 2 位是 NonType → 必须收值实参
//     分派依据是「形参自己的 kind」，而不是「实参长什么样」——
//     这是本次修复的核心：instantiate() 读 templateParams 而非 typeParams。
//   · 缓存键必须能区分 (类型, 值) 组合：Pair<int,8> 与 Pair<double,8> 是不同实例。
//   · 实例名清洗后仍应可读：Pair<int, 8> → Pair_int_8。
//
// 预期行为：编译通过，唯一实例类 Pair_int_8，程序返回 8。
//
// 编译过程中的关键日志：
//   Phase 4 [Instantiation]:
//     [subst:map] 'T' (type) := int
//     [subst:map] 'N' (non-type) := 8
//     ║ Blueprint: Pair <typename T, int N>     ← 形参表逐位按 kind 渲染
//     ║ Instance:  Pair_int_8
//     [subst] ★ TemplateParam 'T' → 'int' (direct replacement)      ← 类型替换
//     [subst] ★ NTTP value substitution: VarExpr 'N' → IntLiteral 8 ← 值替换
//     ║ Mangled: Pair_int_8 → _Z4PairIiLi8EE
//
// mangling 已用 clang++-18 核对：_ZN4PairIiLi8EE5countEv
// =============================================================================

template<class T, int N>
class Pair {
public:
    T first;

    int count() {
        return N;
    }
};

int main() {
    Pair<int, 8> p;
    p.first = 3;
    return p.count();
}
