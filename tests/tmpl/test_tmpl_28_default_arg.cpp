// =============================================================================
// 测试：默认模板实参（Default Template Arguments, [temp.param]/12）
// =============================================================================
// 考察理论点：
//   · 形参可以带默认实参：template<typename T, typename U = void>
//     使用点只写 Box<int> 时，U 自动补成 void。
//   · 补全时机：必须在【选择特化之前】把实参表补完整 ——
//     偏特化/全特化的匹配都是对完整实参表做的（Box<int*,int> 的全特化
//     要有 2 位实参才能匹配上）。
//   · 个数校验相应放宽：实参可以少于形参，但不能多于；且每位缺失的形参
//     都必须有默认值，否则报 "expects at least N argument(s)"。
//   · 默认实参的"空洞"约束（[temp.param]/12）：一旦某位带了默认值，
//     其后每一位都必须带 —— 详见 test_tmpl_32 的报错用例。
//
// 预期行为：编译通过。Box<int> 补全为 Box<int, void>，返回 3。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ default template argument: 'U' = void
//   Phase 3 [Sema]:
//     [sema:targ]   ✓ param 1: 'T' (type) ← int
//     [sema:targ] ⤷ default argument filled: 'U' := void
//     [sema:targ] ✓ template arguments OK: Box<int>
//   Phase 4 [Instantiation]:
//     ║ Instance:  Box_int_void        ← 实例名带上补全后的 void
//     ║ Mangled: Box_int_void → _Z3BoxIivE
//
// 对照验证：clang 对同一源码的 mangling 为 _ZN3BoxIivE... （IivE 部分一致）
// =============================================================================

template<typename T, typename U = void>
class Box {
public:
    T value;

    int tag() {
        return 3;
    }
};

int main() {
    Box<int> b;
    return b.tag();
}
