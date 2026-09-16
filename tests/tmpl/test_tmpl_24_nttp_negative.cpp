// =============================================================================
// 测试：负数非类型实参 Buf<-3>（NTTP 的负数编码）
// =============================================================================
// 考察理论点：
//   · ISO C++ 里 -3 不是字面量，而是「一元减 + 整数字面量」构成的
//     常量表达式（[expr.unary.op] + [expr.const]）。完整实现要解析任意
//     常量表达式再求值（Buf<2+2> 也应支持）——那属于常量折叠，见 ROADMAP 主线 D。
//     本实现只识别最常见的「负号 + 整数字面量」一种形态。
//   · Itanium mangling 中负数不能直接写 '-'（不是合法 mangling 字符），
//     规则是编成 n<绝对值>：Buf<-3> → _Z3BufILin3EE（L…E 是 <expr-primary>）。
//   · 实例名（Assembly 层面的符号片段）另有清洗规则：'-' 也要转 '_'，
//     否则 as 会因非法标识符报错。
//
// 预期行为：编译通过，实例名 Buf__3，符号 _Z3BufILin3EE，程序返回 253
//   （= -3 取低 8 位，即 main 返回值的补码截断）。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ non-type argument (NTTP): negative integer -3
//   Phase 4 [Instantiation]:
//     ║ Instance:  Buf__3
//     ║ Mangled: Buf__3 → _Z3BufILin3EE
//
// mangling 已用 clang++-18 核对：
//   Buf<-3>::size() → _ZN3BufILin3EE4sizeEv   （Lin3E 与本实现逐字符一致）
// =============================================================================

template<int N>
class Buf {
public:
    int size() {
        return N;
    }
};

int main() {
    Buf<-3> b;
    return b.size();
}
