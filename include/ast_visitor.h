// =============================================================================
// include/ast_visitor.h —— AST 访问者（Visitor 模式）
// =============================================================================
// 【为什么需要它】
//   在此之前，每个 AST 消费者都自己写一遍 if-else + dynamic_pointer_cast 链：
//
//       Sema: inferType        ← 14 级
//     CodeGen: emitExpr        ← 13 级
//     CodeGen: emitStmt        ←  8 级
//    main.cpp: dumpExpr        ← 14 级
//
//   四条链结构相同、都为"按节点动态类型分派"而存在，全项目共 147 处
//   dynamic_pointer_cast（改造完成后为 0，见 docs/learn/29）。毛病有二：
//     ① 性能上是 O(n) —— 一个 CallExpr 要试穿前面 7 个 cast 才轮到；
//     ② 结构上新增节点类型时，四条链都要手动补，漏一条不报错、只是静默走 else。
//
// 【Visitor 怎么解决】
//   把"按动态类型分派"这件事收进类型系统：
//     · 每个节点重写 accept(AstVisitor&)，函数体是 v.visit(*this)；
//     · accept 是虚函数 ⇒ 一次虚表跳转（O(1)）就落到正确的 visit 重载；
//     · 重载决议在【编译期】完成 —— *this 的静态类型就是节点自己，
//       所以 v.visit(*this) 绑定到精确的那个重载，全程不需要任何 cast。
//
// 【本文件为什么只有前置声明】
//   visit 的参数是【引用】，而引用参数只要求类型被声明、不要求被定义。
//   所以本文件可以只前置声明 32 个节点类型就把 AstVisitor 定义完整，
//   于是依赖是单向的、无环的：
//
//       ast.h  ──include──▶  ast_visitor.h        （节点实现 accept 需要访问者完整）
//       ast_visitor.h ──✗──▶ ast.h               （不需要，只有前置声明）
//
//   ★ 反面教训：如果把 accept 的【函数体】也挪到本文件（写成
//     `inline void IntLiteralExpr::accept(AstVisitor& v) { ... }`），
//     就会踩上 Itanium ABI 的【键函数】规则 ——
//     类里声明、类外定义的虚函数会成为该类的"键函数"，而 vtable 只在
//     定义键函数的那个 TU 里发射。本项目的 .cpp 大多只 include ast.h，
//     于是谁也没发射 vtable，链接期满屏 undefined reference to
//     `vtable for minicc::DeleteExpr`。
//     改回类内 inline 定义（隐含 inline ⇒ 不构成键函数）后，
//     vtable 在每个用到它的 TU 里以弱符号发射，问题消失。
//
// 【对照 clang】
//   clang 的 RecursiveASTVisitor 由 TableGen 从 StmtNodes.td / DeclNodes.td
//   自动生成，本文件是那份生成结果的手写版：
//     clang : bool VisitBinaryOperator(BinaryOperator *B) { return true; }
//     本项目: virtual void visit(BinaryExpr&) {}                    ← 同样给默认体
//   默认实现为空与 clang 同理：绝大多数访问者只关心少数节点类型，
//   强制 32 个全部实现会让每个消费者都塞满空函数。
//   （代价：新增节点类型不会强制各访问者补分支。但这与改造前的 if-else
//     链完全一致 —— 原来漏了也是静默走 else，没有变差。）
//
// 【怎么用】继承 AstVisitor，重写关心的 visit 重载，然后 node.accept(v)：
//
//     struct MyVisitor : AstVisitor {
//         void visit(BinaryExpr& e) override {
//             std::cout << "二元运算\n";
//             e.left->accept(*this);    // 想递归就自己往下走
//         }
//     };
//     MyVisitor v;
//     root->accept(v);
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
