// ============================================================================
// demo 04 —— 偏序裁决：两个函数模板都能匹配时，选"更特殊"的那个
// ============================================================================
// 理论：[temp.func.order] —— 重载集里若多个函数模板都能匹配，用【偏序】决出胜者：
//   把各自的形参互相代进对方，谁能匹配谁就更特殊。
//
//     pick(T)  与  pick(T*)   传 int* 时两者都能匹配
//     —— 但 T 吃不下 T*（T* 带着"指针"这个结构）⇒ pick(T*) 更特殊，胜出
//
// ★ 常见误解：以为"匹配更精确的赢"。偏序不比精确度，只比【特殊性】。
//
// 看裁决过程：
//   ./build-linux/minicc demos/tmpl/04_partial_order.cpp -S -o /tmp/demo04.s
// 终端可见（搜 "overload"）：
//   [call] pick — 2 function template candidate(s), overload resolution begins
//   [overload] partial ordering check (deduction-based)...
//   [overload]   ⇒ 前者至少与后者同样特化（前者胜）
//
// 预期：退出码 0（pick(p) 返回 2 —— 若没有偏序、错选了通用版，会返回 1）
//
// ⚠ 已知边界（写这个 demo 时发现，尚未修）：
//   本项目函数模板实例的 mangling 是 `_Z + 模板名 + I<实参>E`，【不含函数形参
//   类型】。于是 pick(T x) 与 pick(T* x) 在 T=int 时都叫 `_Z4pickIiE`，
//   而实例缓存以这个名字为键 ⇒ 两个不同函数撞名，后者命中前者的缓存。
//   症状：同一程序里【同时调用】 pick(p) 与 pick(v)，第二个调用会拿到第一个的
//   函数体（实测返回 22，clang 为 21）。
//   对照真实 Itanium mangling：`_Z4pickIiE` 之后还跟形参编码（`Pi`），不冲突。
//   故本 demo 只做单次调用，避免踩到该边界。
// ============================================================================

// 通用版：什么都能接
template <class T>
int pick(T x) { return 1; }

// 指针版：只能接指针 —— 它比通用版更特殊
template <class T>
int pick(T* x) { return 2; }

int main() {
    int v = 5;
    int* p = &v;

    // 传 int*：两个模板都能匹配，偏序判定 pick(T*) 胜出 ⇒ 返回 2
    if (pick(p) != 2) return 1;

    return 0;
}
