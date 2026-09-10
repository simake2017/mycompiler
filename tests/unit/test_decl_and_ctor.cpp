#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "codegen.h"

using namespace minicc;

static TranslationUnit parseCode(const std::string& src) {
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    return parser.parseTranslationUnit();
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. 全局变量声明测试
// ─────────────────────────────────────────────────────────────────────────────
TEST(Declarations, GlobalVariable) {
    std::string src = "int g_counter = 100;\n"
                      "int main() { return g_counter; }\n";
    auto unit = parseCode(src);
    ASSERT_EQ(unit.declarations.size(), 2);

    auto gvar = std::dynamic_pointer_cast<GlobalVarDecl>(unit.declarations[0]);
    ASSERT_NE(gvar, nullptr);
    EXPECT_EQ(gvar->name, "g_counter");
    EXPECT_TRUE(gvar->declaredType->isInt());

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));
    EXPECT_EQ(sema.getGlobalVars().size(), 1);

    CodeGen cg;
    std::string asmCode = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
    EXPECT_NE(asmCode.find(".globl g_counter"), std::string::npos);
    EXPECT_NE(asmCode.find("g_counter(%rip)"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 枚举声明测试
// ─────────────────────────────────────────────────────────────────────────────
TEST(Declarations, EnumAndEnumClass) {
    std::string src = "enum Color { Red = 1, Green, Blue };\n"
                      "enum class Status : int { Ok = 0, Fail = 2 };\n"
                      "int main() { int c = Red; return c; }\n";
    auto unit = parseCode(src);
    ASSERT_EQ(unit.declarations.size(), 3);

    auto enm1 = std::dynamic_pointer_cast<EnumDecl>(unit.declarations[0]);
    ASSERT_NE(enm1, nullptr);
    EXPECT_EQ(enm1->name, "Color");
    EXPECT_FALSE(enm1->isScoped);
    ASSERT_EQ(enm1->items.size(), 3);
    EXPECT_EQ(enm1->items[0].value, 1);
    EXPECT_EQ(enm1->items[1].value, 2);
    EXPECT_EQ(enm1->items[2].value, 3);

    auto enm2 = std::dynamic_pointer_cast<EnumDecl>(unit.declarations[1]);
    ASSERT_NE(enm2, nullptr);
    EXPECT_EQ(enm2->name, "Status");
    EXPECT_TRUE(enm2->isScoped);

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 命名空间声明测试
// ─────────────────────────────────────────────────────────────────────────────
TEST(Declarations, Namespace) {
    std::string src = "namespace Math {\n"
                      "  int add(int a, int b) { return a + b; }\n"
                      "  int g_base = 10;\n"
                      "}\n"
                      "int main() { return Math::add(1, 2); }\n";
    auto unit = parseCode(src);
    ASSERT_GE(unit.declarations.size(), 2);

    auto ns = std::dynamic_pointer_cast<NamespaceDecl>(unit.declarations[0]);
    ASSERT_NE(ns, nullptr);
    EXPECT_EQ(ns->name, "Math");
    EXPECT_EQ(ns->declarations.size(), 2);

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 类型别名测试（using 与 typedef）
// ─────────────────────────────────────────────────────────────────────────────
TEST(Declarations, TypeAlias) {
    std::string src = "using Integer = int;\n"
                      "typedef double Real;\n"
                      "int main() { Integer a = 5; return a; }\n";
    auto unit = parseCode(src);
    ASSERT_EQ(unit.declarations.size(), 3);

    auto ta1 = std::dynamic_pointer_cast<TypeAliasDecl>(unit.declarations[0]);
    ASSERT_NE(ta1, nullptr);
    EXPECT_EQ(ta1->aliasName, "Integer");
    EXPECT_TRUE(ta1->underlyingType->isInt());

    auto ta2 = std::dynamic_pointer_cast<TypeAliasDecl>(unit.declarations[1]);
    ASSERT_NE(ta2, nullptr);
    EXPECT_EQ(ta2->aliasName, "Real");
    EXPECT_TRUE(ta2->underlyingType->isDouble());

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 构造函数与初始化列表
// ─────────────────────────────────────────────────────────────────────────────
TEST(Constructor, InitListAndNew) {
    std::string src = "class Point {\n"
                      "public:\n"
                      "  int x;\n"
                      "  int y;\n"
                      "  Point(int px, int py) : x(px), y(py) {}\n"
                      "  int sum() { return x + y; }\n"
                      "};\n"
                      "int main() {\n"
                      "  Point* p = new Point(3, 4);\n"
                      "  return p->sum();\n"
                      "}\n";
    auto unit = parseCode(src);
    auto cls = std::dynamic_pointer_cast<ClassDecl>(unit.declarations[0]);
    ASSERT_NE(cls, nullptr);

    bool hasCtor = false;
    for (auto& m : cls->methods) {
        if (auto ctor = std::dynamic_pointer_cast<ConstructorDecl>(m)) {
            hasCtor = true;
            EXPECT_EQ(ctor->parameters.size(), 2);
            EXPECT_EQ(ctor->initList.size(), 2);
            EXPECT_EQ(ctor->initList[0].memberName, "x");
            EXPECT_EQ(ctor->initList[1].memberName, "y");
        }
    }
    EXPECT_TRUE(hasCtor);

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));

    CodeGen cg;
    std::string asmCode = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
    EXPECT_NE(asmCode.find("callq Point_Point"), std::string::npos);
    // codegen 对初始化列表字段发射中文注释行（全阶段中文日志规范）：
    //   movq %rax, N(%rcx)    # 初始化字段 x（偏移 N）
    EXPECT_NE(asmCode.find("初始化字段 x"), std::string::npos);
    EXPECT_NE(asmCode.find("初始化字段 y"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. 析构函数与虚析构多态 delete
// ─────────────────────────────────────────────────────────────────────────────
TEST(Destructor, VirtualDestructorAndDelete) {
    std::string src = "class Base {\n"
                      "public:\n"
                      "  virtual ~Base() {}\n"
                      "};\n"
                      "class Derived : public Base {\n"
                      "public:\n"
                      "  virtual ~Derived() {}\n"
                      "};\n"
                      "int main() {\n"
                      "  Base* p = new Derived();\n"
                      "  delete p;\n"
                      "  return 0;\n"
                      "}\n";
    auto unit = parseCode(src);
    ASSERT_EQ(unit.declarations.size(), 3);

    SemanticAnalyzer sema;
    EXPECT_NO_THROW(sema.analyze(unit));

    auto& classTypes = sema.getClassTypes();
    auto baseType = classTypes.at("Base");
    auto derivedType = classTypes.at("Derived");

    EXPECT_TRUE(baseType->classLayout.hasVTable);
    EXPECT_TRUE(derivedType->classLayout.hasVTable);

    CodeGen cg;
    std::string asmCode = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
    EXPECT_NE(asmCode.find("Base_dtor"), std::string::npos);
    EXPECT_NE(asmCode.find("Derived_dtor"), std::string::npos);
    EXPECT_NE(asmCode.find("callq free"), std::string::npos);
}
