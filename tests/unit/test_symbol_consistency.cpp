// =============================================================================
// tests/unit/test_symbol_consistency.cpp —— 定义点与调用点的符号必须一致
// =============================================================================
// 考察理论点：符号决议（汇编期符号表 → 链接期的 undefined reference）
//   · 一个"定义"（`.globl X` + `X:` 标签）与一个"调用"（`callq X`）能接上头，
//     唯一条件是【两边写出同一个字符串】。
//   · 陷阱形态叫"同一条语义判断写两处"：只要定义点与调用点**各自算一次**符号名
//     （而不是共用同一个来源），规则一改就必然单边漂移。
//   · 本项目真踩过（docs/BUGS.md B10）：定义点给【带参】成员方法名追加"参数个数"
//     后缀（`IntVec_at_1`），调用点硬拼 `类名_方法名`（`IntVec_at`）⇒
//     链接期 `undefined reference to 'IntVec_at'`，而编译期全程绿灯。
//   · 对照 clang：符号名由 MangleContext 统一产出，定义与引用共用同一个
//     ItaniumMangleContext::mangleName —— 结构上不可能两边不一致。
//
// 断言的是【不变量】而不是具体名字：
//   汇编里每个 `callq <sym>` 都必须能在同一份汇编里找到 `.globl <sym>`
//   （`callq *...` 间接调用与白名单外部符号除外）。
//   命名规则随便改（加后缀、Itanium 化、换分隔符），只要两边仍然一致，本测试就有意义；
//   反过来，只要有人再写一次"两处各算一遍"，这里立刻红。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='SymbolConsistency.*'
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

// 本链接器内置（或允许外部提供）的符号。它们【没有】`.globl` 定义是正常的：
//   malloc / free —— 自研链接器注入的运行时桩（注入点在 linker.cpp injectRuntime）
//   其余外部原型（printf 等）当前不支持，故不列白名单 —— 列了反而会掩盖问题。
const std::set<std::string>& externalWhitelist() {
    static const std::set<std::string> wl = {"malloc", "free"};
    return wl;
}

std::set<std::string> collect(const std::string& asmCode, const std::string& pattern) {
    std::set<std::string> out;
    std::regex re(pattern);
    for (auto it = std::sregex_iterator(asmCode.begin(), asmCode.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

// `.globl X`（导出定义）
std::set<std::string> definedSymbols(const std::string& asmCode) {
    return collect(asmCode, R"(\.globl\s+([A-Za-z_][A-Za-z0-9_]*))");
}

// `callq X`（直接调用；`callq *%rax` 是间接调用，vtable 走这条，不计）
std::set<std::string> calledSymbols(const std::string& asmCode) {
    return collect(asmCode, R"(callq\s+([A-Za-z_][A-Za-z0-9_]*))");
}

// 断言：每个直接调用的目标都在这份汇编里定义过（或属于白名单）
void expectEveryCallResolves(const std::string& asmCode) {
    auto defined = definedSymbols(asmCode);
    std::vector<std::string> dangling;
    for (const auto& s : calledSymbols(asmCode)) {
        if (defined.count(s)) continue;
        if (externalWhitelist().count(s)) continue;
        dangling.push_back(s);
    }
    std::string msg = "以下被 callq 的符号没有任何 .globl 定义（链接期必 undefined reference）：";
    for (const auto& s : dangling) msg += "\n  - " + s;
    EXPECT_TRUE(dangling.empty()) << msg;
}

}  // namespace

// ── ① 带参成员方法：B10 的原发场景 ─────────────────────────────────────────
// 定义点给带参方法名加了"参数个数"后缀，调用点若硬拼就必然对不上。
TEST(SymbolConsistency, MemberMethodWithParameter) {
    const char* src = R"(
class C {
public:
    int f(int x) { return x; }
};
int main() { C c; return c.f(1) - 1; }
)";
    expectEveryCallResolves(compileQuiet(src));
}

// ── ② 无参成员方法：本来就一致，留作回归保护 ───────────────────────────────
// （B10 只影响"带参"分支 —— 若将来把后缀规则改成无条件追加，这条会先红。）
TEST(SymbolConsistency, MemberMethodWithoutParameter) {
    const char* src = R"(
class C {
public:
    int g() { return 0; }
};
int main() { C c; return c.g(); }
)";
    expectEveryCallResolves(compileQuiet(src));
}

// ── ③【未覆盖】继承来的成员方法调用 ────────────────────────────────────────
// `class Derived : public Base {}` 的 `d.f(3)`（f 定义在 Base）当前直接报
//   [Semantic Error] No member 'f' in class 'Derived'
// —— 成员查找不走基类链（另案，不在本套件覆盖范围；继承的【字段】访问是通的）。
// 缺口本身记在这里，等它修好后再补一条本套件的用例。

// ── ④ 下标糖：读走 at()、写走 set()，两个符号都得回填 ──────────────────────
TEST(SymbolConsistency, SubscriptSugar) {
    const char* src = R"(
class Vec {
public:
    int a0;
    int at(int i) { if (i == 0) { return a0; } return 0; }
    int set(int i, int v) { if (i == 0) { a0 = v; } return 0; }
};
int main() {
    Vec v;
    v[0] = 7;
    return v[0] - 7;
}
)";
    expectEveryCallResolves(compileQuiet(src));
}

// ── ⑤ 成员模板实例：走 resolvedCalleeSymbol 的第一批用户，防退化 ───────────
TEST(SymbolConsistency, MemberTemplateInstance) {
    const char* src = R"(
class Acc {
public:
    template <class T>
    T add(T a) { return a; }
};
int main() { Acc acc; return acc.add(5) - 5; }
)";
    expectEveryCallResolves(compileQuiet(src));
}

// ── ⑥ 虚函数调用：走 vtable 间接调用（callq *），不该被符号回填影响 ────────
// 这条同时钉住"回填只作用于非虚直接调用分支"这个边界 —— 若哪天回填把虚调用
// 也变成静态 callq，语义就错了（丢动态分派），本用例的间接调用会消失。
TEST(SymbolConsistency, VirtualCallStaysIndirect) {
    const char* src = R"(
class Animal {
public:
    virtual int speak() { return 1; }
};
class Dog : public Animal {
public:
    int speak() { return 2; }
};
int main() { Animal* a = new Dog(); int r = a->speak(); return r - 2; }
)";
    const std::string asmCode = compileQuiet(src);
    expectEveryCallResolves(asmCode);
    // 至少存在一次间接调用（vtable 分派）
    EXPECT_NE(asmCode.find("callq *"), std::string::npos)
        << "虚函数调用退化成了静态 callq —— 动态分派丢失";
}
