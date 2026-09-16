// =============================================================================
// 测试：偏特化（Partial Specialization, [temp.class.spec]）
// =============================================================================
// 考察理论点：
//   · 偏特化 = 对主模板形参的【部分约束】：
//       template<class T> struct Box<T*, T>;   // 只接受「指针 + 同类型」
//     它自己也是一段模板（带形参 T），但模板名后跟的是模式而非裸名字。
//   · 匹配算法 = 用使用点实参去【推导】模式里的模板参数（合一算法）：
//       pattern [T*, T] vs 实参 [double*, double]
//         P=T* 配 double* ⇒ T := double ✓
//         P=T  配 double  ⇒ T := double（一致 ✓）⇒ 命中
//   · ★ 核心复用点：这与函数模板实参推导是【同一套算法】
//     （TemplateDeducer::deducePair），偏特化匹配只是换了入参来源。
//     对照 clang：二者也确实共用 DeduceTemplateArguments 一族入口。
//   · 实例名/符号用【使用点实参】而非特化自己的形参：
//       Box<double*, double> → Box_double__double，而不是 Box_double
//     否则 Box<int*,int> 与 Box<double*,double> 会撞成同一个名字。
//
// 预期行为：编译通过，命中偏特化（tag() 返回 1），程序返回 1。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     ★ PARTIAL specialization of 'Box' with pattern <T*, T>
//   Phase 3 [Sema]:
//     [register] ↳ PARTIAL specialization #1 of 'Box' registered
//     [spec:select]   ├─ ② partial specialization 'Box<T*, T>' matched by deduction → USING IT
//   Phase 4 [Instantiation]:
//     ║ Blueprint: Box <typename T> [PARTIAL SPECIALIZATION]
//     ║ Pattern:   Box<T*, T>
//     ║ Instance:  Box_double__double
//     ║ Mangled: Box_double__double → _Z3BoxIPddE
//
// 对照验证（clang 对同一源码）：
//   $ nm t.o → _ZN3BoxIPddE3tagEv
//   Box<double*, double> 命中偏特化，IPddE 部分与本实现逐字符一致。
//   ★ 注意编码的是【使用点实参】(double*, double) 而非偏特化自己的形参 T，
//     所以是 IPddE（两位）而不是 IPdE（一位）——
//     这也正是实例名取 Box_double__double 而不是 Box_double 的原因。
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

int main() {
    Box<double*, double> c;
    return c.tag();
}
