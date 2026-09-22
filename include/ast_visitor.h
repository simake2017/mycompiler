// =============================================================================
// include/ast_visitor.h —— AST 访问者（Visitor 模式）—— 理论见 docs/learn/29
// =============================================================================
// 【旧写法的问题】改一处动态类型 ⇒ 四条 cast 链都要补：
//   Sema::inferType 14 级 / CodeGen::emitExpr 13 级 / emitStmt 8 级 / dumpExpr 14 级
//   漏一条不报错，只是静默走 else（O(n)：CallExpr 要试穿前面 7 个 cast）。
// 【新写法】每个节点一行，分派交给虚表：
//   BinaryExpr::accept(v) { v.visit(*this); }   // 虚表跳转 O(1)，重载编译期定
//   重载决议在【编译期】完成 —— *this 的静态类型就是节点自己，故绑定精确重载、无需 cast。
//
// 【为什么本文件只有前置声明】visit 参数是【引用】，只要求类型被声明、不要求被定义，故只
//   前置声明 32 个节点即可把 AstVisitor 定义完整，依赖单向无环：
//     ast.h ──include──▶ ast_visitor.h（节点实现 accept 需要访问者完整）｜反向 ──✗──▶ ast.h
//
// ★ 陷阱：accept 的【函数体】必须写在类内 inline —— 类里声明、类外定义的虚函数是该类的
//   【键函数】（Itanium ABI），vtable 只在定义它的 TU 发射 ⇒ 链接期满屏 undefined
//   reference to `vtable for minicc::DeleteExpr`。类内 inline 隐含 inline，不构成键函数。
//
// 对照 clang：RecursiveASTVisitor（TableGen 从 StmtNodes.td / DeclNodes.td 生成，本文件是
//   那份生成结果的手写版，同样给每个节点空默认体）：
//     clang : bool VisitBinaryOperator(BinaryOperator *B) { return true; }
//     本项目: virtual void visit(BinaryExpr&) {}
// 【怎么用】继承 AstVisitor、重写关心的 visit 重载，然后 node.accept(v)；想递归就在 visit
//   里对子节点再 accept（访问者只提供分派，不驱动遍历）。
// =============================================================================
#pragma once

namespace minicc {

// ── 节点前置声明（定义见 ast.h）──────────────────────────────────────────────
// 只声明不定义 —— 引用参数足够。这份清单必须与 ast.h 的 NodeKind 保持一致。
// 表达式
struct IntLiteralExpr;
struct BoolLiteralExpr;
struct StringLiteralExpr;
struct NullptrLiteralExpr;
struct VarExpr;
struct BinaryExpr;
struct UnaryExpr;
struct CallExpr;
struct MemberExpr;
struct IndexExpr;
struct NewExpr;
struct DynamicCastExpr;
struct ThisExpr;
struct DeleteExpr;
// 语句
struct ExprStmt;
struct VarDeclStmt;
struct AssignStmt;
struct ReturnStmt;
struct IfStmt;
struct WhileStmt;
struct BlockStmt;
struct DeleteStmt;
// 声明
struct FunctionDecl;
struct ConstructorDecl;
struct DestructorDecl;
struct GlobalVarDecl;
struct EnumDecl;
struct NamespaceDecl;
struct TypeAliasDecl;
struct ClassDecl;
struct DeductionGuideDecl;
struct TemplateDecl;

class AstVisitor {
public:
    virtual ~AstVisitor() = default;

    // ── 表达式（14）──────────────────────────────────────────────────────
    virtual void visit(IntLiteralExpr&) {}
    virtual void visit(BoolLiteralExpr&) {}
    virtual void visit(StringLiteralExpr&) {}
    virtual void visit(NullptrLiteralExpr&) {}
    virtual void visit(VarExpr&) {}
    virtual void visit(BinaryExpr&) {}
    virtual void visit(UnaryExpr&) {}
    virtual void visit(CallExpr&) {}
    virtual void visit(MemberExpr&) {}
    virtual void visit(IndexExpr&) {}
    virtual void visit(NewExpr&) {}
    virtual void visit(DynamicCastExpr&) {}
    virtual void visit(ThisExpr&) {}
    virtual void visit(DeleteExpr&) {}

    // ── 语句（8）────────────────────────────────────────────────────────
    virtual void visit(ExprStmt&) {}
    virtual void visit(VarDeclStmt&) {}
    virtual void visit(AssignStmt&) {}
    virtual void visit(ReturnStmt&) {}
    virtual void visit(IfStmt&) {}
    virtual void visit(WhileStmt&) {}
    virtual void visit(BlockStmt&) {}
    virtual void visit(DeleteStmt&) {}

    // ── 声明（10）───────────────────────────────────────────────────────
    virtual void visit(FunctionDecl&) {}
    virtual void visit(ConstructorDecl&) {}
    virtual void visit(DestructorDecl&) {}
    virtual void visit(GlobalVarDecl&) {}
    virtual void visit(EnumDecl&) {}
    virtual void visit(NamespaceDecl&) {}
    virtual void visit(TypeAliasDecl&) {}
    virtual void visit(ClassDecl&) {}
    virtual void visit(DeductionGuideDecl&) {}
    virtual void visit(TemplateDecl&) {}
};

} // namespace minicc
