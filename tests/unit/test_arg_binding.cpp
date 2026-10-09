// =============================================================================
// tests/unit/test_arg_binding.cpp —— 实参绑定：值类别与限定名成员的位置
// =============================================================================
// 考察理论点（本轮修复的 B25 / B26 / B27 / B24 / B21）：
//   · [dcl.init.ref] 的引用绑定必须先看**值类别**再看类型；本项目此前把两件事
//     写成了两处（模板推导 deducePair 有、非模板调用没有）⇒ 引出 B25/B26。
//     现在判据只有 type.h 的 referenceBindsValueCategory 一处。
//   · [overload.match]/1：候选集非空而可行集为空 ⇒ ill-formed，不许"退回首个"。
//   · [class.member.lookup]/3 名字隐藏：派生类声明了名字，基类整族被隐藏 ——
//     查找必须在"第一个声明了该名字的类"处停住，不继续往基类走。
//   · [expr.prim.id.qual] 限定名的唯一用途是**绕过隐藏**：`D d; d.Base::v` 必须
//     命中 Base 那一条槽位。按裸名查会命中 D 自己那条（findField 的"自身优先"），
//     所以偏移必须由 Sema 回填（名字是给人看的，位置才是机器要的）。
//
// 断言的是【不变量】而非具体拼法：
//   · 两条限定名访问读到的偏移**必须不同**（相同即回填失效 —— 名字用错了，
//     但同一条槽位；这正是"静默算错"的形态，任何守恒式计数都看不出来）
//   · 基类那一条的偏移必须**小于**派生类自己那一条（Itanium：基类子对象在前）
//   · 隐藏生效时编译必须失败，且不产生任何目标代码
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='ArgBinding.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 跑整条流水线（Lexer → Parser → Sema → CodeGen），顺带把这一路的 std::cout
// 日志捞出来 —— 本套件要读的是**回填之后 CodeGen 实际发射的偏移**，
// 故必须跑到汇编，只看 Sema 的日志会把"回填了但没人用"这种错法放过去。
// 语义报错会抛 std::runtime_error ⇒ ok=false（与集成用例的 rc=1 同一件事）。
struct FrontendRun {
    bool        ok = false;
    std::string log;
    std::string asmText;
};

FrontendRun analyzeIt(const std::string& source) {
    FrontendRun r;
    StdoutCapture cap;
    try {
        Lexer lexer(source);
        auto tokens = lexer.tokenizeAll();
        Parser parser(std::move(tokens));
        TranslationUnit unit = parser.parseTranslationUnit();
        SemanticAnalyzer sema;
        sema.analyze(unit);
        CodeGen cg;
        r.asmText = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
        r.ok = true;
    } catch (const std::exception&) {
        r.ok = false;
    }
    r.log = cap.str();
    return r;
}

// 抓汇编里"读某个字段"用的偏移 —— 注释尾巴上的 `偏移 +N`。
// 判据是【CodeGen 实际用的那个数字】，不是 Sema 算出来的中间值：
//   回填了但 CodeGen 不采纳（或反过来）都属于"接错了线"，只有这一层看得见。
std::vector<int> readOffsetsIn(const std::string& asmText, const std::string& needle) {
    std::vector<int> out;
    std::regex re(R"(偏移 \+(\d+))");
    std::istringstream ss(asmText);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find(needle) == std::string::npos) continue;
        std::smatch m;
        if (std::regex_search(line, m, re)) out.push_back(std::stoi(m[1]));
    }
    return out;
}

}  // namespace

// ── ① 限定名必须拿到【基类】那一条的偏移，而不是派生类隐藏它的那一条 ──────────
// 判别力：把 Sema 的回填去掉、改成"按裸名查"，两次访问会落在**同一个偏移**上，
// 汇编照常生成、程序照常跑，只是结果错 —— 只有"两条偏移必须不同"这条不变量看得见。
TEST(ArgBinding, QualifiedMemberPicksTheBaseSubobject) {
    const std::string src = R"(
struct Base { public: int v; };
struct Derived : public Base {
public:
    int v;
    int readBase() { return Base::v; }
    int readOwn()  { return v; }
};
int main() { Derived d; return d.readBase() + d.readOwn(); }
)";
    FrontendRun r = analyzeIt(src);
    ASSERT_TRUE(r.ok) << "本用例应当是合法程序，日志：\n" << r.log;

    // 限定名那条走的是"Sema 回填"分支，注释尾巴写着"读取限定名成员"
    auto baseOff = readOffsetsIn(r.asmText, "读取限定名成员");
    // 裸名那条走的是"按名字查布局"分支，注释尾巴写着"读取字段"
    auto ownOff  = readOffsetsIn(r.asmText, "读取字段 .v");
    ASSERT_FALSE(baseOff.empty()) << "限定名 `Base::v` 没走到回填分支，汇编：\n" << r.asmText;
    ASSERT_FALSE(ownOff.empty())  << "裸名 `v` 没走到按名查找分支，汇编：\n" << r.asmText;

    EXPECT_NE(baseOff.front(), ownOff.front())
        << "`Base::v` 与裸名 `v` 发射了同一个偏移 —— 回填没被采纳（名字是给人看的，"
           "位置才是机器要的；这正是「静默算错」的形态，守恒式计数看不出来）";
    EXPECT_LT(baseOff.front(), ownOff.front())
        << "Itanium 布局：基类子对象在前，派生类自身字段在后";
}

// ── ② 引用绑定判据：const T& 收右值、T&& 收右值、T& 收左值 ────────────────────
// 这三条是 [dcl.init.ref] 的两个调用点（模板推导 / 非模板调用）都要过的同一判据。
// 判别力：判据若只写在模板那条路上（本轮的修复点），非模板那两条立刻红。
TEST(ArgBinding, ReferenceBindingFollowsValueCategory) {
    // const T& ← 右值（模板路径）
    EXPECT_TRUE(analyzeIt(R"(
template<class T> int f(const T& x) { return x; }
int main() { return f(5) - 5; }
)").ok);

    // const T& ← 右值（非模板路径）
    EXPECT_TRUE(analyzeIt(R"(
int f(const int& x) { return x; }
int main() { return f(5) - 5; }
)").ok);

    // int&& ← 右值
    EXPECT_TRUE(analyzeIt(R"(
int f(int&& x) { return x; }
int main() { return f(5) - 5; }
)").ok);

    // int&& ← 左值 —— 必须拒
    EXPECT_FALSE(analyzeIt(R"(
int f(int&& x) { return x; }
int main() { int a = 1; return f(a); }
)").ok) << "右值引用绑左值应当报错（[dcl.init.ref]/5.3）";

    // int& ← 右值 —— 必须拒
    EXPECT_FALSE(analyzeIt(R"(
int f(int& x) { return x; }
int main() { return f(5); }
)").ok) << "非 const 左值引用绑右值应当报错（[dcl.init.ref]/5）";

    // int& ← `*p`（解引用产生左值，[expr.unary.op]/1）—— 必须收
    EXPECT_TRUE(analyzeIt(R"(
int f(int& x) { return x; }
int main() { int a = 1; int* p = &a; return f(*p) - 1; }
)").ok) << "`*p` 是左值，误拒说明值类别模型漏了解引用";
}

// ── ③ 名字隐藏：派生类声明了名字 ⇒ 基类整族不可见（[class.member.lookup]/3）────
// 判别力：去掉"查到名字就停"的守卫，检索会继续走到 Base 并命中 f(int) ⇒ 编译通过。
// 注意 D::f 与 Base::f 的**个数不同**，所以这里淘汰的不是"按个数过滤"那一步 ——
// 淘汰的正是"继续往基类走"那一步。
TEST(ArgBinding, NameHidingStopsBaseLookup) {
    EXPECT_FALSE(analyzeIt(R"(
struct Base { public: int f(int x) { return x; } };
struct Derived : public Base { public: int f() { return 1; } };
int main() { Derived d; return d.f(7); }
)").ok) << "派生类声明了 f 之后，Base::f 应被隐藏 —— 不该查到基类去";

    // 对照组：不声明同名成员时，基类的 f 照常可见
    EXPECT_TRUE(analyzeIt(R"(
struct Base { public: int f(int x) { return x; } };
struct Derived : public Base { public: int g() { return 1; } };
int main() { Derived d; return d.f(7) - 7; }
)").ok);
}

// ── ④ 没有可行候选 ⇒ 报错，不许退回候选集首个（[overload.match]/1）───────────
// 判别力：pickBestByArgs 若退回 candidates.front()，`c.f(p)`（p 是 int*）会被
// 静默派给 f(int) ⇒ 编译通过、运行算错。个数相同 ⇒ 个数过滤那一步也拦不住。
TEST(ArgBinding, NoViableOverloadIsAnError) {
    EXPECT_FALSE(analyzeIt(R"(
struct S { public: int v; };
class C { public: int f(int x) { return x; } int f(S s) { return s.v; } };
int main() { C c; int a = 1; int* p = &a; return c.f(p); }
)").ok) << "int* 既不能转 int 也不能转 S ⇒ 无可行候选，应当报错";
}
