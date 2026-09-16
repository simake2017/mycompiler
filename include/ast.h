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

#include "ast_visitor.h"   // 节点 accept 的实现要访问 AstVisitor，故在此引入。
                           // 依赖是单向的：ast_visitor.h 只前置声明节点类型，
                           // 不回头 include 本文件（理由见那边的文件头注释）。
#include "type.h"
#include "token.h"
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

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
    Constructor, Destructor, DeductionGuide,
};

struct ASTNode {
    NodeKind kind;
    SourceLocation location;
    virtual ~ASTNode() = default;

    // 访问者挂钩：接受一个 AstVisitor，由具体节点在重写里回调对应的 visit 重载。
    // 纯虚 ⇒ 新增节点类型若忘了实现 accept，编译期即报错（不会静默漏掉）。
    // 各节点类里的 accept 一律写成类内 inline 定义（见下方各 struct），
    // 这样不会成为"键函数"，vtable 才能在每个用到的 TU 里弱符号发射。
    virtual void accept(AstVisitor& v) = 0;

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

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct BoolLiteralExpr : Expression {
    bool value;
    explicit BoolLiteralExpr(bool v) : Expression(NodeKind::BoolLiteral), value(v) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct StringLiteralExpr : Expression {
    std::string value;
    explicit StringLiteralExpr(std::string v)
        : Expression(NodeKind::StringLiteral), value(std::move(v)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

struct NullptrLiteralExpr : Expression {
    NullptrLiteralExpr() : Expression(NodeKind::NullptrLiteral) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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
    // 用 TemplateArg（tagged）而非裸 TypePtr：与 Type::templateArgs 保持一致，
    // 使得 foo<4>(x) 能被解析出来——虽然函数模板的 NTTP 尚未实现，
    // 由语义阶段给出"函数模板暂不支持非类型实参"的明确报错，
    // 而不是在语法阶段就崩成"Expected type name"。
    std::vector<TemplateArg> explicitTemplateArgs;
    explicit VarExpr(std::string n) : Expression(NodeKind::Var), name(std::move(n)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 一元表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.unary.op]；clang: UnaryOperator。本项目仅实现 Neg(-) 与 Not(!)。
enum class UnaryOp : uint8_t {
    Neg,    // -x
    Not,    // !x
    Addr,   // &x —— 取地址 [expr.unary.op]/3
};

// demo：return -x;  → ReturnStmt[ UnaryExpr{ op=Neg, operand=VarExpr{x} } ]
struct UnaryExpr : Expression {
    UnaryOp op;
    ExprPtr operand;
    UnaryExpr(UnaryOp o, ExprPtr e)
        : Expression(NodeKind::Unary), op(o), operand(std::move(e)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    // ── 类型限定访问：Cls<Args>::member（静态成员）────────────────────
    // 【为什么单独立标志】obj.field 的 object 是【对象】，运行期要按偏移量取；
    //   而 Cls<Args>::value 的 Cls<Args> 是【类型】，根本没有对象、没有偏移量，
    //   它是个编译期常量。二者语义不同，必须能分辨。
    // 【demo】is_range<decltype(numbers)>::value
    //           → MemberExpr{ object=VarExpr{is_range, explicitTemplateArgs=[decltype(...)]},
    //                         memberName="value", isTypeAccess=true }
    // 【理论】[expr.ref]/[expr.prim.id.qual]：限定名查找（qualified lookup）
    //   在类的作用域（含基类）里找静态数据成员。
    //   本实现把命中的静态常量【折叠成字面量】（见 Sema::foldStaticConst）——
    //   因为值在编译期就已知，运行期不应再去内存里读。
    // 对照 clang：DeclRefExpr(NestedNameSpecifier + ValueDecl)，
    //   Sema::BuildDeclarationNameExpr 走 CXXScopeSpec 的那条路径。
    bool        isTypeAccess = false;

    MemberExpr(ExprPtr obj, std::string member, bool arrow)
        : Expression(NodeKind::Member), object(std::move(obj)),
          memberName(std::move(member)), isArrow(arrow) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── new 表达式 ───────────────────────────────────────────────────────────────
// 对应 [expr.new]；clang: CXXNewExpr。
// demo：new Point()      → NewExpr{ className="Point", constructorArgs=[] }
//       new Point(a, b)  → NewExpr{ className="Point", constructorArgs=[a, b] }
// 结果类型是 Point*（指针）；CodeGen 生成 operator new 调用 + 构造函数调用。
struct NewExpr : Expression {
    std::string            className;
    std::vector<ExprPtr>   constructorArgs;
    // P3 —— new Box<int>() / new Buf<4>() 的模板实参（Parser 填写
    // 语义阶段实例化后把 className 改写为实例名，CodeGen 只看改写后的名字）。
    // 用 TemplateArg（tagged）：同时容纳类型实参与 NTTP 值实参。
    std::vector<TemplateArg> templateArgs;
    explicit NewExpr(std::string cls)
        : Expression(NodeKind::New), className(std::move(cls)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── this 表达式 ──────────────────────────────────────────────────────────────
// 对应 [expr.prim.this]；clang: CXXThisExpr。
// demo：return this;   → ReturnStmt[ ThisExpr ]
// 仅出现在成员函数体内；类型是"指向所属类的指针"（[class.this]）。
struct ThisExpr : Expression {
    ThisExpr() : Expression(NodeKind::This) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    // ── 直接初始化：`Type name(args...);`（[dcl.init]/16）──
    // 【为什么单列而不是塞进 initializer】两者的语义层级不同：
    //   `T x = e;` 是【拷贝初始化】——先造一个 T 再拷/移到 x；
    //   `T x(args);` 是【直接初始化】——直接在 x 的存储上跑构造函数。
    //   本项目的类对象没有拷贝语义，两条路最终都落到"在 x 的槽位上调用
    //   构造函数"，但参数传递方式不同（前者只有一个实参，后者是完整实参表）。
    // 【为什么 CTAD 非要有它】[dcl.type.class.deduct]/1：**类模板实参推导
    //   只在直接初始化（或 `=` 加单个同模板实参）时触发** ——
    //   `MyPtr m(7);` 里没有写 `<...>`，推导的输入就是这串构造实参。
    //   `=` 形式的 `MyPtr m = 7;` 不是 CTAD（那是转换，C++17 起才有拷贝推导）。
    // 空 ⇒ 不是括号初始化；CodeGen 对空表走原来的路径（默认构造 / 标量赋值）。
    std::vector<ExprPtr> ctorArgs;

    // 语义阶段选定并【回填】的构造函数符号（成员 mangledName）。
    // 【为什么不能让 CodeGen 自己拼】mangling 规则是有状态的 —— 同名多参
    //   会追加参数个数后缀（`MyPtr_int_MyPtr_int_1`），而"该调哪个重载"
    //   只有 Sema 知道（它刚做完推导/决议）。CodeGen 若按 `Name_Name`
    //   硬拼，遇到带参构造就会拼出一个不存在的符号（链接期 undefined）。
    // 空 ⇒ 零参构造，CodeGen 回退到 `Name_Name`（老路径，行为不变）。
    std::string ctorSymbol;

    VarDeclStmt(std::string n, TypePtr t, ExprPtr init)
        : Statement(NodeKind::VarDecl), name(std::move(n)),
          declaredType(std::move(t)), initializer(std::move(init)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── return 语句 ──────────────────────────────────────────────────────────────
// 对应 [stmt.return]；clang: ReturnStmt。
// demo：return a;   → ReturnStmt{ value=VarExpr{a} }
//       return;     → ReturnStmt{ value=nullptr }  （void 返回）
struct ReturnStmt : Statement {
    ExprPtr value; // 可为 nullptr（void 返回）
    explicit ReturnStmt(ExprPtr v = nullptr)
        : Statement(NodeKind::Return), value(std::move(v)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 代码块语句 ───────────────────────────────────────────────────────────────
// 对应 [stmt.block]/[stmt.dcl]；clang: CompoundStmt。函数体就是一个 BlockStmt。
// demo：{ int x = 1; return x; }
//   → BlockStmt{ statements=[ VarDeclStmt{x}, ReturnStmt{VarExpr{x}} ] }
struct BlockStmt : Statement {
    std::vector<StmtPtr> statements;
    explicit BlockStmt(std::vector<StmtPtr> stmts = {})
        : Statement(NodeKind::Block), statements(std::move(stmts)) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── delete 语句 ──────────────────────────────────────────────────────────────
// 对应 [stmt.expr] / [expr.delete]；在语句位置出现的 `delete p;` 或 `delete[] p;`
struct DeleteStmt : Statement {
    ExprPtr pointerExpr;
    bool    isArray = false;
    explicit DeleteStmt(ExprPtr p, bool isArr = false)
        : Statement(NodeKind::DeleteStmt), pointerExpr(std::move(p)), isArray(isArr) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using EnumDeclPtr = std::shared_ptr<EnumDecl>;

// ─── 命名空间声明 ─────────────────────────────────────────────────────────────
// 对应 [namespace.def]；clang: NamespaceDecl。
// demo：namespace Math { int add(int a, int b) { return a + b; } }
struct NamespaceDecl : Declaration {
    std::string          name;
    std::vector<DeclPtr> declarations;

    NamespaceDecl() : Declaration(NodeKind::Namespace) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using NamespaceDeclPtr = std::shared_ptr<NamespaceDecl>;

// ─── 类型别名声明 ─────────────────────────────────────────────────────────────
// 对应 [dcl.typedef] / [dcl.type.simple]；clang: TypeAliasDecl / TypedefDecl。
// demo：using IntPtr = int*;  或  typedef int* IntPtr;
struct TypeAliasDecl : Declaration {
    std::string aliasName;
    TypePtr     underlyingType;

    TypeAliasDecl() : Declaration(NodeKind::TypeAlias) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
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
// ─── 静态常量成员（见 ClassDecl::staticConsts）──────────────────────────────
// demo：std::false_type 的 value → { value=0, type=bool }
//       std::true_type  的 value → { value=1, type=bool }
struct StaticConstMember {
    int64_t value = 0;
    TypePtr type;          // 常量自身的类型（bool / int …）
};

struct ClassDecl : Declaration {
    std::string              name;
    std::vector<std::string> baseClassNames; // 基类名列表（空 = 无继承；
                                             // 单继承 = 1 个元素；多继承 = 声明顺序）
    std::vector<FieldInfo>   fields;         // 字段列表
    std::vector<FuncDeclPtr> methods;        // 方法列表
    TypePtr                  classType;      // 对应的 Type 对象
    AccessModifier           currentAccess = AccessModifier::Private;

    // ── 静态常量成员（名 → 值）──────────────────────────────────────────
    // 【用途】std 垫片里的 false_type/true_type 靠它提供 ::value。
    //   Parser 不解析类内 `static const int value = 1;`（该语法未实现），
    //   故这些成员由 SemanticAnalyzer::registerBuiltins 直接注入。
    // 【理论】静态数据成员不占对象内存、没有偏移量，是编译期已知的常量
    //   （[class.static.data]）。查找要走"类作用域 + 基类链"
    //   （[class.member.lookup]），与实例字段的 findField 是两条不同的路径。
    // 对照 clang：VarDecl 且 isStaticDataMember()，
    //   取值走 EvaluatingValueDecl 的常量求值。
    // ★ 带类型：真 C++ 里 std::false_type::value 的类型是 bool 而非 int，
    //   这个差别是可观察的 —— `bool b = Trait<T>::value;` 合法而
    //   `int i = Trait<T>::value;` 在本项目下报错（int→bool 未开放为隐式转换）。
    //   故折叠时按这里的 type 生成对应字面量，而不是一律给 int。
    std::unordered_map<std::string, StaticConstMember> staticConsts;

    // ── 成员类型别名（名 → 目标类型）────────────────────────────────────
    // 【语法】`using type = T;` 与 `typedef T type;`（[dcl.typedef]）
    // 【理论】类型别名是【纯编译期】设施：它不产生新类型、不占内存、
    //   不进符号表（链接器根本不认识它），只在编译期做一次名字替换。
    //   这正是 type_traits 全家桶的出口 —— 每个元函数的"返回值"都是一条
    //   `using type = ...`，所以没有它写不出任何 trait。
    // 【依赖情形】别名目标可以是模板形参（`using type = T;`），
    //   实例化时由 TemplateInstantiator 做结构化替换（与字段/方法同一条路）。
    //   对照 clang：TypedefNameDecl，实例化走 Sema::InstantiateTypedefNameDecl。
    std::unordered_map<std::string, TypePtr> typeAliases;
    std::vector<std::string> typeAliasOrder;   // 保持声明顺序，便于日志可观测

    ClassDecl() : Declaration(NodeKind::Class) {}

    // 便捷访问：第一个基类（无继承时返回空串）——兼容单继承路径的旧语义
    std::string firstBase() const {
        return baseClassNames.empty() ? "" : baseClassNames.front();
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
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

    // ── 默认模板实参（[temp.param]/12）──
    // demo：template<typename T, typename U = void> 里 U 的 `= void`
    //   → defaultArg = TemplateArg{kind=Type, type=void}, hasDefault = true
    // 用 hasDefault 而不是 defaultArg.type == nullptr 判"有无默认"：
    // 显式写 `= void` 时 type 非空但语义上仍是"有默认"，两者必须分开。
    //
    // 【标准约束】默认实参只能在**主模板**（及别名模板）的形参表里给；
    // 偏特化的形参表不得有默认实参。且一旦某位形参有默认值，
    // 其后的每一位都必须有（[temp.param]/12："后续形参必须有默认实参"）。
    TemplateArg       defaultArg;
    bool              hasDefault = false;

    // ── 无名形参（[temp.param]/3）──────────────────────────────────────
    // demo：template <typename T, typename = void>
    //         第 2 位形参没有名字，只用它的默认值参与 void_t 推导。
    // 内部仍给一个合成名（"$unnamed0"）当替换表的键 —— 表总得有键，
    // 而 '$' 不是合法标识符字符，保证用户代码永远写不出同名的引用，
    // 语义上等价于"这个名字不可见"。
    bool              isUnnamed = false;

    SourceLocation    location;
};

// ─── 模板声明的种类（[temp.class.spec] / [temp.expl.spec]）──────────────────
// 一个类模板可以有三种"版本"，同名共存，靠实参匹配择优：
//
//   template<class T, class U = void> struct Box { ... };   Primary     主模板
//   template<class T> struct Box<T*, T> { ... };            PartialSpec 偏特化
//   template<> struct Box<int*, int> { ... };               ExplicitSpec 全特化（显式特化）
//
// 对照 clang：ClassTemplateDecl（主模板）持有 PartialSpecialization 链表
// （ClassTemplatePartialSpecializationDecl）与 Specializations 集合
// （ClassTemplateSpecializationDecl），三者是不同 AST 节点、由 Sema 关联。
// 本项目用"同结构 + 判别枚举"保持 AST 扁平。
//
// 【择优顺序】[temp.class.spec.match] + [temp.expl.spec]/6：
//   ① 全特化精确匹配（最高优先，命中即用）
//   ② 偏特化逐个做形参推导，取匹配成功者
//   ③ 都不中 → 主模板 + 默认实参补全
enum class TemplateSpecKind {
    Primary,      // 主模板
    PartialSpec,  // 偏特化：形参表非空，且 specPattern 中含模板参数（如 T*）
    ExplicitSpec, // 全特化：形参表为空 template<>，specPattern 全为具体类型
};

// ─────────────────────────────────────────────────────────────────────────────
// ─── 推导指引（deduction guide，[temp.deduct.guide]）─────────────────────────
// 对应 clang: CXXDeductionGuideDecl。
// 语法：DeductionGuide := ['template' '<' params '>'] Name '(' params ')' '->' Name '<' args '>' ';'
// demo：template<class T> MyPtr(T) -> MyPtr<T>;      // 从构造实参反推 T
//       Box(int) -> Box<int>;                        // 非模板指引：写死映射
//
// 【它到底解决什么】CTAD 默认只会"拿构造函数当指引用"，可构造函数写不出
//   所有想要的映射 —— 最典型的是：类的构造函数收 `T*`，但你希望 `MyPtr(p)`
//   对 `int*` 推出 `MyPtr<int>` 而不是 `MyPtr<int*>`。推导指引就是给用户
//   一个"改写映射规则"的钩子：它长得像函数，但**不产生任何代码**，
//   只参与 CTAD 那一次推导。
// 【与普通函数的本质区别】没有函数体、没有符号、不参与重载决议 ——
//   它是编译期的纯映射规则，用完即弃。
struct DeductionGuideDecl : Declaration {
    std::vector<TemplateParam> templateParams;  // 指引自身的模板形参（可为空）
    std::string                guideName;       // 被指引的类模板名（如 "MyPtr"）
    std::vector<Parameter>     parameters;      // 指引的形参表（推导模式 P）
    std::vector<TypePtr>       targetArgs;      // `->` 右侧的实参（含模板形参）

    DeductionGuideDecl() : Declaration(NodeKind::DeductionGuide) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using DeductionGuideDeclPtr = std::shared_ptr<DeductionGuideDecl>;

struct TemplateDecl : Declaration {
    std::vector<std::string>   typeParams;     // 模板参数名列表（如 ["T", "N"]，向后兼容）
    std::vector<TemplateParam> templateParams; // 结构化模板形参列表（含类型/非类型区分）
    ClassDeclPtr               classTemplate;  // 类模板蓝图（与 funcTemplate 互斥）
    FuncDeclPtr                funcTemplate;   // 函数模板蓝图（S1+）
    TypeAliasDeclPtr           aliasTemplate;  // 别名模板蓝图：template<T> using X = ...;
    DeductionGuideDeclPtr      guide;          // 推导指引蓝图：template<T> X(T) -> X<T>;

    // ── 特化支持（[temp.class.spec] / [temp.expl.spec]）──
    // specKind    : 本声明是主模板 / 偏特化 / 全特化
    // specPattern : 特化形参模式，对应模板名后尖括号里的那串实参
    //   template<class T> struct Box<T*, T>;      → [T*, T]      （含模板参数 → 偏特化）
    //   template<> struct Box<int*, int>;         → [int*, int]  （全具体 → 全特化）
    //   主模板（struct Box { ... }）无尖括号 → specPattern 为空
    // demo：Box<T*, T> 的 specPattern[0] = Type{Pointer, TemplateParam"T"}
    TemplateSpecKind           specKind = TemplateSpecKind::Primary;
    std::vector<TypePtr>       specPattern;

    TemplateDecl() : Declaration(NodeKind::Template) {}

    bool isPrimary()      const { return specKind == TemplateSpecKind::Primary; }
    bool isPartialSpec()  const { return specKind == TemplateSpecKind::PartialSpec; }
    bool isExplicitSpec() const { return specKind == TemplateSpecKind::ExplicitSpec; }
    bool isSpecialization() const { return specKind != TemplateSpecKind::Primary; }

    bool isClassTemplate()    const { return classTemplate != nullptr; }
    bool isFunctionTemplate() const { return funcTemplate != nullptr; }
    // ── 别名模板 [temp.alias] ──
    // template<class T> using Vec = MyPtr<T>;
    // 【与类模板的本质差别】别名【不是新类型】，只是既有类型的另一个名字：
    //   Vec<int> 和 MyPtr<int> 是【同一个类型】，不产生包装类、不产生新符号。
    //   所以它没有"实例化"这一步，只有"替换"这一步——把形参换掉就完事。
    // 对照 clang：TypeAliasTemplateDecl + AliasTemplateSpecializationType
    //   （clang 为了诊断仍保留一层 sugar，本实现直接解糖到最终类型）。
    bool isAliasTemplate()    const { return aliasTemplate != nullptr; }
    // ── 推导指引（[temp.deduct.guide]）──
    // template<class T> MyPtr(T) -> MyPtr<T>;
    // 【它不是"模板"】这里借用 TemplateDecl 只是因为语法上共用 `template<...>`
    //   前缀外壳。指引本身没有实体、没有符号、不被实例化 —— 它只是 CTAD
    //   在推导时可以被问到的一条"映射规则"。故它也有自己的 templateParams。
    bool isDeductionGuide()   const { return guide != nullptr; }
    // 模板的主名（类名 / 函数名 / 别名名 / 被指引的类模板名）
    const std::string& templateName() const {
        if (isClassTemplate()) return classTemplate->name;
        if (isAliasTemplate()) return aliasTemplate->aliasName;
        if (isDeductionGuide()) return guide->guideName;
        return funcTemplate->name;
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
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
