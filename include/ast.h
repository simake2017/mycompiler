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
//
// 【在管线中的位置】
//   Preprocessor → Lexer → Parser（产出 AST）→ SemanticAnalyzer（填 resolvedType、
//   注册符号、算类布局）→ TemplateDeduction/Instantiation（克隆+替换 AST）
//   → CodeGen（消费 AST 出汇编）。AST 是 Parser 的输出，也是后续阶段的唯一输入。
//
// 【对应 C++ 标准章节】
//   [expr.*]     表达式（expr.prim.literal / expr.prim.id / expr.call /
//                expr.ref / expr.new / expr.prim.this / expr.unary / expr.add 等）
//   [stmt.*]     语句（stmt.expr / stmt.dcl / stmt.return / stmt.select / stmt.iter）
//   [dcl.fct]    函数声明与参数
//   [class] / [class.derived] / [class.virtual]   类、继承、虚函数
//   [temp]       模板（TemplateDecl 蓝图）
//
// 【对应 clang 模块】
//   clang 把声明与语句/表达式分在两棵树上：Decl（include/clang/AST/Decl*.h）
//   与 Stmt/Expr（include/clang/AST/Stmt*.h、Expr*.h）。本文件用单一 ASTNode 基类
//   统一三者，便于教学遍历与打印。
//     ASTNode         ≈ clang::Decl 与 clang::Stmt 的公共概念
//     Expression      ≈ clang::Expr（Expr.h）
//     Statement       ≈ clang::Stmt（Stmt.h）
//     Declaration     ≈ clang::Decl（Decl.h）
//     TranslationUnit ≈ clang::TranslationUnitDecl
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
// 所有节点的公共基类。NodeKind 是"运行时类型标签"，让消费者不必 dynamic_cast
// 就能快速分派（教学上等价于 clang 的 Stmt::StmtClass / Decl::Kind 枚举）。
// location 记录源码位置（token.h 的 SourceLocation），供报错定位。
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
// 对应 clang::Expr。每个表达式经语义分析后都有唯一类型 resolvedType
// （[expr]：每个表达式都有类型）。Parser 阶段 resolvedType 为空，阶段 3 填充。
struct Expression : ASTNode {
    TypePtr resolvedType; // 语义分析阶段填充

    explicit Expression(NodeKind k) : ASTNode(k) {}
};

// ─── 字面量表达式 ─────────────────────────────────────────────────────────────
// 对应 [expr.prim.literal]；clang: IntegerLiteral / CXXBoolLiteralExpr /
// StringLiteral / CXXNullPtrLiteralExpr。它们是 AST 的"叶子"，无子节点。
// demo：return 42;      → ReturnStmt[ IntLiteralExpr{value=42} ]
//       return true;    → ReturnStmt[ BoolLiteralExpr{value=true} ]
//       return "hi";    → ReturnStmt[ StringLiteralExpr{value="hi"} ]
//       return nullptr; → ReturnStmt[ NullptrLiteralExpr ]

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
// 对应 [expr.prim.id]（未限定名查找）；clang: DeclRefExpr。
// demo：return a;   → ReturnStmt[ VarExpr{name="a"} ]
// 语义阶段把 VarExpr 解析到符号表中的具体声明，并填 resolvedType。
struct VarExpr : Expression {
    std::string name;
    // 显式模板实参（S3）：foo<int>(x) 中的 <int>。
    // Parser 在标识符后识别 template-id 时填充；语义阶段据此做显式+推导混合。
    // 挂在 VarExpr 上是因为 template-id 出现在调用的 '(' 之前。
    std::vector<TypePtr> explicitTemplateArgs;
    explicit VarExpr(std::string n) : Expression(NodeKind::Var), name(std::move(n)) {}
};

// ─── 二元表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.add]/[expr.mul]/[expr.rel]/[expr.eq]/[expr.log.and] 等；
// clang: BinaryOperator。BinaryOp 把运算符抽象掉，树形与具体运算符无关。
enum class BinaryOp : uint8_t {
    Add, Sub, Mul, Div, Mod,
    Eq, Neq, Lt, Gt, Le, Ge,
    And, Or,
};

// demo：int c = a + b;
//   VarDeclStmt{ name=c, initializer = BinaryExpr{ op=Add,
//        left = VarExpr{a}, right = VarExpr{b} } }
//   ASCII:     BinaryExpr(+)
//              ├─ left  → VarExpr(a)
//              └─ right → VarExpr(b)
struct BinaryExpr : Expression {
    BinaryOp op;
    ExprPtr  left;
    ExprPtr  right;
    BinaryExpr(BinaryOp o, ExprPtr l, ExprPtr r)
        : Expression(NodeKind::Binary), op(o),
          left(std::move(l)), right(std::move(r)) {}
};

// ─── 一元表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.unary.op]；clang: UnaryOperator。本项目仅实现 Neg(-) 与 Not(!)。
enum class UnaryOp : uint8_t {
    Neg,    // -x
    Not,    // !x
};

// demo：return -x;  → ReturnStmt[ UnaryExpr{ op=Neg, operand=VarExpr{x} } ]
struct UnaryExpr : Expression {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr(UnaryOp o, ExprPtr e)
        : Expression(NodeKind::Unary), op(o), operand(std::move(e)) {}
};

// ─── 函数调用表达式 ───────────────────────────────────────────────────────────
// 对应 [expr.call]；clang: CallExpr。callee 可以是 VarExpr（自由函数）
// 或 MemberExpr（成员方法调用）。
// demo：foo(a, b)  → CallExpr{ callee=VarExpr{foo},
//                              arguments=[ VarExpr{a}, VarExpr{b} ] }
// 函数模板调用 twice(21) 也是 CallExpr；实参推导（S2）用 arguments 的类型驱动。
struct CallExpr : Expression {
    ExprPtr              callee;     // 被调用的函数（可以是 VarExpr 或 MemberExpr）
    std::vector<ExprPtr> arguments;  // 实参列表
    CallExpr(ExprPtr c, std::vector<ExprPtr> args)
        : Expression(NodeKind::Call), callee(std::move(c)),
          arguments(std::move(args)) {}
};

// ─── 成员访问表达式 ───────────────────────────────────────────────────────────
// 对应 [expr.ref]（类成员访问）；clang: MemberExpr。
// demo：obj.field   → MemberExpr{ object=VarExpr{obj}, memberName="field", isArrow=false }
//       ptr->field  → MemberExpr{ object=VarExpr{ptr}, memberName="field", isArrow=true }
// isArrow=true 时语义分析先对指针解引用（取其 pointee 的类布局再查 memberName）。
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
// 对应 [expr.new]；clang: CXXNewExpr。
// demo：new Point()      → NewExpr{ className="Point", constructorArgs=[] }
//       new Point(a, b)  → NewExpr{ className="Point", constructorArgs=[a, b] }
// 结果类型是 Point*（指针）；CodeGen 生成 operator new 调用 + 构造函数调用。
struct NewExpr : Expression {
    std::string            className;
    std::vector<ExprPtr>   constructorArgs;
    explicit NewExpr(std::string cls)
        : Expression(NodeKind::New), className(std::move(cls)) {}
};

// ─── this 表达式 ──────────────────────────────────────────────────────────────
// 对应 [expr.prim.this]；clang: CXXThisExpr。
// demo：return this;   → ReturnStmt[ ThisExpr ]
// 仅出现在成员函数体内；类型是"指向所属类的指针"（[class.this]）。
struct ThisExpr : Expression {
    ThisExpr() : Expression(NodeKind::This) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Statement：语句基类
// ─────────────────────────────────────────────────────────────────────────────
// 对应 clang::Stmt。语句没有值、只描述执行动作（区别于带 resolvedType 的表达式）。
struct Statement : ASTNode {
    explicit Statement(NodeKind k) : ASTNode(k) {}
};

// ─── 表达式语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.expr]。clang 中表达式本身即可充当语句，无独立 ExprStmt 节点；
// 教学实现单独建模，便于区分"表达式"与"以分号结尾的语句"。
// demo：foo();   → ExprStmt[ CallExpr{callee=VarExpr{foo}, arguments=[]} ]
struct ExprStmt : Statement {
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : Statement(NodeKind::ExprStmt), expr(std::move(e)) {}
};

// ─── 变量声明语句（含 auto） ──────────────────────────────────────────────────
// 对应 [dcl.stmt]/[dcl.init]；clang: VarDecl + DeclStmt。
// demo：int x = 5;      → VarDeclStmt{ name=x, declaredType=int, initializer=IntLiteral{5} }
//       auto y = foo(); → VarDeclStmt{ name=y, declaredType=<auto 占位>, initializer=CallExpr }
// auto 的 declaredType 是 Type::makeAuto()，语义阶段用 initializer 的类型回填
// （[dcl.spec.auto]）。
struct VarDeclStmt : Statement {
    std::string name;
    TypePtr     declaredType;    // 声明的类型（可能是 auto 占位符）
    ExprPtr     initializer;     // 初始化表达式（可为 nullptr）

    VarDeclStmt(std::string n, TypePtr t, ExprPtr init)
        : Statement(NodeKind::VarDecl), name(std::move(n)),
          declaredType(std::move(t)), initializer(std::move(init)) {}
};

// ─── 赋值语句 ─────────────────────────────────────────────────────────────────
// 对应 [expr.ass]；clang 用 BinaryOperator(BO_Assign) 表示，教学实现单独建 AssignStmt。
// demo：x = 5;      → AssignStmt{ target=VarExpr{x}, value=IntLiteral{5} }
//       obj.f = 1;  → AssignStmt{ target=MemberExpr{obj.f}, value=IntLiteral{1} }
struct AssignStmt : Statement {
    ExprPtr target;   // 赋值目标（变量或成员访问）
    ExprPtr value;    // 右值
    AssignStmt(ExprPtr t, ExprPtr v)
        : Statement(NodeKind::Assign), target(std::move(t)), value(std::move(v)) {}
};

// ─── return 语句 ──────────────────────────────────────────────────────────────
// 对应 [stmt.return]；clang: ReturnStmt。
// demo：return a;   → ReturnStmt{ value=VarExpr{a} }
//       return;     → ReturnStmt{ value=nullptr }  （void 返回）
struct ReturnStmt : Statement {
    ExprPtr value; // 可为 nullptr（void 返回）
    explicit ReturnStmt(ExprPtr v = nullptr)
        : Statement(NodeKind::Return), value(std::move(v)) {}
};

// ─── if 语句 ──────────────────────────────────────────────────────────────────
// 对应 [stmt.select]/[stmt.if]；clang: IfStmt。
// demo：if (a) { b; } else { c; }
//   → IfStmt{ condition=VarExpr{a},
//             thenBranch=BlockStmt[ ExprStmt{b} ],
//             elseBranch=BlockStmt[ ExprStmt{c} ] }
//   ASCII:     IfStmt
//              ├─ condition  → VarExpr(a)
//              ├─ thenBranch → BlockStmt
//              └─ elseBranch → BlockStmt（可为 nullptr = 无 else）
struct IfStmt : Statement {
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch; // 可为 nullptr
    IfStmt(ExprPtr cond, StmtPtr then_, StmtPtr else_ = nullptr)
        : Statement(NodeKind::If), condition(std::move(cond)),
          thenBranch(std::move(then_)), elseBranch(std::move(else_)) {}
};

// ─── while 语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.iter]/[stmt.while]；clang: WhileStmt。
// demo：while (i) { i = i - 1; }
//   → WhileStmt{ condition=VarExpr{i},
//                body=BlockStmt[ AssignStmt{ target=VarExpr{i},
//                        value=BinaryExpr{Sub, VarExpr{i}, IntLiteral{1}} } ] }
struct WhileStmt : Statement {
    ExprPtr condition;
    StmtPtr body;
    WhileStmt(ExprPtr cond, StmtPtr b)
        : Statement(NodeKind::While), condition(std::move(cond)),
          body(std::move(b)) {}
};

// ─── 代码块语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.block]/[stmt.dcl]；clang: CompoundStmt。函数体就是一个 BlockStmt。
// demo：{ int x = 1; return x; }
//   → BlockStmt{ statements=[ VarDeclStmt{x}, ReturnStmt{VarExpr{x}} ] }
struct BlockStmt : Statement {
    std::vector<StmtPtr> statements;
    explicit BlockStmt(std::vector<StmtPtr> stmts = {})
        : Statement(NodeKind::Block), statements(std::move(stmts)) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Declaration：顶层声明基类
// ─────────────────────────────────────────────────────────────────────────────
// 对应 clang::Decl。声明引入名字（函数/类/模板），是符号表的构成单元。
struct Declaration : ASTNode {
    explicit Declaration(NodeKind k) : ASTNode(k) {}
};

// ─── 参数定义 ─────────────────────────────────────────────────────────────────
// 对应 [dcl.fct] 的函数参数；clang: ParmVarDecl。
// demo：int f(int a) 的参数列表 → [ Parameter{name="a", type=int} ]
struct Parameter {
    std::string name;
    TypePtr     type;
};

// ─── 函数声明 ─────────────────────────────────────────────────────────────────
// 对应 [dcl.fct]/[dcl.fct.def]；clang: FunctionDecl（成员函数为 CXXMethodDecl）。
// demo：int f(int a) { return a; }
//   → FunctionDecl{ name=f, returnType=int,
//                   parameters=[ Parameter{a:int} ],
//                   body=BlockStmt[ ReturnStmt[ VarExpr{a} ] ] }
//   ASCII:     FunctionDecl(f)
//              ├─ returnType → int
//              ├─ parameters → [ a : int ]
//              └─ body → BlockStmt
//                          └─ ReturnStmt
//                              └─ VarExpr(a)
// isVirtual/isOverride 用于类的虚函数（[class.virtual]）；mangledName 由阶段4填充。
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
// 对应 [class]/[class.mem]；clang: CXXRecordDecl。继承见 [class.derived]。
// demo：class Point : public Base { int x; int foo() { ... } };
//   → ClassDecl{ name=Point, baseClassName="Base",
//                fields=[ FieldInfo{x:int} ],
//                methods=[ FunctionDecl{foo} ],
//                classType=<Type: Class Point> }
// 语义阶段把 ClassDecl 翻译成 Type 的 ClassLayout（算偏移 / 建 vtable）。
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
// 阶段2（Parser）遇到 template<typename T> class MyPtr {...} 或
// template<typename T> T twice(T x) {...} 时，将整个声明"冻结"为蓝图，
// 暂不解析内部语义。
// 等到阶段4（模板实例化）遇到 MyPtr<int>（类模板），或遇到 twice(21) 这样的
// 调用（函数模板，由 S2+ 实参推导驱动）时，才克隆并替换 T → int。
//
// classTemplate 与 funcTemplate 互斥（S1+）：
// 一个 TemplateDecl 要么是类模板，要么是函数模板。
// 对照 clang：ClassTemplateDecl 和 FunctionTemplateDecl 都继承自 TemplateDecl，
// 这里用"双槽位 + 判别方法"代替继承，保持 AST 扁平、便于教学。
//
// demo：template<typename T> T twice(T x) { return x + x; }
//   → TemplateDecl{ typeParams=["T"], classTemplate=nullptr,
//                   funcTemplate=FunctionDecl{ name=twice,
//                       returnType=<TemplateParam T>,
//                       parameters=[ Parameter{x, <TemplateParam T>} ],
//                       body=BlockStmt[ ReturnStmt[ BinaryExpr{Add,
//                           VarExpr{x}, VarExpr{x}} ] ] } }
//   （T 以 TemplateParam 占位类型存在；实例化时才被替换为实际类型）
// ─────────────────────────────────────────────────────────────────────────────
struct TemplateDecl : Declaration {
    std::vector<std::string> typeParams;     // 模板参数名列表（如 ["T", "U"]）
    ClassDeclPtr             classTemplate;  // 类模板蓝图（与 funcTemplate 互斥）
    FuncDeclPtr              funcTemplate;   // 函数模板蓝图（S1+）

    TemplateDecl() : Declaration(NodeKind::Template) {}

    bool isClassTemplate()    const { return classTemplate != nullptr; }
    bool isFunctionTemplate() const { return funcTemplate != nullptr; }
    // 模板的主名（类名或函数名）
    const std::string& templateName() const {
        return isClassTemplate() ? classTemplate->name : funcTemplate->name;
    }
};
using TemplateDeclPtr = std::shared_ptr<TemplateDecl>;

// ─────────────────────────────────────────────────────────────────────────────
// TranslationUnit：编译单元的根节点
// ─────────────────────────────────────────────────────────────────────────────
// 对应 clang::TranslationUnitDecl。Parser 的最终产物，CodeGen 从这里开始遍历。
// demo：一个含 1 函数 + 1 类的源文件
//   → TranslationUnit{ declarations=[ FunctionDecl, ClassDecl ] }
struct TranslationUnit {
    std::vector<DeclPtr> declarations;  // 所有顶层声明
};

} // namespace minicc
