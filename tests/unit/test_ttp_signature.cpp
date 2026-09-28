// =============================================================================
// tests/unit/test_ttp_signature.cpp —— 模板模板实参的【逐位签名匹配】
// =============================================================================
// 考察理论点（[temp.arg.template]/2-3，对应 docs/learn/33 §3.4）：
//   `template<template<class,class> class C>` 里的内层形参表是一份【签名】，
//   实参模板（`Wrap<Pair>` 的 Pair）必须逐位对上它。四条判据：
//     ① 位数相同 —— 少了 too few、多了 too many（**哪怕多出的位有默认实参**：
//        旧标准的"多余位有默认即可"被 P0522R0 取代，clang -std=c++20 实测拒绝）
//     ② 逐位同 kind：类型 ↔ 类型、值 ↔ 值、模板 ↔ 模板
//     ③ 值位还要声明类型相同：template<int> 与 template<unsigned> 不匹配
//     ④ 通过则正常绑定（本例还顺带盯住形参位的日志标签是 (template) 而非 (non-type)）
//
// 为什么值得单测而不是只留集成用例：语义判据就一处 ttpSignatureMismatch，
//   四条分支各是一条 return —— 集成用例一个文件只能停在一个分支上（fail-fast），
//   要四份文件；这里在一个进程里跑完五段源码，把每条分支都钉死。
//
// ★ 断言的是【诊断子串】而非整句：整句里有形参名、位数等随实现走的细节，
//   但"哪种不匹配"（too few / too many / kind / type）必须能区分出来 ——
//   四条分支互相覆盖（比如把 too many 写成 too few）会被这里立刻抓住。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='TtpSignature.*'
// =============================================================================

#include <gtest/gtest.h>

#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <memory>
#include <stdexcept>
#include <string>

using namespace minicc;

namespace {

// 源码 → Sema 日志；语义错误以 runtime_error 抛出（SemanticAnalyzer::error）。
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

// 跑一段源码，把抛出的语义错误正文取出来；没抛 = 返回空串。
// 注意：编译期日志被吞进 cap，异常里带的是 [Semantic Error] 原文。
std::string semaErrorOf(const std::string& src) {
    try {
        semaLogOf(src);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

// 四段源码只差"实参模板 Box2/Buf 的签名"这一处，其余（外壳 Wrap）完全一致 ——
// 这样任何一条分支被写错成另一条，都会在对应的 EXPECT_NE 上炸出来。
const char* kWrap2 =                       // 要 [class, class]
    "template <template <class, class> class C, class T>\n"
    "struct Wrap2 { C<T, T> inner; };\n";
const char* kWrap1 =                       // 要 [class]
    "template <template <class> class C>\n"
    "struct Wrap1 { C<int> inner; };\n";
const char* kWrapI =                       // 要 [int]
    "template <template <int> class C>\n"
    "struct WrapI { C<3> inner; };\n";
const char* kWrapU =                       // 要 [unsigned]
    "template <template <unsigned> class C>\n"
    "struct WrapU { C<3> inner; };\n";

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ① 位数少了（[temp.arg.template]/3）：P=[class,class] ← A=[class]
// ─────────────────────────────────────────────────────────────────────────────
TEST(TtpSignature, TooFewTemplateParameters) {
    const std::string src =
        std::string(kWrap2) +
        "template <class T> struct Box { T value; };\n"
        "int main() { Wrap2<Box, int> w; return 0; }\n";

    const std::string err = semaErrorOf(src);
    EXPECT_NE(err.find("has different template parameters"), std::string::npos) << err;
    EXPECT_NE(err.find("too few template parameters"), std::string::npos) << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// ② 位数多了（同上）—— 多出的那一位【带默认实参也不行】
// ─────────────────────────────────────────────────────────────────────────────
// 这是本批最值得守的一条：P0522R0 之前的规则允许"多出的位有默认值"，
// 本项目第一版就按旧规则写的，被 clang++-18 -std=c++20 探针当场否掉。
TEST(TtpSignature, TooManyTemplateParametersEvenWithDefault) {
    const std::string src =
        std::string(kWrap1) +
        "template <class T, class U = void> struct Box2 { T a; U b; };\n"
        "int main() { Wrap1<Box2> w; return 0; }\n";

    const std::string err = semaErrorOf(src);
    EXPECT_NE(err.find("has different template parameters"), std::string::npos) << err;
    EXPECT_NE(err.find("too many template parameters"), std::string::npos)
        << "默认实参不能把「多」补成「对」（P0522R0）： " << err;
    EXPECT_EQ(err.find("too few"), std::string::npos) << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 逐位 kind 不同：P=[int]（值位）← A=[class]（类型位）
// ─────────────────────────────────────────────────────────────────────────────
TEST(TtpSignature, KindMismatch) {
    const std::string src =
        std::string(kWrapI) +
        "template <class T> struct Box { T value; };\n"
        "int main() { WrapI<Box> w; return 0; }\n";

    const std::string err = semaErrorOf(src);
    EXPECT_NE(err.find("has a different kind"), std::string::npos) << err;
    EXPECT_NE(err.find("parameter 1"), std::string::npos) << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ 值位还要类型相同：P=[unsigned] ← A=[int]
// ─────────────────────────────────────────────────────────────────────────────
// 注意这条**不是** kind 不匹配（两位都是非类型位），只看 nonType 是否同型 ——
// 少了这一层，`template<unsigned>` 位置可以塞 `template<int>`，替换出负数下标不报错。
TEST(TtpSignature, NonTypeParameterTypeMismatch) {
    const std::string src =
        std::string(kWrapU) +
        "template <int N> struct Buf { int value; };\n"
        "int main() { WrapU<Buf> w; return 0; }\n";

    const std::string err = semaErrorOf(src);
    EXPECT_NE(err.find("has a different type"), std::string::npos) << err;
    EXPECT_EQ(err.find("different kind"), std::string::npos)
        << "两位都是值位 ⇒ 不该报 kind：" << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 形态层：模板【特化类型】`Box<int>` 不是模板名 —— 必须拒，不能"看名字眼熟就放行"
// ─────────────────────────────────────────────────────────────────────────────
// ★ 这是本文件里唯一一条**不是**关于"两张形参表对比"的判据，而是它的前置：
//   Parser 把 `Box` 与 `Box<int>` 都建成 Class 节点、名字段**都是 "Box"**，
//   只查"这个名字在注册表里吗"就会被 `Box<int>` 骗过去 ⇒ `<int>` 静默蒸发、
//   这一位按裸 `Box` 用；产物与 `Wrap<Box,int>` 逐字节相同，**连算错都看不出来**。
TEST(TtpSignature, TemplateIdArgumentIsNotATemplateName) {
    const std::string src =
        "template <template <class> class C, class T>\n"
        "struct Wrap { C<T> inner; };\n"
        "template <class T> struct Box { T value; };\n"
        "int main() { Wrap<Box<int>, int> w; return 0; }\n";

    const std::string err = semaErrorOf(src);
    EXPECT_NE(err.find("must be a class template"), std::string::npos) << err;
    EXPECT_NE(err.find("'Box<int>'"), std::string::npos)
        << "诊断要把带实参的那个 id 原样印出来（'Box<int>'），而不是只印名字段 'Box'：" << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑥ 正例不变量：签名对上 ⇒ 绑定成功，且日志把这一位标成 (template)
// ─────────────────────────────────────────────────────────────────────────────
TEST(TtpSignature, MatchingSignatureBinds) {
    const std::string src =
        std::string(kWrap2) +
        "template <class A, class B> struct Pair { A x; B y; };\n"
        "int main() { Wrap2<Pair, int> w; return 0; }\n";

    std::string log;
    ASSERT_NO_THROW(log = semaLogOf(src));

    EXPECT_NE(log.find("✓ 签名匹配: 'C' ← 'Pair'"), std::string::npos) << log;
    // 位置重解释：该位的 kind 由【形参表】裁定为 template，而不是解析器看到的"类型名"
    EXPECT_NE(log.find("param 1: 'C' (template) ← Pair"), std::string::npos)
        << "形参位是 template 位，日志标签不能写成 (non-type)/(type)：" << log;
}
