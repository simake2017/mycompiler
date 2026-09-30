// =============================================================================
// 测试：错误用例 —— 值实参窄化到 char（形参是【窄】类型的那半边）
// =============================================================================
// 考察理论点：
//   · 与 test_tmpl_65（→ bool）、test_tmpl_66（→ unsigned）同一条判据的第三个面：
//     形参类型是 char 时，可表示集合是 [-128,127]（本机 char 为有符号，
//     [basic.fundamental]/7 说这是实现定义），`300` 装不下 ⇒ 窄化 ⇒ 非法。
//   · 值域边界由 Type::integerBitWidth() 决定（char → 8 位），
//     与形参的**解析写法**无关：`char` / `signed char` 都是 8 位有符号，
//     但它们是**不同的类型**，故 `D<300>` 与 `SC<300>` 各自报各自的类型名。
//   · ★ 这条同时钉住 [basic.fundamental]/2 的三元区分：
//     `char` / `signed char` / `unsigned char` 是三个不同的 TypeKind，
//     诊断文本里必须如实印出**形参声明的那一个**（不是归一成 `char`）。
//
// 预期行为：语义分析阶段（Phase 3）拦下，退出码 1，不产生任何实例。
//
// 期望报错（退出码 1，日志含）：
//   [ERROR] [Semantic Error] … non-type template argument evaluates to 300,
//     which cannot be narrowed to type 'char'
//
// clang 核对：
//   $ clang++-18 -std=c++20 -fsyntax-only test_tmpl_67_error_nttp_char_narrowing.cpp
//   → error: non-type template argument evaluates to 300, which cannot be
//            narrowed to type 'char' [-Wc++11-narrowing]
// =============================================================================

template<char C>
struct D {
    int v;
};

int main() {
    D<300> a;   // ← 300 装不进 8 位 char ⇒ 必须在此拦下
    return 0;
}
