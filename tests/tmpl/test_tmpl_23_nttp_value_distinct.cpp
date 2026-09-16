// =============================================================================
// 测试：不同非类型实参 → 不同实例（NTTP 参与实例身份判定）
// =============================================================================
// 考察理论点：
//   · [temp.inst]/3：同一实参组合只实例化一次（缓存命中），
//     但【不同】实参组合必须是不同实例——NTTP 的「值」也是实参的一部分身份。
//   · 实例缓存键 = 模板名 + 各实参的可读串。TemplateArg::toString 按 kind 分派
//     （类型给类型名、值给数字），因此 Buf<4> 与 Buf<8> 天然是不同键，
//     不会互相顶掉——若把值也当成类型串（旧实现会打成 "?"），两个实例会撞车。
//   · 实例名清洗 + mangling 都要能把两个值区分开：
//       Buf<4> → Buf_4 / _Z3BufILi4EE
//       Buf<8> → Buf_8 / _Z3BufILi8EE
//
// 预期行为：编译通过，产出【两个】独立实例类 Buf_4 与 Buf_8，程序返回 4。
//
// 编译过程中的关键日志：
//   Phase 4 [Instantiation]:
//     ║ Instance:  Buf_4 ... ║ Mangled: Buf_4 → _Z3BufILi4EE
//     ║ Instance:  Buf_8 ... ║ Mangled: Buf_8 → _Z3BufILi8EE
//   （两个实例各自独立走完整的字段/方法替换，互不干扰）
//
// mangling 已用 clang++-18 核对：
//   Buf<4>::size() → _ZN3BufILi4EE4sizeEv
//   Buf<8>::size() → _ZN3BufILi8EE4sizeEv
// =============================================================================

template<int N>
class Buf {
public:
    int size() {
        return N;
    }
};

int main() {
    Buf<4> a;
    Buf<8> b;
    return a.size();
}
