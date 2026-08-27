// =============================================================================
// tests/unit/test_ast_nodes.cpp —— AST 节点体系白盒单元测试
// =============================================================================
// 被测对象：include/ast.h 三大节点族 + src/parser.cpp 建树形状
//
// 考察理论点（对应 docs/learn/10-ast.md）：
//   1. 三大节点族分类：表达=有类型可求值 / 语句=控制流程 / 声明=引入名字
//      （对应 clang Expr/Stmt/Decl 三分；[expr] [stmt] [basic.scope]）
//   2. NodeKind 运行时标签 + 继承双轨：switch 分派用 kind，字段访问用
//      static_pointer_cast（对应 clang Stmt::StmtClass / Decl::Kind）
//   3. 优先级即嵌套：1 + 2 * 3 → Add(1, Mul(2,3))，括号信息被树形吸收，
//      AST 无括号节点（抽象语法树之"抽象"）
//   4. TranslationUnit 根节点（对应 clang TranslationUnitDecl）
//   5. SourceLocation 源码位置记录（报错定位的基础）
//   6. 职责分离：Parser 阶段 resolvedType 为空，语义分析后填充
//   7. 模板蓝图冻结：TemplateDecl 的 classTemplate/funcTemplate 互斥双槽位
//
// 观测风格与 test_expr_parser.cpp 对齐（共享 obs_helpers.h）：
//   ① StdoutCapture 捕获 Parser 阶段日志；
//   ② dumpWithExplanation 可视化 box：[输入] → [trace] → [AST 树]；
//   ③ dumpExprTree/dumpStmtTree 递归嵌套打印，树的形状直接可见。
// =============================================================================

#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "ast.h"
#include "obs_helpers.h"
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 辅助：解析整段源码为 TranslationUnit（捕获并丢弃阶段日志）
TranslationUnit parseSrc(const std::string& src) {
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    StdoutCapture cap;
    return parser.parseTranslationUnit();
}

// 辅助：解析 + 捕获日志 + 可视化（主观测入口），返回整棵树
TranslationUnit parseShow(const std::string& src, const char* what,
                          std::string& trace) {
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    TranslationUnit unit;
    { StdoutCapture cap; unit = parser.parseTranslationUnit(); trace = cap.str(); }
    dumpWithExplanation(what, src, trace, [&] {
        std::printf("TranslationUnit(%zu 个顶层声明)\n", unit.declarations.size());
        for (auto& d : unit.declarations) {
            if (auto f = std::dynamic_pointer_cast<FunctionDecl>(d)) {
                std::printf("├─ FunctionDecl(%s)\n", f->name.c_str());
                if (f->body) dumpStmtTree(f->body, "│  ", true);
            } else if (auto c = std::dynamic_pointer_cast<ClassDecl>(d)) {
                std::printf("├─ ClassDecl(%s)\n", c->name.c_str());
            } else if (auto t = std::dynamic_pointer_cast<TemplateDecl>(d)) {
                std::printf("├─ TemplateDecl(%s): %s\n", t->templateName().c_str(),
                            t->isClassTemplate() ? "类模板" : "函数模板");
            } else {
                std::printf("├─ <声明 kind=%d>\n", static_cast<int>(d->kind));
            }
        }
    });
    return unit;
}

// 辅助：取第 0 个顶层函数声明的函数体语句列表
std::vector<StmtPtr> bodyOf(const TranslationUnit& unit, size_t funcIdx = 0) {
    size_t seen = 0;
    for (auto& d : unit.declarations) {
        if (auto f = std::dynamic_pointer_cast<FunctionDecl>(d)) {
            if (seen++ == funcIdx) return f->body ? f->body->statements : std::vector<StmtPtr>{};
        }
    }
    return {};
}

} // anonymous namespace

// =============================================================================
// 1. 三大节点族：顶层声明族全谱（声明 = 引入名字）
// =============================================================================
TEST(AstNodeFamilies, TopLevelDeclarationKinds) {
    std::string trace;
    std::string src =
        "int g_counter = 0;\n"                                  // GlobalVar
        "enum Color { Red, Green };\n"                          // Enum
        "using IntPtr = int;\n"                                 // TypeAlias
        "namespace Math { int one() { return 1; } }\n"          // Namespace
        "class Point { int x; };\n"                             // Class
        "template<typename T> T id(T v) { return v; }\n"        // Template
        "int main() { return 0; }\n";                           // Function
    auto unit = parseShow(src, "声明族全谱：7 种顶层声明", trace);

    ASSERT_EQ(unit.declarations.size(), 7);
    EXPECT_EQ(unit.declarations[0]->kind, NodeKind::GlobalVar);
    EXPECT_EQ(unit.declarations[1]->kind, NodeKind::Enum);
    EXPECT_EQ(unit.declarations[2]->kind, NodeKind::TypeAlias);
    EXPECT_EQ(unit.declarations[3]->kind, NodeKind::Namespace);
    EXPECT_EQ(unit.declarations[4]->kind, NodeKind::Class);
    EXPECT_EQ(unit.declarations[5]->kind, NodeKind::Template);
    EXPECT_EQ(unit.declarations[6]->kind, NodeKind::Function);

    // NamespaceDecl 内部再嵌一层声明（声明可嵌套，语句不可做顶层声明）
    auto ns = std::dynamic_pointer_cast<NamespaceDecl>(unit.declarations[3]);
    ASSERT_NE(ns, nullptr);
    ASSERT_EQ(ns->declarations.size(), 1);
    EXPECT_EQ(ns->declarations[0]->kind, NodeKind::Function);
}

// =============================================================================
// 2. 表达式族：叶子字面量 + 类型标签（[expr.prim.literal]）
// =============================================================================
TEST(AstExprFamily, LiteralLeaves) {
    std::string trace;
    auto unit = parseSrc(
        "int main() { return 42; }\n");
    auto stmts = bodyOf(unit);
    ASSERT_EQ(stmts.size(), 1);
    auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmts[0]);
    ASSERT_NE(ret, nullptr);
    auto lit = std::dynamic_pointer_cast<IntLiteralExpr>(ret->value);
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value, 42);
    EXPECT_EQ(lit->kind, NodeKind::IntLiteral);
    // Parser 阶段语义未分析：类型必须为空（职责分离）
    EXPECT_EQ(lit->resolvedType, nullptr);
}

// =============================================================================
// 3. 树形即优先级：1 + 2 * 3 的嵌套形状（括号不进树）
// =============================================================================
TEST(AstTreeShape, PrecedenceEncodedAsNesting) {
    std::string trace;
    auto unit = parseSrc("int main() { return 1 + 2 * 3; }\n");
    auto stmts = bodyOf(unit);
    auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmts[0]);
    auto add = std::dynamic_pointer_cast<BinaryExpr>(ret->value);
    ASSERT_NE(add, nullptr);
    EXPECT_EQ(add->op, BinaryOp::Add);

    // 左子树是叶子 1；右子树是更深一层的 Mul —— 嵌套即优先级
    EXPECT_NE(std::dynamic_pointer_cast<IntLiteralExpr>(add->left), nullptr);
    auto mul = std::dynamic_pointer_cast<BinaryExpr>(add->right);
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, BinaryOp::Mul);
    dumpWithExplanation("树形即优先级：1 + 2 * 3", "1 + 2 * 3", "",
                        [&] { dumpExprTree(add, "", true); });

    // (1 + 2) * 3：括号改变了形状，但树上没有任何括号节点（"抽象"的含义）
    auto unit2 = parseSrc("int main() { return (1 + 2) * 3; }\n");
    auto ret2 = std::dynamic_pointer_cast<ReturnStmt>(bodyOf(unit2)[0]);
    auto mul2 = std::dynamic_pointer_cast<BinaryExpr>(ret2->value);
    ASSERT_NE(mul2, nullptr);
    EXPECT_EQ(mul2->op, BinaryOp::Mul);                     // 根变成了 Mul
    auto add2 = std::dynamic_pointer_cast<BinaryExpr>(mul2->left);
    ASSERT_NE(add2, nullptr);
    EXPECT_EQ(add2->op, BinaryOp::Add);                     // Add 下沉为左子树
}

// =============================================================================
// 4. 语句族全谱：函数体即 BlockStmt，语句 = 控制流程（[stmt]）
// =============================================================================
TEST(AstStmtFamily, StatementKindSpectrum) {
    std::string trace;
    std::string src =
        "int main() {\n"
        "    int x = 5;\n"          // VarDecl
        "    x = x + 1;\n"          // Assign
        "    foo();\n"              // ExprStmt
        "    if (x > 3) { x = 0; } else { x = 1; }\n"  // If（含 else）
        "    while (x) { x = x - 1; }\n"               // While
        "    return x;\n"           // Return
        "}\n";
    auto unit = parseShow(src, "语句族全谱：函数体 = BlockStmt", trace);
    auto stmts = bodyOf(unit);
    ASSERT_EQ(stmts.size(), 6);
    EXPECT_EQ(stmts[0]->kind, NodeKind::VarDecl);
    EXPECT_EQ(stmts[1]->kind, NodeKind::Assign);
    EXPECT_EQ(stmts[2]->kind, NodeKind::ExprStmt);
    EXPECT_EQ(stmts[3]->kind, NodeKind::If);
    EXPECT_EQ(stmts[4]->kind, NodeKind::While);
    EXPECT_EQ(stmts[5]->kind, NodeKind::Return);

    // AssignStmt：target 与 value 都是表达式（语句包含表达式，反之不成立）
    auto assign = std::dynamic_pointer_cast<AssignStmt>(stmts[1]);
    ASSERT_NE(assign, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<VarExpr>(assign->target), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<BinaryExpr>(assign->value), nullptr);
}

// =============================================================================
// 5. If / While 的子树结构：condition + 分支（[stmt.select] / [stmt.iter]）
// =============================================================================
TEST(AstStmtFamily, IfElseAndWhileSubtrees) {
    auto unit = parseSrc(
        "int main() {\n"
        "    if (1 > 2) { return 1; } else { return 2; }\n"
        "    while (0) { return 3; }\n"
        "}\n");
    auto stmts = bodyOf(unit);

    auto ifs = std::dynamic_pointer_cast<IfStmt>(stmts[0]);
    ASSERT_NE(ifs, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<BinaryExpr>(ifs->condition), nullptr);
    EXPECT_EQ(ifs->thenBranch->kind, NodeKind::Block);
    ASSERT_NE(ifs->elseBranch, nullptr);                     // else 分支存在
    EXPECT_EQ(ifs->elseBranch->kind, NodeKind::Block);

    auto wh = std::dynamic_pointer_cast<WhileStmt>(stmts[1]);
    ASSERT_NE(wh, nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<IntLiteralExpr>(wh->condition), nullptr);
    EXPECT_EQ(wh->body->kind, NodeKind::Block);

    // 无 else 的 if：elseBranch 必须为空指针（不是空块）
    auto unit2 = parseSrc("int main() { if (1) { return 0; } return 1; }\n");
    auto ifs2 = std::dynamic_pointer_cast<IfStmt>(bodyOf(unit2)[0]);
    ASSERT_NE(ifs2, nullptr);
    EXPECT_EQ(ifs2->elseBranch, nullptr);
}

// =============================================================================
// 6. 调用与成员访问：CallExpr 的 callee 多态（[expr.call] / [expr.ref]）
// =============================================================================
TEST(AstExprFamily, CallCalleePolymorphism) {
    std::string src =
        "class Point {\n"
        "public:\n"
        "    int x;\n"
        "    int sum() { return x; }\n"
        "};\n"
        "int main() {\n"
        "    Point* p = new Point();\n"
        "    int r = p->sum();\n"
        "    delete p;\n"
        "    return r;\n"
        "}\n";
    std::string trace;
    auto unit = parseShow(src, "callee 多态：自由函数名 / 成员访问链", trace);
    auto stmts = bodyOf(unit);

    // new 表达式：NewExpr 挂在 VarDecl 的 initializer 上
    auto decl = std::dynamic_pointer_cast<VarDeclStmt>(stmts[0]);
    auto ne = std::dynamic_pointer_cast<NewExpr>(decl->initializer);
    ASSERT_NE(ne, nullptr);
    EXPECT_EQ(ne->className, "Point");

    // 方法调用：CallExpr.callee 是 MemberExpr（isArrow=true）
    auto callDecl = std::dynamic_pointer_cast<VarDeclStmt>(stmts[1]);
    auto call = std::dynamic_pointer_cast<CallExpr>(callDecl->initializer);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->arguments.size(), 0);
    auto mem = std::dynamic_pointer_cast<MemberExpr>(call->callee);
    ASSERT_NE(mem, nullptr);
    EXPECT_TRUE(mem->isArrow);
    EXPECT_EQ(mem->memberName, "sum");
    // object 又是 VarExpr(p)：链式结构 Member(Var(p))
    EXPECT_NE(std::dynamic_pointer_cast<VarExpr>(mem->object), nullptr);

    // delete 语句
    EXPECT_EQ(stmts[2]->kind, NodeKind::DeleteStmt);
}

// =============================================================================
// 7. 声明族细节：类成员结构与构造函数初始化列表（[class]/[class.base.init]）
// =============================================================================
TEST(AstDeclFamily, ClassAndConstructorStructure) {
    std::string src =
        "class Point {\n"
        "public:\n"
        "    int x;\n"
        "    int y;\n"
        "    Point(int px, int py) : x(px), y(py) {}\n"
        "};\n";
    std::string trace;
    auto unit = parseShow(src, "ClassDecl：字段 + 构造函数 + 初始化列表", trace);
    auto cls = std::dynamic_pointer_cast<ClassDecl>(unit.declarations[0]);
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->name, "Point");
    ASSERT_EQ(cls->fields.size(), 2);
    EXPECT_EQ(cls->fields[0].name, "x");
    EXPECT_EQ(cls->fields[1].name, "y");

    // 构造函数：ConstructorDecl 继承 FunctionDecl（构造是特殊的函数声明）
    ASSERT_EQ(cls->methods.size(), 1);
    auto ctor = std::dynamic_pointer_cast<ConstructorDecl>(cls->methods[0]);
    ASSERT_NE(ctor, nullptr);
    EXPECT_EQ(ctor->parameters.size(), 2);
    ASSERT_EQ(ctor->initList.size(), 2);
    EXPECT_EQ(ctor->initList[0].memberName, "x");
    ASSERT_EQ(ctor->initList[0].arguments.size(), 1);
    EXPECT_NE(std::dynamic_pointer_cast<VarExpr>(ctor->initList[0].arguments[0]),
              nullptr);
}

// =============================================================================
// 8. 模板蓝图冻结：classTemplate 与 funcTemplate 互斥（[temp]/两阶段查找第一阶段）
// =============================================================================
TEST(AstDeclFamily, TemplateBlueprintSlots) {
    std::string src =
        "template<typename T> class Box { T v; };\n"
        "template<typename T> T id(T v) { return v; }\n";
    std::string trace;
    auto unit = parseShow(src, "TemplateDecl 双槽位：类/函数模板互斥", trace);
    ASSERT_EQ(unit.declarations.size(), 2);

    auto ct = std::dynamic_pointer_cast<TemplateDecl>(unit.declarations[0]);
    ASSERT_NE(ct, nullptr);
    EXPECT_TRUE(ct->isClassTemplate());
    EXPECT_FALSE(ct->isFunctionTemplate());
    ASSERT_NE(ct->classTemplate, nullptr);
    EXPECT_EQ(ct->funcTemplate, nullptr);                    // 互斥：另一槽位为空
    EXPECT_EQ(ct->templateName(), "Box");
    ASSERT_EQ(ct->typeParams.size(), 1);
    EXPECT_EQ(ct->typeParams[0], "T");

    auto ft = std::dynamic_pointer_cast<TemplateDecl>(unit.declarations[1]);
    ASSERT_NE(ft, nullptr);
    EXPECT_TRUE(ft->isFunctionTemplate());
    EXPECT_FALSE(ft->isClassTemplate());
    ASSERT_NE(ft->funcTemplate, nullptr);
    EXPECT_EQ(ft->classTemplate, nullptr);
    EXPECT_EQ(ft->templateName(), "id");
    // 蓝图体内模板参数以占位类型存在（返回类型名字是 T）
    EXPECT_EQ(ft->funcTemplate->returnType->toString(), "T");
}

// =============================================================================
// 9. SourceLocation：节点携带源码位置（诊断定位的基础）
// =============================================================================
TEST(AstMetadata, SourceLocations) {
    auto unit = parseSrc(
        "int first() { return 1; }\n"     // 第 1 行
        "\n"
        "int second() { return 2; }\n");  // 第 3 行
    ASSERT_EQ(unit.declarations.size(), 2);
    EXPECT_EQ(unit.declarations[0]->location.line, 1);
    EXPECT_EQ(unit.declarations[1]->location.line, 3);
    // toString 输出 "行:列" 格式，直接拼进诊断消息
    EXPECT_EQ(unit.declarations[0]->location.toString(), "1:1");
}

// =============================================================================
// 10. 职责分离：语义分析前后，表达式的类型从空到实（[expr]：每表达式有类型）
// =============================================================================
TEST(AstMetadata, ResolvedTypeFilledBySema) {
    std::string src = "int main() { int x = 5; return x + 1; }\n";
    auto unit = parseSrc(src);

    auto stmts = bodyOf(unit);
    ASSERT_EQ(stmts.size(), 2);                                // [0]=VarDecl, [1]=Return
    auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmts[1]);
    ASSERT_NE(ret, nullptr);
    auto bin = std::dynamic_pointer_cast<BinaryExpr>(ret->value);
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->resolvedType, nullptr);                    // Parser 阶段：空

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));
    ASSERT_NE(bin->resolvedType, nullptr);                    // Sema 之后：填充
    EXPECT_TRUE(bin->resolvedType->isInt());

    // 叶子与中间节点一致地被标注
    ASSERT_NE(bin->left->resolvedType, nullptr);
    EXPECT_TRUE(bin->left->resolvedType->isInt());
}
