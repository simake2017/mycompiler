// =============================================================================
// test_tmpl_60_error_ttp_arg_is_template_id.cpp
//   —— 错误用例：拿【模板特化类型】`Box<int>` 去填模板模板形参
// =============================================================================
// 考察理论点（[temp.arg.template]/1-2，对应 docs/learn/33 §3.3）：
//   模板模板形参这一位要的是【模板】本身，不是"某个特化出来的类型"。
//   `Wrap<Box<int>, int>` 里的 `Box<int>` 是一个**类型**（模板特化类型，
//   clang 里是 TemplateSpecializationType），不是模板名 ⇒ ill-formed。
//   对照 clang：err_template_template_parm_mismatch
//   / "template template argument must be a class template or type alias template"。
//
// ★ 为什么这条最阴（值得单独一个文件）：
//   Parser 把实参位的 `Box` 和 `Box<int>` **都建成 Class 节点**，两者只差
//   `templateArgs` 空不空 —— 也就是说，只查"实参的名字在不在模板注册表里"
//   的守卫，会被 `Box<int>` 的**名字段**（同样是 "Box"）骗过去：`<int>` 静默蒸发，
//   这一位当成裸 `Box` 用。而它的产物与写 `Wrap<Box, int>` **逐字节相同**
//   （替换期 `C<int>` → `Box<int>`，落地成同一个 `Box_int`）——
//   **连"算错"这个症状都没有**，属于比"报错指错地方"更隐蔽的一档。
//   本实现此前正是如此（rc=0 静默接受），补的判据是：
//       `!arg.type->templateArgs.empty()`  ⇒ 带实参的 id 是类型，不是模板名
//
// 姊妹用例：
//   · 拿普通类型填（`Wrap<int,int>`）      → test_tmpl_54
//   · 位数 / 逐位 kind 不符                → test_tmpl_58 / 59
//   · 通过的正例（含别名模板作实参）        → test_tmpl_57
//
// 预期行为：语义分析阶段（Phase 3）resolveType 的模板 id 分支拦下 ——
//   注意拦在**进 registerTemplateCall/实例化之前**。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Semantic Error] ...
//     template argument 1 for 'Wrap' ('C') must be a class template,
//     but 'Box<int>' is not a template
//
// 验证命令：
//   ./minicc tests/tmpl/test_tmpl_60_error_ttp_arg_is_template_id.cpp; echo "exit=$?"
//   应见上述 [ERROR]，exit=1
//   clang++-18 -std=c++20 -fsyntax-only <同名文件> 2>&1 | head -3
//   应见 clang 的同一诊断（template argument for template template parameter
//   must be a class template or type alias template）
// =============================================================================

template <template <class> class C, class T>
struct Wrap {
    C<T> inner;
};

template <class T>
struct Box {
    T value;
};

int main() {
    Wrap<Box<int>, int> w;   // ← Box<int> 是【类型】不是模板名：必须在此拦下
    return 0;
}
