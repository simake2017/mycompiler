// =============================================================================
// tests/unit/test_vtable_symbols.cpp —— vtable 槽里引用的符号必须都有人定义
// =============================================================================
// 考察理论点：Itanium ABI 的 vtable 是【数据】(`.quad <sym>`)，槽里放的是函数地址 ——
//   在目标文件层面它就是一个**重定位条目**，链接器只认裸字符串，不看类型。
//   于是"同一件事在两处各算一遍名字"这类缺陷，编译期全绿、汇编期看不出来，
//   只有链接器会喊 undefined reference。
//   · 对照 clang：vtable 由 CodeGenModule::EmitVTable 填，槽里的地址就是刚刚
//     发射过的函数（GlobalDecl 句柄），名字由同一个 MangleContext 产出 ⇒
//     结构上不可能"槽里的名字"与"定义的名字"不一致。
//   · 本实现没有句柄，只能各自拼字符串 —— 于是判据必须**收口到一个函数**
//     （semantic_analyzer.cpp 的 memberMethodSymbolName）。本套件钉住的就是这条。
//
// 断言的是【不变量】而不是具体名字：
//   汇编里每个 `.quad <sym>`（局部标签 `.L*` 与纯数字除外）都必须能找到
//   `.globl <sym>` 的定义。命名规则随便改（后缀、Itanium 化、换分隔符），
//   只要两边仍一致，本测试就有意义；一旦有人再写一次"两处各算一遍"，立刻红。
//
// 与 tests/unit/test_symbol_consistency.cpp 互补：那条管 `callq X`（代码引用），
//   本套件管 `.quad X`（数据引用 —— vtable 槽、RTTI 指针都走这条）。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='VTableSymbols.*'
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

// `.quad X`（数据引用：vtable 槽 / RTTI 指针 / 基类 typeinfo 指针）
// 纯数字（offset-to-top、字段偏移）与局部标签（`.Ltype_name_*`，TU 内解析）不计。
std::set<std::string> dataRefs(const std::string& asmCode) {
    std::set<std::string> out;
    std::regex re(R"(\.quad\s+([A-Za-z_][A-Za-z0-9_]*))");
    for (auto it = std::sregex_iterator(asmCode.begin(), asmCode.end(), re);
         it != std::sregex_iterator(); ++it) {
        std::string sym = (*it)[1].str();
        if (sym.starts_with(".L")) continue;   // 局部标签：TU 内自行解析
        out.insert(sym);
    }
    return out;
}

// 不变量：每个数据引用的符号都必须有 `.globl` 定义
void expectEveryDataRefResolves(const std::string& asmCode) {
    auto defined = definedSymbols(asmCode);
    std::vector<std::string> dangling;
    for (const auto& s : dataRefs(asmCode)) {
        if (!defined.count(s)) dangling.push_back(s);
    }
    std::string msg = "以下被 .quad 引用的符号没有任何 .globl 定义（链接期必 undefined reference）：";
    for (const auto& s : dangling) msg += "\n  - " + s;
    EXPECT_TRUE(dangling.empty()) << msg;
}

}  // namespace

// ── ①【B20 缺陷 a】带参虚函数：槽里的名字必须带 `_<形参个数>` 后缀 ─────────────
// 定义点写 Shape_area_1、槽位写 Shape_area 的那种错配，这里立刻红。
TEST(VTableSymbols, ParameterizedVirtualSlotMatchesDefinition) {
    const char* src = R"(
class Shape {
public:
    virtual int area(int k) { return k; }
};
class Square : public Shape {
public:
    int area(int k) { return k * 2; }
};
int main() { Square s; Shape* p = &s; return p->area(3) - 6; }
)";
    const std::string asmCode = compileQuiet(src);
    auto refs = dataRefs(asmCode);
    // 覆写后主表槽指向派生类实现，且名字带参数个数后缀
    EXPECT_TRUE(refs.count("Square_area_1"))
        << "vtable 槽里没有 Square_area_1 —— 槽位与定义点算出的名字不一致";
    EXPECT_EQ(refs.count("Square_area"), 0u)
        << "槽里出现了无后缀的 Square_area（定义点产出的是 Square_area_1）";
    expectEveryDataRefResolves(asmCode);
}

// ── ②【B20 缺陷 b】次基类【未覆写】的槽：名字必须原样透传上游基类 ──────────────
// 菱形 Diamond : P, Q（P 主 Q 次，都没覆写 f/g）⇒ 次表槽必须是 X_f / X_g_1。
// 旧实现按"次基类名"重造出 Q_f ⇒ 凭空捏造一个没人定义的符号。
TEST(VTableSymbols, SecondaryInheritedSlotKeepsUpstreamSymbol) {
    const char* src = R"(
class X {
public:
    int x;
    virtual int f() { return 7; }
    virtual int g(int k) { return k; }
};
class P : public X {};
class Q : public X {};
class Diamond : public P, public Q {};
int main() { Diamond d; Q* pq = &d; return pq->f() + pq->g(5) - 12; }
)";
    const std::string asmCode = compileQuiet(src);
    auto refs = dataRefs(asmCode);
    EXPECT_TRUE(refs.count("X_f")) << "次表槽没指向上游实现 X_f";
    EXPECT_TRUE(refs.count("X_g_1")) << "次表槽没指向上游实现 X_g_1（带参后缀）";
    EXPECT_EQ(refs.count("Q_f"), 0u)
        << "次表槽里出现了 Q_f —— 这是按次基类名重造的假符号（没人定义它）";
    EXPECT_EQ(refs.count("P_f"), 0u) << "次表槽里出现了 P_f —— 同上";
    expectEveryDataRefResolves(asmCode);
}

// ── ③【回归保护】本类覆写【次基类】的虚函数：走 thunk 跳板 ────────────────────
// 这是 thunk 真正被发射的场合：调用方手上是 B*（this 指向次基类子对象），
// 而实现是本类的 D::h —— 必须经跳板把 this 调回对象起始（adjust=-16）。
// 修复 ② 时最容易连带弄坏这半边：把"覆写"也当成"未覆写"一起透传掉，
// 槽里就变成 C_h（丢动态分派），或者指向 D_h 却不调整 this（丢 this 调整）。
TEST(VTableSymbols, OverrideOfSecondaryBaseVirtualUsesThunk) {
    const char* src = R"(
class C { public: int c; virtual int h() { return 1; } };
class B : public C { public: int b; };
class Z { public: int z; virtual int k() { return 9; } };
class D : public Z, public B { public: int d; virtual int h() { return 2; } };
int main() { D d; B* pb = &d; return pb->h() - 2; }
)";
    const std::string asmCode = compileQuiet(src);
    auto refs = dataRefs(asmCode);
    EXPECT_NE(asmCode.find("_thunk"), std::string::npos)
        << "覆写次基类虚函数没有发射 thunk —— this 调整量丢失";
    // 次表槽引用的是 thunk（thunk 自己再跳到 D_h）；thunk 标签必须有定义
    EXPECT_TRUE(refs.count("D_h_thunk16"))
        << "次表槽没指向 thunk 跳板 D_h_thunk16";
    EXPECT_TRUE(definedSymbols(asmCode).count("D_h"))
        << "thunk 的目标 D_h 没有被定义";
    expectEveryDataRefResolves(asmCode);
}

// ── ④【回归保护】RTTI 指针同属数据引用，别在收口符号名时漏掉 ─────────────────
TEST(VTableSymbols, RttiAndTypeinfoPointersResolve) {
    const char* src = R"(
class A { public: int a; virtual int f() { return 1; } };
class B : public A { public: int b; int f() { return 2; } };
int main() { B b; A* p = &b; return p->f() - 2; }
)";
    const std::string asmCode = compileQuiet(src);
    EXPECT_TRUE(dataRefs(asmCode).count("_ZTI1A")) << "没找到 A 的 typeinfo 引用";
    EXPECT_TRUE(dataRefs(asmCode).count("_ZTI1B")) << "没找到 B 的 typeinfo 引用";
    expectEveryDataRefResolves(asmCode);
}

// ── ⑤【已知缺口，故意不写用例】同名重载与槽位身份 ────────────────────────────
// 槽位匹配目前只比【裸名】，不比形参表 —— 于是
//     class C { virtual int f(); int f(int); };
// 里 f(int) 会"认领" f() 的槽：槽改指 C_f_1、且 f(int) 被误标成 virtual。
// 实测（clang 对照）：`c.f() + c.f(2) - 3` clang rc=0，minicc 编译 rc=0 但
// 运行返回 255 —— **静默算错**，因此这里【不写】"只要符号都能解析"的用例：
// 那种断言会在程序算错的同时保持全绿，等于把缺陷固化成契约
// （承 tests/mi/test_mi_04_error.cpp 那次的教训）。
// 缺口登记在 docs/BUGS.md B20 缺陷 c。
