// =============================================================================
// test_tmpl_32_error_default_gap.cpp —— 错误用例：默认模板实参出现"空洞"
// =============================================================================
// 考察理论点：[temp.param]/12 —— 一旦某位形参带了默认实参，其后的每一位
//   都必须带默认实参。否则调用者用位置实参永远无法"跳过"中间那一位：
//     template<class T = int, class U> class C;   // ✗ U 没有默认值
//     C<>          → T 取 int，U 无处可取
//     C<int,double>→ 倒是能写，但标准仍然禁止这个声明本身
//   对照 clang：err_template_param_default_arg_missing
//     "template parameter 'U' must have a default argument because
//      'T' (declared before it) has one"
//
// 为什么值得单独守：这条约束是【声明期】检查，与实参表无关——
//   即使代码里从没写过 C<>，声明本身就该报错。
//   本实现把它放在 parseTemplateDecl 解析完形参表之后立即检查。
//
// 预期行为：语法分析阶段（Phase 2）报错退出，不进语义分析。
//
// 期望报错（退出码非 0，日志含）：
//   [ERROR] [Parse Error] ...
//     template parameter 'U' must have a default argument because
//     'T' (declared before it) has one
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_32_error_default_gap.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
// =============================================================================

template<class T = int, class U>
class C {
public:
    T first;
    U second;
};

int main() {
    return 0;
}
