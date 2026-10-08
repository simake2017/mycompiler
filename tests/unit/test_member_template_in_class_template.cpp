// =============================================================================
// tests/unit/test_member_template_in_class_template.cpp —— 类模板里的成员模板
// =============================================================================
// 考察理论点：两层模板形参的【分别处理】（[temp.mem] × [temp.inst]）
//   `template<class T> struct Box { template<class U> T convert(U x); };` 有【两层】
//   模板形参，它们的绑定时机【不同】：
//     · 外层 T —— 由【类】的实例化绑定（Box<int> ⇒ T := int），此刻就定；
//     · 内层 U —— 由【调用点实参】推导绑定（b.convert('a') ⇒ U := char），此刻才能定。
//   于是"类实例化"这一步必须产出这么一份东西：把蓝图里的成员模板复制一份，
//   **只替换外层形参、保留内层形参**，挂到实例类名下。
//   · 替换早了（把 U 也替换掉）⇒ 调用点没东西可推导；
//   · 不替换（原样搬蓝图）⇒ 实例里还是 T，算出的类型是错的；
//   · 忘了搬 ⇒ 实例类手里一张空表 ⇒ "No member ... in class 'Box_int'"（B18）。
//   对照 clang：SemaTemplateInstantiateDecl.cpp 的 InstantiateDecl 对
//   TemplateDecl 走 TransformTemplateDecl —— 拿类的实参 TreeTransform 一遍，
//   内层形参保持未绑定（它们属于 DeclContext 之外的 TemplateParameterList）。
//
// 断言的是【不变量】，不是具体符号名：
//   ① 守恒 —— 实例类的成员模板表与蓝图的表【同名同数】（没丢、没多）
//   ② 分层 —— 克隆体的返回类型里外层 T 已兑现，形参里内层 U 原样保留
//   ③ 隔离 —— 同一蓝图的两个实例（Box<int> / Box<long>）各拿各的绑定，互不串味
//             （克隆若是"就地改蓝图"而不是复制，这条立刻红）
//   ④ 符号一致 —— 每个 `callq X` 都能在这份汇编里找到 `.globl X`
//   ⑤ 内层实参不同 ⇒ 实例符号不同（U 没被提前钉死，才有两个实例可分）
//
// 突变负向验证（改完跑一遍，确认本套件真的会红）：
//   把 template_instantiation.cpp 里那段 "5.6 克隆成员模板" 的循环整体注释掉
//   ⇒ ①②③④⑤ 五条全红（错误文案 No member 'pick' in class 'Box_int'）。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='MemberTemplateInClassTemplate.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "obs_helpers.h"
#include "parser.h"
#include "semantic_analyzer.h"

#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 一个成员模板的可观测摘要（从 Sema 的类声明表里拷出来 —— Sema 一析构就没了，
// 故在编译阶段就地快照成纯字符串，测试里再做断言）
struct MtInfo {
    std::string name;                            // 成员模板名（pick / convert）
    size_t      innerParamCount = 0;             // 内层形参个数（U 的个数）
    std::vector<std::string> innerParamNames;    // 内层形参名
    std::string returnType;                      // 克隆体的返回类型（已过外层替换）
    std::vector<std::string> paramTypes;         // 克隆体的形参类型
};

struct Compiled {
    std::string asmCode;
    std::map<std::string, std::vector<MtInfo>> memberTemplatesOf;  // 类名 → 成员模板表
    bool semaOk = false;                          // 语义分析是否跑到底（异常 ⇒ false）
};

Compiled compile(const std::string& src) {
    Compiled out;
    StdoutCapture cap;   // 吞掉全阶段中文日志（本套件只看结构，不看日志）
    try {
        Lexer lexer(src);
        Parser parser(lexer.tokenizeAll());
        TranslationUnit unit = parser.parseTranslationUnit();
        SemanticAnalyzer sema;
        sema.analyze(unit);
        out.semaOk = true;

        for (const auto& [className, cls] : sema.getClassDecls()) {
            std::vector<MtInfo> infos;
            for (const auto& mt : cls->memberTemplates) {
                MtInfo info;
                info.name = mt->templateName();
                info.innerParamCount = mt->templateParams.size();
                for (const auto& p : mt->templateParams) info.innerParamNames.push_back(p->name);
                if (mt->funcTemplate) {
                    info.returnType = mt->funcTemplate->returnType
                        ? mt->funcTemplate->returnType->toString() : "void";
                    for (const auto& p : mt->funcTemplate->parameters) {
                        info.paramTypes.push_back(p.type ? p.type->toString() : "?");
                    }
                }
                infos.push_back(std::move(info));
            }
            if (!infos.empty()) out.memberTemplatesOf[className] = std::move(infos);
        }

        CodeGen cg;
        out.asmCode = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
    } catch (const std::exception&) {
        out.semaOk = false;
    }
    return out;
}

// 找某个类的成员模板摘要；找不到返回 nullptr
const MtInfo* findMt(const Compiled& c, const std::string& cls, const std::string& name) {
    auto it = c.memberTemplatesOf.find(cls);
    if (it == c.memberTemplatesOf.end()) return nullptr;
    for (const auto& mt : it->second) {
        if (mt.name == name) return &mt;
    }
    return nullptr;
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

std::set<std::string> definedSymbols(const std::string& asmCode) {
    return collect(asmCode, R"(\.globl\s+([A-Za-z_][A-Za-z0-9_]*))");
}

std::set<std::string> calledSymbols(const std::string& asmCode) {
    return collect(asmCode, R"(callq\s+([A-Za-z_][A-Za-z0-9_]*))");
}

}  // namespace

// ── ① 守恒：实例类必须带着蓝图那张成员模板表 ────────────────────────────────
// B18 的原发场景：表没搬 ⇒ 调用点查不到 ⇒ "No member 'pick' in class 'Box_int'"。
TEST(MemberTemplateInClassTemplate, InstanceCarriesMemberTemplates) {
    const char* src = R"(
template <class T>
struct Box {
    T first;
    template <class U>
    U pick(U x) { return x; }
};
int main() { Box<int> b; return b.pick(7) - 7; }
)";
    Compiled c = compile(src);
    ASSERT_TRUE(c.semaOk) << "语义分析抛异常（多半就是 B18 症状）";

    const MtInfo* mt = findMt(c, "Box_int", "pick");
    ASSERT_NE(mt, nullptr) << "实例类 Box_int 里没有成员模板 pick —— 蓝图那张表没搬过来";
    EXPECT_EQ(mt->innerParamCount, 1u);
    EXPECT_EQ(mt->innerParamNames.at(0), "U") << "内层形参名被改掉了";
}

// ── ② 分层：外层 T 已兑现，内层 U 原样保留 ──────────────────────────────────
// 这条把"两层形参分别处理"钉死：返回类型里出现 int（外层替换生效），
// 形参类型里仍然是 U（内层没被提前替换 —— 否则调用点就没得推导了）。
TEST(MemberTemplateInClassTemplate, OuterBoundInnerKept) {
    const char* src = R"(
template <class T>
struct Box {
    template <class U>
    T convert(U x) { T v = x; return v; }
};
int main() { Box<int> b; return b.convert(7) - 7; }
)";
    Compiled c = compile(src);
    ASSERT_TRUE(c.semaOk);

    const MtInfo* mt = findMt(c, "Box_int", "convert");
    ASSERT_NE(mt, nullptr);
    EXPECT_EQ(mt->returnType, "int") << "外层 T 没被替换成 int（克隆漏了替换）";
    ASSERT_EQ(mt->paramTypes.size(), 1u);
    EXPECT_EQ(mt->paramTypes.at(0), "U") << "内层 U 被提前替换了 —— 调用点将无法推导";
}

// ── ③ 隔离：两个实例各拿各的绑定，蓝图不被就地改写 ──────────────────────────
// 克隆若是"就地改蓝图"（把 subst 直接写回蓝图节点），第二个实例会拿到上一个的绑定：
// 症状是 Box<long> 的 convert 也返回 int —— 算错但不报错，最坏的一类。
TEST(MemberTemplateInClassTemplate, InstancesDoNotShareBlueprintState) {
    const char* src = R"(
template <class T>
struct Box {
    template <class U>
    T convert(U x) { return x; }
};
int main() {
    Box<int>  bi;
    Box<long> bl;
    return bi.convert(1) + bl.convert(2) - 3;
}
)";
    Compiled c = compile(src);
    ASSERT_TRUE(c.semaOk);

    const MtInfo* a = findMt(c, "Box_int", "convert");
    const MtInfo* b = findMt(c, "Box_long", "convert");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->returnType, "int");
    EXPECT_EQ(b->returnType, "long") << "第二个实例串到了第一个的绑定（蓝图被就地改写）";
    EXPECT_NE(a->returnType, b->returnType);
}

// ── ④ 内层实参不同 ⇒ 实例符号不同（且都真的定义过）────────────────────────
// U 若被提前钉死成某一个类型，两个调用会算出同一个符号 ⇒ 静默复用第一个实例。
TEST(MemberTemplateInClassTemplate, DistinctInnerArgsDistinctSymbols) {
    const char* src = R"(
template <class T>
struct Box {
    template <class U>
    U pick(U x) { return x; }
};
int main() {
    Box<int> b;
    int v = 2;
    int  r = b.pick(1);      // U := int
    int* p = b.pick(&v);     // U := int*
    if (p != &v) return 1;
    return r - 1;
}
)";
    Compiled c = compile(src);
    ASSERT_TRUE(c.semaOk);

    auto defined = definedSymbols(c.asmCode);
    std::vector<std::string> pickSymbols;
    for (const auto& s : defined) {
        if (s.rfind("Box_int_pick", 0) == 0) pickSymbols.push_back(s);
    }
    EXPECT_GE(pickSymbols.size(), 2u)
        << "两个不同内层实参的实例只编出一个符号 —— 内层形参被提前钉死了？";
}

// ── ⑤ 符号一致：每个 callq 目标都要有 .globl 定义 ──────────────────────────
// 成员模板调用靠 Sema 回填 resolvedCalleeSymbol（CodeGen 硬拼拼不出实例符号），
// 这条守着回填链路：回填丢了 ⇒ 汇编里 callq 一个没有定义的符号 ⇒ 链接期炸。
TEST(MemberTemplateInClassTemplate, EveryCallResolves) {
    const char* src = R"(
template <class T>
struct Box {
    T seed;
    template <class U>
    T mix(U x) { return seed; }
};
int main() {
    Box<int> b;
    b.seed = 9;
    return b.mix('a') - 9;
}
)";
    Compiled c = compile(src);
    ASSERT_TRUE(c.semaOk);

    auto defined = definedSymbols(c.asmCode);
    std::vector<std::string> dangling;
    for (const auto& s : calledSymbols(c.asmCode)) {
        if (defined.count(s)) continue;
        if (s == "malloc" || s == "free") continue;   // 自研链接器注入的运行时桩
        dangling.push_back(s);
    }
    std::string msg = "以下被 callq 的符号没有 .globl 定义：";
    for (const auto& s : dangling) msg += "\n  - " + s;
    EXPECT_TRUE(dangling.empty()) << msg;
}
