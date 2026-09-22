// =============================================================================
// tests/unit/test_expr_parser.cpp —— 表达式解析器白盒单元测试（优先级链全覆盖）
// =============================================================================
// 被测对象：src/parser.cpp 表达式解析优先级链
//   parseExpression → parseOrExpr → parseAndExpr → parseEqualityExpr
//     → parseComparisonExpr → parseAdditiveExpr → parseMultiplicativeExpr
//     → parseUnaryExpr → parsePostfixExpr → parsePrimaryExpr
//
// 考察理论点：
//   1. [expr.prim.literal]  字面量：int / bool / string / nullptr / this
//   2. 优先级分层递归下降（Precedence Climbing 的文法编码）：
//        优先级低 → 高
//        || → && → ==/!= → <,>,<=,>= → +,- → *,/,% → 一元 -,! → 后缀 ()/./-> → primary
//      每层产生式：left := 下一层(); while (本层运算符) { left = Bin(op,left,下一层()); }
//      对应 clang：ParseExpression / ParseRHSOfBinaryExpression（ParseExpr.cpp）
//   3. 左结合性：while 循环向左折叠（1-2-3 → Sub(Sub(1,2),3)），[expr.ass] 除外
//   4. [expr.prim.paren]    括号表达式递归回 parseExpression（无独立括号节点）
//   5. [temp.names]         template-id 歧义消解：foo<int>(x) 是 template-id，
//                           a < b 是比较 —— 试探法（存档→空跑→回滚），
//                           对应 clang ParseImplicitTemplateId 的简化版
//   6. 错误处理：panic 模式，语法错误抛 std::runtime_error（"[Parse Error] ..."）
//
// 观测风格与 test_preprocessor.cpp 对齐（共享 obs_helpers.h）：
//   ① StdoutCapture 捕获 Lexer/Parser 各阶段日志流水；
//   ② dumpWithExplanation 可视化 box：[输入] → [各阶段 trace + 中文解释] → [输出]；
//   ③ dumpExprTree/dumpStmtTree 递归嵌套打印 AST —— 优先级链每一层的
//      嵌套深度直接可见，递归下降的递归过程即树形深度。
// =============================================================================

#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "type.h"
#include "obs_helpers.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 辅助：把表达式文本包装进 "int test_fn() { return <expr>; }" 解析，取回表达式
// trace 出参承接 StdoutCapture 捕获到的阶段日志流水。
ExprPtr parseExpr(const std::string& exprStr, std::string& trace) {
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

// 辅助：解析 + 捕获 + 可视化（每个用例的主观测入口）
ExprPtr parseExprShow(const std::string& exprStr, const char* what) {
    std::string trace;
    auto e = parseExpr(exprStr, trace);
    dumpWithExplanation(what, exprStr, trace, [&]{ dumpExprTree(e, "", true); });
    return e;
}

// 辅助：解析完整函数体，返回语句列表（用于测试赋值/表达式语句）
std::vector<StmtPtr> parseBody(const std::string& body, std::string& trace) {
    std::string src = "int test_fn() { " + body + " }\n";
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    TranslationUnit unit;
    { StdoutCapture cap; unit = parser.parseTranslationUnit(); trace = cap.str(); }
    auto func = std::dynamic_pointer_cast<FunctionDecl>(unit.declarations[0]);
    if (!func || !func->body) return {};
    return func->body->statements;
}

// 辅助：断言并解包为 BinaryExpr，顺便校验运算符
std::shared_ptr<BinaryExpr> asBinary(const ExprPtr& e, BinaryOp op) {
    auto bin = std::dynamic_pointer_cast<BinaryExpr>(e);
    EXPECT_NE(bin, nullptr) << "expected BinaryExpr";
    if (bin) EXPECT_EQ(bin->op, op);
    return bin;
}

// 辅助：断言并解包为 IntLiteralExpr，顺便校验值
std::shared_ptr<IntLiteralExpr> asInt(const ExprPtr& e, int64_t v) {
    auto lit = std::dynamic_pointer_cast<IntLiteralExpr>(e);
    EXPECT_NE(lit, nullptr) << "expected IntLiteralExpr";
    if (lit) EXPECT_EQ(lit->value, v);
    return lit;
}

// 辅助：断言并解包为 VarExpr，顺便校验名字
std::shared_ptr<VarExpr> asVar(const ExprPtr& e, const std::string& name) {
    auto var = std::dynamic_pointer_cast<VarExpr>(e);
    EXPECT_NE(var, nullptr) << "expected VarExpr";
    if (var) EXPECT_EQ(var->name, name);
    return var;
}

// 辅助：预期解析失败 —— 捕获抛错前的阶段日志流水，打印可视化 box + 错误消息
void expectParseError(const std::string& exprStr, const char* what) {
    std::string trace, msg;
    try {
        std::string t;
        parseExpr(exprStr, t);
        trace = std::move(t);
    } catch (const std::runtime_error& e) {
        msg = e.what();
    }
    dumpWithExplanation(what, exprStr, trace, [&]{
        std::printf("✗ 解析失败（预期）: %s\n", msg.empty() ? "(未抛出!)" : msg.c_str());
    });
    EXPECT_FALSE(msg.empty()) << "应抛出 std::runtime_error: " << exprStr;
}

} // anonymous namespace

// =============================================================================
// 1. primary 层：全部字面量形态（[expr.prim.literal]）
// =============================================================================
TEST(ExprParserPrimary, AllLiteralForms) {
    // 整数字面量（含 0 与 int64 上界）
    asInt(parseExprShow("42", "primary：整数字面量"), 42);
    asInt(parseExprShow("0", "primary：零"), 0);
    asInt(parseExprShow("9223372036854775807", "primary：int64 上界"), INT64_MAX);

    // 布尔字面量
    auto t = std::dynamic_pointer_cast<BoolLiteralExpr>(parseExprShow("true", "primary：true"));
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->value);
    auto f = std::dynamic_pointer_cast<BoolLiteralExpr>(parseExprShow("false", "primary：false"));
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->value);

    // 字符串字面量
    auto s = std::dynamic_pointer_cast<StringLiteralExpr>(
        parseExprShow("\"hello minicc\"", "primary：字符串字面量"));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->value, "hello minicc");

    // nullptr
    EXPECT_NE(std::dynamic_pointer_cast<NullptrLiteralExpr>(
                  parseExprShow("nullptr", "primary：nullptr")), nullptr);

    // this（[expr.prim.this]；语法层合法即可，语义合法性由阶段3检查）
    EXPECT_NE(std::dynamic_pointer_cast<ThisExpr>(
                  parseExprShow("this", "primary：this")), nullptr);

    // 变量引用（[expr.prim.id]）
    asVar(parseExprShow("someVar", "primary：变量引用"), "someVar");
}

// =============================================================================
// 2. 一元层：- 与 ! 任意叠加（文法：unary := ('-'|'!') unary | postfix）
// =============================================================================
TEST(ExprParserUnary, ChainedUnaryOperators) {
    // -5 → Neg(Int(5))（词法上 '-' 与 '5' 分离，负号是一元运算符而非字面量一部分）
    auto e1 = parseExprShow("-5", "一元：-5 → Neg(Int(5))");
    auto neg = std::dynamic_pointer_cast<UnaryExpr>(e1);
    ASSERT_NE(neg, nullptr);
    EXPECT_EQ(neg->op, UnaryOp::Neg);
    asInt(neg->operand, 5);

    // --x 在文法下合法 → Neg(Neg(Var(x)))（语义合法性另说）
    // 树形：Unary(-) → Unary(-) → Var(x)，嵌套两层即递归两次
    auto e2 = parseExprShow("--x", "一元叠加：--x → Neg(Neg(Var(x)))");
    auto outer = std::dynamic_pointer_cast<UnaryExpr>(e2);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->op, UnaryOp::Neg);
    auto inner = std::dynamic_pointer_cast<UnaryExpr>(outer->operand);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->op, UnaryOp::Neg);
    asVar(inner->operand, "x");

    // !flag → Not(Var(flag))
    auto e3 = parseExprShow("!flag", "一元：!flag → Not(Var(flag))");
    auto notE = std::dynamic_pointer_cast<UnaryExpr>(e3);
    ASSERT_NE(notE, nullptr);
    EXPECT_EQ(notE->op, UnaryOp::Not);
    asVar(notE->operand, "flag");
}

// =============================================================================
// 3. 优先级全链：一条表达式贯穿所有优先级层
//    a + b == c && d || e
//    结合顺序（低→高绑定）：Or( And( Eq(Add(a,b), c), d ), e )
// =============================================================================
TEST(ExprParserPrecedence, FullChainShape) {
    auto e = parseExprShow("a + b == c && d || e",
                           "优先级全链：Or(And(Eq(Add(a,b),c),d),e)");
    ASSERT_NE(e, nullptr);

    auto orE = asBinary(e, BinaryOp::Or);
    ASSERT_NE(orE, nullptr);

    auto andE = asBinary(orE->left, BinaryOp::And);
    ASSERT_NE(andE, nullptr);
    asVar(orE->right, "e");

    auto eqE = asBinary(andE->left, BinaryOp::Eq);
    ASSERT_NE(eqE, nullptr);
    asVar(andE->right, "d");

    auto addE = asBinary(eqE->left, BinaryOp::Add);
    ASSERT_NE(addE, nullptr);
    asVar(addE->left, "a");
    asVar(addE->right, "b");
    asVar(eqE->right, "c");
}

// =============================================================================
// 4. 算术层内部优先级：*/% 比 +- 绑定更紧
// =============================================================================
TEST(ExprParserPrecedence, ArithmeticLevels) {
    // 1 + 2 * 3 → Add(1, Mul(2,3))
    auto e1 = asBinary(parseExprShow("1 + 2 * 3", "算术：1 + 2 * 3 → Add(1, Mul(2,3))"),
                       BinaryOp::Add);
    ASSERT_NE(e1, nullptr);
    asInt(e1->left, 1);
    asBinary(e1->right, BinaryOp::Mul);

    // 2 * 3 + 4 * 5 → Add(Mul(2,3), Mul(4,5))（两侧都是更高优先级层）
    auto e2 = asBinary(parseExprShow("2 * 3 + 4 * 5", "算术：两侧都是乘法层"),
                       BinaryOp::Add);
    ASSERT_NE(e2, nullptr);
    asBinary(e2->left, BinaryOp::Mul);
    asBinary(e2->right, BinaryOp::Mul);

    // 10 - 2 * 3 % 4 → Sub(10, Mod(Mul(2,3), 4))
    auto e3 = asBinary(parseExprShow("10 - 2 * 3 % 4",
                                     "算术：% 与 * 同层左结合，整体比 - 紧"),
                       BinaryOp::Sub);
    ASSERT_NE(e3, nullptr);
    asInt(e3->left, 10);
    auto modE = asBinary(e3->right, BinaryOp::Mod);
    ASSERT_NE(modE, nullptr);
    asBinary(modE->left, BinaryOp::Mul);
    asInt(modE->right, 4);

    // a + b == c 中 == 比 + 松 → Eq(Add(a,b), c)
    auto e4 = asBinary(parseExprShow("a + b == c", "跨层：== 比 + 松"), BinaryOp::Eq);
    ASSERT_NE(e4, nullptr);
    asBinary(e4->left, BinaryOp::Add);
    asVar(e4->right, "c");

    // a < b == c > d → Eq(Lt(a,b), Gt(c,d))：比较层比相等层绑定紧
    auto e5 = asBinary(parseExprShow("a < b == c > d", "跨层：比较层比相等层紧"),
                       BinaryOp::Eq);
    ASSERT_NE(e5, nullptr);
    asBinary(e5->left, BinaryOp::Lt);
    asBinary(e5->right, BinaryOp::Gt);

    // a || b && c → Or(a, And(b,c))：&& 比 || 紧
    auto e6 = asBinary(parseExprShow("a || b && c", "跨层：&& 比 || 紧"), BinaryOp::Or);
    ASSERT_NE(e6, nullptr);
    asVar(e6->left, "a");
    asBinary(e6->right, BinaryOp::And);
}

// =============================================================================
// 5. 一元 vs 二元：一元比 */% 绑定更紧（-a * b → Mul(Neg(a), b)）
// =============================================================================
TEST(ExprParserPrecedence, UnaryBindsTighterThanBinary) {
    // -a * b → Mul(Neg(a), b)
    auto e1 = asBinary(parseExprShow("-a * b", "一元更紧：-a * b → Mul(Neg(a), b)"),
                       BinaryOp::Mul);
    ASSERT_NE(e1, nullptr);
    auto neg = std::dynamic_pointer_cast<UnaryExpr>(e1->left);
    ASSERT_NE(neg, nullptr);
    EXPECT_EQ(neg->op, UnaryOp::Neg);
    asVar(e1->right, "b");

    // a * -b → Mul(a, Neg(b))
    auto e2 = asBinary(parseExprShow("a * -b", "一元更紧：a * -b → Mul(a, Neg(b))"),
                       BinaryOp::Mul);
    ASSERT_NE(e2, nullptr);
    asVar(e2->left, "a");
    auto negR = std::dynamic_pointer_cast<UnaryExpr>(e2->right);
    ASSERT_NE(negR, nullptr);
    EXPECT_EQ(negR->op, UnaryOp::Neg);

    // -(a + b) → Neg(Add(a,b))：括号内先算，再套一元
    auto e3 = std::dynamic_pointer_cast<UnaryExpr>(
        parseExprShow("-(a + b)", "括号 + 一元：-(a+b) → Neg(Add(a,b))"));
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->op, UnaryOp::Neg);
    asBinary(e3->operand, BinaryOp::Add);

    // !a == b → Eq(Not(a), b)：一元比相等层紧
    auto e4 = asBinary(parseExprShow("!a == b", "一元更紧：!a == b → Eq(Not(a), b)"),
                       BinaryOp::Eq);
    ASSERT_NE(e4, nullptr);
    auto notE = std::dynamic_pointer_cast<UnaryExpr>(e4->left);
    ASSERT_NE(notE, nullptr);
    EXPECT_EQ(notE->op, UnaryOp::Not);

    // -f(x) → Neg(Call)：一元作用于整个后缀表达式
    auto e5 = std::dynamic_pointer_cast<UnaryExpr>(
        parseExprShow("-f(x)", "一元作用于后缀：-f(x) → Neg(Call)"));
    ASSERT_NE(e5, nullptr);
    EXPECT_EQ(e5->op, UnaryOp::Neg);
    EXPECT_NE(std::dynamic_pointer_cast<CallExpr>(e5->operand), nullptr);
}

// =============================================================================
// 6. 左结合性：同层运算符 while 循环向左折叠
// =============================================================================
TEST(ExprParserAssociativity, LeftFoldAtEveryLevel) {
    // 1 - 2 - 3 → Sub(Sub(1,2), 3)：注意嵌套在 left 侧 = 向左折叠
    auto e1 = asBinary(parseExprShow("1 - 2 - 3", "左结合：Sub(Sub(1,2), 3) 嵌套在左孩子"),
                       BinaryOp::Sub);
    ASSERT_NE(e1, nullptr);
    asBinary(e1->left, BinaryOp::Sub);
    asInt(e1->right, 3);

    // 100 / 10 / 2 → Div(Div(100,10), 2)
    auto e2 = asBinary(parseExprShow("100 / 10 / 2", "左结合：除法链"), BinaryOp::Div);
    ASSERT_NE(e2, nullptr);
    asBinary(e2->left, BinaryOp::Div);
    asInt(e2->right, 2);

    // 8 % 3 % 2 → Mod(Mod(8,3), 2)
    auto e3 = asBinary(parseExprShow("8 % 3 % 2", "左结合：取模链"), BinaryOp::Mod);
    ASSERT_NE(e3, nullptr);
    asBinary(e3->left, BinaryOp::Mod);

    // a == b == c → Eq(Eq(a,b), c)（左结合，语义合法性另说）
    auto e4 = asBinary(parseExprShow("a == b == c", "左结合：相等链"), BinaryOp::Eq);
    ASSERT_NE(e4, nullptr);
    asBinary(e4->left, BinaryOp::Eq);
    asVar(e4->right, "c");

    // a < b < c → Lt(Lt(a,b), c)
    auto e5 = asBinary(parseExprShow("a < b < c", "左结合：比较链"), BinaryOp::Lt);
    ASSERT_NE(e5, nullptr);
    asBinary(e5->left, BinaryOp::Lt);

    // a && b && c → And(And(a,b), c)
    auto e6 = asBinary(parseExprShow("a && b && c", "左结合：逻辑与链"), BinaryOp::And);
    ASSERT_NE(e6, nullptr);
    asBinary(e6->left, BinaryOp::And);

    // a || b || c → Or(Or(a,b), c)
    auto e7 = asBinary(parseExprShow("a || b || c", "左结合：逻辑或链"), BinaryOp::Or);
    ASSERT_NE(e7, nullptr);
    asBinary(e7->left, BinaryOp::Or);
}

// =============================================================================
// 7. 括号：递归回 parseExpression，且无独立括号节点（直接返回内层表达式）
// =============================================================================
TEST(ExprParserParen, GroupingOverridesPrecedence) {
    // (1 + 2) * 3 → Mul(Add(1,2), 3)；注意外层没有括号包装节点
    auto e1 = asBinary(parseExprShow("(1 + 2) * 3", "括号：无括号节点，直接返回内层"),
                       BinaryOp::Mul);
    ASSERT_NE(e1, nullptr);
    asBinary(e1->left, BinaryOp::Add);
    asInt(e1->right, 3);

    // ((a)) 层层剥开还是 Var(a)
    asVar(parseExprShow("((a))", "括号：层层剥开仍是 Var(a)"), "a");

    // (a + b) * (c - d) → Mul(Add, Sub)
    auto e2 = asBinary(parseExprShow("(a + b) * (c - d)", "括号：两侧分组"), BinaryOp::Mul);
    ASSERT_NE(e2, nullptr);
    asBinary(e2->left, BinaryOp::Add);
    asBinary(e2->right, BinaryOp::Sub);

    // (a || b) && c → And(Or(a,b), c)：括号反转默认优先级
    auto e3 = asBinary(parseExprShow("(a || b) && c", "括号：反转默认优先级"),
                       BinaryOp::And);
    ASSERT_NE(e3, nullptr);
    asBinary(e3->left, BinaryOp::Or);
    asVar(e3->right, "c");
}

// =============================================================================
// 8. 后缀层：调用与成员访问链（[expr.post]），且后缀比算术层绑定紧
// =============================================================================
TEST(ExprParserPostfix, CallAndMemberChains) {
    // f(g(1)) 嵌套调用：Call 的实参又是一个 Call —— 后缀层的递归
    auto e1 = std::dynamic_pointer_cast<CallExpr>(
        parseExprShow("f(g(1))", "嵌套调用：f(g(1))"));
    ASSERT_NE(e1, nullptr);
    asVar(e1->callee, "f");
    ASSERT_EQ(e1->arguments.size(), 1u);
    auto innerCall = std::dynamic_pointer_cast<CallExpr>(e1->arguments[0]);
    ASSERT_NE(innerCall, nullptr);
    asVar(innerCall->callee, "g");
    asInt(innerCall->arguments[0], 1);

    // 2 * f(3) → Mul(2, Call)：后缀层比乘法层绑定紧
    auto e2 = asBinary(parseExprShow("2 * f(3)", "后缀更紧：2 * f(3)"), BinaryOp::Mul);
    ASSERT_NE(e2, nullptr);
    asInt(e2->left, 2);
    EXPECT_NE(std::dynamic_pointer_cast<CallExpr>(e2->right), nullptr);

    // getObj().field：调用结果再取成员
    auto e3 = std::dynamic_pointer_cast<MemberExpr>(
        parseExprShow("getObj().field", "调用后取成员：getObj().field"));
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->memberName, "field");
    EXPECT_FALSE(e3->isArrow);
    EXPECT_NE(std::dynamic_pointer_cast<CallExpr>(e3->object), nullptr);

    // a.b(1).c → Member( Call( Member(a.b), [1] ), c )
    auto e4 = std::dynamic_pointer_cast<MemberExpr>(
        parseExprShow("a.b(1).c", "成员/调用链：a.b(1).c"));
    ASSERT_NE(e4, nullptr);
    EXPECT_EQ(e4->memberName, "c");
    auto midCall = std::dynamic_pointer_cast<CallExpr>(e4->object);
    ASSERT_NE(midCall, nullptr);
    auto firstMember = std::dynamic_pointer_cast<MemberExpr>(midCall->callee);
    ASSERT_NE(firstMember, nullptr);
    EXPECT_EQ(firstMember->memberName, "b");
    asVar(firstMember->object, "a");

    // 实参内含复杂表达式：f(1 + 2 * 3, -g())
    auto e5 = std::dynamic_pointer_cast<CallExpr>(
        parseExprShow("f(1 + 2 * 3, -g())", "实参递归：f(1 + 2 * 3, -g())"));
    ASSERT_NE(e5, nullptr);
    ASSERT_EQ(e5->arguments.size(), 2u);
    auto arg0 = asBinary(e5->arguments[0], BinaryOp::Add);
    ASSERT_NE(arg0, nullptr);
    asBinary(arg0->right, BinaryOp::Mul);
    auto arg1 = std::dynamic_pointer_cast<UnaryExpr>(e5->arguments[1]);
    ASSERT_NE(arg1, nullptr);
    EXPECT_EQ(arg1->op, UnaryOp::Neg);

    // 空实参列表：f()
    auto e6 = std::dynamic_pointer_cast<CallExpr>(parseExprShow("f()", "空调用：f()"));
    ASSERT_NE(e6, nullptr);
    EXPECT_TRUE(e6->arguments.empty());
}

// =============================================================================
// 9. new 表达式（[expr.new]）：类名 + 可选构造参数
// =============================================================================
TEST(ExprParserNew, NewExpressionForms) {
    // new Node（无括号，无参）
    auto e1 = std::dynamic_pointer_cast<NewExpr>(
        parseExprShow("new Node", "new：无括号无参"));
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->className, "Node");
    EXPECT_TRUE(e1->constructorArgs.empty());

    // new Node()（空实参列表）
    auto e2 = std::dynamic_pointer_cast<NewExpr>(
        parseExprShow("new Node()", "new：空实参列表"));
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->className, "Node");
    EXPECT_TRUE(e2->constructorArgs.empty());

    // new Node(1 + 1, f(2))：构造参数是完整表达式
    auto e3 = std::dynamic_pointer_cast<NewExpr>(
        parseExprShow("new Node(1 + 1, f(2))", "new：构造参数是完整表达式"));
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->className, "Node");
    ASSERT_EQ(e3->constructorArgs.size(), 2u);
    asBinary(e3->constructorArgs[0], BinaryOp::Add);
    EXPECT_NE(std::dynamic_pointer_cast<CallExpr>(e3->constructorArgs[1]), nullptr);
}

// =============================================================================
// 10. template-id 歧义消解（[temp.names]，试探法：存档→空跑→回滚）
//     foo<int>(x)  → VarExpr(foo).explicitTemplateArgs=[int] 后接调用
//     a < b        → 回滚为比较表达式
// =============================================================================
TEST(ExprParserTemplateId, ExplicitArgsAndComparisonRollback) {
    // foo<int>(x)：单实参
    auto e1 = std::dynamic_pointer_cast<CallExpr>(
        parseExprShow("foo<int>(x)", "template-id：foo<int>(x) 单显参"));
    ASSERT_NE(e1, nullptr);
    auto callee1 = asVar(e1->callee, "foo");
    ASSERT_NE(callee1, nullptr);
    ASSERT_EQ(callee1->explicitTemplateArgs.size(), 1u);
    // 显式模板实参是 tagged 的 TemplateArg（[temp.arg]），取值前先看 kind：
    // 这里是类型实参，再取 .type 判类型。
    ASSERT_TRUE(callee1->explicitTemplateArgs[0].isType());
    EXPECT_TRUE(callee1->explicitTemplateArgs[0].type->isInt());
    ASSERT_EQ(e1->arguments.size(), 1u);
    asVar(e1->arguments[0], "x");

    // foo<int, double>(x)：多实参
    auto e2 = std::dynamic_pointer_cast<CallExpr>(
        parseExprShow("foo<int, double>(x)", "template-id：多显参"));
    ASSERT_NE(e2, nullptr);
    auto callee2 = asVar(e2->callee, "foo");
    ASSERT_NE(callee2, nullptr);
    ASSERT_EQ(callee2->explicitTemplateArgs.size(), 2u);
    ASSERT_TRUE(callee2->explicitTemplateArgs[0].isType());
    ASSERT_TRUE(callee2->explicitTemplateArgs[1].isType());
    EXPECT_TRUE(callee2->explicitTemplateArgs[0].type->isInt());
    EXPECT_TRUE(callee2->explicitTemplateArgs[1].type->isDouble());

    // foo<>(x)：空显参列表也算 template-id
    auto e3 = std::dynamic_pointer_cast<CallExpr>(
        parseExprShow("foo<>(x)", "template-id：空显参列表 <>"));
    ASSERT_NE(e3, nullptr);
    auto callee3 = asVar(e3->callee, "foo");
    ASSERT_NE(callee3, nullptr);
    EXPECT_TRUE(callee3->explicitTemplateArgs.empty());

    // a < b：试探失败回滚 → 普通比较（不是 template-id）
    auto e4 = asBinary(parseExprShow("a < b", "歧义消解：a < b 试探失败回滚为比较"),
                       BinaryOp::Lt);
    ASSERT_NE(e4, nullptr);
    auto lhs = asVar(e4->left, "a");
    ASSERT_NE(lhs, nullptr);
    EXPECT_TRUE(lhs->explicitTemplateArgs.empty()); // 未误判为 template-id

    // a < b > c：两次比较，左结合 → Gt(Lt(a,b), c)
    auto e5 = asBinary(parseExprShow("a < b > c", "歧义消解：a < b > c 仍是比较链"),
                       BinaryOp::Gt);
    ASSERT_NE(e5, nullptr);
    asBinary(e5->left, BinaryOp::Lt);
    asVar(e5->right, "c");
}

// =============================================================================
// 11. 命名空间限定名：a::b::c 折叠进单个名字（[basic.lookup.qual]）
// =============================================================================
TEST(ExprParserQualifiedNames, ColonColonFolding) {
    // std::foo(1) → CallExpr，callee 名字 "std::foo"
    auto e1 = std::dynamic_pointer_cast<CallExpr>(
        parseExprShow("std::foo(1)", "限定名：std::foo(1) 折叠为单个名字"));
    ASSERT_NE(e1, nullptr);
    asVar(e1->callee, "std::foo");

    // A::B::x → VarExpr 名字 "A::B::x"
    asVar(parseExprShow("A::B::x", "限定名：A::B::x 折叠为单个名字"), "A::B::x");
}

// =============================================================================
// 12. 语句层：赋值语句与表达式语句（[expr.ass] / [stmt.expr]）
// =============================================================================
TEST(ExprParserStatements, AssignmentAndExprStmt) {
    std::string trace;
    std::string body = "x = 5; obj.f = 1 + 2; y = a < b; foo();";
    auto stmts = parseBody(body, trace);
    dumpWithExplanation("语句层：赋值 + 表达式语句（4 条）", body, trace, [&]{
        for (size_t i = 0; i < stmts.size(); ++i)
            dumpStmtTree(stmts[i], "", i + 1 == stmts.size());
    });
    ASSERT_EQ(stmts.size(), 4u);

    // x = 5 → AssignStmt{ target=Var(x), value=Int(5) }
    auto a1 = std::dynamic_pointer_cast<AssignStmt>(stmts[0]);
    ASSERT_NE(a1, nullptr);
    asVar(a1->target, "x");
    asInt(a1->value, 5);

    // obj.f = 1 + 2 → 赋值目标是 MemberExpr，值是完整表达式
    auto a2 = std::dynamic_pointer_cast<AssignStmt>(stmts[1]);
    ASSERT_NE(a2, nullptr);
    auto target = std::dynamic_pointer_cast<MemberExpr>(a2->target);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->memberName, "f");
    asBinary(a2->value, BinaryOp::Add);

    // y = a < b → 赋值号右侧的 '<' 正确回滚为比较（非 template-id）
    auto a3 = std::dynamic_pointer_cast<AssignStmt>(stmts[2]);
    ASSERT_NE(a3, nullptr);
    asVar(a3->target, "y");
    asBinary(a3->value, BinaryOp::Lt);

    // foo(); → ExprStmt[ CallExpr ]
    auto s4 = std::dynamic_pointer_cast<ExprStmt>(stmts[3]);
    ASSERT_NE(s4, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<CallExpr>(s4->expr), nullptr);
}

// =============================================================================
// 13. 错误处理：panic 模式抛 std::runtime_error（"[Parse Error] ..."）
// =============================================================================
TEST(ExprParserErrors, ThrowRuntimeErrorOnSyntaxError) {
    // "1 +"：右操作数缺失，primary 层遇到 ';' 报错
    expectParseError("1 +", "错误：右操作数缺失");

    // "(1 + 2"：缺少右括号
    expectParseError("(1 + 2", "错误：缺少右括号");

    // "1 *"：二元乘法右操作数缺失。
    // ★ 这里刻意用 "1 *" 而不是 "*3" —— 后者自一元解引用实现后已【语法合法】
    //   （无左操作数 ⇒ 判为解引用 [expr.unary.op]/1），只在语义阶段报
    //   "Indirection requires pointer operand"。判据就是"位置上有没有左操作数"，
    //   语义侧的用例见 tests/unit/test_expressions.cpp 的 Semantics.DerefRequiresPointer。
    expectParseError("1 *", "错误：乘法右操作数缺失");

    // "f(1,)"：尾逗号后缺少实参
    expectParseError("f(1,)", "错误：尾逗号后缺少实参");

    // "new 5"：new 后必须是类名（标识符）
    expectParseError("new 5", "错误：new 后必须是类名");

    // 错误消息带 "[Parse Error]" 前缀（与 Parser::errorAt 格式一致）
    try {
        std::string t;
        parseExpr("1 +", t);
        FAIL() << "expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("[Parse Error]"), std::string::npos);
    }
}
