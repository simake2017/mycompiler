// =============================================================================
// tests/unit/test_member_identity.cpp —— 成员函数的【身份】到底是什么
// =============================================================================
// 考察理论点：
//   · [class.member.lookup] 只产出**候选声明集**（按名字查），不筛类型；
//     筛类型是 [overload.best.viable] 的事（个数 → 隐式转换序列 → 最优）。
//     所以"身份"至少是（名字 + 形参类型），"名字 + 参数个数"是**不够**的：
//     `f(int)` 与 `f(S)` 的（名字，个数）完全相同。
//   · 这个判据在本实现里有**三个落点**，错一个就出一种症状（BUGS.md B22 / B20 缺陷 c）：
//       ① 汇编符号名（registerFunction 定 mangledName）
//       ② vtable 槽位身份（processClassDecl 认领槽位 —— 认错就把非虚的标成虚的）
//       ③ 调用点选择（findMethodInClass / 构造函数选定 —— 选错就静默调错函数）
//     三者必须共用同一份判据，故本套件同时钉住三处。
//   · 对照 clang：声明是 Decl* 句柄，查找/匹配/发射天然是同一个东西；
//     名字只在最后一步由 MangleContext 产出（且必然 Injectable：`_ZN1C1fEi` vs `_ZN1C1fE1S`）。
//     minicc 没有句柄，只能靠**收口到同一个函数**来达到同等效果。
//
// 断言的是【不变量】而不是具体拼法（命名规则随便改都不该误报）：
//   · 两个同签名个数的方法 ⇒ 必须**两个不同的符号**（否则汇编期 symbol already defined）
//   · 非虚的重载**不得**出现在任何 `.quad` 里（vtable 槽只装虚函数）
//   · 派生类覆写后，槽位数不因基类多出一个同名的非虚重载而变化
//   · `D a(1);` 与 `D b(s);` 两条构造调用必须落到**不同的**构造函数符号
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='MemberIdentity.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 源码 → 完整汇编（语义日志全部吞掉）
std::string compileQuiet(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    CodeGen cg;
    return cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
}

std::set<std::string> collect(const std::string& text, const std::string& pattern) {
    std::set<std::string> out;
    std::regex re(pattern);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

// 带某前缀的 `.globl` 定义（= 实际发射出来的函数符号）
std::set<std::string> definitionsWithPrefix(const std::string& asmCode,
                                            const std::string& prefix) {
    std::set<std::string> out;
    for (auto& s : collect(asmCode, R"(\.globl\s+([A-Za-z_][A-Za-z0-9_]*))")) {
        if (s.starts_with(prefix)) out.insert(s);
    }
    return out;
}

// 带某前缀的 `.quad` 数据引用（= vtable 槽里放的地址）
std::set<std::string> dataRefsWithPrefix(const std::string& asmCode,
                                         const std::string& prefix) {
    std::set<std::string> out;
    for (auto& s : collect(asmCode, R"(\.quad\s+([A-Za-z_][A-Za-z0-9_]*))")) {
        if (s.starts_with(prefix) && !s.starts_with(".L")) out.insert(s);
    }
    return out;
}

// 所有 `callq 目标`（CodeGen 调函数时用的符号；间接调用 `callq *%rax` 不匹配）
std::set<std::string> callTargetsWithPrefix(const std::string& asmCode,
                                            const std::string& prefix) {
    std::set<std::string> out;
    for (auto& s : collect(asmCode, R"(callq\s+([A-Za-z_][A-Za-z0-9_]*))")) {
        if (s.starts_with(prefix)) out.insert(s);
    }
    return out;
}

const char* kStructS = R"(
struct S { public: int v; };
)";

}  // namespace

// ── ① 同签名个数的两个方法 ⇒ 必须两个不同的符号 ───────────────────────────────
// 修复前：两者都定名 `C_f_1` ⇒ as 报 symbol 'C_f_1' is already defined（编译 rc=1）。
// 这里不断言具体拼法，只断言"两个方法各有各的符号"，命名规则怎么改都成立。
TEST(MemberIdentity, SameNameSameArityGetDistinctSymbols) {
    const std::string src = std::string(kStructS) + R"(
class C {
public:
    int f(int x) { return 1; }
    int f(S s) { return 2; }
};
int main() { C c; S s; return c.f(1) + c.f(s); }
)";
    auto defs = definitionsWithPrefix(compileQuiet(src), "C_f");
    EXPECT_EQ(defs.size(), 2u)
        << "两个同签名个数的重载应各有定义，实际发射了 " << defs.size() << " 个 C_f* 定义";
}

// ── ② 没有同签名个数的兄弟时，符号名不该被改动（承重不变量）──────────────────
// 类型后缀是"**只在必要时**才挂"的 —— 否则全项目既有产物会集体漂移（logdiff 基线）。
TEST(MemberIdentity, LoneMethodNameUnchanged) {
    const std::string withSibling = std::string(kStructS) + R"(
class C { public: int f(int x) { return x; } };
)";
    const std::string alone = std::string(kStructS) + R"(
class C {
public:
    int f(int x) { return x; }
    int g(int x) { return x; }
};
)";
    // 两种写法里 f(int) 都是"独占该名字"⇒ 符号必须一字不差
    EXPECT_EQ(definitionsWithPrefix(compileQuiet(withSibling), "C_f"),
              definitionsWithPrefix(compileQuiet(alone), "C_f"));
}

// ── ③ 非虚的重载不得占 vtable 槽（B20 缺陷 c 的核心不变量）─────────────────────
// 槽位身份若窄化成裸名，非虚的 `f(int)` 会认领虚的 `f()` 的槽并被顺手标成 virtual。
// 症状不是报错而是**静默算错**（c.f(2) 走虚调用跳进无参的 f()，实测返回 255）。
TEST(MemberIdentity, NonVirtualOverloadDoesNotOccupyVTableSlot) {
    const std::string src = std::string(kStructS) + R"(
class C {
public:
    virtual int f() { return 1; }
    int f(int x) { return x + 10; }
};
int main() { C c; return c.f() + c.f(2); }
)";
    const std::string asmCode = compileQuiet(src);
    auto defs = definitionsWithPrefix(asmCode, "C_f");
    auto refs = dataRefsWithPrefix(asmCode, "C_f");      // 占槽（= 被当成虚函数）
    auto direct = callTargetsWithPrefix(asmCode, "C_f"); // 被**直接**调用（= 非虚）

    EXPECT_EQ(defs.size(), 2u) << "两个重载都该有定义";
    EXPECT_EQ(refs.size(), 1u)
        << "vtable 里应只有【虚的】那一个 f 槽，实际有 " << refs.size() << " 个";

    // ★ 判别力最强的一条：每个重载「要么占一个槽（虚、经 vtable 间接调）、
    //   要么被直接 callq」，两者互斥且穷尽 —— 本实现的虚调用一律 `callq *%rax`
    //   不去虚化，非虚调用一律直接 callq，所以这个计数是**语义**而非拼法。
    //   槽位身份窄化成裸名时，非虚的 f(int) 被当成虚函数 ⇒ 它既占了槽、
    //   又没人直接调它 ⇒ 0 + 1 ≠ 2，立刻红（而"槽里只有一个符号"那条不变量看不出来）。
    for (const auto& d : direct)
        EXPECT_EQ(refs.count(d), 0u) << d << " 既占了 vtable 槽、又被直接调用（身份判错了）";
    EXPECT_EQ(direct.size() + refs.size(), defs.size())
        << "有重载既不在槽里、也没被直接调用 —— 它被误当成了另一个函数";
}

// ── ④ 派生类覆写后，槽位数不因基类多出的同名非虚重载而变化 ────────────────────
TEST(MemberIdentity, DerivedOverrideKeepsSlotCount) {
    const std::string src = std::string(kStructS) + R"(
class Base {
public:
    virtual int f() { return 1; }
    int f(int x) { return x + 10; }
};
class Derived : public Base {
public:
    virtual int f() { return 100; }
};
// ★ 非虚重载那一半必须经 Base* 调（`p->f(2)`）—— 写成 `d.f(2)` 是**非法程序**：
//   Derived 声明了 f ⇒ 基类整族被隐藏（[class.member.lookup]/3），clang 报
//   "too many arguments to function call, expected 0, have 1; did you mean 'Base::f'?"。
//   本用例此前写的正是 `d.f(2)`，能过只是因为本实现在这里**太宽**（BUGS.md B24）；
//   B24 修好后它当场变红 —— 又一次印证"基线会把失败固化成契约"。
int main() { Derived d; Base* p = &d; return p->f() + p->f(2); }
)";
    const std::string asmCode = compileQuiet(src);
    auto refs = dataRefsWithPrefix(asmCode, "Derived_f");
    EXPECT_EQ(refs.size(), 1u)
        << "Derived 只有一个虚函数 f()，槽位应恰好 1 个，实际 " << refs.size();

    // 覆写后槽里指向 Derived 的实现；非虚重载的符号绝不进槽
    auto derivedDefs = definitionsWithPrefix(asmCode, "Derived_f");
    for (const auto& r : refs)
        EXPECT_TRUE(derivedDefs.count(r)) << "槽里的 " << r << " 不是 Derived 里发射过的符号";
}

// ── ⑤★ 判别力最强的一条：经基类指针的虚调用**必须**是间接调用 ─────────────────
// 为什么非得有这一条：③④ 的不变量是**对称**的 —— 槽位身份认错时，"谁在槽里、谁被直接调"
//   会整体互换，计数照样配平（实测：删掉 signature 判据，③④⑥ 全绿）。
//   而这一条不对称：认错槽位 ⇒ CodeGen 按 resolvedCalleeSymbol 精确比对**落空** ⇒
//   虚调用退化成 `callq Base_f` 直接调用 ⇒ 运行期不派发，返回基类结果（静默算错）。
//   正确时恰好一次 `callq *%rax`，认错时 0 次 —— 与命名规则完全无关。
TEST(MemberIdentity, VirtualCallOnPointerStaysVirtual) {
    const std::string src = std::string(kStructS) + R"(
class Base {
public:
    virtual int f() { return 1; }
    int f(int x) { return x + 10; }
};
class Derived : public Base {
public:
    virtual int f() { return 100; }
};
int main() { Derived d; Base* p = &d; return p->f(); }
)";
    const std::string asmCode = compileQuiet(src);

    size_t indirect = 0;
    std::regex re(R"(callq\s+\*)");
    for (auto it = std::sregex_iterator(asmCode.begin(), asmCode.end(), re);
         it != std::sregex_iterator(); ++it) {
        ++indirect;
    }
    EXPECT_EQ(indirect, 1u)
        << "经 Base* 调虚函数应产生 1 次间接调用（callq *%rax），实际 " << indirect
        << " 次 —— 0 次说明它退化成了直接调用（槽位身份认错，运行期不派发）";
}

// ── ⑥ 构造函数的选定也按类型（B22 第三处现场）─────────────────────────────────
// 修复前：按【参数个数】选 ⇒ `D b(s);` 撞上声明序第一个构造函数（编译全绿、运行错）。
TEST(MemberIdentity, CtorSelectedByArgumentType) {
    const std::string src = std::string(kStructS) + R"(
class D {
public:
    int tag;
    D(int x) { tag = 1; }
    D(S s) { tag = 2; }
};
int main() { D a(1); S s; D b(s); return a.tag + b.tag; }
)";
    auto targets = callTargetsWithPrefix(compileQuiet(src), "D_D");
    EXPECT_EQ(targets.size(), 2u)
        << "两次构造调用应落到两个不同的构造函数符号，实际 " << targets.size() << " 个";
}
