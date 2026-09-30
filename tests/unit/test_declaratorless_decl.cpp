// =============================================================================
// tests/unit/test_declaratorless_decl.cpp —— 无声明符的声明 + 空语句（bug B16）
// =============================================================================
// 被测对象：src/parser.cpp 的 parseStatement / isDeclaratorlessDecl
//           include/ast.h 的 EmptyStmt（NodeKind::Empty）
//
// 考察理论点：
//   1. **[dcl.dcl]/1 的 init-declarator-list 是【可选】的**
//        simple-declaration := decl-specifier-seq init-declarator-list? ';'
//      故 `decl-specifier-seq ';'` 合法，语义是"什么都不声明"。
//      clang 为此专门有 `-Wmissing-declarations: declaration does not declare anything`。
//      ★ 修复前 minicc 把它当错误拒收 —— 与 B9 同属"错误拒绝合法程序"。
//
//   2. **[stmt]/1 的 null statement（`;`）** —— clang: NullStmt。
//      与 ① 语义相同（都不产生动作），故共用 EmptyStmt 一个节点。
//
//   3. ★ **两者都不实例化任何模板**。这是本条最容易做错的地方：看到
//      `A<int*, int**>;` 很自然会去 resolveType 从而触发偏特化实例化，
//      但 clang 不会 —— 用探针验证过：
//        template<class T> struct A<T,T*> { typename T::nope boom; };
//        int main() { A<int*, int**>; }    // clang: 只有 warning，boom 没被实例化
//        int main() { A<int*, int**> v; }  // clang: error: no type named 'nope'
//      本实现靠"EmptyStmt 不携带类型"从结构上保证这一点（Sema 无从 resolveType）。
//
//   4. **反向守卫**：`int*;` / `int* const;` 必须【继续报错】——
//      ptr-operator 后面必须有 declarator-id（clang: expected unqualified-id）。
//      若把守卫写成"类型后跟 ';' 就放行"，这两条会被静默接受。
//
// 观测风格与其它 unit 测试对齐（共享 obs_helpers.h：
//   StdoutCapture 吞日志 + dumpWithExplanation 可视化 [输入]→[trace]→[断言]）。
// =============================================================================
#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "ast.h"
#include "type.h"
#include "obs_helpers.h"

#include <sstream>
#include <string>
#include <vector>

using namespace minicc;

namespace {

// 辅助：解析整段源码（吞掉阶段日志）
TranslationUnit parseCode(const std::string& src) {
    Lexer lexer(src);
    Parser parser(lexer.tokenizeAll());
    StdoutCapture cap;
    return parser.parseTranslationUnit();
}

// 辅助：取 main 的语句体
std::vector<StmtPtr> mainBody(const TranslationUnit& unit) {
    for (auto& d : unit.declarations) {
        if (auto f = std::dynamic_pointer_cast<FunctionDecl>(d)) {
            if (f->name == "main") return f->body ? f->body->statements : std::vector<StmtPtr>{};
        }
    }
    return {};
}

// 辅助：断言第 i 条语句是 EmptyStmt，返回它（失败则返回 nullptr）
std::shared_ptr<EmptyStmt> asEmpty(const std::vector<StmtPtr>& stmts, size_t i) {
    if (i >= stmts.size()) { ADD_FAILURE() << "语句体只有 " << stmts.size() << " 条"; return nullptr; }
    auto e = std::dynamic_pointer_cast<EmptyStmt>(stmts[i]);
    EXPECT_NE(e, nullptr) << "第 " << i << " 条不是 EmptyStmt";
    return e;
}

// 辅助：跑完整 Sema，回传日志（用于断言"有没有发生实例化"）
std::string semaLog(const std::string& src) {
    auto unit = parseCode(src);
    SemanticAnalyzer sema;
    StdoutCapture cap;
    sema.analyze(unit);
    return cap.str();
}

// 辅助：预期解析失败（抛 std::runtime_error），回传错误文案
std::string expectParseError(const std::string& src) {
    std::string msg;
    try {
        parseCode(src);
    } catch (const std::runtime_error& e) {
        msg = e.what();
    }
    return msg;
}

} // namespace

// =============================================================================
// 1. [stmt]/1 null statement —— 裸分号
// =============================================================================
TEST(DeclaratorlessDecl, NullStatementIsEmptyStmt) {
    const std::string src = R"(
        int main() {
            ;;                       // 连续空语句
            if (1) ;                 // 单语句体位置的空语句
            while (0) ;
            return 0;
        }
    )";

    auto unit = parseCode(src);
    auto body = mainBody(unit);

    // 语句体：EmptyStmt, EmptyStmt, If, While, Return
    ASSERT_EQ(body.size(), 5u);
    asEmpty(body, 0);
    asEmpty(body, 1);

    // ★ 标签不变式（零 RTTI 转换的前提，同 test_ast_visitor）：kind 与实际类型一致
    for (auto& s : body) {
        if (s->kind == NodeKind::Empty) {
            EXPECT_NE(std::dynamic_pointer_cast<EmptyStmt>(s), nullptr);
        }
    }

    // 空语句是叶子：无子节点（对照 clang NullStmt 同样只有位置）
    dumpWithExplanation("空语句 `;` ⇒ EmptyStmt（[stmt]/1）", src, "", [&]{
        std::printf("main 语句体 %zu 条：EmptyStmt, EmptyStmt, If, While, Return\n", body.size());
    });
}

// =============================================================================
// 2. [dcl.dcl]/1 —— 类型关键字开头的无声明符声明
// =============================================================================
TEST(DeclaratorlessDecl, TypeKeywordWithoutDeclaratorIsAccepted) {
    // clang++-18 -std=c++20：三行都只给 -Wmissing-declarations 警告，rc=0
    const std::string src = R"(
        int main() {
            int;
            const int;
            unsigned int;
            return 0;
        }
    )";

    auto unit = parseCode(src);
    auto body = mainBody(unit);

    ASSERT_EQ(body.size(), 4u) << "三条声明 + return";
    asEmpty(body, 0);
    asEmpty(body, 1);
    asEmpty(body, 2);
}

// =============================================================================
// 3. ★ B16 正主：模板 id 开头的无声明符声明
// =============================================================================
// 修复前：前瞻跳完 `A` + `<...>` 后停在 ';'，check(Identifier) 不成立
//         ⇒ 回滚走表达式分支 ⇒ 把 `A` 当标识符 ⇒ 撞死在第 2 个实参的 `int` 上：
//           [Parse Error] 12:7 at 'int': Unexpected token 'int' in expression
//         报错位置指向实参中间，离根因（少了声明符）很远。
TEST(DeclaratorlessDecl, TemplateIdWithoutDeclaratorIsAccepted) {
    const std::string src = R"(
        template<class a, class b>
        struct A { };
        template<class T>
        struct A<T, T*> { };
        int main() {
            A<int*, int**>;              // ← 无声明符的声明
            return 0;
        }
    )";

    auto unit = parseCode(src);
    auto body = mainBody(unit);

    ASSERT_EQ(body.size(), 2u) << "一条空语句 + return";
    asEmpty(body, 0);
}

// 限定名同理：`Box<int>::type;` / `Plain::Int;`
TEST(DeclaratorlessDecl, QualifiedTypeIdWithoutDeclaratorIsAccepted) {
    const std::string src = R"(
        template<class T>
        struct Box { using type = T; };
        int main() {
            Box<int>::type;
            return 0;
        }
    )";

    auto unit = parseCode(src);
    auto body = mainBody(unit);

    ASSERT_EQ(body.size(), 2u);
    asEmpty(body, 0);
}

// =============================================================================
// 4. ★ 不实例化任何模板（与 clang 逐例核对过的行为）
// =============================================================================
TEST(DeclaratorlessDecl, DoesNotInstantiateAnyTemplate) {
    const std::string tmpl = R"(
        template<class a, class b>
        struct A { };
        template<class T>
        struct A<T, T*> { };
    )";

    // 对照组：写成【变量声明】⇒ 走偏特化匹配 ⇒ 实例化出 A<int*,int**>（符号 A_intP_intPP）
    {
        std::string log = semaLog(tmpl + "int main() { A<int*, int**> v; return 0; }");
        EXPECT_NE(log.find("A_intP_intPP"), std::string::npos)
            << "变量声明应当实例化（对照组，证明探针有效）";
    }

    // 被测：写成【无声明符的声明】⇒ 什么都不声明 ⇒ 零实例化
    // EmptyStmt 不携带类型，Sema 根本无从 resolveType —— 由结构保证，非靠约定。
    {
        std::string log = semaLog(tmpl + "int main() { A<int*, int**>; return 0; }");
        EXPECT_EQ(log.find("A_intP_intPP"), std::string::npos)
            << "无声明符的声明不得实例化模板（clang 同此）";
        EXPECT_EQ(log.find("[spec:select]"), std::string::npos)
            << "既没实例化，就不该走到偏特化三路择优";
    }
}

// =============================================================================
// 4b. 实例化的成员函数体内含空语句：cloneStmt 必须显式列 Empty 分支
// =============================================================================
// 突变验证：删掉 template_instantiation.cpp 的 `case NodeKind::Empty:`，
//           它会落到 default 返回 nullptr ⇒ 语句体里混进空指针节点 ⇒ 下面变红。
//           （只靠集成测试也能发现——test_basics_02 会编不过；这里从白盒钉住。）
TEST(DeclaratorlessDecl, EmptyStmtSurvivesInstantiationClone) {
    const std::string src = R"(
        template<typename T>
        struct Widget {
            T v;
            int probe() {
                ;
                int;
                return 7;
            }
        };
        int main() { Widget<int> w; return w.probe(); }
    )";

    auto unit = parseCode(src);
    SemanticAnalyzer sema;
    {
        StdoutCapture cap;
        sema.analyze(unit);
    }

    // 找到实例化出来的 Widget_int，检查 probe 的语句体
    auto& decls = sema.getClassDecls();
    auto it = decls.find("Widget_int");
    ASSERT_NE(it, decls.end()) << "Widget<int> 应已实例化";

    FuncDeclPtr probe;
    for (auto& m : it->second->methods) {
        if (m->name == "probe") { probe = m; break; }
    }
    ASSERT_NE(probe, nullptr) << "实例类应有 probe 方法";
    ASSERT_NE(probe->body, nullptr);

    // ★ 不变量 1：语句体里不得出现空指针（cloneStmt 落到 default 就会）
    for (size_t i = 0; i < probe->body->statements.size(); i++) {
        EXPECT_NE(probe->body->statements[i], nullptr)
            << "第 " << i << " 条语句是 nullptr —— cloneStmt 漏了 NodeKind::Empty";
    }

    // ★ 不变量 2：空语句必须原样保留，不得被吞掉（4 条：; / int; / return + ?）
    size_t emptyCount = 0;
    for (auto& s : probe->body->statements) {
        if (s && s->kind == NodeKind::Empty) emptyCount++;
    }
    EXPECT_EQ(emptyCount, 2u) << "体内两条空语句（`;` 与 `int;`）都要克隆出来";
    EXPECT_EQ(probe->body->statements.size(), 3u);
}

// =============================================================================
// 5. 反向守卫：ptr-operator 后面必须有名字，不能一并放行
// =============================================================================
// 突变验证：把 isDeclaratorlessDecl 里的 `!isPointer() && !isReference()` 删掉
//           （改成"类型后跟 ';' 就放行"），下面两条立刻变红。
TEST(DeclaratorlessDecl, PointerDeclaratorIsStillRejected) {
    // clang: error: declaration of 'int *' has no name
    EXPECT_FALSE(expectParseError("int main() { int*; return 0; }").empty())
        << "`int*;` 必须报错";

    // clang: error: expected unqualified-id（`* const` 是 ptr-operator 的一部分）
    EXPECT_FALSE(expectParseError("int main() { int* const; return 0; }").empty())
        << "`int* const;` 必须报错 —— 剥 const 后仍是指针";

    // clang: error: expected unqualified-id
    EXPECT_FALSE(expectParseError("int main() { int&; return 0; }").empty())
        << "`int&;` 必须报错";
}

// =============================================================================
// 6. 回归护栏：修复不能把正常声明带偏
// =============================================================================
TEST(DeclaratorlessDecl, OrdinaryDeclarationsStillWork) {
    const std::string src = R"(
        struct S { int x; };
        template<class T>
        struct Box { T v; };
        int main() {
            int a;
            const int b = 1;
            int* p;
            S s;
            S& r = s;
            Box<int> bx;
            ;                       // 空语句夹在中间
            return a + b;
        }
    )";

    auto unit = parseCode(src);
    auto body = mainBody(unit);

    ASSERT_EQ(body.size(), 8u);
    // 前 6 条都是 VarDeclStmt（不是 EmptyStmt！），第 7 条才是空语句
    for (size_t i = 0; i < 6; i++) {
        ASSERT_EQ(body[i]->kind, NodeKind::VarDecl) << "第 " << i << " 条应是变量声明";
    }
    asEmpty(body, 6);
    EXPECT_EQ(body[7]->kind, NodeKind::Return);

    // 对照：`S s;` 的声明符存在 ⇒ 不能被"无声明符"那段逻辑吃掉
    auto v = std::dynamic_pointer_cast<VarDeclStmt>(body[3]);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->name, "s");
}
