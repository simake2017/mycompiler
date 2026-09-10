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
    Var, Binary, Unary, Call, Member, New, This, Delete, DynamicCast, Index,
    // 语句
    ExprStmt, VarDecl, Return, If, While, Block, Assign, DeleteStmt,
    // 声明
    Function, Class, Template, GlobalVar, Enum, Namespace, TypeAlias,
    Constructor, Destructor,
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
// 【核心特征】：表达式的核心在于“计算”和“求值”。它一定有确定的类型（resolvedType），并且一定能计算出一个具体的值。
// 【与 Statement 的区别】：表达式可以作为另一个表达式的一部分（如 a + (b * c)），而语句不行。表达式本身一般不单独存在，除非被包装成 ExprStmt（如 foo();）。
// 【与 Declaration 的区别】：表达式不向符号表引入新名字，只是读取已有的名字或计算新的值。
// 【代码示例】：
//    - `42`           （IntLiteralExpr，类型 int，值 42）
//    - `a + b`        （BinaryExpr，类型由 a 和 b 决定，计算它们的和）
//    - `foo(x, y)`    （CallExpr，类型为 foo 的返回值类型，值为函数的执行结果）
//
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

// ─── 下标访问表达式（语法糖，降级为 at()/set() 调用）────────────────────────
// 对应 [expr.sub]（下标运算符）；clang: ArraySubscriptExpr /
// CXXOperatorCallExpr（operator[] 重载形式）。
// demo：v[i]       → IndexExpr{ object=VarExpr{v}, index=VarExpr{i} }
// 设计（教学版 operator[] 的"糖化"）：
//   真 C++ 里 v[i] 是 operator[] 重载调用；minicc 尚无运算符重载，
//   于是把下标语法直接降级为两个约定方法：
//     右值位置（读）：int x = v[i];   ≡  int x = v.at(i);
//     左值位置（写）：v[i] = x;       ≡  v.set(i, x);
//   类只要实现 at(int)/set(int,int) 两个成员方法，就自动获得 [] 手感
//   （Vector<T>/Map<K,V> 封装即建立在此约定上）。
// 语义阶段：inferIndex 校验 object 是类类型且类里有 at() 方法；
//           结果类型 = at() 的返回类型。
// 代码生成：emitExpr(IndexExpr) → 发射 this + 实参、callq <类名>_at；
//           emitAssign 见 AssignStmt.target 为 IndexExpr 时改发 <类名>_set。
struct IndexExpr : Expression {
    ExprPtr object;  // 被下标的容器对象（类类型）
    ExprPtr index;   // 下标表达式（按约定方法签名校验）
    IndexExpr(ExprPtr obj, ExprPtr idx)
        : Expression(NodeKind::Index),
          object(std::move(obj)), index(std::move(idx)) {}
};

// ─── new 表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.new]；clang: CXXNewExpr。
// demo：new Point()      → NewExpr{ className="Point", constructorArgs=[] }
//       new Point(a, b)  → NewExpr{ className="Point", constructorArgs=[a, b] }
// 结果类型是 Point*（指针）；CodeGen 生成 operator new 调用 + 构造函数调用。
struct NewExpr : Expression {
    std::string            className;
    std::vector<ExprPtr>   constructorArgs;
    // ★ wangyang: P3 —— new Box<int>() 的模板实参（Parser 填写，
    // 语义阶段实例化后把 className 改写为实例名，CodeGen 只看改写后的名字）
    std::vector<TypePtr>   templateArgs;
    explicit NewExpr(std::string cls)
        : Expression(NodeKind::New), className(std::move(cls)) {}
};

// ─── dynamic_cast 表达式 ─────────────────────────────────────────────────────
// 对应 [expr.dynamic.cast]；clang: CXXDynamicCastExpr。
// demo：dynamic_cast<Derived*>(p)
//   → DynamicCastExpr{ targetClassName="Derived", operand=VarExpr{p} }
// 目标类型必须是"类名*"（本项目只实现指针形式）；结果类型是 Derived*。
// 运行时借助 RTTI（typeinfo 基类链）判断实际对象类型能否到达目标类型：
// 成功 → 返回原指针；失败 → 返回 0（对应真实 C++ 的空指针）。
// targetType 由语义阶段在检查通过后填充（目标类的指针类型）。
struct DynamicCastExpr : Expression {
    std::string targetClassName;  // dynamic_cast<这里>中的类名
    ExprPtr     operand;          // 被转换的表达式
    DynamicCastExpr(std::string cls, ExprPtr op)
        : Expression(NodeKind::DynamicCast),
          targetClassName(std::move(cls)), operand(std::move(op)) {}
};

// ─── this 表达式 ──────────────────────────────────────────────────────────────
// 对应 [expr.prim.this]；clang: CXXThisExpr。
// demo：return this;   → ReturnStmt[ ThisExpr ]
// 仅出现在成员函数体内；类型是"指向所属类的指针"（[class.this]）。
struct ThisExpr : Expression {
    ThisExpr() : Expression(NodeKind::This) {}
};

// ─── delete 表达式 ────────────────────────────────────────────────────────────
// 对应 [expr.delete]；clang: CXXDeleteExpr。
// demo：delete ptr;  → DeleteExpr{ pointerExpr=VarExpr{ptr}, isArray=false }
// 先调用析构函数（如果是类类型指针），再调用 free 释放堆内存。
struct DeleteExpr : Expression {
    ExprPtr pointerExpr;
    bool    isArray = false; // 是否为 delete[]
    explicit DeleteExpr(ExprPtr p, bool isArr = false)
        : Expression(NodeKind::Delete), pointerExpr(std::move(p)), isArray(isArr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Statement：语句基类
// ─────────────────────────────────────────────────────────────────────────────
// 【核心特征】：语句的核心在于“控制流程”和“执行动作”。它没有类型，也不会计算出一个可被后续引用的值。
// 【与 Expression 的区别】：语句是一条完整的指令，控制程序怎么跑（循环、条件、返回），而表达式只是指令里算数的部分。你不能写 `int x = if (a) { 1; };`，因为 `if` 是语句没有值。
// 【与 Declaration 的区别】：语句执行具体的运行时逻辑，而声明侧重于向编译器报告“这里有个什么东西”。虽然变量声明（VarDeclStmt）在 C++ 中算作语句，但大部分纯声明（如类、函数蓝图）不是。
// 【代码示例】：
//    - `return 0;`         （ReturnStmt，动作是退出函数）
//    - `if (flag) { ... }` （IfStmt，动作是分支跳转）
//    - `x = 5;`            （AssignStmt，动作是修改内存，本项目里算作语句。注：C++里赋值本身是表达式，这里简化了）
//
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

// ─── delete 语句 ──────────────────────────────────────────────────────────────
// 对应 [stmt.expr] / [expr.delete]；在语句位置出现的 `delete p;` 或 `delete[] p;`
struct DeleteStmt : Statement {
    ExprPtr pointerExpr;
    bool    isArray = false;
    explicit DeleteStmt(ExprPtr p, bool isArr = false)
        : Statement(NodeKind::DeleteStmt), pointerExpr(std::move(p)), isArray(isArr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Declaration：顶层声明基类
// ─────────────────────────────────────────────────────────────────────────────
// 【核心特征】：声明的核心在于“向符号表引入新的名字（标识符）”，并描述它们的结构和签名，供后面的代码引用。
// 【与 Expression 的区别】：声明是编译期的概念，它告诉编译器“如何看待”后面的表达式，声明本身不产出运行时的值。
// 【与 Statement 的区别】：声明大多不能放在普通语句的位置随便执行。顶层声明如类、函数、模板，它们是构建程序骨架的基石，而语句是填充在函数体里的血肉。
// 【代码示例】：
//    - `int foo(int x);`                     （FunctionDecl，引入名字 foo）
//    - `class Point { int x; int y; };`      （ClassDecl，引入名字 Point，以及它的内存布局）
//    - `template<typename T> class MyPtr;`   （TemplateDecl，引入一个需要实例化的蓝图）
//
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

// ─── 构造函数初始化器 ─────────────────────────────────────────────────────────
// 对应 [class.base.init]；clang: CXXCtorInitializer。
// demo：Point(int x, int y) : m_x(x), m_y(y) {} 中的 m_x(x)
struct CtorInitializer {
    std::string          memberName; // 字段名或基类名
    std::vector<ExprPtr> arguments;  // 传入的实参
    SourceLocation       location;
};

// ─── 构造函数声明 ─────────────────────────────────────────────────────────────
// 对应 [class.ctor]；clang: CXXConstructorDecl。
// 构造函数没有返回值类型，可携带初始化列表 initList。
struct ConstructorDecl : FunctionDecl {
    std::vector<CtorInitializer> initList;
    bool                         isDefaultCtor = false; // 是否为编译器合成的默认构造

    ConstructorDecl() {
        kind = NodeKind::Constructor;
    }
};
using CtorDeclPtr = std::shared_ptr<ConstructorDecl>;

// ─── 析构函数声明 ─────────────────────────────────────────────────────────────
// 对应 [class.dtor]；clang: CXXDestructorDecl。
// 析构函数名字为 ~ClassName，无参无返回值，可声明为 virtual。
struct DestructorDecl : FunctionDecl {
    bool isDefaultDtor = false; // 是否为编译器合成的默认析构

    DestructorDecl() {
        kind = NodeKind::Destructor;
    }
};
using DtorDeclPtr = std::shared_ptr<DestructorDecl>;

// ─── 全局变量声明 ─────────────────────────────────────────────────────────────
// 对应 [dcl.dcl] / [dcl.init]；clang: VarDecl (isStaticDataMember() == false && hasGlobalStorage())。
// demo：int g_counter = 0;
struct GlobalVarDecl : Declaration {
    std::string name;
    TypePtr     declaredType;
    ExprPtr     initializer; // 可为 nullptr

    GlobalVarDecl() : Declaration(NodeKind::GlobalVar) {}
};
using GlobalVarDeclPtr = std::shared_ptr<GlobalVarDecl>;

// ─── 枚举声明 ─────────────────────────────────────────────────────────────────
// 对应 [dcl.enum]；clang: EnumDecl / EnumConstantDecl。
// demo：enum Color { Red = 1, Green, Blue };
//       enum class Status : int { Ok = 0, Error = -1 };
struct EnumItem {
    std::string    name;
    int64_t        value = 0;
    bool           hasCustomValue = false;
    ExprPtr        valueExpr = nullptr;
    SourceLocation location;
};

struct EnumDecl : Declaration {
    std::string           name;
    bool                  isScoped = false; // enum class / enum struct
    TypePtr               underlyingType;   // 底层类型（默认 int）
    std::vector<EnumItem> items;

    EnumDecl() : Declaration(NodeKind::Enum) {}
};
using EnumDeclPtr = std::shared_ptr<EnumDecl>;

// ─── 命名空间声明 ─────────────────────────────────────────────────────────────
// 对应 [namespace.def]；clang: NamespaceDecl。
// demo：namespace Math { int add(int a, int b) { return a + b; } }
struct NamespaceDecl : Declaration {
    std::string          name;
    std::vector<DeclPtr> declarations;

    NamespaceDecl() : Declaration(NodeKind::Namespace) {}
};
using NamespaceDeclPtr = std::shared_ptr<NamespaceDecl>;

// ─── 类型别名声明 ─────────────────────────────────────────────────────────────
// 对应 [dcl.typedef] / [dcl.type.simple]；clang: TypeAliasDecl / TypedefDecl。
// demo：using IntPtr = int*;  或  typedef int* IntPtr;
struct TypeAliasDecl : Declaration {
    std::string aliasName;
    TypePtr     underlyingType;

    TypeAliasDecl() : Declaration(NodeKind::TypeAlias) {}
};
using TypeAliasDeclPtr = std::shared_ptr<TypeAliasDecl>;

// ─── 类声明 ───────────────────────────────────────────────────────────────────
// 对应 [class]/[class.mem]；clang: CXXRecordDecl。继承见 [class.derived]。
// demo：class Point : public Base { int x; int foo() { ... } };
//   → ClassDecl{ name=Point, baseClassNames=["Base"],
//                fields=[ FieldInfo{x:int} ],
//                methods=[ FunctionDecl{foo} ],
//                classType=<Type: Class Point> }
// 多继承：class D : public A, public B { ... } → baseClassNames=["A","B"]
//   （[class.mi]；本项目仅支持 public 非虚继承，声明顺序即子对象摆放顺序）
// 语义阶段把 ClassDecl 翻译成 Type 的 ClassLayout（算偏移 / 建 vtable）。
struct ClassDecl : Declaration {
    std::string              name;
    std::vector<std::string> baseClassNames; // 基类名列表（空 = 无继承；
                                             // 单继承 = 1 个元素；多继承 = 声明顺序）
    std::vector<FieldInfo>   fields;         // 字段列表
    std::vector<FuncDeclPtr> methods;        // 方法列表
    TypePtr                  classType;      // 对应的 Type 对象
    AccessModifier           currentAccess = AccessModifier::Private;

    ClassDecl() : Declaration(NodeKind::Class) {}

    // 便捷访问：第一个基类（无继承时返回空串）——兼容单继承路径的旧语义
    std::string firstBase() const {
        return baseClassNames.empty() ? "" : baseClassNames.front();
    }
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
// ─── 模板形参（类型形参 vs 非类型形参 NTTP）──────────────────────────────
// 对应 [temp.param]；clang: TemplateTypeParmDecl / NonTypeTemplateParmDecl
enum class TemplateParamKind {
    Type,    // typename T, class T
    NonType, // int N, bool Flag 等非类型模板参数 (NTTP)
};

struct TemplateParam {
    TemplateParamKind kind = TemplateParamKind::Type;
    std::string       name;
    TypePtr           nonType = nullptr; // 非类型形参对应的类型（如 int）
    SourceLocation    location;
};

// ─────────────────────────────────────────────────────────────────────────────
struct TemplateDecl : Declaration {
    std::vector<std::string>   typeParams;     // 模板参数名列表（如 ["T", "N"]，向后兼容）
    std::vector<TemplateParam> templateParams; // 结构化模板形参列表（含类型/非类型区分）
    ClassDeclPtr               classTemplate;  // 类模板蓝图（与 funcTemplate 互斥）
    FuncDeclPtr                funcTemplate;   // 函数模板蓝图（S1+）

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
