// =============================================================================
// tests/unit/test_ast_visitor.cpp —— AST 分派（访问者 + 标签）白盒单元测试
// =============================================================================
// 被测对象：include/ast_visitor.h（AstVisitor 基类）+ include/ast.h（accept 挂钩）
//
// 考察理论点（对应 docs/learn/29-ast-dispatch-two-idioms.md）：
//   1. **双重分派**：node->accept(v) 按【节点】类型分派，v.visit(*this) 按
//      【访问者】类型分派。重载决议在【编译期】就完成 —— `*this` 的静态类型
//      就是节点自身，所以 v.visit(*this) 精确绑定到那一个重载，**零 RTTI**。
//   2. **visit 默认空体**：与 clang `RecursiveASTVisitor` 的 `return true;` 同理，
//      只重写关心的重载即可，其余 31 个不报错、不强制实现。
//   3. **访问者不自动递归**：想往下走必须在 visit 里显式再 accept 子节点 ——
//      这正是 Sema / CodeGen 能把"遍历顺序"握在自己手里的原因
//      （比如赋值要先算右值再算目标地址）。
//   4. ★ **标签不变式**：`node->kind` 恒等于节点的实际类型。这是全项目
//      `static_pointer_cast` 能替代 `dynamic_pointer_cast` 的**唯一前提**
//      （与 LLVM `cast<>` 依赖 `classof()` 查 `getKind()` 同理）。
//      本测试用 RTTI 作 oracle 把它钉住：kind 与 dynamic_cast 结果必须一致。
//
// 观测风格与其它 unit 测试对齐（共享 obs_helpers.h）：
//   ① StdoutCapture 捕获 Parser 阶段日志；
//   ② dumpWithExplanation 可视化 box：[输入] → [trace] → [断言]
#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "ast_visitor.h"
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

// 辅助：取第 funcIdx 个函数的语句体
std::vector<StmtPtr> bodyOf(const TranslationUnit& unit, size_t funcIdx = 0) {
    size_t seen = 0;
    for (auto& d : unit.declarations) {
        if (auto f = std::dynamic_pointer_cast<FunctionDecl>(d)) {
            if (seen++ == funcIdx) {
                return f->body ? f->body->statements : std::vector<StmtPtr>{};
            }
        }
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试用访问者
// ─────────────────────────────────────────────────────────────────────────────

// ① 计数器：只重写关心的几个重载，其余走默认空体
struct Counter : AstVisitor {
    int intLiteral = 0;
    int binary = 0;
    int call = 0;
    int blockStmt = 0;

    void visit(IntLiteralExpr&) override { intLiteral++; }
    void visit(BinaryExpr& expr) override {
        binary++;
        expr.left->accept(*this);      // ★ 不自动递归 —— 这一行是"想要"才写
        expr.right->accept(*this);
    }
    void visit(CallExpr& expr) override {
        call++;
        expr.callee->accept(*this);
        for (auto& a : expr.arguments) a->accept(*this);
    }
    void visit(BlockStmt& block) override {
        blockStmt++;
        for (auto& s : block.statements) s->accept(*this);
    }
};

// ② 只重写一个重载的访问者：验证"默认空体"确实让别的节点无痛跳过
struct OnlyReturn : AstVisitor {
    int hits = 0;
    void visit(ReturnStmt&) override { hits++; }
};

// ─────────────────────────────────────────────────────────────────────────────
// 标签不变式审计：用 RTTI 作 oracle，逐节点核对 kind
// ─────────────────────────────────────────────────────────────────────────────
// 为什么值得单独测：全项目 147 处 dynamic_pointer_cast 已经全部换成
// `kind` 判断 + static_pointer_cast。static 转换是【零检查】的 ——
// 一旦某个节点的 kind 被设错，转换就静默产生未定义行为，而不是返回空。
// 所以"kind 与类型一致"是必须钉死的不变量。
int auditExpr(const ExprPtr& e);

int auditStmt(const StmtPtr& s) {
    if (!s) return 0;
    int n = 1;
    switch (s->kind) {
        case NodeKind::ExprStmt: {
            auto x = std::dynamic_pointer_cast<ExprStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->expr);
            break;
        }
        case NodeKind::VarDecl: {
            auto x = std::dynamic_pointer_cast<VarDeclStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->initializer);
            break;
        }
        case NodeKind::Assign: {
            auto x = std::dynamic_pointer_cast<AssignStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->target) + auditExpr(x->value);
            break;
        }
        case NodeKind::Return: {
            auto x = std::dynamic_pointer_cast<ReturnStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->value);
            break;
        }
        case NodeKind::If: {
            auto x = std::dynamic_pointer_cast<IfStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->condition) + auditStmt(x->thenBranch) + auditStmt(x->elseBranch);
            break;
        }
        case NodeKind::While: {
            auto x = std::dynamic_pointer_cast<WhileStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->condition) + auditStmt(x->body);
            break;
        }
        case NodeKind::Block: {
            auto x = std::dynamic_pointer_cast<BlockStmt>(s);
            EXPECT_NE(x, nullptr);
            for (auto& inner : x->statements) n += auditStmt(inner);
            break;
        }
        case NodeKind::DeleteStmt: {
            auto x = std::dynamic_pointer_cast<DeleteStmt>(s);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->pointerExpr);
            break;
        }
        default:
            ADD_FAILURE() << "未覆盖的语句 kind";
            break;
    }
    return n;
}

int auditExpr(const ExprPtr& e) {
    if (!e) return 0;
    int n = 1;
    switch (e->kind) {
        case NodeKind::IntLiteral:
            EXPECT_NE(std::dynamic_pointer_cast<IntLiteralExpr>(e), nullptr);
            break;
        case NodeKind::BoolLiteral:
            EXPECT_NE(std::dynamic_pointer_cast<BoolLiteralExpr>(e), nullptr);
            break;
        case NodeKind::NullptrLiteral:
            EXPECT_NE(std::dynamic_pointer_cast<NullptrLiteralExpr>(e), nullptr);
            break;
        case NodeKind::StringLiteral:
            EXPECT_NE(std::dynamic_pointer_cast<StringLiteralExpr>(e), nullptr);
            break;
        case NodeKind::Var:
            EXPECT_NE(std::dynamic_pointer_cast<VarExpr>(e), nullptr);
            break;
        case NodeKind::This:
            EXPECT_NE(std::dynamic_pointer_cast<ThisExpr>(e), nullptr);
            break;
        case NodeKind::Binary: {
            auto x = std::dynamic_pointer_cast<BinaryExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->left) + auditExpr(x->right);
            break;
        }
        case NodeKind::Unary: {
            auto x = std::dynamic_pointer_cast<UnaryExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->operand);
            break;
        }
        case NodeKind::Call: {
            auto x = std::dynamic_pointer_cast<CallExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->callee);
            for (auto& a : x->arguments) n += auditExpr(a);
            break;
        }
        case NodeKind::Member: {
            auto x = std::dynamic_pointer_cast<MemberExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->object);
            break;
        }
        case NodeKind::Index: {
            auto x = std::dynamic_pointer_cast<IndexExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->object) + auditExpr(x->index);
            break;
        }
        case NodeKind::New: {
            auto x = std::dynamic_pointer_cast<NewExpr>(e);
            EXPECT_NE(x, nullptr);
            for (auto& a : x->constructorArgs) n += auditExpr(a);
            break;
        }
        case NodeKind::DynamicCast: {
            auto x = std::dynamic_pointer_cast<DynamicCastExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->operand);
            break;
        }
        case NodeKind::Delete: {
            auto x = std::dynamic_pointer_cast<DeleteExpr>(e);
            EXPECT_NE(x, nullptr);
            n += auditExpr(x->pointerExpr);
            break;
        }
        default:
            ADD_FAILURE() << "未覆盖的表达式 kind";
            break;
    }
    return n;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. accept 精确路由到那一个 visit 重载
// ─────────────────────────────────────────────────────────────────────────────
TEST(AstVisitorDispatch, AcceptRoutesToExactOverload) {
    auto unit = parseSrc("int main() { return 1 + 2 * 3; }\n");
    auto stmts = bodyOf(unit);
    ASSERT_EQ(stmts.size(), 1);
    auto ret = std::static_pointer_cast<ReturnStmt>(stmts[0]);
    auto bin = std::static_pointer_cast<BinaryExpr>(ret->value);   // Add(1, Mul(2,3))

    // ① 第一次分派按【节点】类型：同一个 IntLiteral 只落到 visit(IntLiteralExpr&)
    Counter c;
    bin->left->accept(c);
    EXPECT_EQ(c.intLiteral, 1);
    EXPECT_EQ(c.binary, 0);        // 精确匹配 —— 不会"顺手"匹配到别的重载
    EXPECT_EQ(c.call, 0);
    EXPECT_EQ(c.blockStmt, 0);

    // ② 第二次分派按【访问者】类型：同一个节点换访问者，行为随之改变
    OnlyReturn v;
    ret->accept(v);
    EXPECT_EQ(v.hits, 1);          // ReturnStmt 送进 OnlyReturn ⇒ 命中
    bin->left->accept(v);
    EXPECT_EQ(v.hits, 1);          // 同一个节点送进 OnlyReturn ⇒ 默认空体，无变化
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. visit 默认空体：不关心的节点静默跳过，不报错
// ─────────────────────────────────────────────────────────────────────────────
TEST(AstVisitorDispatch, DefaultVisitsAreNoops) {
    // 函数体两条语句：If 语句 + 顶层 return
    auto unit = parseSrc("int main() { if (1) { return 2; } return 3; }\n");
    auto body = bodyOf(unit);
    ASSERT_EQ(body.size(), 2);
    EXPECT_EQ(body[0]->kind, NodeKind::If);

    OnlyReturn v;
    for (auto& s : body) s->accept(v);
    // ★ 只命中【顶层】那条 return：If 语句里的 return 2 不会被看到 ——
    //   访问者不自动递归，没写 accept 的分支就是静默不访问。
    EXPECT_EQ(v.hits, 1);

    // 显式下钻才看得到：If → BlockStmt → ReturnStmt，**每一层都要自己走**
    OnlyReturn v2;
    auto ifs = std::static_pointer_cast<IfStmt>(body[0]);
    ifs->thenBranch->accept(v2);                       // 只到 BlockStmt：默认空体 ⇒ 0
    EXPECT_EQ(v2.hits, 0);
    auto thenBlock = std::static_pointer_cast<BlockStmt>(ifs->thenBranch);
    for (auto& s : thenBlock->statements) s->accept(v2);   // 再走一层 ⇒ 命中
    EXPECT_EQ(v2.hits, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 递归由调用方驱动：自己写 accept 才往下走
// ─────────────────────────────────────────────────────────────────────────────
TEST(AstVisitorDispatch, RecursionIsCallerDriven) {
    std::string trace;
    auto unit = parseSrc("int main() { return 1 + 2 * 3; }\n");
    auto stmts = bodyOf(unit);
    ASSERT_EQ(stmts.size(), 1);
    auto ret = std::static_pointer_cast<ReturnStmt>(stmts[0]);

    dumpWithExplanation("访问者递归由调用方驱动：1 + 2 * 3", "1 + 2 * 3", trace, [&] {
        Counter c;
        ret->value->accept(c);        // 从根表达式进入
        std::printf("BinaryExpr 命中 %d 次，IntLiteral 命中 %d 次\n",
                    c.binary, c.intLiteral);
        // 树形：Add(1, Mul(2, 3)) ⇒ 2 个 Binary、3 个 IntLiteral
        EXPECT_EQ(c.binary, 2);
        EXPECT_EQ(c.intLiteral, 3);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. ★ 标签不变式：kind 恒等于真实类型（零 RTTI 转换的安全前提）
// ─────────────────────────────────────────────────────────────────────────────
TEST(AstVisitorDispatch, KindTagMatchesRuntimeType) {
    std::string trace;
    std::string src =
        "class Point { int x; Point() { x = 1; } };\n"
        "int f(int a) { return a; }\n"
        "int main() {\n"
        "    int a = 1;\n"
        "    if (a) { a = a + 2; } else { a = f(a); }\n"
        "    while (a) { a = a - 1; }\n"
        "    return a;\n"
        "}\n";
    auto unit = parseSrc(src);
    auto body = bodyOf(unit, 1);      // 第 1 个函数体（f 是第 0 个）
    ASSERT_FALSE(body.empty());

    dumpWithExplanation("标签不变式：逐节点核对 kind 与 dynamic_cast", src, trace, [&] {
        int total = 0;
        for (auto& s : body) total += auditStmt(s);
        std::printf("审计通过：kind 与 RTTI 类型逐一相符（共 %d 个语句/表达式节点）\n", total);
        EXPECT_GT(total, 10);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. ★ 代价面：忘记下钻 = 静默漏访问（访问者不自动递归的另一面）
// ─────────────────────────────────────────────────────────────────────────────
// 这条用例锁住的是"不自动递归"的**代价**，而它正是改造前的等价行为
// （旧代码漏一个 else-if 也是静默走 else），所以没有变差 —— 但值得知道。
TEST(AstVisitorDispatch, ForgettingToDrillSilentlySkipsSubtrees) {
    std::string trace;
    std::string src =
        "int f(int a) { return a; }\n"
        "int main() { int a = f(1) + f(2); return a; }\n";
    auto unit = parseSrc(src);
    auto body = bodyOf(unit, 1);      // main 的函数体

    dumpWithExplanation("忘记下钻 ⇒ 子树的节点一个也访问不到", src, trace, [&] {
        // ① 只重写【语句】重载：语句层看得见，表达式层下沉无人负责
        struct StmtOnly : AstVisitor {
            int varDecl = 0, ret = 0, call = 0;
            void visit(VarDeclStmt&) override { varDecl++; }   // 不碰 initializer
            void visit(ReturnStmt&) override { ret++; }
            void visit(CallExpr&) override { call++; }
        } only;
        for (auto& s : body) s->accept(only);
        std::printf("语句层: VarDecl=%d Return=%d  —— 但 CallExpr=%d（漏了）\n",
                    only.varDecl, only.ret, only.call);
        EXPECT_EQ(only.varDecl, 1);
        EXPECT_EQ(only.ret, 1);
        EXPECT_EQ(only.call, 0);      // ★ 两次 f(...) 调用一次都没被访问

        // ② 补上下沉那一行，同样两语句 ⇒ CallExpr 立即变成 2
        struct Drilled : AstVisitor {
            int call = 0;
            void visit(CallExpr& e) override {
                call++;
                e.callee->accept(*this);
                for (auto& a : e.arguments) a->accept(*this);
            }
            void visit(VarDeclStmt& s) override {
                if (s.initializer) s.initializer->accept(*this);
            }
            // ★ 少这一层也照样漏：initializer 是 BinaryExpr，不下钻就到底了
            void visit(BinaryExpr& e) override {
                e.left->accept(*this);
                e.right->accept(*this);
            }
        } full;
        for (auto& s : body) s->accept(full);
        std::printf("补上下沉: CallExpr=%d\n", full.call);
        EXPECT_EQ(full.call, 2);
    });
}
