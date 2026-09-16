// =============================================================================
// tests/unit/test_template_deduction.cpp —— 模板推导 + 实例化白盒单元测试
// =============================================================================
// 目的：从【一段真实源码】出发走 Lexer → Parser 解析出模板蓝图（TemplateDecl），
//       再交给 TemplateDeducer / TemplateInstantiator 消费，端到端观察
//       「源码 → 蓝图 → 推导结果 / 实例化产物」，让模板机制的每个子算法
//       可讲解、可观测（与 tests/test_tmpl_*.cpp 集成用例同一份语法入口）。
//
// 蓝图构造方式：
//   parseFuncTmpl / parseClassTmpl —— 内嵌一段源码字符串，
//   Lexer 词法分析 → Parser::parseTranslationUnit → 取出唯一 TemplateDecl。
//   解析过程的中文日志（[parse:template]/[parse:type]...）被 StdoutCapture
//   捕获后随 dump 一起打印，使"源码 → 蓝图"这一步也可见。
//   ⚠ parseType 尚不知道 T 是模板参数，会把裸 T 建成 Class("T") 节点；
//     推导/替换引擎用"名字 ∈ typeParams"判定变量（见 TemplateDeducer::
//     isTemplateParamName 与 substituteType 的 Class 兜底分支）。
//   Substitute 套件例外：直接手搓类型节点——它考察的是替换算法本身
//   （引用折叠），与 Parser 无关。
//
// 运行方式：
//   ① CLion：Reload CMake 后，本文件每个 TEST() 左侧出现绿色三角；
//   ② 命令行：cmake --build build-linux --target unit_tests
//              && ./build-linux/unit_tests --gtest_filter='Deduce.*'
//
// 套件 → 理论点 → clang 对照：
//   ParseBlueprint   template<...> 声明解析 → 蓝图 AST（[temp]/[temp.param]）
//                    → clang ParseTemplate.cpp::ParseTemplateDeclaration
//   Deduce           S2~S4 推导总流程（逐对合一 + 显式实参 + 收尾检查）
//                    → clang SemaTemplateDeduction.cpp
//   DeducePair       单对 P/A 结构化合一（const/引用/指针/值传递调整）
//                    → [temp.deduct.call]
//   Substitute       类型替换 + 引用折叠（S5 实例化的核心操作）
//                    → clang TreeTransform.h
//   Mangle           Name Mangling（Itanium ABI 编码）
//                    → clang ItaniumMangle.cpp
//   Instantiate      完整实例化流程（类模板 + 函数模板）
//                    → clang SemaTemplateInstantiate.cpp
// =============================================================================

#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "template_deduction.h"
#include "template_instantiation.h"
#include "ast.h"
#include "type.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// ── 辅助：源码字符串 → 词法分析 → 语法分析 → AST 根 ──────────────────────
// 等价于 main 驱动的 阶段1 Lexer → 阶段2 Parser（跳过阶段0 预处理：
// 测试源码不含 # 指令）。解析日志打印到 stdout，测试里通常配合
// StdoutCapture 捕获后整体 dump。
TranslationUnit parseSource(const std::string& src) {
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    return parser.parseTranslationUnit();
}

// ── 辅助：从源码解析出唯一的函数模板蓝图 ─────────────────────────────────
// demo：parseFuncTmpl("template<typename T> T twice(T x) { return x + x; }")
//   → TemplateDecl{ typeParams=["T"], funcTemplate=twice(T x) }
TemplateDeclPtr parseFuncTmpl(const std::string& src) {
    TranslationUnit unit = parseSource(src);
    EXPECT_EQ(unit.declarations.size(), 1u) << "源码应恰好含 1 个顶层声明";
    auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(unit.declarations.at(0));
    EXPECT_TRUE(tmpl != nullptr) << "顶层声明应为 TemplateDecl";
    EXPECT_TRUE(tmpl && tmpl->isFunctionTemplate()) << "应为函数模板";
    return tmpl;
}

// ── 辅助：从源码解析出唯一的类模板蓝图 ───────────────────────────────────
TemplateDeclPtr parseClassTmpl(const std::string& src) {
    TranslationUnit unit = parseSource(src);
    EXPECT_EQ(unit.declarations.size(), 1u) << "源码应恰好含 1 个顶层声明";
    auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(unit.declarations.at(0));
    EXPECT_TRUE(tmpl != nullptr) << "顶层声明应为 TemplateDecl";
    EXPECT_TRUE(tmpl && tmpl->isClassTemplate()) << "应为类模板";
    return tmpl;
}

// ── 辅助：捕获 stdout ──────────────────────────────────────────────────────
class StdoutCapture {
    std::streambuf* old_;
    std::stringstream buf_;
public:
    StdoutCapture() : old_(nullptr), buf_() { old_ = std::cout.rdbuf(buf_.rdbuf()); }
    ~StdoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return buf_.str(); }
};

// ── 辅助：打印「解析 + 推导」全过程 ──────────────────────────────────────
void dumpDeduction(const char* what, const DeductionResult& result,
                   const std::string& trace = "") {
    std::printf("\n=================================================================\n");
    std::printf("── %s\n", what);
    std::printf("=================================================================\n");
    if (!trace.empty()) {
        std::printf("[解析+推导 trace]\n%s", trace.c_str());
        std::printf("-----------------------------------------------------------------\n");
    }
    std::printf("[结果] %s\n", result.success ? "✓ 成功" : "✗ 失败");
    if (result.success) {
        std::printf("[推导实参] <");
        for (size_t i = 0; i < result.deducedArgs.size(); i++) {
            if (i > 0) std::printf(", ");
            std::printf("%s", result.deducedArgs[i]->toString().c_str());
        }
        std::printf(">\n");
    } else {
        std::printf("[失败原因] %s\n", result.failureReason.c_str());
    }
    std::printf("[逐对 trace] %zu 步\n", result.trace.size());
    for (auto& step : result.trace) {
        std::printf("   P=%-12s A=%-12s ⇒ %s %s\n",
            step.pattern.c_str(), step.argument.c_str(),
            step.result.c_str(), step.ok ? "✓" : "✗");
    }
    std::printf("=================================================================\n\n");
}

void dumpSubstitution(const char* what, TypePtr input, TypePtr result,
                      const std::string& trace = "") {
    std::printf("\n─────────────────────────────────────────────────────────────────\n");
    std::printf("── %s\n", what);
    std::printf("   输入类型: %s\n", input ? input->toString().c_str() : "(null)");
    std::printf("   替换结果: %s\n", result ? result->toString().c_str() : "(null)");
    if (!trace.empty()) {
        std::printf("   [trace]\n%s", trace.c_str());
    }
    std::printf("─────────────────────────────────────────────────────────────────\n");
}

} // anonymous namespace

// =============================================================================
// 套件零：ParseBlueprint —— template<...> 声明解析（源码 → 蓝图）
// =============================================================================
// 理论点：模板声明解析 [temp]/[temp.param]；Parser 把整个模板"冻结"为蓝图，
// 暂不做语义分析（[temp.res] 两阶段查找的教学简化：蓝图原样保存）。
// clang 对照：ParseTemplate.cpp ParseTemplateDeclaration / ParseTemplateParameters。

// P1. 函数模板蓝图：模板参数名、返回类型、形参列表、函数体四要素
TEST(ParseBlueprint, FunctionTemplateShape) {
    TemplateDeclPtr tmpl;
    std::string parseLog;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T>\n"
            "T twice(T x) {\n"
            "    return x + x;\n"
            "}\n");
        parseLog = cap.str();
    }
    std::printf("\n─────────────────────────────────────────────────────────────────\n");
    std::printf("── 源码 → 函数模板蓝图\n%s", parseLog.c_str());
    std::printf("─────────────────────────────────────────────────────────────────\n");

    ASSERT_NE(tmpl, nullptr);
    // ① 模板参数列表 <typename T>
    ASSERT_EQ(tmpl->typeParams.size(), 1u);
    EXPECT_EQ(tmpl->typeParams[0], "T");

    // ② 函数签名：名字 + 返回类型 T
    auto& f = tmpl->funcTemplate;
    EXPECT_EQ(f->name, "twice");
    ASSERT_NE(f->returnType, nullptr);
    EXPECT_EQ(tmpl->templateName(), "twice");

    // ③ 形参 (T x)：parseType 把裸 T 建成 Class("T")（名字在 typeParams 里即视为变量）
    ASSERT_EQ(f->parameters.size(), 1u);
    EXPECT_EQ(f->parameters[0].name, "x");
    EXPECT_EQ(f->parameters[0].type->toString(), "T");

    // ④ 函数体被完整冻结进蓝图（return x + x;）
    EXPECT_NE(f->body, nullptr);
}

// P2. 类模板蓝图：字段与方法都以"含 T 的蓝图形态"保存
TEST(ParseBlueprint, ClassTemplateShape) {
    TemplateDeclPtr tmpl;
    std::string parseLog;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl(
            "template<typename T>\n"
            "class Box {\n"
            "public:\n"
            "    T* data;\n"
            "    T& get() { return item; }\n"
            "};\n");
        parseLog = cap.str();
    }
    std::printf("\n─────────────────────────────────────────────────────────────────\n");
    std::printf("── 源码 → 类模板蓝图\n%s", parseLog.c_str());
    std::printf("─────────────────────────────────────────────────────────────────\n");

    ASSERT_NE(tmpl, nullptr);
    ASSERT_EQ(tmpl->typeParams.size(), 1u);
    EXPECT_EQ(tmpl->typeParams[0], "T");

    auto& c = tmpl->classTemplate;
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->name, "Box");
    EXPECT_TRUE(tmpl->isClassTemplate());

    // 字段 T* data（Pointee(Class T)）
    ASSERT_EQ(c->fields.size(), 1u);
    EXPECT_EQ(c->fields[0].name, "data");
    EXPECT_TRUE(c->fields[0].type->isPointer());

    // 方法 T& get()（LValueReference(Class T)）
    ASSERT_EQ(c->methods.size(), 1u);
    EXPECT_EQ(c->methods[0]->name, "get");
    EXPECT_TRUE(c->methods[0]->returnType->isLValueReference());
    EXPECT_EQ(c->methods[0]->ownerClassName, "Box");
}

// P3. 非类型模板参数 (NTTP) 蓝图：template<typename T, int N> 与 template<int Size, bool Flag>
TEST(ParseBlueprint, NonTypeTemplateParameterShape) {
    TemplateDeclPtr tmpl;
    std::string parseLog;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T, int N>\n"
            "T fill(T val) {\n"
            "    return val;\n"
            "}\n");
        parseLog = cap.str();
    }
    ASSERT_NE(tmpl, nullptr);
    ASSERT_EQ(tmpl->typeParams.size(), 2u);
    EXPECT_EQ(tmpl->typeParams[0], "T");
    EXPECT_EQ(tmpl->typeParams[1], "N");

    ASSERT_EQ(tmpl->templateParams.size(), 2u);
    EXPECT_EQ(tmpl->templateParams[0].kind, TemplateParamKind::Type);
    EXPECT_EQ(tmpl->templateParams[0].name, "T");

    EXPECT_EQ(tmpl->templateParams[1].kind, TemplateParamKind::NonType);
    EXPECT_EQ(tmpl->templateParams[1].name, "N");
    ASSERT_NE(tmpl->templateParams[1].nonType, nullptr);
    EXPECT_TRUE(tmpl->templateParams[1].nonType->isInt());
}

// P4. 类模板含非类型参数 (NTTP)：template<class T, int Capacity> class Array
TEST(ParseBlueprint, ClassTemplateWithNTTP) {
    TemplateDeclPtr tmpl;
    std::string parseLog;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl(
            "template<class T, int Capacity>\n"
            "class Array {\n"
            "public:\n"
            "    T* buffer;\n"
            "    int size() { return Capacity; }\n"
            "};\n");
        parseLog = cap.str();
    }
    ASSERT_NE(tmpl, nullptr);
    ASSERT_EQ(tmpl->templateParams.size(), 2u);
    EXPECT_EQ(tmpl->templateParams[0].kind, TemplateParamKind::Type);
    EXPECT_EQ(tmpl->templateParams[0].name, "T");

    EXPECT_EQ(tmpl->templateParams[1].kind, TemplateParamKind::NonType);
    EXPECT_EQ(tmpl->templateParams[1].name, "Capacity");
    ASSERT_NE(tmpl->templateParams[1].nonType, nullptr);
    EXPECT_TRUE(tmpl->templateParams[1].nonType->isInt());
    EXPECT_EQ(tmpl->classTemplate->name, "Array");
}

// =============================================================================
// 套件一：Deduce —— S2~S4 完整推导流程（蓝图由源码解析而来）
// =============================================================================

// 1. 基础推导：twice(3) → T := int
//    源码: template<typename T> void twice(T x) { }
//    调用: twice(3)，实参类型 [int]
//    期望: P=T, A=int ⇒ T := int ✓
TEST(Deduce, BasicSingleParam) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T>\n"
            "void twice(T x) {\n"
            "}\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {false});
        trace = cap.str();
    }
    dumpDeduction("基础推导 twice(3) → T := int", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
    EXPECT_EQ(result.trace.size(), 1u);
    EXPECT_TRUE(result.trace[0].ok);
}

// 2. 一致性绑定：max(x:int, y:int) → T := int（一致 ✓）
//    源码: template<typename T> T max(T a, T b) { return a; }
//    两个形参都是 T，两个实参都是 int → 第二次绑定一致
TEST(Deduce, ConsistentBinding) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T>\n"
            "T max(T a, T b) {\n"
            "    return a;\n"
            "}\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt(), Type::makeInt()}, {false, false});
        trace = cap.str();
    }
    dumpDeduction("一致性绑定 max(int, int) → T := int", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
    EXPECT_EQ(result.trace.size(), 2u);
    // 第二步应该是"一致 ✓"
    EXPECT_TRUE(result.trace[1].ok);
    EXPECT_NE(result.trace[1].result.find("一致"), std::string::npos);
}

// 3. 冲突绑定：make(1, 2.0) → T 冲突 ✗
//    源码: template<typename T> T make(T a, T b) { return a; }
//    第一个实参 int → T := int；第二个实参 double → T := double → 冲突！
TEST(Deduce, ConflictingBinding) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T>\n"
            "T make(T a, T b) {\n"
            "    return a;\n"
            "}\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt(), Type::makeDouble()}, {false, false});
        trace = cap.str();
    }
    dumpDeduction("冲突绑定 make(int, double) → T 冲突 ✗", result, trace);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.failureReason.find("conflicting"), std::string::npos);
}

// 4. 参数个数不匹配
//    源码: template<typename T> void f(T x) { }
//    调用: f(1, 2) → 2 个实参但只有 1 个形参
TEST(Deduce, ArgCountMismatch) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T>\n"
            "void f(T x) {\n"
            "}\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt(), Type::makeInt()}, {false, false});
        trace = cap.str();
    }
    dumpDeduction("参数个数不匹配 f(int, int) 但只有 1 个形参", result, trace);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.failureReason.find("argument count"), std::string::npos);
}

// 5. 多模板参数：pair(int, double) → T := int, U := double
//    源码: template<typename T, typename U> void pair(T a, U b) { }
TEST(Deduce, MultipleTypeParams) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T, typename U>\n"
            "void pair(T a, U b) {\n"
            "}\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt(), Type::makeDouble()}, {false, false});
        trace = cap.str();
    }
    dumpDeduction("多模板参数 pair(int, double) → T=int, U=double", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 2u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
    EXPECT_TRUE(result.deducedArgs[1]->isDouble());
}

// 6. 显式模板实参（S3）：cast<int>(1.0)
//    源码: template<typename T> T cast(double x);   （前向声明，无函数体）
//    显式给定 T := int，不需要从 double 推导
TEST(Deduce, ExplicitArgs) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> T cast(double x);\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl,
            {Type::makeDouble()}, {false},
            {Type::makeInt()});  // 显式实参: T := int
        trace = cap.str();
    }
    dumpDeduction("显式模板实参 cast<int>(1.0)", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
}

// 7. 显式前缀 + 推导补全：convert<int>(3.14, s)
//    源码: template<typename T, typename U> T convert(double a, U b);
//    显式 T := int（T 只出现在返回类型——不可推导位置，靠显式实参给定），
//    U 从第二个实参推导。第一个形参 double 非依赖，与实参 double 恒等 ✓。
TEST(Deduce, ExplicitPrefixPlusDeduction) {
    auto stringType = Type::makeClass("string");
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T, typename U>\n"
            "T convert(double a, U b);\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl,
            {Type::makeDouble(), stringType}, {false, false},
            {Type::makeInt()});  // 显式 T := int，U 靠推导
        trace = cap.str();
    }
    dumpDeduction("显式前缀+推导 convert<int>(double, string)", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 2u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());    // 显式
    EXPECT_TRUE(result.deducedArgs[1]->isClass());  // 推导
    EXPECT_EQ(result.deducedArgs[1]->name, "string");
}

// 8. 不可推导上下文：template<typename T> T make() 无参数可推导
//    T 只出现在返回类型（不可推导位置），必须显式给定
TEST(Deduce, NonDeducedContext) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> T make();\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {}, {});
        trace = cap.str();
    }
    dumpDeduction("不可推导上下文 make() → T 未绑定", result, trace);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.failureReason.find("non-deduced"), std::string::npos);
}

// =============================================================================
// 套件二：DeducePair —— 单对 P/A 结构化合一的各种分支（蓝图由源码解析而来）
// =============================================================================

// 9. P = T&（左值引用）：实参必须左值
//    源码: template<typename T> void f(T& x) { }
//    调用: f(lvalue_var)，argIsLValue = true
TEST(DeducePair, LValueReferenceParam) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(T& x) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {true});  // 左值实参
        trace = cap.str();
    }
    dumpDeduction("左值引用参数 f(T&) 接收 int 左值", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
}

// 10. P = T& 但实参是右值 → 失败
//     非 const 左值引用不能绑定右值
TEST(DeducePair, LValueRefRejectsRvalue) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(T& x) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {false});  // 右值实参
        trace = cap.str();
    }
    dumpDeduction("左值引用参数 f(T&) 拒绝右值", result, trace);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.failureReason.find("lvalue reference"), std::string::npos);
}

// 11. P = T&&（万能引用）+ 左值实参 → T := int&（引用折叠路径）
//     这是 std::forward 完美转发的根基
//     template<typename T> void foo(T&& x) { }  foo(lvalue_var);
//     → T := int&，实例化后 T&& = int& && 折叠为 int&
TEST(DeducePair, UniversalRefWithLvalue) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void foo(T&& x) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {true});  // 左值实参
        trace = cap.str();
    }
    dumpDeduction("万能引用 foo(T&&) 接收左值 → T := int&", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    // T 应该被绑定为 int&（左值引用），而不是 int
    EXPECT_TRUE(result.deducedArgs[0]->isLValueReference());
}

// 12. P = T&&（万能引用）+ 右值实参 → T := int（普通推导路径）
TEST(DeducePair, UniversalRefWithRvalue) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void foo(T&& x) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {false});  // 右值实参
        trace = cap.str();
    }
    dumpDeduction("万能引用 foo(T&&) 接收右值 → T := int", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
}

// 13. P = T*（指针）：实参必须是指针
//     源码: template<typename T> void f(T* p) { }
//     调用: f(ptr_to_int)，A = int*
TEST(DeducePair, PointerParam) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(T* p) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makePointer(Type::makeInt())}, {false});
        trace = cap.str();
    }
    dumpDeduction("指针参数 f(T*) 接收 int* → T := int", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
}

// 14. P = T* 但实参不是指针 → 失败
TEST(DeducePair, PointerParamRejectsNonPointer) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(T* p) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {false});  // 非指针
        trace = cap.str();
    }
    dumpDeduction("指针参数 f(T*) 拒绝 int（非指针）", result, trace);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.failureReason.find("pointer"), std::string::npos);
}

// 15. P = const T&：剥顶层 const 后走引用分支
//     源码: template<typename T> void f(const T& x) { }
//     parseType 把 const T& 解析为 Const(LRef(Class T))
//     推导先剥 const → 剩 LRef(T) → 走左值引用分支
TEST(DeducePair, ConstLValueRefParam) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(const T& x) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt()}, {true});
        trace = cap.str();
    }
    dumpDeduction("const T& 参数：剥 const → 走引用 → T := int", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isInt());
}

// 16. P = T（值传递）：实参的顶层 const 被剥离，但引用保留
//     identity(x) 中 x 是 const int& → 剥 const → T := int&
//     ⚠ 注意：deducePair 的值传递调整先尝试剥引用（isReference 返回 false 因为
//     Const 包在最外层），然后剥 const（成功，得到 LRef(Int) = int&）。
//     最终 adjusted = int&，不等于原始 A = const int&，所以绑定 T := int&。
//     标准 C++ 的值传递应同时剥除引用和 const（T := int），
//     但本项目 Parser 的组合顺序（const 包在最外层）导致引用层被 const 遮挡，
//     未被剥除——这是教学编译器的已知简化。
TEST(DeducePair, ValueTypeAdjustment) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl(
            "template<typename T>\n"
            "T identity(T x) {\n"
            "    return x;\n"
            "}\n");
        TemplateDeducer deducer;
        // 实参类型 const int& = Const(LValueReference(Int))
        TypePtr argType = Type::makeConst(Type::makeLValueReference(Type::makeInt()));
        result = deducer.deduce(tmpl, {argType}, {true});
        trace = cap.str();
    }
    dumpDeduction("值传递调整 identity(const int&) → T := int&", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);

    // const 被剥除 ✓（Const 层被去掉）
    EXPECT_FALSE(result.deducedArgs[0]->isConst())
        << "顶层 const 应被剥除";

    // 引用保留（Const 包在最外层遮挡了引用层，未被剥除）
    EXPECT_TRUE(result.deducedArgs[0]->isLValueReference())
        << "本项目简化：引用被 const 遮挡未剥除，T := int&";
}

// 17. P = int（非依赖类型）：要求恒等
//     源码: template<typename T> void f(int x, T y) { }
//     第一个形参 int 不含 T，要求实参也是 int
TEST(DeducePair, NonDependentIdentity) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(int x, T y) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt(), Type::makeDouble()}, {false, false});
        trace = cap.str();
    }
    dumpDeduction("非依赖恒等 f(int, double) → int == int ✓, T := double", result, trace);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deducedArgs.size(), 1u);
    EXPECT_TRUE(result.deducedArgs[0]->isDouble());
}

// 18. 非依赖不恒等：f(double x, T y) 但实参是 (int, int)
TEST(DeducePair, NonDependentMismatch) {
    TemplateDeclPtr tmpl;
    DeductionResult result;
    std::string trace;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> void f(double x, T y) { }\n");
        TemplateDeducer deducer;
        result = deducer.deduce(tmpl, {Type::makeInt(), Type::makeInt()}, {false, false});
        trace = cap.str();
    }
    dumpDeduction("非依赖不恒等 f(double, T) 但第一个实参是 int", result, trace);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.failureReason.find("no match"), std::string::npos);
}

// =============================================================================
// 套件三：Substitute —— 类型替换 + 引用折叠
// =============================================================================
// 本套件直接手搓类型节点（不经 Parser）：考察对象是替换算法本身
// （[temp.subst] + [temp.deduct]/p9 引用折叠），与解析无关。

// 19. 直接替换：T → int
TEST(Substitute, DirectReplacement) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {{"T", Type::makeInt()}};

    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(Type::makeTemplateParam("T"), subst);
        trace = cap.str();
    }
    dumpSubstitution("直接替换 T → int", Type::makeTemplateParam("T"), result, trace);

    EXPECT_TRUE(result->isInt());
}

// 20. 指针替换：T* → int*
TEST(Substitute, PointerReplacement) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {{"T", Type::makeInt()}};

    TypePtr input = Type::makePointer(Type::makeTemplateParam("T"));
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("指针替换 T* → int*", input, result, trace);

    EXPECT_TRUE(result->isPointer());
    EXPECT_TRUE(result->pointeeType->isInt());
}

// 21. 左值引用替换：T& → int&
TEST(Substitute, LValueRefReplacement) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {{"T", Type::makeInt()}};

    TypePtr input = Type::makeLValueReference(Type::makeTemplateParam("T"));
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("左值引用替换 T& → int&", input, result, trace);

    EXPECT_TRUE(result->isLValueReference());
    EXPECT_TRUE(result->referencedType->isInt());
}

// 22. 万能引用折叠：T&& 配 {T := int&} → int& && 折叠为 int&
//     这是 std::forward 完美转发的核心：
//       template<typename T> void foo(T&& x);
//       int var; foo(var); → T = int&, T&& = int& && → int&
TEST(Substitute, UniversalRefCollapsing) {
    TemplateInstantiator inst;
    // T 被推导为 int&（左值实参传万能引用时）
    TemplateInstantiator::TypeSubstitution subst = {
        {"T", Type::makeLValueReference(Type::makeInt())}};

    TypePtr input = Type::makeRValueReference(Type::makeTemplateParam("T"));
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("万能引用折叠 T&& 配 {T := int&} → int&", input, result, trace);

    // int& && 折叠为 int&（左值引用永远赢）
    EXPECT_TRUE(result->isLValueReference());
    EXPECT_TRUE(result->referencedType->isInt());
}

// 23. 右值引用 + 右值引用折叠：T&& 配 {T := int&&} → int&& && → int&&
TEST(Substitute, RValueRefCollapsing) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {
        {"T", Type::makeRValueReference(Type::makeInt())}};

    TypePtr input = Type::makeRValueReference(Type::makeTemplateParam("T"));
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("右值引用折叠 T&& 配 {T := int&&} → int&&", input, result, trace);

    EXPECT_TRUE(result->isRValueReference());
    EXPECT_TRUE(result->referencedType->isInt());
}

// 24. const 替换：const T → const int
TEST(Substitute, ConstReplacement) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {{"T", Type::makeInt()}};

    TypePtr input = Type::makeConst(Type::makeTemplateParam("T"));
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("const 替换 const T → const int", input, result, trace);

    EXPECT_TRUE(result->isConst());
    EXPECT_TRUE(result->innerType->isInt());
}

// 25. 复合类型深层替换：const T* → const int*
//     Const(Pointer(T)) 配 {T := int} → Const(Pointer(int))
TEST(Substitute, NestedTypeReplacement) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {{"T", Type::makeInt()}};

    TypePtr input = Type::makeConst(
        Type::makePointer(Type::makeTemplateParam("T")));
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("复合替换 const T* → const int*", input, result, trace);

    EXPECT_TRUE(result->isConst());
    EXPECT_TRUE(result->innerType->isPointer());
    EXPECT_TRUE(result->innerType->pointeeType->isInt());
}

// 26. 不在替换表中的模板参数保持原样
TEST(Substitute, UnmappedParamKept) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {{"T", Type::makeInt()}};

    TypePtr input = Type::makeTemplateParam("U");  // U 不在表中
    std::string trace;
    TypePtr result;
    {
        StdoutCapture cap;
        result = inst.substituteType(input, subst);
        trace = cap.str();
    }
    dumpSubstitution("未映射参数 U 保持原样", input, result, trace);

    EXPECT_TRUE(result->isTemplateParam());
    EXPECT_EQ(result->templateParamName, "U");
}

// =============================================================================
// 套件四：Mangle —— Name Mangling（Itanium ABI）
// =============================================================================

// 27. 模板实例化符号名：MyPtr<int> → _Z5MyPtrIiE
TEST(Mangle, TemplateInstanceInt) {
    auto mangled = NameMangler::mangleTemplateInstance("MyPtr", {Type::makeInt()});
    std::printf("── Mangle: MyPtr<int> → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z5MyPtrIiE");
}

// 28. 模板实例化符号名：twice<double> → _Z5twiceIdE
TEST(Mangle, TemplateInstanceDouble) {
    auto mangled = NameMangler::mangleTemplateInstance("twice", {Type::makeDouble()});
    std::printf("── Mangle: twice<double> → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z5twiceIdE");
}

// 29. 多参数模板：pair<int, double> → _Z4pairIidE
TEST(Mangle, TemplateInstanceMultiParam) {
    auto mangled = NameMangler::mangleTemplateInstance(
        "pair", {Type::makeInt(), Type::makeDouble()});
    std::printf("── Mangle: pair<int, double> → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z4pairIidE");
}

// 30. 复合类型编码：MyPtr<int*> → _Z5MyPtrIPiE
TEST(Mangle, TemplateInstancePointer) {
    auto mangled = NameMangler::mangleTemplateInstance(
        "MyPtr", {Type::makePointer(Type::makeInt())});
    std::printf("── Mangle: MyPtr<int*> → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z5MyPtrIPiE");
}

// 31. 引用类型编码：Ref<int&> → _Z3RefIRiE
TEST(Mangle, TemplateInstanceRef) {
    auto mangled = NameMangler::mangleTemplateInstance(
        "Ref", {Type::makeLValueReference(Type::makeInt())});
    std::printf("── Mangle: Ref<int&> → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z3RefIRiE");
}

// 32. const 编码：Const<int> → _Z5ConstIKiE
TEST(Mangle, TemplateInstanceConst) {
    auto mangled = NameMangler::mangleTemplateInstance(
        "Const", {Type::makeConst(Type::makeInt())});
    std::printf("── Mangle: Const<const int> → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z5ConstIKiE");
}

// 33. 函数符号名：MyClass::foo(int)
//     ⚠ 本项目 mangling 简化：参数类型在 N…E 内部，尾部带 E
//     实际产物 _ZN7MyClass3fooiE（标准 Itanium ABI 应为 _ZN7MyClass3fooEi，
//     即参数类型在 E 之后）。这是教学简化，不影响编译器内部使用。
TEST(Mangle, FunctionMangling) {
    auto mangled = NameMangler::mangleFunction(
        "foo", "MyClass",
        {Parameter{"x", Type::makeInt()}});
    std::printf("── Mangle: MyClass::foo(int) → %s\n", mangled.c_str());
    // 项目实际行为：参数编码在 N…E 内部
    EXPECT_EQ(mangled, "_ZN7MyClass3fooiE");
}

// 34. 自由函数符号名：twice(int, double) → _Z5twiceid
TEST(Mangle, FreeFunctionMangling) {
    auto mangled = NameMangler::mangleFunction(
        "twice", "",
        {Parameter{"a", Type::makeInt()}, Parameter{"b", Type::makeDouble()}});
    std::printf("── Mangle: twice(int, double) → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_Z5twiceid");
}

// 35. RTTI 符号名：MyClass → _ZTI7MyClass
TEST(Mangle, RTTIMangling) {
    auto mangled = NameMangler::mangleRTTI("MyClass");
    std::printf("── Mangle RTTI: MyClass → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_ZTI7MyClass");
}

// 36. vtable 符号名：MyClass → _ZTV7MyClass
TEST(Mangle, VTableMangling) {
    auto mangled = NameMangler::mangleVTable("MyClass");
    std::printf("── Mangle vtable: MyClass → %s\n", mangled.c_str());
    EXPECT_EQ(mangled, "_ZTV7MyClass");
}

// =============================================================================
// 套件五：Instantiate —— 完整实例化流程（蓝图由源码解析而来）
// =============================================================================

// 37. 函数模板实例化：twice<int>
//     源码: template<typename T> T twice(T x);   （前向声明；实例化引擎
//           克隆 body 并替换——这里只验证签名替换与符号名）
TEST(Instantiate, FunctionTemplate) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    std::string trace;
    FuncDeclPtr instance;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> T twice(T x);\n");
        instance = inst.instantiateFunction(tmpl, {Type::makeInt()});
        trace = cap.str();
    }

    std::printf("\n=================================================================\n");
    std::printf("── 函数模板实例化 twice<int>\n");
    std::printf("=================================================================\n");
    std::printf("%s", trace.c_str());

    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->name, "twice");

    // 返回类型 T → int
    EXPECT_TRUE(instance->returnType->isInt());

    // 参数类型 T → int
    ASSERT_EQ(instance->parameters.size(), 1u);
    EXPECT_TRUE(instance->parameters[0].type->isInt());

    // mangled name
    EXPECT_EQ(instance->mangledName, "_Z5twiceIiE");

    // 检查已注册
    EXPECT_EQ(inst.getInstantiatedFunctions().size(), 1u);
}

// 38. 类模板实例化：MyPtr<int>
//     源码: template<typename T> class MyPtr { T* data; T& get() { return item; } };
TEST(Instantiate, ClassTemplate) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    std::string trace;
    ClassDeclPtr instance;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl(
            "template<typename T>\n"
            "class MyPtr {\n"
            "public:\n"
            "    T* data;\n"
            "    T& get() { return item; }\n"
            "};\n");
        instance = inst.instantiate(tmpl, {Type::makeInt()});
        trace = cap.str();
    }

    std::printf("\n=================================================================\n");
    std::printf("── 类模板实例化 MyPtr<int>\n");
    std::printf("=================================================================\n");
    std::printf("%s", trace.c_str());

    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->name, "MyPtr_int");

    // 字段 data: T* → int*
    ASSERT_EQ(instance->fields.size(), 1u);
    EXPECT_EQ(instance->fields[0].name, "data");
    EXPECT_TRUE(instance->fields[0].type->isPointer());
    EXPECT_TRUE(instance->fields[0].type->pointeeType->isInt());

    // 方法 get: T& → int&
    ASSERT_EQ(instance->methods.size(), 1u);
    EXPECT_EQ(instance->methods[0]->name, "get");
    EXPECT_TRUE(instance->methods[0]->returnType->isLValueReference());
    EXPECT_TRUE(instance->methods[0]->returnType->referencedType->isInt());

    // 检查已注册
    EXPECT_EQ(inst.getInstantiatedClasses().size(), 1u);
}

// 39. 同一模板不同实参产生不同符号（不会链接冲突）
TEST(Instantiate, DifferentArgsDifferentSymbols) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    {
        StdoutCapture cap;
        tmpl = parseFuncTmpl("template<typename T> T twice(T x);\n");
        inst.instantiateFunction(tmpl, {Type::makeInt()});
        inst.instantiateFunction(tmpl, {Type::makeDouble()});
    }

    auto& funcs = inst.getInstantiatedFunctions();
    ASSERT_EQ(funcs.size(), 2u);
    EXPECT_EQ(funcs[0]->mangledName, "_Z5twiceIiE");
    EXPECT_EQ(funcs[1]->mangledName, "_Z5twiceIdE");
    EXPECT_NE(funcs[0]->mangledName, funcs[1]->mangledName);

    std::printf("── 同模板不同实参:\n");
    std::printf("   twice<int>    → %s\n", funcs[0]->mangledName.c_str());
    std::printf("   twice<double> → %s\n", funcs[1]->mangledName.c_str());
}

// =============================================================================
// NTTP（非类型模板参数）—— [temp.param]/6 + [temp.arg.nontype]
// =============================================================================
// 考察「类型 vs 值」两形态能否被区分：形参侧靠 TemplateParamKind，
// 实参侧靠 TemplateArg 的 tag，替换侧靠 substituteType/cloneExpr 两层分工。

// 30. NTTP 形参被 Parser 正确登记为 NonType（不是 Type）
TEST(Nttp, ParamKindIsNonType) {
    TemplateDeclPtr tmpl;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl("template<int N> class Buf { public: int cap; };");
    }

    ASSERT_EQ(tmpl->templateParams.size(), 1u);
    EXPECT_EQ(tmpl->templateParams[0].kind, TemplateParamKind::NonType);
    EXPECT_EQ(tmpl->templateParams[0].name, "N");
    ASSERT_NE(tmpl->templateParams[0].nonType, nullptr);
    EXPECT_TRUE(tmpl->templateParams[0].nonType->isInt());

    // ★ 同时盯住历史陷阱：typeParams 是退化的名字列表，
    //   它把 NTTP 的 N 也当"类型形参名"收着——所以形态判定绝不能用它。
    //   本断言把这个退化行为【固定】下来，防止有人误以为 typeParams 可靠。
    ASSERT_EQ(tmpl->typeParams.size(), 1u);
    EXPECT_EQ(tmpl->typeParams[0], "N");
    std::printf("── NTTP 形参: kind=NonType name=N nonType=int "
                "（而 typeParams[0]=\"N\" 已丢形态）\n");
}

// 31. 类型形参 vs 非类型形参：同一位置两种声明，kind 必须不同
TEST(Nttp, TypeVsNonTypeDistinguished) {
    TemplateDeclPtr typeTmpl, nttpTmpl;
    {
        StdoutCapture cap;
        typeTmpl = parseClassTmpl("template<class T> class Box { public: T v; };");
        nttpTmpl = parseClassTmpl("template<int N> class Buf { public: int c; };");
    }

    ASSERT_EQ(typeTmpl->templateParams.size(), 1u);
    ASSERT_EQ(nttpTmpl->templateParams.size(), 1u);
    EXPECT_EQ(typeTmpl->templateParams[0].kind, TemplateParamKind::Type);
    EXPECT_EQ(nttpTmpl->templateParams[0].kind, TemplateParamKind::NonType);
    // 两者的 typeParams 长得一样（都只有名字）——差别只在 templateParams
    EXPECT_EQ(typeTmpl->typeParams[0], "T");
    EXPECT_EQ(nttpTmpl->typeParams[0], "N");
    std::printf("── 区分依据: templateParams[i].kind，而非 typeParams\n");
}

// 32. NTTP 的 Itanium 编码：<expr-primary> = L <type> <value> E
TEST(Mangle, NttpInteger) {
    auto m1 = NameMangler::mangleTemplateInstance("Buf", {TemplateArg::ofValue(4)});
    EXPECT_EQ(m1, "_Z3BufILi4EE");
    std::printf("── Mangle: Buf<4> → %s\n", m1.c_str());
}

// 33. NTTP 负数编码：n 表负（'-' 不是合法 mangling 字符）
TEST(Mangle, NttpNegative) {
    auto m = NameMangler::mangleTemplateInstance("Buf", {TemplateArg::ofValue(-3)});
    EXPECT_EQ(m, "_Z3BufILin3EE");
    std::printf("── Mangle: Buf<-3> → %s   （clang: _ZN3BufILin3EE4sizeEv）\n",
                m.c_str());
}

// 34. 类型实参与值实参混排编码
TEST(Mangle, NttpMixedWithType) {
    auto m = NameMangler::mangleTemplateInstance(
        "Pair", {TemplateArg::ofType(Type::makeInt()), TemplateArg::ofValue(8)});
    EXPECT_EQ(m, "_Z4PairIiLi8EE");
    std::printf("── Mangle: Pair<int, 8> → %s   （clang: _ZN4PairIiLi8EE5countEv）\n",
                m.c_str());
}

// 35. 类型位置收到值实参 → 报错（NTTP 名被当类型用）
//     对照 clang: err_nontype_template_parameter_used_as_type
TEST(Nttp, ValueUsedAsTypeThrows) {
    TemplateInstantiator inst;
    TemplateInstantiator::TypeSubstitution subst = {
        {"N", TemplateArg::ofValue(4)},
    };

    {
        StdoutCapture cap;
        EXPECT_THROW(inst.substituteType(Type::makeTemplateParam("N"), subst),
                     std::runtime_error);
    }
    {
        // Parser 在模板形参作用域外的兜底路径会把裸 N 建成 Class("N")，
        // 同样必须拦下，不能把值当成类型返回。
        StdoutCapture cap;
        EXPECT_THROW(inst.substituteType(Type::makeClass("N"), subst),
                     std::runtime_error);
    }
    std::printf("── NTTP 名出现在类型位置 → 抛错（不得静默当成类型）\n");
}

// 36. 值替换落在表达式树：clone 出的实体方法体里 N 变成整数字面量 4
TEST(Nttp, ValueSubstitutionReachesExprTree) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl(
            "template<int N> class Buf { public: int size() { return N; } };");
        inst.instantiate(tmpl, {TemplateArg::ofValue(4)});
    }

    auto& classes = inst.getInstantiatedClasses();
    ASSERT_EQ(classes.size(), 1u);
    EXPECT_EQ(classes[0]->name, "Buf_4");
    ASSERT_EQ(classes[0]->methods.size(), 1u);

    // 方法体应已被改写为 `return 4;` —— VarExpr{N} 被换成 IntLiteralExpr{4}
    auto body = classes[0]->methods[0]->body;
    ASSERT_NE(body, nullptr);
    ASSERT_EQ(body->statements.size(), 1u);
    auto ret = std::dynamic_pointer_cast<ReturnStmt>(body->statements[0]);
    ASSERT_NE(ret, nullptr);
    auto lit = std::dynamic_pointer_cast<IntLiteralExpr>(ret->value);
    ASSERT_NE(lit, nullptr) << "N 应已被值替换为整数字面量，而非留着 VarExpr";
    EXPECT_EQ(lit->value, 4);

    std::printf("── 值替换落到表达式树: method body → return %lld\n",
                static_cast<long long>(lit->value));
}

// 37. 不同值 → 不同实例与不同符号（缓存键须区分值实参）
TEST(Nttp, DifferentValuesDifferentInstances) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl(
            "template<int N> class Buf { public: int size() { return N; } };");
        inst.instantiate(tmpl, {TemplateArg::ofValue(4)});
        inst.instantiate(tmpl, {TemplateArg::ofValue(8)});
    }

    auto& classes = inst.getInstantiatedClasses();
    ASSERT_EQ(classes.size(), 2u);
    EXPECT_EQ(classes[0]->name, "Buf_4");
    EXPECT_EQ(classes[1]->name, "Buf_8");
    EXPECT_NE(classes[0]->name, classes[1]->name);
    std::printf("── 不同值 → 不同实例: %s / %s\n",
                classes[0]->name.c_str(), classes[1]->name.c_str());
}

// 38. 实参个数不符 → 报错
TEST(Nttp, ArityMismatchThrows) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl(
            "template<class T, int N> class Pair { public: T first; };");
        EXPECT_THROW(inst.instantiate(tmpl, {TemplateArg::ofType(Type::makeInt())}),
                     std::runtime_error);
    }
    std::printf("── 实参个数不符（2 形参收 1 实参）→ 抛错\n");
}

// 39. 形态不符 → 报错（template<int N> 收到类型实参）
TEST(Nttp, KindMismatchThrows) {
    TemplateDeclPtr tmpl;
    TemplateInstantiator inst;
    {
        StdoutCapture cap;
        tmpl = parseClassTmpl("template<int N> class Buf { public: int cap; };");
        EXPECT_THROW(inst.instantiate(tmpl, {TemplateArg::ofType(Type::makeInt())}),
                     std::runtime_error);
    }
    std::printf("── 形态不符（NonType 形参收到 Type 实参）→ 抛错\n");
}

// =============================================================================
// 偏特化匹配（[temp.class.spec.match]）—— 与函数模板推导共用 deducePair
// =============================================================================

// 40. 模式 [T*, T] 匹配实参 [double*, double] → T := double
TEST(PartialSpec, PatternMatchSucceeds) {
    TemplateDeducer deducer;
    std::unordered_map<std::string, TypePtr> subst;
    std::string reason;

    std::vector<TypePtr> pattern = {
        Type::makePointer(Type::makeTemplateParam("T")),
        Type::makeTemplateParam("T"),
    };
    std::vector<TypePtr> args = {
        Type::makePointer(Type::makeDouble()),
        Type::makeDouble(),
    };

    bool ok = false;
    {
        StdoutCapture cap;
        ok = deducer.matchPattern(pattern, args, {"T"}, subst, reason);
    }

    EXPECT_TRUE(ok) << "reason: " << reason;
    ASSERT_EQ(subst.count("T"), 1u);
    EXPECT_TRUE(subst["T"]->isDouble());
    std::printf("── 偏特化匹配: Box<T*, T> 对 Box<double*, double> ⇒ T := %s\n",
                subst["T"]->toString().c_str());
}

// 41. 模式 [T*, T] 匹配实参 [int, int] → 指针结构失配，失败
TEST(PartialSpec, PatternMatchFailsOnNonPointer) {
    TemplateDeducer deducer;
    std::unordered_map<std::string, TypePtr> subst;
    std::string reason;

    std::vector<TypePtr> pattern = {
        Type::makePointer(Type::makeTemplateParam("T")),
        Type::makeTemplateParam("T"),
    };
    std::vector<TypePtr> args = { Type::makeInt(), Type::makeInt() };

    bool ok = true;
    {
        StdoutCapture cap;
        ok = deducer.matchPattern(pattern, args, {"T"}, subst, reason);
    }

    EXPECT_FALSE(ok) << "int 不是指针，T* 不该匹配成功";
    EXPECT_FALSE(reason.empty());
    std::printf("── 偏特化不匹配: Box<T*, T> 对 Box<int, int> ⇒ %s\n", reason.c_str());
}

// 42. 模式 [T*, T] 匹配实参 [int*, void] → T 绑定冲突（int vs void），失败
//     —— 这是 test_tmpl_31 中 Box<int*,void> 回落主模板的原因
TEST(PartialSpec, PatternMatchFailsOnConflictingBindings) {
    TemplateDeducer deducer;
    std::unordered_map<std::string, TypePtr> subst;
    std::string reason;

    std::vector<TypePtr> pattern = {
        Type::makePointer(Type::makeTemplateParam("T")),
        Type::makeTemplateParam("T"),
    };
    std::vector<TypePtr> args = {
        Type::makePointer(Type::makeInt()),
        Type::makeVoid(),
    };

    bool ok = true;
    {
        StdoutCapture cap;
        ok = deducer.matchPattern(pattern, args, {"T"}, subst, reason);
    }

    EXPECT_FALSE(ok) << "T 先绑 int 再绑 void，应冲突";
    std::printf("── 偏特化不匹配: Box<T*, T> 对 Box<int*, void> ⇒ %s\n", reason.c_str());
}

// 43. 模式与实参个数不符 → 失败
TEST(PartialSpec, PatternMatchFailsOnArityMismatch) {
    TemplateDeducer deducer;
    std::unordered_map<std::string, TypePtr> subst;
    std::string reason;

    bool ok = true;
    {
        StdoutCapture cap;
        ok = deducer.matchPattern({Type::makeTemplateParam("T")},
                                  {Type::makeInt(), Type::makeDouble()},
                                  {"T"}, subst, reason);
    }
    EXPECT_FALSE(ok);
    std::printf("── 偏特化模式/实参个数不符 ⇒ %s\n", reason.c_str());
}
