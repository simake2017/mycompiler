// =============================================================================
// 测试：引用折叠（Reference Collapsing）—— 四条规则 + 两个曾经答错的落地点
// =============================================================================
// 考察理论点：
//   ① [dcl.ref]/6 折叠规则（C++11 起进入标准，DR 106/540 上溯到 C++98/03）：
//
//          T&  &   → T&        T&  &&  → T&
//          T&& &   → T&        T&& &&  → T&&
//
//      一句话：只要有一层是左值引用，结果就是左值引用；两层都是右值引用才是右值引用。
//      它不是"新造类型"的规则，而是【消除矛盾】：C++ 里根本不存在"引用的引用"
//      （[dcl.ref]/4、/5），所以每当语言机制（typedef / 模板形参 / decltype）
//      间接拼出嵌套引用时，必须当场归一成合法类型。
//      ★ 本用例因此把"引用类型分类器"写成两层的（见下），
//        单层模式匹配分不出 `int&` 与嵌套的 `int& &` —— 而这正是当年的漏网之鱼。
//   ② [temp.deduct.call]/3 万能引用（forwarding reference）：P 是"模板参数的右值引用"
//      且实参为左值时，推导把 A 按左值引用处理 ⇒ T := A&。这是 std::forward 的根基。
//   ③ [expr.type]/1 表达式没有引用类型：引用在确定表达式类型时即被剥掉，
//      所以"实参是 int& 变量"与"实参是 int 变量"对推导是【同一件事】
//      —— 同一函数模板必须只被实例化出一份代码。
//
// ── 观测手法：把类型层的事实变成可断言的整数 ──
//   C++ 的折叠发生在编译期类型上，运行期看不见。这里用偏特化做分类：
//
//       IsRef<U>       : 0 = U 非引用      1 = U 是引用
//       RefKind<X>     : 0 = 非引用        1 = 单层左值引用
//                        2 = 单层右值引用  9 = ★嵌套引用（非法结构，本该不存在）
//
//   为什么 RefKind 要多一层：模式 `X&` 对 `int&` 和 `int& &` 都能匹配成功
//   （只是绑到内层的 T 不同），单层匹配【分辨不出嵌套】。所以 RefKind<T&> 的分支里
//   再看一眼自己的 T 是不是引用 —— 是引用就说明 X 其实嵌了两层。
//
//       RefKind<int&>    : 匹配 T&，T := int   → IsRef<int>  = 0 → 1
//       RefKind<int& &>  : 匹配 T&，T := int&  → IsRef<int&> = 1 → 9   ← 嵌套暴露
//
//   两个落地点各管一半：
//     · 成员声明处 —— LRefHolder<T&> / RRefHolder<T&&>，decltype(p->ref) 取替换后的
//       【成员声明类型】。四条规则在这里的答案互不相同，无法互相遮盖。
//     · 万能引用推导处 —— deduce_kind(T&& x) 体内 RefKind<T>，检验推导出的 T 本身。
//
//   为什么用指针 p->ref 而不直接建对象：含引用成员的类在 clang 里默认构造是
//   ImplicitlyDeleted（引用必须初始化），而 minicc 目前不检查这一条 —— 为保持
//   "同源可被 clang 编译"（本项目 oracle 惯例），这里用不求值的 decltype 取类型。
//
// ── 预期行为 ──
//   编译成功，退出码 = 0；任何一条答错则返回两位诊断码：
//
//       11: 规则① T&  &  → 非左值引用     12: 规则③ T&& &  → 非左值引用  ★曾答 int&&
//       13: 规则② T&  && → 非左值引用     14: 规则④ T&& && → 非右值引用
//       21: 左值实参         ⇒ T 未推出 int&
//       22: 左值引用变量实参 ⇒ T 未推出 int&  ★曾推出 int& &（分类器答 9）
//       23: 右值引用【变量】 ⇒ 未按左值表达式处理
//       24: 右值实参         ⇒ T 不是非引用
//
//   注：RefKind 出现嵌套引用时答 9，故"本该是 1、实际是 9"会如实落进对应规则的码。
//       修复前实测：rc = 12（规则③ 折叠出 int&&，分类器随之失准）。
//
//   与 clang 对照：clang++-18 -std=c++20 编译本文件并运行，退出码同为 0。
//
//   复现命令：
//     ./build-linux/minicc tests/tmpl/test_tmpl_52_reference_collapsing.cpp -o /tmp/t52
//     /tmp/t52; echo $?                       # → 0
//     日志里搜 "[subst] ★ Reference collapsing" 可见四个折叠点各自的输入/输出；
//     搜 "[deduction]   P=T&&" 可见万能引用路径上 T 被绑成了什么。
//
// ── clang oracle（同源，可另存为 assert 版）──
//   #include <type_traits>
//   template<class T> struct L { T&  ref; };
//   template<class T> struct R { T&& ref; };
//   static_assert(std::is_same_v<decltype(((L<int& >*)0)->ref), int& >);
//   static_assert(std::is_same_v<decltype(((L<int&&>*)0)->ref), int& >);   // ★ 规则③
//   static_assert(std::is_same_v<decltype(((R<int& >*)0)->ref), int& >);   //   规则②
//   static_assert(std::is_same_v<decltype(((R<int&&>*)0)->ref), int&&>);   //   规则④
//
// ── 已知边界（本用例未覆盖，见 docs/ROADMAP）──
//   · 含引用成员的类默认构造：clang 报 "call to implicitly-deleted default
//     constructor"，minicc 静默放行（无引用绑定检查）。
//   · 指向引用的指针（[dcl.ref]/5）：`T* p` 配 T := int& 时 minicc 静默产出
//     `int&*`，clang 报 "'p' declared as a pointer to a reference of type 'int &'"。
// =============================================================================

// ── 分类器第一层：U 是不是引用 ──
template <typename U> struct IsRef    { int v() { return 0; } };
template <typename U> struct IsRef<U&> { int v() { return 1; } };

// ── 分类器第二层：把类型 X 归一成 0 / 1 / 2，嵌套引用（非法）落到 9 ──
template <typename T> struct RefKind { int v() { return 0; } };          // 非引用

template <typename T> struct RefKind<T&> {                               // 左值引用
    int v() {
        IsRef<T> i;
        if (i.v() == 0) return 1;   // T 非引用 ⇒ X 是【单层】左值引用
        return 9;                   // T 是引用   ⇒ X 嵌了两层 —— 折叠漏做了
    }
};

template <typename T> struct RefKind<T&&> {                              // 右值引用
    int v() {
        IsRef<T> i;
        if (i.v() == 0) return 2;   // 单层右值引用
        return 9;                   // 嵌套
    }
};

// ── 折叠点 A：成员声明是 'T&'，成员类型 = T 与 '&' 折叠的结果 ──
//   规则① LRefHolder<int&>  → 成员 T& = int& &  → int&
//   规则③ LRefHolder<int&&> → 成员 T& = int&& & → int&    ★ 曾错答 int&&
template <typename T> struct LRefHolder { T& ref; };

// ── 折叠点 B：成员声明是 'T&&' ──
//   规则② RRefHolder<int&>  → 成员 T&& = int& &&  → int&
//   规则④ RRefHolder<int&&> → 成员 T&& = int&& && → int&&
template <typename T> struct RRefHolder { T&& ref; };

// ── 折叠点 C：万能引用推导（[temp.deduct.call]/3）──
//   T 由实参推出，形参声明类型 T&& 再与 T 折叠。这里看的是【T 本身】：
//     右值实参 ⇒ T := int（非引用）      左值实参 ⇒ T := int&（一层左值引用）
//   ★ 曾经的错法：左值引用变量（int& ra）推出嵌套引用 T := int& &
//     —— 推导把 A 的声明类型再套一层 '&' 就完事，没人做折叠。
//     后果是同一函数模板被实例化两份（_Z2idIRiE 与 _Z2idIRRiE），
//     且 `int& &` 还会被 Type::toString 印成 "int&&"，与真右值引用同形。
template <typename T> int deduce_kind(T&& x) {
    RefKind<T> k;
    return k.v();
}

int main() {
    int a = 1;
    int& ra = a;      // 左值引用变量（[expr.type]/1：表达式 ra 的类型是 int，不是 int&）
    int&& rr = 2;     // 右值引用变量：命名之后，它同样是【左值】表达式

    // ── 四条折叠规则（成员声明处）──
    // 用指针 + decltype 取成员声明类型：decltype 不求值，且绕开"引用成员类无法默认构造"。
    LRefHolder<int&>*  p1 = nullptr;
    LRefHolder<int&&>* p2 = nullptr;
    RRefHolder<int&>*  p3 = nullptr;
    RRefHolder<int&&>* p4 = nullptr;

    RefKind<decltype(p1->ref)> t1;    // 规则① T&  &   → int&
    RefKind<decltype(p2->ref)> t2;    // 规则③ T&& &   → int&    ★
    RefKind<decltype(p3->ref)> t3;    // 规则② T&  &&  → int&
    RefKind<decltype(p4->ref)> t4;    // 规则④ T&& &&  → int&&

    // 注意：RefKind 遇嵌套引用答 9，所以下面的 != 检查同时覆盖"折错了"与"折出了嵌套"。
    if (t1.v() != 1) return 11;   // 规则① T&  &  → 左值引用
    if (t2.v() != 1) return 12;   // 规则③ T&& &  → 左值引用  ★本用例的核心
    if (t3.v() != 1) return 13;   // 规则② T&  && → 左值引用
    if (t4.v() != 2) return 14;   // 规则④ T&& && → 右值引用

    // ── 万能引用推导 ──
    int d1 = deduce_kind(a);      // 左值          ⇒ T := int&
    int d2 = deduce_kind(ra);     // 左值引用变量  ⇒ T := int&  ★曾推出 int& & ⇒ 9
    int d3 = deduce_kind(rr);     // 右值引用变量是左值表达式 ⇒ T := int&
    int d4 = deduce_kind(7);      // 右值          ⇒ T := int

    if (d1 != 1) return 21;       // 左值实参 ⇒ T 是左值引用
    if (d2 != 1) return 22;       // ★ 修好前此处得 9：推导出的 T 是嵌套引用
    if (d3 != 1) return 23;       // 右值引用【变量】仍是左值表达式
    if (d4 != 0) return 24;       // 右值实参 ⇒ T 是裸类型

    return 0;
}
