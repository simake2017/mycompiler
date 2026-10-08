// =============================================================================
// 测试：类模板里的成员模板（两层形参分别绑定，[temp.mem] × [temp.inst]）
// =============================================================================
// 考察理论点：
//   · `template<class T> struct Box { template<class U> T convert(U x); };` 有【两层】
//     模板形参，绑定时机不同：
//       外层 T —— 类实例化时绑定（Box<int> ⇒ T := int）
//       内层 U —— 调用点按实参推导绑定（b.convert('a') ⇒ U := char）
//     所以"类实例化"这一步的产物必须是：把成员模板复制一份、**只替换外层形参、
//     保留内层形参**，挂到实例类名下。对照 clang：InstantiateDecl 对 TemplateDecl
//     走 TransformTemplateDecl（SemaTemplateInstantiateDecl.cpp），内层形参保持未绑定。
//   · 与 tests/tmpl/test_tmpl_56_member_templates.cpp 的区别：那里类是【普通类】，
//     成员模板的推导与实例化一条路走到底；这里成员模板【住在类模板里】，
//     多出一层"随类实例化被搬运"的环节 —— 本文件守的就是这一环。
//   · 回归说明（docs/BUGS.md B18）：修复前 `b.convert('a')` 报
//       [Semantic Error] No member 'convert' in class 'Box_int'（rc=1）
//     —— 解析层一切正常（[parse:member] ★ 'convert' 是成员模板），断在语义层：
//     实例类 Box_int 手里那张成员模板表是空的（蓝图类从不经过 processClassDecl，
//     而实例化时又没把这张表搬过去）。
//
// 预期行为：
//   · bi.convert('a') ⇒ U := char，返回类型是【外层 T = int】 ⇒ 11
//   · bl.convert('a') ⇒ 同一个蓝图的另一个实例 ⇒ 返回类型 long ⇒ 22（互不串味）
//   · bi.identity(3)  ⇒ U := int  ⇒ 符号 Box_int_identity_int
//   · bi.identity(&v) ⇒ U := int* ⇒ 符号 Box_int_identity_intP（★ 与上不撞）
//   · 再次 bi.identity(4) ⇒ 回到 int 实例（缓存命中，不重复实例化）
//   程序返回 0。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     [parse:member] ★ 'convert' 是成员模板（1 个模板形参，[temp.mem]）—— 调用点按实参推导实例化
//   Phase 3 [Sema]（★ 下面这行是 B18 修复的观测点：表挂到了实例类名下）:
//     ↳ member template 'Box_int::convert' registered (1 template parameter(s))
//     [call] Box_int::convert — 成员模板候选，开始推导（[temp.mem]）
//     [call] Box_int::convert → Box_int_convert_a (member template instantiated) → int
//   Phase 4 [实例化]（只替换外层 T，内层 U 原样保留）:
//     ║ ── Member Template Substitution ──
//     ║   member template 'convert' <U> → 挂到实例类 'Box_int'（内层形参不替换）
//       [clone:method] 'convert' return type: T → [subst] ★ TemplateParam 'T' → 'int'
//       [clone:method]   param 'x' : U → U        ← ★ 内层形参原样
//
// 可复现实验：
//   ./minicc tests/tmpl/test_tmpl_69_member_template_in_class_template.cpp -o /tmp/t69 && /tmp/t69; echo $?
//   clang++-18 -std=c++20 tests/tmpl/test_tmpl_69_member_template_in_class_template.cpp -o /tmp/t69c && /tmp/t69c; echo $?
// =============================================================================

template <class T>
struct Box {
    T seed;

    Box(T s) : seed(s) {}

    // ★ 返回【外层】T：克隆进实例类时 T 必须已经兑现（Box_int 的返回 int）
    template <class U>
    T convert(U x) {
        return seed;          // 访问成员 ⇒ 实例带隐式 this，且 this 是 Box_int*
    }

    // ★ 返回【内层】U：每个实参类型一份实例，符号靠实参后缀区分
    template <class U>
    U identity(U x) {
        return x;
    }
};

// ★ 偏特化里的成员模板：这一份"搬运"发生在【被选中的那个蓝图】上 ——
//   克隆取的必须是 templateDecl->classTemplate（本次命中的蓝图），
//   而不是主模板。主模板 / 偏特化 / 全特化三种蓝图一视同仁。
template <class T>
struct Box<T*> {
    template <class U>
    U pick(U x) { return x; }
};

int main() {
    int bad = 0;

    Box<int>  bi(11);
    Box<long> bl(22);

    // 1. 外层 T := int（Box<int> 的实例类里，convert 的返回类型已是 int）
    if (bi.convert('a') != 11) bad = 1;

    // 2. 同一个蓝图的另一个实例：T := long —— 两个实例的绑定必须互不干扰
    if (bl.convert('a') != 22) bad = 2;

    // 3. 内层 U := int
    if (bi.identity(3) != 3) bad = 3;

    // 4. 内层 U := int* ⇒ 另一个实例符号（与第 3 步不撞）
    int v = 5;
    int* p = bi.identity(&v);
    if (p != &v) bad = 4;

    // 5. 回到 int 实例（应与第 3 步命中同一份实例）
    if (bi.identity(4) != 4) bad = 5;

    // 6. 偏特化蓝图里的成员模板（Box<T*> 命中 ⇒ 搬的是这份蓝图）
    Box<int*> bp;
    int w = 7;
    int* q = bp.pick(&w);
    if (q != &w) bad = 6;

    return bad;
}
