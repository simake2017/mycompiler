// =============================================================================
// tests/unit/test_decltype_sfinae.cpp —— decltype / SFINAE / 偏序 白盒单元测试
// =============================================================================
// 目的：从【一段真实源码】出发走 Lexer → Parser → SemanticAnalyzer，
//       直接观察三者中最难"看一眼就懂"的三套机制的内部结果：
//         decltype  —— 两套求值规则的实际取值
//         SFINAE    —— 替换失败是"软失败"还是"硬错误"
//         偏序      —— [temp.class.order] 到底选中了哪一条偏特化
//
// 与 tests/tmpl/test_tmpl_33..43 的分工：
//   tests/tmpl/*.cpp 是【黑盒集成】用例：跑 minicc 与 clang 对比退出码。
//   本文件是【白盒单元】用例：不开子进程，直接断言 Sema 内部的产物，
//   可以断言到"实例化的那个类里 tag() 返回的常量是多少"这一层。
//
// 观察手法（偏序部分的关键）：
//   类模板实例化后，主模板与任一派生偏特化产出的实例类【名字是一样的】
//   （Box<int**> 无论走哪条都叫 Box_intPP）。所以不能靠名字判断，
//   必须看实例类的成员函数体 —— 见 helper `returnedConstantOf`。
//   这也是 SemanticAnalyzer 开放 getClassDecls() 的原因。
//
// 运行方式：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='Decltype.*:Sfinae.*:PartialOrder.*'
//
// 套件 → 理论点 → clang 对照：
//   Decltype      [dcl.type.decltype] 两套规则（声明类型 vs 表达式类型+左值）
//                 → clang SemaType.cpp::BuildDecltypeType / Sema::ActOnDecltype
//   Sfinae        [temp.deduct]/8 软失败 vs 硬错误的分界
//                 → clang SemaTemplateDeduction.cpp（SFINAEFailure 的传播范围）
//   PartialOrder  [temp.class.order] 部分排序（互推 + 唯一合成类型）
//                 → clang SemaTemplate.cpp::isMoreSpecializedThan
// =============================================================================

#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "ast.h"
#include "type.h"
#include "sfinae.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

using namespace minicc;

namespace {

// ── 辅助：源码字符串 → 词法分析 → 语法分析 → AST 根 ──────────────────────
TranslationUnit parseCode(const std::string& src) {
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    return parser.parseTranslationUnit();
}

// ── 辅助：吞掉分析期日志，让测试输出只剩断言结果 ──────────────────────────
class StdoutCapture {
    std::streambuf* old_;
    std::stringstream buf_;
public:
    StdoutCapture() : old_(nullptr) { old_ = std::cout.rdbuf(buf_.rdbuf()); }
    ~StdoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return buf_.str(); }
};

// ── 辅助：取实例类 clsName 的 methodName 方法【返回的整型常量】──────────
// 【为什么需要它】偏序择优的结果无法从实例类名字上分辨：
//   主模板 Box<T> 与偏特化 Box<T**> 对实参 <int**> 都产出 'Box_intPP'。
//   唯一可靠的观察点是实例出来的函数体里那个 return 语句的常量值。
//
// 返回 std::nullopt 表示没找到（测试里配合 ASSERT_TRUE 报错）。
std::optional<int64_t> returnedConstantOf(
    const std::unordered_map<std::string, ClassDeclPtr>& decls,
    const std::string& clsName, const std::string& methodName) {

    auto it = decls.find(clsName);
    if (it == decls.end()) return std::nullopt;

    for (auto& m : it->second->methods) {
        if (m->name != methodName) continue;
        if (!m->body) return std::nullopt;
        for (auto& st : m->body->statements) {
            if (st->kind != NodeKind::Return) continue;
            auto ret = std::dynamic_pointer_cast<ReturnStmt>(st);
            if (!ret || !ret->value) return std::nullopt;
            if (auto lit = std::dynamic_pointer_cast<IntLiteralExpr>(ret->value))
                return lit->value;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// ── 辅助：跑完整分析，返回 (sema, 是否抛异常) ─────────────────────────────
// 注意 Sema 不能拷贝，故用 lambda 形式就地断言。
template <typename Fn>
void withSema(const std::string& src, Fn&& fn) {
    auto unit = parseCode(src);
    SemanticAnalyzer sema;
    StdoutCapture cap;
    sema.analyze(unit);
    fn(sema, cap.str());
}

} // namespace

// =============================================================================
// decltype（[dcl.type.decltype]）
// =============================================================================

// 1. 两套规则的分水岭：decltype(e) 里 e 是不是【未加括号的 id-expression】
//      decltype(a)   → a 是未加括号的 id-expression ⇒ 取【声明类型】int
//      decltype((a)) → 多一层括号 ⇒ 退化为普通表达式 ⇒ 取【表达式类型】
//                      且 a 是左值 ⇒ 结果带引用 ⇒ int&
//
//    本项目不直接暴露"求值后的类型"，改用一个可观测的等价物：
//    把两种 decltype 分别喂给类模板偏特化 Probe<T> / Probe<T&>，
//    看哪一条被选中 —— 选中 T& 就说明 decltype 的结果确实是引用类型。
TEST(Decltype, ParenRuleSelectsReferenceSpecialization) {
    std::string src =
        "template <typename T> struct Probe { int tag() { return 0; } };\n"
        "template <typename T> struct Probe<T&> { int tag() { return 1; } };\n"
        "int main() {\n"
        "    int a = 5;\n"
        "    Probe<decltype(a)>   p1;\n"    // int   → 主模板
        "    Probe<decltype((a))> p2;\n"    // int&  → 偏特化
        "    return p1.tag() + p2.tag();\n"
        "}\n";

    withSema(src, [](SemanticAnalyzer& sema, const std::string& log) {
        auto& decls = sema.getClassDecls();

        // decltype(a) = int ⇒ 实例类名 'Probe_int'，走主模板 ⇒ tag() = 0
        auto t1 = returnedConstantOf(decls, "Probe_int", "tag");
        ASSERT_TRUE(t1.has_value()) << "未找到实例类 Probe_int::tag；日志尾部:\n"
                                    << log.substr(log.size() > 2000 ? log.size() - 2000 : 0);
        EXPECT_EQ(*t1, 0) << "decltype(a) 应为声明类型 int（主模板）";

        // decltype((a)) = int& ⇒ 实例类名 'Probe_intR'，走偏特化 ⇒ tag() = 1
        auto t2 = returnedConstantOf(decls, "Probe_intR", "tag");
        ASSERT_TRUE(t2.has_value()) << "未找到实例类 Probe_intR::tag —— "
                                       "说明 decltype((a)) 没有取到引用类型 int&";
        EXPECT_EQ(*t2, 1) << "decltype((a)) 应为表达式类型 int&（偏特化）";
    });
}

// 2. decltype 覆盖三种使用形态：变量 / 函数调用 / 取地址
//    对照 [expr.unary.op]/3：&x 的类型是"指向 x 类型"的指针。
TEST(Decltype, VariableCallAndAddressOf) {
    std::string src =
        "int twice(int x) { return x + x; }\n"
        "int main() {\n"
        "    int a = 7;\n"
        "    decltype(a)        b = 3;\n"      // b : int
        "    decltype(twice(1)) c = 5;\n"      // c : int（取函数返回类型，不调用！）
        "    decltype(&a)       p = &a;\n"     // p : int*
        "    return b + c;\n"
        "}\n";

    // 只要分析不抛异常即说明三个 decltype 都解析并求值成功；
    // 真正的类型正确性由 tests/tmpl/test_tmpl_33 的退出码回归把关。
    EXPECT_NO_THROW(withSema(src, [](SemanticAnalyzer&, const std::string&) {}));
}

// =============================================================================
// SFINAE（[temp.deduct]/8）
// =============================================================================

// 3. 软失败：探测不成立时【静默回退主模板】，不抛异常、不报错。
//    这是 SFINAE 的字面含义 —— Substitution Failure Is Not An Error。
TEST(Sfinae, SoftFailureFallsBackToPrimary) {
    std::string src =
        "struct HasBegin { int begin() { return 3; } };\n"
        "struct Empty { };\n"
        "template <typename T, typename = void>\n"
        "struct Probe : public std::false_type { int tag() { return 0; } };\n"
        "template <typename T>\n"
        "struct Probe<T, std::void_t<decltype(std::declval<T>().begin())>>\n"
        "    : public std::true_type { int tag() { return 1; } };\n"
        "int main() {\n"
        "    Probe<HasBegin> a;\n"
        "    Probe<Empty>    b;\n"
        "    return a.tag() + b.tag();\n"
        "}\n";

    withSema(src, [](SemanticAnalyzer& sema, const std::string& log) {
        auto& decls = sema.getClassDecls();

        auto hit  = returnedConstantOf(decls, "Probe_HasBegin_void", "tag");
        auto miss = returnedConstantOf(decls, "Probe_Empty_void", "tag");

        ASSERT_TRUE(hit.has_value())  << "Probe<HasBegin> 未实例化";
        ASSERT_TRUE(miss.has_value()) << "Probe<Empty> 未实例化 —— 软失败被误当成硬错误了？";
        EXPECT_EQ(*hit, 1)  << "HasBegin 有 begin() ⇒ 应命中偏特化";
        EXPECT_EQ(*miss, 0) << "Empty 无 begin() ⇒ 应静默回退主模板";

        // 日志断言：软失败的标志性 trace 必须出现
        EXPECT_NE(log.find("[sfinae]"), std::string::npos)
            << "缺少 [sfinae] 追踪日志";
        EXPECT_NE(log.find("falling back to PRIMARY template"), std::string::npos)
            << "缺少回退主模板的 trace";
    });
}

// 4. 硬错误：兜底不存在时，同一个替换失败会升级为编译错误。
//    ★ 与 Test 3 对照阅读：失败原因完全相同，差别只在主模板接不接得住。
TEST(Sfinae, HardErrorWhenNoFallback) {
    std::string src =
        "struct Empty { };\n"
        "template <typename T, typename = void>\n"
        "struct OnlyProbe { int other() { return 0; } };\n"   // 没有 value
        "template <typename T>\n"
        "struct OnlyProbe<T, std::void_t<decltype(std::declval<T>().begin())>>\n"
        "    : public std::true_type { };\n"
        "int main() {\n"
        "    bool v = OnlyProbe<Empty>::value;\n"
        "    return v ? 1 : 0;\n"
        "}\n";

    EXPECT_THROW(withSema(src, [](SemanticAnalyzer&, const std::string&) {}),
                 std::runtime_error)
        << "偏特化被移除且主模板没有 value ⇒ 必须报硬错误";
}

// 5. void_t 探测是"全有或全无"：部分满足不算命中。
//    HalfRange 有 begin() 没有 end()，第一个探测通过、第二个失败 ——
//    整个偏特化照样被移出候选集。
TEST(Sfinae, VoidTIsAllOrNothing) {
    std::string src =
        "struct FullRange { int begin() { return 1; } int end() { return 2; } };\n"
        "struct HalfRange { int begin() { return 1; } };\n"
        "template <typename T, typename = void>\n"
        "struct is_range : public std::false_type { int tag() { return 0; } };\n"
        "template <typename T>\n"
        "struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),\n"
        "                               decltype(std::declval<T>().end())>>\n"
        "    : public std::true_type { int tag() { return 1; } };\n"
        "int main() {\n"
        "    is_range<FullRange> a;\n"
        "    is_range<HalfRange> b;\n"
        "    return a.tag() + b.tag();\n"
        "}\n";

    withSema(src, [](SemanticAnalyzer& sema, const std::string&) {
        auto& decls = sema.getClassDecls();
        auto full = returnedConstantOf(decls, "is_range_FullRange_void", "tag");
        auto half = returnedConstantOf(decls, "is_range_HalfRange_void", "tag");

        ASSERT_TRUE(full.has_value());
        ASSERT_TRUE(half.has_value());
        EXPECT_EQ(*full, 1) << "两个成员齐全 ⇒ 命中";
        EXPECT_EQ(*half, 0) << "只满足一半 ⇒ 必须整体失效，不能部分命中";
    });
}

// =============================================================================
// 偏序裁决（[temp.class.order]）
// =============================================================================

// 6. T* 与 T** 同时匹配 <int**> ⇒ 更特化的 T** 胜出。
TEST(PartialOrder, PointerDepthPicksMoreSpecialized) {
    std::string src =
        "template <typename T> struct Box { int tag() { return 0; } };\n"
        "template <typename T> struct Box<T*>  { int tag() { return 1; } };\n"
        "template <typename T> struct Box<T**> { int tag() { return 2; } };\n"
        "int main() {\n"
        "    Box<int>   a;\n"
        "    Box<int*>  b;\n"
        "    Box<int**> c;\n"
        "    return a.tag() + b.tag() + c.tag();\n"
        "}\n";

    withSema(src, [](SemanticAnalyzer& sema, const std::string&) {
        auto& d = sema.getClassDecls();
        EXPECT_EQ(returnedConstantOf(d, "Box_int", "tag"),     0);
        EXPECT_EQ(returnedConstantOf(d, "Box_intP", "tag"), 1);
        EXPECT_EQ(returnedConstantOf(d, "Box_intPP", "tag"), 2)
            << "T* 与 T** 都匹配 <int**> ⇒ 偏序应选更特化的 T**";
    });
}

// 7. 偏序与【声明顺序】无关 —— 把两条偏特化倒过来声明，结论必须一样。
//    ★ 这条是判别性实验：一个"取最后声明的匹配者"的错误实现
//      能通过 Test 6，但必然在这里翻车。
TEST(PartialOrder, DeclarationOrderIndependent) {
    std::string src =
        "template <typename T> struct Box { int tag() { return 0; } };\n"
        "template <typename T> struct Box<T**> { int tag() { return 2; } };\n"  // 更特化者在前
        "template <typename T> struct Box<T*>  { int tag() { return 1; } };\n"
        "int main() {\n"
        "    Box<int*>  b;\n"
        "    Box<int**> c;\n"
        "    return b.tag() + c.tag();\n"
        "}\n";

    withSema(src, [](SemanticAnalyzer& sema, const std::string&) {
        auto& d = sema.getClassDecls();
        EXPECT_EQ(returnedConstantOf(d, "Box_intP", "tag"), 1)
            << "若实现是'取最后一个匹配者'，这里会错误地得到 2";
        EXPECT_EQ(returnedConstantOf(d, "Box_intPP", "tag"), 2);
    });
}

// 8. 互不更特化 ⇒ 歧义 ⇒ 必须报错，绝不能静默挑一个。
TEST(PartialOrder, AmbiguousSpecsThrow) {
    std::string src =
        "template <typename T, typename U> struct P { int tag() { return 0; } };\n"
        "template <typename T, typename U> struct P<T*, U> { int tag() { return 1; } };\n"
        "template <typename T, typename U> struct P<T, U*> { int tag() { return 2; } };\n"
        "int main() {\n"
        "    P<int*, int*> b;\n"    // 两条都匹配，且互不更特化
        "    return b.tag();\n"
        "}\n";

    EXPECT_THROW(withSema(src, [](SemanticAnalyzer&, const std::string&) {}),
                 std::runtime_error)
        << "'P<T*,U>' 与 'P<T,U*>' 对 <int*,int*> 互不更特化 ⇒ 必须报歧义";
}

// 9. 三路偏序：链式更特化关系靠传递性 + dominance 循环裁决。
//    T*** > T** > T*，且 <int***> 会让三条偏特化【同时】匹配。
TEST(PartialOrder, ThreeWayChain) {
    std::string src =
        "template <typename T> struct Box { int tag() { return 0; } };\n"
        "template <typename T> struct Box<T*>   { int tag() { return 1; } };\n"
        "template <typename T> struct Box<T**>  { int tag() { return 2; } };\n"
        "template <typename T> struct Box<T***> { int tag() { return 3; } };\n"
        "int main() {\n"
        "    Box<int**>  c;\n"
        "    Box<int***> d;\n"
        "    return c.tag() + d.tag();\n"
        "}\n";

    withSema(src, [](SemanticAnalyzer& sema, const std::string& log) {
        auto& d = sema.getClassDecls();
        EXPECT_EQ(returnedConstantOf(d, "Box_intPP", "tag"), 2);
        EXPECT_EQ(returnedConstantOf(d, "Box_intPPP", "tag"), 3)
            << "三条偏特化同时匹配 <int***> ⇒ 应选出最特化的 T***";

        // 三路裁决的 trace 必须出现（可观测性要求）
        EXPECT_NE(log.find("[order]"), std::string::npos)
            << "缺少 [order] 偏序裁决追踪日志";
    });
}

// =============================================================================
// SFINAE 协议本身（include/sfinae.h）
// =============================================================================
// 前面三个套件测的是"用 SFINAE 能做什么"，本套件测的是"协议本身对不对"。
// 协议只有一条判据，但它是整个机制的命门：
//
//     Sfinae::attempt 捕获 SubstitutionFailure   → 软失败（候选移除）
//     Sfinae::attempt 放行其它异常               → 硬错误（穿过去）
//
// 这条判据一旦写反（例如 catch 了 std::runtime_error），
// 症状是"本该报错的程序静默通过" —— 比直接报错危险得多，
// 而且前面的集成测试全都发现不了。故单独立套件钉住。

// 10. 软失败被吸收：fn 抛 SubstitutionFailure ⇒ attempt 返回 false，不向外抛。
TEST(SfinaeProtocol, AbsorbsSubstitutionFailure) {
    std::string reason = "<未写入>";
    bool ok = true;
    {
        StdoutCapture cap;
        ok = Sfinae::attempt("测试候选", [] {
            throw SubstitutionFailure("探测条件不成立");
        }, &reason);
    }
    EXPECT_FALSE(ok) << "SubstitutionFailure 必须被吸收成软失败";
    EXPECT_EQ(reason, "探测条件不成立") << "失败原因必须回传给调用方";
}

// 11. 硬错误穿过去：fn 抛普通 runtime_error ⇒ attempt 不得捕获。
//     ★ 这是本套件存在的理由 —— 写反了前面 9 个测试全都照样绿。
TEST(SfinaeProtocol, PropagatesRealErrors) {
    bool threw = false;
    std::string what;
    try {
        StdoutCapture cap;
        Sfinae::attempt("测试候选", [] {
            throw std::runtime_error("这是真错误，不是替换失败");
        });
    } catch (const std::runtime_error& e) {
        threw = true;
        what = e.what();
    }
    EXPECT_TRUE(threw)
        << "普通 runtime_error 必须穿出 Sfinae::attempt —— 若被吞掉，"
           "SFINAE 就变成了'任何错误都静默忽略'";
    EXPECT_EQ(what, "这是真错误，不是替换失败");
}

// 12. 候选成立：fn 正常返回 ⇒ attempt 返回 true。
TEST(SfinaeProtocol, AcceptsValidCandidate) {
    std::string reason;
    bool ok = false;
    {
        StdoutCapture cap;
        ok = Sfinae::attempt("测试候选", [] { /* 什么也不抛 */ }, &reason);
    }
    EXPECT_TRUE(ok);
    EXPECT_TRUE(reason.empty()) << "成功时不应写入失败原因";
}

// 13. 直接上下文深度：只有 attempt 执行期间 depth > 0。
//     这条对应 [temp.deduct]/8 的"直接上下文"边界 —— 把它变成可观测状态，
//     是为了让"失败此刻是否享有 SFINAE 待遇"在运行期有据可查。
TEST(SfinaeProtocol, ImmediateContextDepth) {
    EXPECT_FALSE(SfinaeContext::inImmediateContext())
        << "进入 attempt 之前不该处于直接上下文内";
    EXPECT_EQ(SfinaeContext::depth(), 0);

    bool insideDepth = false;
    {
        StdoutCapture cap;
        Sfinae::attempt("测试候选", [&] {
            insideDepth = SfinaeContext::inImmediateContext();
        });
    }
    EXPECT_TRUE(insideDepth) << "attempt 执行期间应处于直接上下文内";
    EXPECT_FALSE(SfinaeContext::inImmediateContext())
        << "attempt 返回后必须恢复（RAII 析构）";
}

// 14. 嵌套：attempt 内再 attempt，深度累加、逐层复原。
TEST(SfinaeProtocol, NestedImmediateContexts) {
    int outerDepth = -1, innerDepth = -1;
    {
        StdoutCapture cap;
        Sfinae::attempt("外层", [&] {
            outerDepth = SfinaeContext::depth();
            Sfinae::attempt("内层", [&] { innerDepth = SfinaeContext::depth(); });
        });
    }
    EXPECT_EQ(outerDepth, 1);
    EXPECT_EQ(innerDepth, 2);
    EXPECT_EQ(SfinaeContext::depth(), 0) << "嵌套退出后必须归零";
}
