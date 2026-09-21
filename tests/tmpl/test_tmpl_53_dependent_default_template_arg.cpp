// =============================================================================
// 测试：依赖的默认模板实参 —— `template<class T, class U = T>`
// =============================================================================
// 考察理论点：
//   ① [temp.param]/12 默认模板实参：实参表可以写不满，缺的由默认值补齐。
//   ② ★ 默认实参可以是【依赖】的 —— 它可以引用前面已经声明的形参：
//
//          template<class T, class U = T>  struct Box { T a; U b; };
//          Box<int>            ⇒ U 的默认值 `T` 必须是 int
//          template<class T, class U = T*> struct P { U p; };
//          P<int>              ⇒ U = int*（默认实参是【复合类型】，不只是裸名）
//
//      关键在【何时求值】：默认实参在蓝图里以【依赖形式】保存（这里是
//      TemplateParam("T") 这棵树），到【使用点】才用已推出的实参实例化它。
//      对照 clang：Sema::SubstDefaultTemplateArgument —— 存依赖形式是对的，
//      但使用点必须做那一步替换，否则默认值会以"半成品"形态漏进实例。
//      这正是 CLAUDE.md 记的「替换 ≠ 实例化」的又一例。
//   ③ 与 [dcl.fct.default] 的函数默认实参同构：默认值在【使用点】按当时
//      已知的上下文求值，而不是在声明点。
//
// ── 这里曾有两条真 bug（本用例同时守住）──
//   bug 1（Parser）：形参名压入模板形参作用域的时机在【整个形参表解析完之后】，
//     而默认实参在【形参表的循环内】解析 ⇒ `U = T` 里的 T 被建成普通类名
//     Class("T")，直接报 "unknown type name 'T'"，连编译都过不去。
//     修法：形参一建好就压栈（src/parser.cpp parseTemplateDecl 的循环内）。
//   bug 2（Sema）：默认值是依赖的，却没在使用点替换 ⇒ 一声不吭地算错：
//       实例名成了 Box_int_T（应 Box_int_int）、
//       字段 b 的类型停在裸的 T（大小 0 字节）。
//     修法：补全实参表时，先用前 i 位已绑定的实参替换默认值
//     （src/semantic_analyzer.cpp 的 "用默认实参把实参表补全" 段）。
//
// ── 预期行为 ──
//   编译成功，退出码 = 0；任何一处错则返回两位诊断码：
//       11: U 未成 int（Box<int>::b 不是 4 字节）
//       12: U 未成 int（写入/读回的值不对）
//       21: 复合默认实参 T* 未解成 int*
//       22: 第二处默认实参（带显式第二位）被误替换
//
//   与 clang 对照：clang++-18 -std=c++20 编译运行本文件，退出码同为 0。
//   oracle（is_same_v 版）：
//       static_assert(std::is_same_v<decltype(((Box<int>*)0)->b), int>);
//       static_assert(std::is_same_v<decltype(((PtrBox<int>*)0)->p), int*>);
//       static_assert(sizeof(Box<int>) == 8);
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_53_dependent_default_template_arg.cpp -o /tmp/t53
//     /tmp/t53; echo $?                       # → 0
//     日志里搜 "is dependent" 可见默认实参被替换的那一步：
//       [sema:targ] ⤷ default argument 'U' is dependent: T → int
//       [instantiate:name] 'Box<int,int>' → 汇编符号前缀 'Box_int_int'
// =============================================================================

// ── ① 裸名依赖：U 的默认值就是前一个形参 T ──
template <class T, class U = T>
struct Box {
    T a;
    U b;
};

// ── ② 复合依赖：默认值是【用 T 拼出来的类型】T* ──
template <class T, class U = T*>
struct PtrBox {
    U p;
};

// ── ③ 默认值只在写不满时生效：两位都给时不该被碰 ──
template <class T, class U = T>
struct Two {
    T a;
    U b;
};

int main() {
    // ① U 必须解成 int：b 是 4 字节，能独立存取
    Box<int> x;
    x.a = 1;
    x.b = 2;
    if (x.a != 1) return 11;
    if (x.b != 2) return 12;

    // ② 复合默认实参 T* → int*
    // 注：这里只比较指针本身，不解引用 —— 一元 '*' 解引用本实现尚未支持
    //     （见 CLAUDE.md 的已知缺口 ⑧），与本用例考察点无关。
    int v = 7;
    int* pv = &v;
    PtrBox<int> q;
    q.p = pv;
    if (q.p != pv) return 21;

    // ③ 显式给第二位时，默认值不参与
    // 第二位特意给成 int*（而非默认的 int），这样"到底用没用默认值"一眼可辨。
    int w = 5;
    Two<int, int*> t2;
    t2.a = 3;
    t2.b = &w;
    if (t2.a != 3) return 31;
    if (t2.b != &w) return 32;

    return 0;
}
