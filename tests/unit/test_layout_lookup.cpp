// =============================================================================
// tests/unit/test_layout_lookup.cpp —— 多继承下的「成员住在哪个子对象里」
// =============================================================================
// 考察理论点（对应 docs/BUGS.md B11~B15、docs/learn/17）：
//   1. Itanium 主基类优化（[class.mi]）：primary base 只在【动态（多态）基类】里选。
//      本类自身多态而所有基类都非多态时 ⇒【没有主基类】：vptr 自己占 offset 0，
//      全部基类子对象从 8 起摆。
//   2. [class.member.lookup]/3：派生类声明的同名成员【隐藏】基类的 —— 且这条规则
//      在"自身字段为此被改过显示名"的实现下最容易做反（本项目一度做反，见 B14）。
//   3. [class.member.lookup]/8：两个不同基类子对象各有一个同名成员 ⇒ ill-formed，
//      必须报歧义，不能"先到先得"静默绑一个。
//   4. 布局寻址应当按【结构】（哪个子对象、第几条记录）而非【名字字符串】——
//      对照 clang：成员是 FieldDecl* + getFieldIndex()，
//      RecordLayoutBuilder 从不做字符串匹配。
//
// 断言的是【不变量】而不是症状值（同 test_codegen_frame.cpp 的风格）：
//   · 多态类的任何一个字段都不得落在 [0,8)（_vptr 区）；
//   · findField 返回的那条记录，其 viaBase 必须与"它真正来自哪个直接基类"一致；
//   · 名字查找的结果不得随"显示名前缀是第几代"改变。
// 这样即使将来改了显示名的拼法（"B.x" → "A.B.x"），只要语义没坏，测试仍然有效。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='LayoutLookup.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 源码 → Sema（类布局全在本对象里）；日志被吞掉。
// 返回 shared_ptr 而非值：Sema 不必可移动，且 StdoutCapture 只护住 analyze 这一段。
std::shared_ptr<SemanticAnalyzer> analyzeQuiet(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    auto sema = std::make_shared<SemanticAnalyzer>();
    sema->analyze(unit);
    return sema;
}

// 取某类的布局；类不存在时返回 nullptr
const ClassLayout* layoutOf(const std::shared_ptr<SemanticAnalyzer>& sema,
                            const std::string& cls) {
    const auto& types = sema->getClassTypes();
    auto it = types.find(cls);
    return it == types.end() ? nullptr : &it->second->classLayout;
}

// 源码 → 编译期日志（Sema 的 cout 全捕获）；用于断言"报了什么错 / 打了什么日志"
std::string semaLogOf(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    return cap.str();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 【B11】本类自身多态 + 全部基类非多态 ⇒ 没有主基类，_vptr 独占 offset 0
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutLookup, VptrNeverOverlapsFields) {
    // 三种形态一起验：① 单非多态基类+本类虚函数（原 bug 现场）
    //                    ② 两个非多态基类+本类虚函数
    //                    ③ 多态基类在前（primary = P，回归既有正确行为）
    const std::vector<std::string> sources = {
        "struct B { int name; };\n"
        "struct A : B { virtual int f() { return 5; } };\n"
        "int main() { A a; a.name = 3; return a.f() + a.name; }\n",

        "struct B { int x; };\n"
        "struct C { int y; };\n"
        "struct A : B, C { virtual int f() { return 5; } };\n"
        "int main() { A a; a.x = 1; a.y = 2; return a.f() + a.x + a.y; }\n",

        "struct P { virtual int f() { return 1; } };\n"
        "struct B { int x; };\n"
        "struct A : B, P { int own; };\n"
        "int main() { A a; return 0; }\n",
    };

    for (size_t i = 0; i < sources.size(); ++i) {
        auto sema = analyzeQuiet(sources[i]);
        for (auto& [name, type] : sema->getClassTypes()) {
            const auto& layout = type->classLayout;
            if (!layout.hasVTable) continue;
            // ★ 不变量：有 vptr 的类，没有任何字段压在 [0, 8) 上
            for (const auto& f : layout.fields) {
                EXPECT_FALSE(f.offset < 8)
                    << "用例 " << i << "：类 '" << name << "' 的字段 '" << f.name
                    << "' 偏移 " << f.offset << " 落在 _vptr 区（0..7）—— 写它即写坏虚表指针";
            }
        }
    }

    // 现场钉值：clang oracle ⇒ sizeof(A)=16、B::name 在偏移 8
    auto sema = analyzeQuiet(sources[0]);
    const ClassLayout* a = layoutOf(sema, "A");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->fields.size(), 1u);
    EXPECT_EQ(a->fields[0].name, "B.name");
    EXPECT_EQ(a->fields[0].offset, 8u) << "clang：A.size=16、offsetof(A,name)=8";
    EXPECT_EQ(a->totalSize, 16u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 【B14】自身字段隐藏基类同名字段（[class.member.lookup]/3）
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutLookup, OwnFieldHidesInheritedOne) {
    // clang oracle：d.x 是 D::x（=5），A::x 要靠 d.A::x 才拿到（=7）
    const std::string src =
        "class A { public: int x; A() : x(7) {} int getA() { return x; } };\n"
        "class D : public A { public: int x; D() : x(5) {} };\n"
        "int main() { D d; return d.getA() * 10 + d.x; }\n";   // clang = 75

    auto sema = analyzeQuiet(src);
    const ClassLayout* d = layoutOf(sema, "D");
    ASSERT_NE(d, nullptr);

    const FieldInfo* hit = d->findField("x");
    ASSERT_NE(hit, nullptr) << "d.x 必须查得到（它只是被隐藏，不是不存在）";
    // ★ 不变量：命中的必须是【本类自身】那条（viaBase 为空），而不是基类那条
    EXPECT_TRUE(hit->viaBase.empty())
        << "d.x 命中了经 '" << hit->viaBase << "' 的 '" << hit->name
        << "'，而 [class.member.lookup]/3 规定派生类声明隐藏基类声明";
    EXPECT_EQ(d->findFields("x").size(), 1u)
        << "自身字段已隐藏基类同名字段 ⇒ 不是歧义，命中应唯一";

    // 基类那条仍然存在、仍在低偏移（这就是"隐藏"与"覆盖"的区别）
    const FieldInfo* baseHit = layoutOf(sema, "A")->findField("x");
    ASSERT_NE(baseHit, nullptr);
    EXPECT_TRUE(baseHit->viaBase.empty());   // A 的 x 是 A 自己的
    EXPECT_LT(baseHit->offset, hit->offset);
}

// ─────────────────────────────────────────────────────────────────────────────
// 【B13/B15】名字查找不得依赖"显示名前缀是第几代" + 歧义必须可检出
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutLookup, LookupIsPrefixIndependentAndAmbiguityIsVisible) {
    // 祖辈前缀与直接基类撞名：X : P, B，其中 P 自己也继承了一个 B
    // 真 C++：o.x ill-formed（两个 B 子对象各有一份），此处只验布局与歧义可检出
    const std::string src =
        "struct Q { virtual int g() { return 0; } };\n"
        "struct B { int x; };\n"
        "struct P : Q, B { int y; };\n"
        "struct X : P, B { int tag; };\n"
        "int main() { return 0; }\n";

    auto sema = analyzeQuiet(src);
    const ClassLayout* x = layoutOf(sema, "X");
    ASSERT_NE(x, nullptr);

    // 两条来自不同子对象的同名字段，必须【各自】落在自己的子对象里
    std::vector<const FieldInfo*> hits;
    for (auto& f : x->fields)
        if (f.bareName() == "x") hits.push_back(&f);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_NE(hits[0]->offset, hits[1]->offset)
        << "两条记录偏移相同 ⇒ 其中一条被算到了错的子对象上（B15 现场：+8 被算成 +16）";
    for (auto* f : hits) {
        // ★ 不变量：viaBase 必须是【本类的直接基类】之一（'P' / 'B'），
        //   而不是祖辈那条记录里留下的旧值
        EXPECT_TRUE(f->viaBase == "P" || f->viaBase == "B")
            << "viaBase='" << f->viaBase << "' 不是 X 的直接基类 ⇒ 偏移会算到错的子对象";
    }
    EXPECT_EQ(x->findFields("x").size(), 2u)
        << "两个子对象各有一份 ⇒ 查找必须能看出「不止一条」（歧义可检出）";

    // 歧义访问要【响亮报错】（[class.member.lookup]/8），而不是静默取第一条
    const std::string amb =
        "struct Q { virtual int g() { return 0; } };\n"
        "struct B { int x; };\n"
        "struct P : Q, B { int y; };\n"
        "struct X : P, B { int tag; };\n"
        "int main() { X o; o.x = 1; return 0; }\n";
    bool threw = false;
    try {
        semaLogOf(amb);
    } catch (const std::runtime_error& e) {
        threw = true;
        std::string msg = e.what();
        EXPECT_NE(msg.find("ambiguous"), std::string::npos) << msg;
    }
    EXPECT_TRUE(threw) << "o.x 在真 C++ 里是 ill-formed，本实现必须报错而非静默绑定";
}

// ─────────────────────────────────────────────────────────────────────────────
// 【B12】继承来的成员方法要查得到（inferMember 与 inferCall 同一条查找规则）
// ─────────────────────────────────────────────────────────────────────────────
TEST(LayoutLookup, InheritedMethodIsFound) {
    const std::string src =
        "struct A { int g() { return 5; } };\n"
        "struct D : A { };\n"
        "int main() { D d; return d.g(); }\n";

    std::string log = semaLogOf(src);   // 不抛异常即通过（修复前抛 "No member 'g' in class 'D'"）
    EXPECT_NE(log.find("[member] D.g() → int"), std::string::npos)
        << "继承方法的日志缺失 —— 成员访问没走基类链";
    EXPECT_NE(log.find("via 'A'"), std::string::npos)
        << "日志应点明方法是在哪一层命中的";

    // 多级链 + 指针形态（p->g()）
    const std::string deep =
        "struct A { int g() { return 7; } };\n"
        "struct B : A { };\n"
        "struct C : B { };\n"
        "int main() { C* p = new C(); return p->g(); }\n";
    std::string log2 = semaLogOf(deep);
    EXPECT_NE(log2.find("[member] C.g() → int"), std::string::npos);
}
