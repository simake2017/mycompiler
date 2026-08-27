// =============================================================================
// tests/unit/test_expressions.cpp —— 表达式语法解析与类型推导白盒单元测试
// =============================================================================
// 考察理论点：
//   1. [expr.prim]      基本表达式：字面量 (int/bool/string/nullptr)、变量、this
//   2. [expr.unary]     一元运算：负号 -、逻辑非 !
//   3. [expr.mul]/[expr.add]/[expr.rel]/[expr.eq]/[expr.log.and]/[expr.log.or]
//                       优先级分层递归下降解析 (Precedence Climbing 文法映射)
//   4. [expr.post]      后缀表达式：函数调用 CallExpr、成员访问 MemberExpr (. 与 ->)
//   5. [expr.new]/[expr.delete]
//                       对象创建 new 与释放 delete 语法与语义推导
//   6. 语义类型推导与提升：常用算术转换 (int+double -> double)、比较运算 (-> bool)
//
// 观测风格与 test_preprocessor.cpp 对齐：
//   ① StdoutCapture 捕获 Lexer/Parser/Sema 各阶段的 std::cout 中文日志流水；
//   ② dumpWithExplanation 按「源行分块」打印 trace，每行日志后附中文解释；
//   ③ dumpExprTree 递归嵌套打印 AST（├─/└─ 树形），把递归下降解析的
//      递归过程（嵌套结构）直接可视化 —— 这是表达式解析最核心的可观测面。
// =============================================================================

#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "ast.h"
#include "type.h"
#include "obs_helpers.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace minicc;

namespace {


// 辅助函数：从函数体中解析并提取第一个表达式（解析期间捕获阶段日志）
// 例如 "return 1 + 2 * 3;" -> BinaryExpr(Add, ...)
ExprPtr parseExpressionFromSource(const std::string& exprStr, std::string& trace) {
    std::string src = "int test_fn() { return " + exprStr + "; }\n";
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    TranslationUnit unit;
    { StdoutCapture cap; unit = parser.parseTranslationUnit(); trace = cap.str(); }
    if (unit.declarations.empty()) return nullptr;
    auto func = std::dynamic_pointer_cast<FunctionDecl>(unit.declarations[0]);
    if (!func || !func->body || func->body->statements.empty()) return nullptr;
    auto ret = std::dynamic_pointer_cast<ReturnStmt>(func->body->statements[0]);
    return ret ? ret->value : nullptr;
}

// 辅助函数：解析并运行完整的语义分析（全程捕获阶段日志）
std::pair<TranslationUnit, std::string> analyzeSource(const std::string& src) {
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    TranslationUnit unit;
    std::string trace;
    { StdoutCapture cap;
      unit = parser.parseTranslationUnit();
      SemanticAnalyzer sema;
      sema.analyze(unit);
      trace = cap.str(); }
    return {std::move(unit), trace};
}

} // anonymous namespace

// =============================================================================
// 1. 基本字面量与基本表达式解析测试
// =============================================================================
TEST(ExprLiterals, PrimitiveLiterals) {
    // 整数字面量
    std::string trace;
    auto e1 = parseExpressionFromSource("42", trace);
    dumpWithExplanation("整数字面量 42", "42", trace, [&]{ dumpExprTree(e1, "", true); });
    ASSERT_NE(e1, nullptr);
    auto intLit = std::dynamic_pointer_cast<IntLiteralExpr>(e1);
    ASSERT_NE(intLit, nullptr);
    EXPECT_EQ(intLit->value, 42);

    // 负整数字面量（通过一元负号 Neg 节点解析）
    auto e2 = parseExpressionFromSource("-100", trace);
    dumpWithExplanation("负整数字面量 -100（Unary(Neg) 包裹）", "-100", trace,
                        [&]{ dumpExprTree(e2, "", true); });
    ASSERT_NE(e2, nullptr);
    auto unaryNeg = std::dynamic_pointer_cast<UnaryExpr>(e2);
    ASSERT_NE(unaryNeg, nullptr);
    EXPECT_EQ(unaryNeg->op, UnaryOp::Neg);
    auto negOperand = std::dynamic_pointer_cast<IntLiteralExpr>(unaryNeg->operand);
    ASSERT_NE(negOperand, nullptr);
    EXPECT_EQ(negOperand->value, 100);

    // 布尔字面量
    auto e3 = parseExpressionFromSource("true", trace);
    dumpWithExplanation("布尔字面量 true", "true", trace, [&]{ dumpExprTree(e3, "", true); });
    ASSERT_NE(e3, nullptr);
    auto boolLit1 = std::dynamic_pointer_cast<BoolLiteralExpr>(e3);
    ASSERT_NE(boolLit1, nullptr);
    EXPECT_TRUE(boolLit1->value);

    auto e4 = parseExpressionFromSource("false", trace);
    ASSERT_NE(e4, nullptr);
    auto boolLit2 = std::dynamic_pointer_cast<BoolLiteralExpr>(e4);
    ASSERT_NE(boolLit2, nullptr);
    EXPECT_FALSE(boolLit2->value);

    // 字符串字面量
    auto e5 = parseExpressionFromSource("\"minicc compiler\"", trace);
    dumpWithExplanation("字符串字面量", "\"minicc compiler\"", trace,
                        [&]{ dumpExprTree(e5, "", true); });
    ASSERT_NE(e5, nullptr);
    auto strLit = std::dynamic_pointer_cast<StringLiteralExpr>(e5);
    ASSERT_NE(strLit, nullptr);
    EXPECT_EQ(strLit->value, "minicc compiler");

    // nullptr
    auto e6 = parseExpressionFromSource("nullptr", trace);
    dumpWithExplanation("nullptr 字面量", "nullptr", trace, [&]{ dumpExprTree(e6, "", true); });
    ASSERT_NE(e6, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<NullptrLiteralExpr>(e6), nullptr);
}

// =============================================================================
// 2. 一元表达式与逻辑非
// =============================================================================
TEST(ExprUnary, LogicNotAndChainedUnary) {
    // 逻辑非 !flag
    std::string trace;
    auto e1 = parseExpressionFromSource("!true", trace);
    dumpWithExplanation("逻辑非 !true", "!true", trace, [&]{ dumpExprTree(e1, "", true); });
    ASSERT_NE(e1, nullptr);
    auto un1 = std::dynamic_pointer_cast<UnaryExpr>(e1);
    ASSERT_NE(un1, nullptr);
    EXPECT_EQ(un1->op, UnaryOp::Not);
    EXPECT_NE(std::dynamic_pointer_cast<BoolLiteralExpr>(un1->operand), nullptr);

    // 连续一元运算 !-x：嵌套 Unary 体现一元解析的递归
    auto e2 = parseExpressionFromSource("!-5", trace);
    dumpWithExplanation("连续一元 !-5（Unary 嵌套 = 递归解析）", "!-5", trace,
                        [&]{ dumpExprTree(e2, "", true); });
    ASSERT_NE(e2, nullptr);
    auto un2 = std::dynamic_pointer_cast<UnaryExpr>(e2);
    ASSERT_NE(un2, nullptr);
    EXPECT_EQ(un2->op, UnaryOp::Not);
    auto innerUn = std::dynamic_pointer_cast<UnaryExpr>(un2->operand);
    ASSERT_NE(innerUn, nullptr);
    EXPECT_EQ(innerUn->op, UnaryOp::Neg);
}

// =============================================================================
// 3. 运算符优先级与结合性测试 (Precedence & Associativity)
// =============================================================================
TEST(ExprPrecedence, ArithmeticAndPrecedenceClimbing) {
    // 1 + 2 * 3 应该被解析为 Add(1, Mul(2, 3))
    std::string trace;
    auto e1 = parseExpressionFromSource("1 + 2 * 3", trace);
    dumpWithExplanation("优先级爬升：1 + 2 * 3 → Add(1, Mul(2,3))", "1 + 2 * 3", trace,
                        [&]{ dumpExprTree(e1, "", true); });
    ASSERT_NE(e1, nullptr);
    auto add = std::dynamic_pointer_cast<BinaryExpr>(e1);
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::Add);

    auto leftLit = std::dynamic_pointer_cast<IntLiteralExpr>(add->left);
    ASSERT_NE(leftLit, nullptr);
    EXPECT_EQ(leftLit->value, 1);

    auto rightMul = std::dynamic_pointer_cast<BinaryExpr>(add->right);
    ASSERT_NE(rightMul, nullptr);
    EXPECT_EQ(rightMul->op, BinaryOp::Mul);

    // 括号改变优先级：(1 + 2) * 3 应该被解析为 Mul(Add(1, 2), 3)
    auto e2 = parseExpressionFromSource("(1 + 2) * 3", trace);
    dumpWithExplanation("括号提升优先级：(1 + 2) * 3 → Mul(Add(1,2), 3)", "(1 + 2) * 3", trace,
                        [&]{ dumpExprTree(e2, "", true); });
    ASSERT_NE(e2, nullptr);
    auto mul = std::dynamic_pointer_cast<BinaryExpr>(e2);
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::Mul);

    auto leftAdd = std::dynamic_pointer_cast<BinaryExpr>(mul->left);
    ASSERT_NE(leftAdd, nullptr);
    EXPECT_EQ(leftAdd->op, BinaryOp::Add);

    // 左结合性测试：10 - 5 - 2 应该被解析为 Sub(Sub(10, 5), 2)
    auto e3 = parseExpressionFromSource("10 - 5 - 2", trace);
    dumpWithExplanation("左结合：10 - 5 - 2 → Sub(Sub(10,5), 2)", "10 - 5 - 2", trace,
                        [&]{ dumpExprTree(e3, "", true); });
    ASSERT_NE(e3, nullptr);
    auto outerSub = std::dynamic_pointer_cast<BinaryExpr>(e3);
    ASSERT_NE(outerSub, nullptr);
    EXPECT_EQ(outerSub->op, BinaryOp::Sub);

    auto innerSub = std::dynamic_pointer_cast<BinaryExpr>(outerSub->left);
    ASSERT_NE(innerSub, nullptr);
    EXPECT_EQ(innerSub->op, BinaryOp::Sub);
    auto right2 = std::dynamic_pointer_cast<IntLiteralExpr>(outerSub->right);
    ASSERT_NE(right2, nullptr);
    EXPECT_EQ(right2->value, 2);
}

// =============================================================================
// 4. 关系比较与逻辑运算符优先级
// =============================================================================
TEST(ExprLogicAndRelational, MixedLogicPrecedence) {
    // a < b && c >= d || e == f
    // 优先级：(<, >=) > (==) > (&&) > (||)
    // 根节点应为 ||，其左孩子为 &&，右孩子为 ==
    std::string trace;
    auto e = parseExpressionFromSource("a < b && c >= d || e == f", trace);
    dumpWithExplanation("混合优先级：|| 为根，嵌套 && 与比较",
                        "a < b && c >= d || e == f", trace,
                        [&]{ dumpExprTree(e, "", true); });
    ASSERT_NE(e, nullptr);
    auto orExpr = std::dynamic_pointer_cast<BinaryExpr>(e);
    ASSERT_NE(orExpr, nullptr);
    EXPECT_EQ(orExpr->op, BinaryOp::Or);

    // 左侧应为 &&
    auto andExpr = std::dynamic_pointer_cast<BinaryExpr>(orExpr->left);
    ASSERT_NE(andExpr, nullptr);
    EXPECT_EQ(andExpr->op, BinaryOp::And);

    // && 的左侧是 a < b
    auto ltExpr = std::dynamic_pointer_cast<BinaryExpr>(andExpr->left);
    ASSERT_NE(ltExpr, nullptr);
    EXPECT_EQ(ltExpr->op, BinaryOp::Lt);

    // && 的右侧是 c >= d
    auto geExpr = std::dynamic_pointer_cast<BinaryExpr>(andExpr->right);
    ASSERT_NE(geExpr, nullptr);
    EXPECT_EQ(geExpr->op, BinaryOp::Ge);

    // || 的右侧是 e == f
    auto eqExpr = std::dynamic_pointer_cast<BinaryExpr>(orExpr->right);
    ASSERT_NE(eqExpr, nullptr);
    EXPECT_EQ(eqExpr->op, BinaryOp::Eq);
}

// =============================================================================
// 5. 后缀表达式：函数调用与成员访问
// =============================================================================
TEST(ExprPostfixAndCalls, CallAndMemberAccessChain) {
    // 函数调用：foo(1, 2 + 3)
    std::string trace;
    auto e1 = parseExpressionFromSource("foo(1, 2 + 3)", trace);
    dumpWithExplanation("函数调用 foo(1, 2 + 3)", "foo(1, 2 + 3)", trace,
                        [&]{ dumpExprTree(e1, "", true); });
    ASSERT_NE(e1, nullptr);
    auto call = std::dynamic_pointer_cast<CallExpr>(e1);
    ASSERT_NE(call, nullptr);
    auto callee = std::dynamic_pointer_cast<VarExpr>(call->callee);
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(callee->name, "foo");
    ASSERT_EQ(call->arguments.size(), 2u);

    // 成员访问链与方法调用：p->next.getValue()
    // MemberExpr 嵌套链：Call( Member(.getValue) ← Member(->next) ← Var(p) )
    auto e2 = parseExpressionFromSource("p->next.getValue()", trace);
    dumpWithExplanation("成员访问链 + 方法调用 p->next.getValue()",
                        "p->next.getValue()", trace,
                        [&]{ dumpExprTree(e2, "", true); });
    ASSERT_NE(e2, nullptr);
    auto methodCall = std::dynamic_pointer_cast<CallExpr>(e2);
    ASSERT_NE(methodCall, nullptr);
    auto memberDot = std::dynamic_pointer_cast<MemberExpr>(methodCall->callee);
    ASSERT_NE(memberDot, nullptr);
    EXPECT_EQ(memberDot->memberName, "getValue");
    EXPECT_FALSE(memberDot->isArrow);

    auto memberArrow = std::dynamic_pointer_cast<MemberExpr>(memberDot->object);
    ASSERT_NE(memberArrow, nullptr);
    EXPECT_EQ(memberArrow->memberName, "next");
    EXPECT_TRUE(memberArrow->isArrow);
}

// =============================================================================
// 6. new 与 delete 表达式解析
// =============================================================================
TEST(ExprNewAndDelete, DynamicAllocationSyntax) {
    // new Node(10, 20)
    std::string trace;
    auto e1 = parseExpressionFromSource("new Node(10, 20)", trace);
    dumpWithExplanation("new 表达式 new Node(10, 20)", "new Node(10, 20)", trace,
                        [&]{ dumpExprTree(e1, "", true); });
    ASSERT_NE(e1, nullptr);
    auto newExpr = std::dynamic_pointer_cast<NewExpr>(e1);
    ASSERT_NE(newExpr, nullptr);
    EXPECT_EQ(newExpr->className, "Node");
    ASSERT_EQ(newExpr->constructorArgs.size(), 2u);

    // 语句中的 delete ptr
    std::string src = "class C {}; int main() { C* p = new C(); delete p; return 0; }\n";
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    TranslationUnit unit;
    { StdoutCapture cap; unit = parser.parseTranslationUnit(); trace = cap.str(); }
    auto dumpFn = [&]{
        auto mainFn2 = std::dynamic_pointer_cast<FunctionDecl>(unit.declarations[1]);
        if (mainFn2 && mainFn2->body) {
            std::printf("FunctionDecl(main)\n");
            for (size_t i = 0; i < mainFn2->body->statements.size(); ++i)
                dumpStmtTree(mainFn2->body->statements[i], "",
                             i + 1 == mainFn2->body->statements.size());
        }
    };
    dumpWithExplanation("delete 语句（含 VarDecl/new/return 完整函数体）", src, trace, dumpFn);
    ASSERT_EQ(unit.declarations.size(), 2u);
    auto mainFn = std::dynamic_pointer_cast<FunctionDecl>(unit.declarations[1]);
    ASSERT_NE(mainFn, nullptr);
    ASSERT_GE(mainFn->body->statements.size(), 2u);
    auto delStmt = std::dynamic_pointer_cast<DeleteStmt>(mainFn->body->statements[1]);
    ASSERT_NE(delStmt, nullptr);
    EXPECT_FALSE(delStmt->isArray);
}

// =============================================================================
// 7. 语义推导与类型计算集成测试
// =============================================================================
TEST(ExprSemanticInference, TypeResolutionAndPromotion) {
    std::string src =
        "int main() {\n"
        "  int a = 10;\n"
        "  double b = 3;\n"
        "  bool flag = a < 20 && true;\n"
        "  auto c = a + 5;\n"
        "  return c;\n"
        "}\n";

    auto [unit, trace] = analyzeSource(src);
    auto mainFn = std::dynamic_pointer_cast<FunctionDecl>(unit.declarations[0]);
    auto dumpFn = [&]{
        if (mainFn && mainFn->body) {
            std::printf("FunctionDecl(main)\n");
            for (size_t i = 0; i < mainFn->body->statements.size(); ++i)
                dumpStmtTree(mainFn->body->statements[i], "",
                             i + 1 == mainFn->body->statements.size());
        }
        if (mainFn) {
            // auto 推导后：initializer 的 resolvedType 即为最终类型
            auto vc = std::dynamic_pointer_cast<VarDeclStmt>(mainFn->body->statements[3]);
            if (vc && vc->initializer && vc->initializer->resolvedType)
                std::printf("★ auto c 的初始化表达式推导类型: %s → %s\n",
                            vc->declaredType ? vc->declaredType->toString().c_str() : "?",
                            vc->initializer->resolvedType->toString().c_str());
        }
    };
    dumpWithExplanation("语义阶段：auto 推导 + 类型提升（Parse→Sema 全流水）",
                        src, trace, dumpFn);

    ASSERT_NE(mainFn, nullptr);
    ASSERT_EQ(mainFn->body->statements.size(), 5u);

    // auto c 应该被推导为 int
    auto varC = std::dynamic_pointer_cast<VarDeclStmt>(mainFn->body->statements[3]);
    ASSERT_NE(varC, nullptr);
    EXPECT_EQ(varC->name, "c");
    EXPECT_TRUE(varC->declaredType->isInt());
}
