#pragma once
// =============================================================================
// 阶段 2：抽象语法树 (AST) 节点定义
// =============================================================================
// AST 是源代码的树状结构表示，是编译器各阶段之间传递信息的核心载体。
//
// 节点层次：
//   ASTNode（基类）
//   ├── Expression（表达式：有类型、有值）
//   │   ├── IntLiteralExpr      // 42
//   │   ├── BoolLiteralExpr     // true / false
//   │   ├── StringLiteralExpr   // "hello"
//   │   ├── NullptrLiteralExpr  // nullptr
//   │   ├── VarExpr             // 变量引用
//   │   ├── BinaryExpr          // a + b
//   │   ├── UnaryExpr           // -x, !x
//   │   ├── CallExpr            // foo(a, b)
//   │   ├── MemberExpr          // obj.field
//   │   ├── NewExpr             // new Foo()
//   │   └── ThisExpr            // this
//   ├── Statement（语句：无值，执行动作）
//   │   ├── ExprStmt            // 表达式语句
//   │   ├── VarDeclStmt         // 变量声明（含 auto）
//   │   ├── ReturnStmt          // return expr;
//   │   ├── IfStmt              // if / else
//   │   ├── WhileStmt           // while 循环
//   │   ├── BlockStmt           // { ... }
//   │   └── AssignStmt          // lhs = rhs
//   └── Declaration（顶层声明）
//       ├── FunctionDecl        // 函数声明
//       ├── ClassDecl           // 类声明
//       └── TemplateDecl        // 模板声明（蓝图）
// =============================================================================

#include "type.h"
#include "token.h"
#include <memory>
#include <string>
#include <vector>
#include <variant>

namespace minicc {

// ─── 前向声明与智能指针别名 ──────────────────────────────────────────────────
struct ASTNode;
struct Expression;
struct Statement;
struct Declaration;

using ASTNodePtr     = std::shared_ptr<ASTNode>;
using ExprPtr        = std::shared_ptr<Expression>;
using StmtPtr        = std::shared_ptr<Statement>;
using DeclPtr        = std::shared_ptr<Declaration>;

// ─────────────────────────────────────────────────────────────────────────────
// ASTNode 基类
// ─────────────────────────────────────────────────────────────────────────────
enum class NodeKind : uint8_t {
    // 表达式
    IntLiteral, BoolLiteral, StringLiteral, NullptrLiteral,
    Var, Binary, Unary, Call, Member, New, This,
    // 语句
    ExprStmt, VarDecl, Return, If, While, Block, Assign,
    // 声明
    Function, Class, Template,
};

struct ASTNode {
    NodeKind kind;
    SourceLocation location;
    virtual ~ASTNode() = default;

protected:
    explicit ASTNode(NodeKind k) : kind(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Expression：表达式基类（有类型信息）
// ─────────────────────────────────────────────────────────────────────────────
struct Expression : ASTNode {
    TypePtr resolvedType; // 语义分析阶段填充

    explicit Expression(NodeKind k) : ASTNode(k) {}
};

// ─── 字面量表达式 ─────────────────────────────────────────────────────────────

struct IntLiteralExpr : Expression {
    int64_t value;
    explicit IntLiteralExpr(int64_t v) : Expression(NodeKind::IntLiteral), value(v) {}
};

struct BoolLiteralExpr : Expression {
    bool value;
    explicit BoolLiteralExpr(bool v) : Expression(NodeKind::BoolLiteral), value(v) {}
};

struct StringLiteralExpr : Expression {
    std::string value;
    explicit StringLiteralExpr(std::string v)
        : Expression(NodeKind::StringLiteral), value(std::move(v)) {}
};

struct NullptrLiteralExpr : Expression {
    NullptrLiteralExpr() : Expression(NodeKind::NullptrLiteral) {}
};

// ─── 变量引用 ─────────────────────────────────────────────────────────────────
struct VarExpr : Expression {
    std::string name;
    explicit VarExpr(std::string n) : Expression(NodeKind::Var), name(std::move(n)) {}
};

// ─── 二元表达式 ───────────────────────────────────────────────────────────────
enum class BinaryOp : uint8_t {
    Add, Sub, Mul, Div, Mod,
    Eq, Neq, Lt, Gt, Le, Ge,
    And, Or,
};

struct BinaryExpr : Expression {
    BinaryOp op;
    ExprPtr  left;
    ExprPtr  right;
    BinaryExpr(BinaryOp o, ExprPtr l, ExprPtr r)
        : Expression(NodeKind::Binary), op(o),
          left(std::move(l)), right(std::move(r)) {}
};

// ─── 一元表达式 ───────────────────────────────────────────────────────────────
enum class UnaryOp : uint8_t {
    Neg,    // -x
    Not,    // !x
};

struct UnaryExpr : Expression {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr(UnaryOp o, ExprPtr e)
        : Expression(NodeKind::Unary), op(o), operand(std::move(e)) {}
};

// ─── 函数调用表达式 ───────────────────────────────────────────────────────────
struct CallExpr : Expression {
    ExprPtr              callee;     // 被调用的函数（可以是 VarExpr 或 MemberExpr）
    std::vector<ExprPtr> arguments;  // 实参列表
    CallExpr(ExprPtr c, std::vector<ExprPtr> args)
        : Expression(NodeKind::Call), callee(std::move(c)),
          arguments(std::move(args)) {}
};

// ─── 成员访问表达式 ───────────────────────────────────────────────────────────
struct MemberExpr : Expression {
    ExprPtr     object;      // obj.field 中的 obj
    std::string memberName;  // 字段名或方法名
    bool        isArrow;     // true 表示 ->, false 表示 .
    bool        isMethodCall = false; // 是否是方法调用（语义分析阶段确定）

    MemberExpr(ExprPtr obj, std::string member, bool arrow)
        : Expression(NodeKind::Member), object(std::move(obj)),
          memberName(std::move(member)), isArrow(arrow) {}
};

// ─── new 表达式 ───────────────────────────────────────────────────────────────
struct NewExpr : Expression {
    std::string            className;
    std::vector<ExprPtr>   constructorArgs;
    explicit NewExpr(std::string cls)
        : Expression(NodeKind::New), className(std::move(cls)) {}
};

// ─── this 表达式 ──────────────────────────────────────────────────────────────
struct ThisExpr : Expression {
    ThisExpr() : Expression(NodeKind::This) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Statement：语句基类
// ─────────────────────────────────────────────────────────────────────────────
struct Statement : ASTNode {
    explicit Statement(NodeKind k) : ASTNode(k) {}
};

// ─── 表达式语句 ───────────────────────────────────────────────────────────────
struct ExprStmt : Statement {
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : Statement(NodeKind::ExprStmt), expr(std::move(e)) {}
};

// ─── 变量声明语句（含 auto） ──────────────────────────────────────────────────
struct VarDeclStmt : Statement {
    std::string name;
    TypePtr     declaredType;    // 声明的类型（可能是 auto 占位符）
    ExprPtr     initializer;     // 初始化表达式（可为 nullptr）

    VarDeclStmt(std::string n, TypePtr t, ExprPtr init)
        : Statement(NodeKind::VarDecl), name(std::move(n)),
          declaredType(std::move(t)), initializer(std::move(init)) {}
};

// ─── 赋值语句 ─────────────────────────────────────────────────────────────────
struct AssignStmt : Statement {
    ExprPtr target;   // 赋值目标（变量或成员访问）
    ExprPtr value;    // 右值
    AssignStmt(ExprPtr t, ExprPtr v)
        : Statement(NodeKind::Assign), target(std::move(t)), value(std::move(v)) {}
};

// ─── return 语句 ──────────────────────────────────────────────────────────────
struct ReturnStmt : Statement {
    ExprPtr value; // 可为 nullptr（void 返回）
    explicit ReturnStmt(ExprPtr v = nullptr)
        : Statement(NodeKind::Return), value(std::move(v)) {}
};

// ─── if 语句 ──────────────────────────────────────────────────────────────────
struct IfStmt : Statement {
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch; // 可为 nullptr
    IfStmt(ExprPtr cond, StmtPtr then_, StmtPtr else_ = nullptr)
        : Statement(NodeKind::If), condition(std::move(cond)),
          thenBranch(std::move(then_)), elseBranch(std::move(else_)) {}
};

// ─── while 语句 ───────────────────────────────────────────────────────────────
struct WhileStmt : Statement {
    ExprPtr condition;
    StmtPtr body;
    WhileStmt(ExprPtr cond, StmtPtr b)
        : Statement(NodeKind::While), condition(std::move(cond)),
          body(std::move(b)) {}
};

// ─── 代码块语句 ───────────────────────────────────────────────────────────────
struct BlockStmt : Statement {
    std::vector<StmtPtr> statements;
    explicit BlockStmt(std::vector<StmtPtr> stmts = {})
        : Statement(NodeKind::Block), statements(std::move(stmts)) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Declaration：顶层声明基类
// ─────────────────────────────────────────────────────────────────────────────
struct Declaration : ASTNode {
    explicit Declaration(NodeKind k) : ASTNode(k) {}
};

// ─── 参数定义 ─────────────────────────────────────────────────────────────────
struct Parameter {
    std::string name;
    TypePtr     type;
};

// ─── 函数声明 ─────────────────────────────────────────────────────────────────
struct FunctionDecl : Declaration {
    std::string          name;
    TypePtr              returnType;
    std::vector<Parameter> parameters;
    std::shared_ptr<BlockStmt> body;      // nullptr = 纯声明（无实现）
    bool                 isVirtual  = false;
    bool                 isOverride = false;
    std::string          mangledName;     // 符号修饰后的名字（阶段4填充）
    std::string          ownerClassName;  // 所属类名（如果是成员函数）

    FunctionDecl() : Declaration(NodeKind::Function) {}
};
using FuncDeclPtr = std::shared_ptr<FunctionDecl>;

// ─── 类声明 ───────────────────────────────────────────────────────────────────
struct ClassDecl : Declaration {
    std::string              name;
    std::string              baseClassName;  // 基类名（空 = 无继承）
    std::vector<FieldInfo>   fields;         // 字段列表
    std::vector<FuncDeclPtr> methods;        // 方法列表
    TypePtr                  classType;      // 对应的 Type 对象
    AccessModifier           currentAccess = AccessModifier::Private;

    ClassDecl() : Declaration(NodeKind::Class) {}
};
using ClassDeclPtr = std::shared_ptr<ClassDecl>;

// ─── 模板声明（代码蓝图） ─────────────────────────────────────────────────────
// 阶段2（Parser）遇到 template<typename T> class MyPtr {...} 时，
// 将整个类声明"冻结"为一个蓝图，暂不解析内部语义。
// 等到阶段4（模板实例化）遇到 MyPtr<int> 时，才克隆并替换 T → int。
// ─────────────────────────────────────────────────────────────────────────────
struct TemplateDecl : Declaration {
    std::vector<std::string> typeParams;     // 模板参数名列表（如 ["T", "U"]）
    ClassDeclPtr             classTemplate;  // 模板类蓝图

    TemplateDecl() : Declaration(NodeKind::Template) {}
};
using TemplateDeclPtr = std::shared_ptr<TemplateDecl>;

// ─────────────────────────────────────────────────────────────────────────────
// TranslationUnit：编译单元的根节点
// ─────────────────────────────────────────────────────────────────────────────
struct TranslationUnit {
    std::vector<DeclPtr> declarations;  // 所有顶层声明
};

} // namespace minicc
