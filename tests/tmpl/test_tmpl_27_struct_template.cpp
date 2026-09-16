// =============================================================================
// 测试：struct 写模板（Template with 'struct' keyword）
// =============================================================================
// 考察理论点：
//   · 模板声明的体可以是 class 也可以是 struct，二者只差【默认访问级别】
//     （class 默认 private，struct 默认 public），模板机制本身毫无区别。
//     对照 clang：ParseClassSpecifier 不区分二者，都是 CXXRecordDecl +
//     TagTypeKind::Class / Struct；访问级别由 defaultAccess 决定。
//   · 本项目早期 ParseTemplateDecl 的分派只判了 KwClass，导致
//     `template<typename T> struct Box {...}` 报
//     "Expected 'class' or a function signature"——非模板的 struct 却是支持的。
//     本测试守住这条：模板体也必须认 struct。
//
// 预期行为：编译通过，struct 模板可正常实例化并调用其 public 方法，返回 7。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:template] parsing class body for template...
//     [parse:template] ◀ class template 'Holder' with 1 parameter(s) stored as blueprint
//   Phase 4 [Instantiation]:
//     ║ Instance:  Holder_int
//
// 对照验证（clang 只把它当成访问级别的差异）：
//   $ clang++-18 -std=c++20 -c test_tmpl_27_struct_template.cpp -o t.o && echo OK
//   → OK
// =============================================================================

template<typename T>
struct Holder {
    T value;

    T get() {
        return value;
    }
};

int main() {
    Holder<int> h;
    h.value = 7;
    return h.get();
}
