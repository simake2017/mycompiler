// =============================================================================
// 测试：非类型模板参数 NTTP 基础 (Non-Type Template Parameter, [temp.param]/6)
// =============================================================================
// 考察理论点：
//   · 模板形参有两种形态——类型形参（typename/class T）与非类型形参（int N）。
//     后者是"值"不是"类型"，标准依据 [temp.param]/6、实参依据 [temp.arg.nontype]。
//   · 形参形态只存在于 TemplateDecl::templateParams（带 TemplateParamKind），
//     退化的 typeParams 名字列表分不出来——这正是本测试要守住的点。
//   · 模板实参是 tagged 的 TemplateArg（对应 clang::TemplateArgument）：
//       Box<int> → TemplateArg{kind=Type,     type=int}
//       Buf<4>   → TemplateArg{kind=Integral, value=4}
//   · 替换分两层：类型替换走 substituteType（类型位置），
//     值替换走 cloneExpr（表达式位置，VarExpr{N} → IntLiteral{4}）。
//
// 预期行为：编译通过，生成实例类 Buf_4，程序返回 4。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ non-type parameter registered: int 'N'
//   Phase 3 [Sema]:
//     [sema:targ]   ✓ param 1: 'N' (non-type) ← 4
//     [sema:targ] ✓ template arguments OK: Buf<4>
//   Phase 4 [Instantiation]:
//     [subst:map] 'N' (non-type) := 4
//     ║ Blueprint: Buf <int N>          ← 注意不是 "typename N"
//     ║ Instance:  Buf_4
//     [subst] ★ NTTP value substitution: VarExpr 'N' → IntLiteral 4
//     ║ Mangled: Buf_4 → _Z3BufILi4EE
//
// mangling 已用 clang++-18 核对：
//   $ clang++-18 -std=c++20 -c test_tmpl_21_nttp_basic.cpp -o t.o && nm t.o
//   → _ZN3BufILi4EE4sizeEv   （ILi4EE 部分与本实现逐字符一致）
// =============================================================================

template<int N>
class Buf {
public:
    int cap;

    int size() {
        return N;
    }
};

int main() {
    Buf<4> b;
    return b.size();
}
