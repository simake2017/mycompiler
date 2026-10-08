// =============================================================================
// 测试：错误用例 —— 成员【函数】模板不能是虚函数（[temp.mem]/2）
// =============================================================================
// 考察理论点：
//   · [temp.mem]/2 末句：member function templates shall not be virtual。
//     道理在实现层面：虚函数要求"每个动态类型在 vtable 里有一张固定条目"，
//     而模板实例是**按需产生**的 —— 写 `template<class U> virtual U f(U)` 时
//     根本不知道要有几条条目、更不知道 U 有哪些取值。
//   · 与 [B19](docs/BUGS.md) 同批：static/virtual 的识别位置从"template 之前"
//     挪到"形参表之后"（标准写法所在处）。static 是**合法**的（→ test_tmpl_70），
//     virtual 是**非法**的（→ 本文件）—— 同一次改动里两种相反的处置，
//     判据都来自标准而不是解析器的方便。
//
// 预期行为：语法分析阶段（Phase 2）当场拦下，退出码 1，不产生任何实例。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Parse Error] … at 'virtual':
//     'virtual' cannot be specified on member function templates
//
// clang 核对：
//   $ clang++-18 -std=c++20 -fsyntax-only test_tmpl_71_error_virtual_member_template.cpp
//   → error: 'virtual' cannot be specified on member function templates
// =============================================================================

struct O {
    template <class U>
    virtual U f(U x) { return x; }   // ← 必须在此拦下
};

int main() {
    O o;
    return 0;
}
