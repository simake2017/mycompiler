// =============================================================================
// 测试：模板形参名的作用域从【其声明符之后】才开始 —— [basic.scope.pdecl]/9
// =============================================================================
// 考察理论点：
//   [basic.scope.pdecl]/9 规定，模板形参名的声明点（point of declaration）
//   在其声明符【之后】：
//
//     template <class T, class U = T> struct A {};   // ✓ T 来自【上一轮】
//     template <class U = U>          struct B {};   // ✗ 这里的 U 不是"自己"
//
//   第二行里的 `U` 在解析默认实参时【还没进入作用域】，所以它不会绑定到
//   自己，而是去【外层】找一个叫 U 的类型；找不到就是未知类型名。
//
//   ★ 最容易搞反的一点：`template<class T, class U = T>` 能通过，靠的
//     【不是】"本轮把 U 自己提前注册了"，而是 "T 在【上一轮循环】就注册了"。
//     实证（clang++-18 -std=c++20 -fsyntax-only）：
//       template<class T, class U = T> struct A {T a;U b;}; A<int> x;  ✅ rc=0
//       template<class T = int, class U = T*> struct C {}; C<> z;      ✅ rc=0
//       template<class U = U> struct B {};                  ❌ unknown type name 'U'
//
//   clang 源码对照（clang/lib/Parse/ParseTemplate.cpp，函数 ParseTypeParameter）：
//     :654 注释原文 ——
//       "Per C++0x [basic.scope.pdecl]p9, we parse the default argument
//        before we introduce the type parameter into the local scope."
//     :671  ParseTypeName(...)           ← 先解析默认实参
//     :676  Actions.ActOnTypeParameter   ← 后建 Decl
//     SemaTemplate.cpp:1074 的 S->AddDecl(Param) 才是真正入作用域的那一步。
//   ⇒ 顺序是"先默认实参、后注册自己"，不是反过来。
//
//   ★ 本项目此前是真 bug，两处合起来才算修好：
//     bug 1（Parser）所有形参都在【形参表循环结束之后】才注册 ⇒ 连上一轮的
//       T 都没进门，`template<class T, class U = T>` 里的 T 被建成 Class("T")，
//       报 unknown type name 'T'，且报错点看不出根因。
//       修法：注册点挪进循环内、且排在默认实参解析【之后】
//       （src/parser.cpp 的 parseTemplateDecl）。
//     bug 2（Sema）默认值即使正确存成依赖形式，补全实参表时也没做替换 ⇒
//       实例名成 Box_int_T、字段 0 字节，【编译通过但静默算错】—— 比报错更危险。
//       修法见 src/semantic_analyzer.cpp 的"用默认实参把实参表补全"段。
//     正向用例见 tests/tmpl/test_tmpl_53_dependent_default_template_arg.cpp。
//
// ── 预期行为（本文件属于【必须编译失败】的负向测试）──
//   期望退出码：minicc 非 0
//   期望报错文案（关键片段）：
//     [Semantic Error] ...: unknown type name 'U'
//   对照 clang 的报错文案（就是同一句）：
//     error: unknown type name 'U'
//   —— 本实现里默认实参里的 `U` 查找不到任何形参，被建成 Class("U")，
//      语义阶段 resolveType 查不到该类型名，报出与 clang 相同的句子。
//
//   ★ 本文件不参与"退出码对齐 clang"的常规回归（两边都失败，
//     要比的是【报错文案】而非退出码）。
//
//   复现命令：
//     ./build-linux/minicc \
//       tests/tmpl/test_tmpl_54_error_tparam_scope_after_declarator.cpp -o /tmp/x
//     echo $?                                     # → 非 0
//     ... 2>&1 | grep "unknown type name 'U'"     # → 命中
// =============================================================================

// ★ 非法：第 2 位形参的默认实参引用了【它自己】。
//   按 [basic.scope.pdecl]/9，此刻 U 尚未进入作用域 ⇒ 必须报错。
//   若能编译通过，说明实现"提前注册了本轮的形参"，比 clang 宽松。
template <class T, class U = U>
struct SelfDefault {
    T a;
    U b;
};

int main() {
    SelfDefault<int> x;
    x.a = 1;
    return 0;
}
