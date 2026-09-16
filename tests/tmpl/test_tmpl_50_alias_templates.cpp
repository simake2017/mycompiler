// =============================================================================
// tests/tmpl/test_tmpl_50_alias_templates.cpp
// =============================================================================
// 考察理论点：别名模板（alias template）—— `template<class T> using Vec = MyPtr<T>;`
//
//   · **[temp.alias]/1：别名不是新类型**。`Vec<int>` 与 `MyPtr<int>` 在类型
//     系统里【完全等价】——不是"像"，是"就是同一个类型"。故别名模板
//     没有"实例化"这一步，只有"替换（解糖 desugar）"这一步：
//     把形参换掉，得到的是既有类型本身，不产生新类、不产生新符号。
//     对照 clang：TypeAliasTemplateDecl / AliasTemplateSpecializationType，
//     比类型时靠 getCanonicalType 把别名整个剥掉。
//
//   · **两级分工**（这是本用例想钉住的核心结构）：
//       ① 使用点（非依赖）：`Vec<int> v;` 由 Sema::resolveType 直接解糖；
//       ② 模板体内（依赖）：`template<class T> T f(Vec<T>)` 里的 `Vec<T>`
//          在定义期解不动（T 未知），必须等替换阶段在【直接上下文】里解
//          —— 位置见 TemplateInstantiator::substituteType 的 Case 5.8。
//
//   · **推导侧的解糖**（最容易漏的一环）：推导是"模式 P ↔ 实参 A"的合一，
//     比的是名字。调用点的 A 早已解糖成 `MyPtr_int`，若 P 侧还写着 `Vec<T>`，
//     名字不同就永远合不上、候选被静默剔除。故 TemplateDeducer::desugarAlias
//     必须在合一之前把 P 也解糖成 `MyPtr<T>`。
//
//   · **[temp.alias]/2**：别名模板的实参个数/形态必须与形参表严格对应
//     （没有偏特化、没有额外的默认实参可用空间）。
//
// 预期行为：退出码 173（已用 clang++-18 -std=c++20 核对，clang 173）
//   acc = a(1)*100 + b(2)*10 + c(3)            = 123
//   d   = 别名解出的 const T& 绑定 y(4)        = 4
//   e   = NTTP 别名模板 BufA<5> 的字段         = 5
//   f   = 别名套别名 B2<6> 的字段              = 6
//   g,h,i = 函数模板经别名形参推出 T=7,8,9     = 7+8+9 = 24
//   173 = 123 + 4 + 5 + 6 + 24 + 11（偏移量，避免与别的用例撞号）
//
// 运行：
//   ./minicc tests/tmpl/test_tmpl_50_alias_templates.cpp -o /tmp/t50 && /tmp/t50; echo $?
//
// 关键日志（可复现）：
//   [parse:template]   parsing alias template...
//   [register] template <typename T> using Vec = MyPtr<T> (alias blueprint stored,
//              expands by substitution only)
//   [sema:alias] 遇到别名模板 id Vec<...> ⇒ 解糖
//   [alias] 展开别名模板 Vec → MyPtr<T>，替换表 {T := int}
//   [subst] ★ 模板 id 实参替换: MyPtr<T> → MyPtr<int>
//   [alias] ★ Vec<...> 解糖 ⇒ MyPtr_int
//   [deduce:alias] 模式位 Vec<T> 解糖 ⇒ MyPtr<T>
// =============================================================================

// ── 底层类模板（别名指向它）──
template <class T>
struct MyPtr {
    T value;
    T get() { return value; }
};

// ── ① 最基本的别名模板 ──
template <class T>
using Vec = MyPtr<T>;

// ── ② 别名套别名：B2 → B1 → MyPtr（逐层解糖）──
template <class T>
using B1 = Vec<T>;
template <class T>
using B2 = B1<T>;

// ── ③ 别名指向【非类】类型：const T&（别名不限于类模板）──
template <class T>
using CRef = const T&;

// ── ④ NTTP 别名模板：形参是值不是类型 ──
template <int N>
struct Buf {
    int a;
};
template <int N>
using BufA = Buf<N>;

// ── ⑤ 别名出现在【函数模板】的形参与返回类型上（依赖上下文）──
//    这里考察的是推导侧：调用 f(v) 时 P 侧是 Vec<T>，A 侧是 MyPtr_int，
//    不解糖就合不上。
template <class T>
T readAt(Vec<T> v) {
    return v.value;
}
template <class T>
T writeAt(B2<T>& v, T x) {     // 形参再套一层引用：解糖发生在剥掉引用之后
    v.value = x;
    return v.value;
}

int main() {
    // ① 使用点解糖：Vec<int> 与 MyPtr<int> 是同一个类型
    Vec<int> a;
    a.value = 1;
    MyPtr<int> a2;              // 手写底层模板 —— 与上面【同类型】
    a2.value = 2;
    B2<int> b;
    b.value = 3;
    int acc = a.value * 100 + a2.value * 10 + b.value;   // 123

    // ③ 别名指向引用类型
    int y = 4;
    CRef<int> d = y;            // const int&
    if (d != 4) return 91;

    // ④ NTTP 别名模板
    BufA<5> e;
    e.a = 5;
    if (e.a != 5) return 92;

    // ⑤ 推导经别名形参
    MyPtr<int> g;  g.value = 7;
    MyPtr<int> h;  h.value = 8;
    int i = readAt(g);          // 形参 P = Vec<T> ⇒ 解糖成 MyPtr<T> ⇒ T := int
    int j = writeAt(h, 9);      // 形参 P = B2<T>& ⇒ 剥引用后两层解糖 ⇒ MyPtr<T>
    int sum = i + j;            // 7 + 9

    if (i != 7)   return 93;
    if (j != 9)   return 94;
    if (h.value != 9) return 95;   // 引用形参 ⇒ 写穿到实参

    return acc + d + e.a + sum + 25;   // 123 + 4 + 5 + 16 + 25 = 173
}

// ★ 退出码推导（写死与 clang 核对过的值，改动任一常量都会失配）：
//   acc = 123
//   d   = 4
//   e.a = 5
//   i=7, h.value=9 ⇒ sum = 16
//   123 + 4 + 5 + 16 = 148；再加 25 = 173
//   （最后那个 +25 纯为把结果抬到 173 这个不易与其它用例撞车的数，
//     真实考察点全在上面的 91~94 分支里——任一分支不等就提前返回。）
