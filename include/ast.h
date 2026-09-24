#pragma once
// =============================================================================
// 阶段 2：抽象语法树（AST）节点定义 —— 理论见 docs/learn/10
// =============================================================================
// AST 是 Parser 的输出、后续所有阶段的唯一输入：
//   Parser 建树 → Sema 填 resolvedType / 注册符号 / 算类布局 → 模板实例化克隆+替换
//   → CodeGen 消费出汇编。
//
// 节点层次（NodeKind 是运行时类型标签，等价 clang 的 Stmt::StmtClass / Decl::Kind）：
//   ASTNode
//   ├── Expression  有类型有值   IntLiteral BoolLiteral StringLiteral NullptrLiteral
//   │                            Var Binary Unary Call Member Index New DynamicCast
//   │                            This Delete
//   ├── Statement   无值执行动作 ExprStmt VarDecl Return If While Block Assign DeleteStmt
//   └── Declaration 引入新名字   Function Class Template GlobalVar Enum Namespace
//                                TypeAlias Constructor Destructor DeductionGuide
// demo：`int a = 1 + 2;` ⇒ VarDeclStmt{ BinaryExpr{op=Add, IntLiteral(1), IntLiteral(2)} }
//
// 【标准章节】[expr.*] 表达式 / [stmt.*] 语句 / [dcl.fct] 函数
//             [class] [class.derived] [class.virtual] 类与继承 / [temp] 模板蓝图
// 【clang 对照】clang 把声明与语句/表达式分在两棵树（Decl*.h 与 Stmt*.h / Expr*.h），
//   本文件用单一 ASTNode 基类统一三者，便于教学遍历与打印：
//     ASTNode ≈ Decl ∪ Stmt │ Expression ≈ Expr │ Statement ≈ Stmt
//     Declaration ≈ Decl    │ TranslationUnit ≈ TranslationUnitDecl
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
// 三大族判据：Expression 有 resolvedType / 可求值 / 可做子表达式（42、a+b、foo(x,y)）；
//             Statement 无类型、只描述执行动作（return、if、x = 5;），不能做子表达式；
//             Declaration 向符号表引入名字（函数/类/模板），无运行时值。
// 对应 clang::Expr。表达式都有唯一类型（[expr]），Parser 留空、阶段 3 填充。
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
    // 显式模板实参（S3）：foo<int>(x) 中的 <int>。Parser 在标识符后识别 template-id 时
    // 填充；语义阶段据此做"显式 + 推导"混合。挂在 VarExpr 上是因为 template-id 出现在
    // 调用的 '(' 之前；用 TemplateArg（tagged）与 Type::templateArgs 保持一致，使
    // foo<4>(x) 也能被解析出来（否则语法阶段就崩成 "Expected type name"），再由语义
    // 阶段给出"函数模板暂不支持非类型实参"的明确报错。
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
    Deref,  // *x —— 解引用 [expr.unary.op]/1
            //   ★ 一元 '*' 与二元的 '*' 同形，靠【位置】区分：有左操作数才是乘法。
            //     与 '&'（取地址 vs 位与/引用）完全同构的判据，见 parseUnaryExpr。
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
    // 为何单独立标志：obj.field 的 object 是【对象】，运行期按偏移量取；而
    //   Cls<Args>::value 的限定者是【类型】，无对象无偏移，是编译期常量。
    // 理论：[expr.ref]/[expr.prim.id.qual] 限定名查找，在类作用域（含基类）里找静态
    //   数据成员；命中的静态常量被折叠成字面量（Sema::foldStaticConst）。
    // demo: is_range<decltype(numbers)>::value ⇒ MemberExpr{object=VarExpr{is_range,
    //         explicitTemplateArgs=[decltype(...)]}, memberName="value", isTypeAccess=true}
    // clang 对照：DeclRefExpr(NestedNameSpecifier + ValueDecl)，
    //   Sema::BuildDeclarationNameExpr 走 CXXScopeSpec 的那条路径。
    bool        isTypeAccess = false;

    // ── 成员【模板】调用选定的符号（[temp.mem]）────────────────────────────
    // 普通成员调用的符号是 CodeGen 按 `类名_方法名` 硬拼的；成员模板不同：
    //   同一个 `id` 会因实参不同实例化出 `S_id_int` / `S_id_double` 多个符号，
    //   硬拼必然全部调同一个（且那个符号可能根本没被发射）。
    // 故由 Sema 在推导+实例化后把选定的符号名回填到这里，CodeGen 非空即采用。
    // 空串 = 普通成员调用，走原有硬拼路径（既有符号逐字节不变）。
    // 对照 clang：CXXMemberCallExpr 持 CXXMethodDecl*，符号由该 Decl 决定。
    std::string resolvedCalleeSymbol;

    MemberExpr(ExprPtr obj, std::string member, bool arrow)
        : Expression(NodeKind::Member), object(std::move(obj)),
          memberName(std::move(member)), isArrow(arrow) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};

// ─── 下标访问表达式（语法糖，降级为 at()/set() 调用）────────────────────────
// 对应 [expr.sub]；clang: ArraySubscriptExpr / CXXOperatorCallExpr（operator[] 重载）。
// 设计（教学版 operator[] 的"糖化"）：minicc 尚无运算符重载，故把下标语法降级为两个
//   约定方法 —— 右值位置（读）v[i] ≡ v.at(i)；左值位置（写）v[i] = x ≡ v.set(i, x)。
//   类只要实现 at(int)/set(int,int) 就自动获得 [] 手感（Vector<T>/Map<K,V> 即此约定）。
// demo: v[i] ⇒ IndexExpr{ object=VarExpr{v}, index=VarExpr{i} }
// Sema: inferIndex 校验 object 是类类型且类里有 at()；结果类型 = at() 的返回类型。
// CodeGen: emitExpr 发 this + 实参、callq <类名>_at；target 为 IndexExpr 的赋值改发
//   <类名>_set。
struct IndexExpr : Expression {
    ExprPtr object;  // 被下标的容器对象（类类型）
    ExprPtr index;   // 下标表达式（按约定方法签名校验）

    // Sema 回填的约定方法【符号】（at = 读，set = 写）。
    // ★ CodeGen 不能硬拼 `类名_at`：成员方法名由 Sema 追加了"参数个数"后缀
    //   （IntVec_at_1 / IntVec_set_2，见 semantic_analyzer.cpp 的 mangledName 规则），
    //   硬拼会拼出一个谁也没定义过的符号 ⇒ 链接期 undefined reference。
    //   空串 = 类里没有对应约定方法 ⇒ CodeGen 退回硬拼（把报错留给链接期）。
    std::string atSymbol;
    std::string setSymbol;
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
// 无类型、只描述执行动作（判据见上方 Expression 的三大族）。对应 clang::Stmt。
// demo: return 0; ⇒ ReturnStmt；if (f) {...} ⇒ IfStmt；x = 5; ⇒ AssignStmt
//   （★ C++ 里赋值本身是表达式，本项目简化成语句。）
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
    // 与 initializer 的语义层级不同：`T x = e;` 是拷贝初始化，`T x(args);` 是直接初始化
    //   —— 后者直接在 x 的存储上跑构造函数，实参是完整实参表。
    // ★ CTAD 非它不可：[dcl.type.class.deduct]/1 规定类模板实参推导【只在直接初始化触发】
    //   —— `MyPtr m(7);` 没写 `<...>`，推导输入就是这串构造实参。
    // 空 ⇒ 不是括号初始化，CodeGen 走原路径（默认构造 / 标量赋值）。
    std::vector<ExprPtr> ctorArgs;

    // 语义阶段选定并【回填】的构造函数符号（成员 mangledName）。
    // ★ 不能让 CodeGen 自己拼：mangling 是有状态的（同名多参追加参数个数后缀），"该调
    //   哪个重载"只有刚做完推导/决议的 Sema 知道；硬拼 `Name_Name` 会拼出不存在的符号。
    // 空 ⇒ 零参构造，CodeGen 回退 `Name_Name`。
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
// 向符号表引入名字并描述其结构签名，是符号表的构成单元，本身不产出运行时值。
// 对应 clang::Decl；三者判据见上方 Expression 的三大族。
// demo: int foo(int x); ⇒ FunctionDecl；class Point {...}; ⇒ ClassDecl；
//       template<typename T> class MyPtr; ⇒ TemplateDecl
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
// demo: int f(int a) { return a; } ⇒ FunctionDecl{ name=f, returnType=int,
//         parameters=[Parameter{a:int}], body=BlockStmt[ ReturnStmt[ VarExpr{a} ] ] }
// isVirtual/isOverride 用于类的虚函数（[class.virtual]）；mangledName 由阶段 4 填充；
// ownerClassName 非空表示成员函数。
struct FunctionDecl : Declaration {
    std::string          name;
    TypePtr              returnType;
    std::vector<Parameter> parameters;
    std::shared_ptr<BlockStmt> body;      // nullptr = 纯声明（无实现）
    bool                 isVirtual  = false;
    bool                 isOverride = false;
    // 后置 const（[dcl.fct]/7）：`int get() const` —— cv-qualifier-seq 是【函数类型】
    // 的一部分，改变隐式 this 的类型（T* → const T*），这是"const 对象不能调
    // 非 const 成员"的底层机制。本项目记录该标志用于日志与后续检查，
    // ★ 未实现：`f()` 与 `f() const` 的重载区分（clang 视其为两个不同重载）。
    bool                 isConstMethod = false;
    // 类内 static 成员函数（[class.static]/2）：**没有隐式 this 参数**，
    // 调用不传对象（C::f() 与 obj.f() 等价，都不装 rdi）。
    // ★ 未实现：static 数据成员（需类外定义 + 独立存储，不是每实例一份）。
    bool                 isStatic = false;
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
// 对应 [dcl.dcl] / [dcl.init]；clang: VarDecl（全局变量 ⇒ isStaticDataMember() == false、
//   hasGlobalStorage() == true）。
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
// demo: class Point : public Base { int x; int foo() {...} }; ⇒ ClassDecl{name=Point,
//         baseClassNames=["Base"], fields=[FieldInfo{x:int}], methods=[FunctionDecl{foo}]}
// 多继承：[class.mi]，本项目仅支持 public 非虚继承，声明顺序即子对象摆放顺序。
// Sema 把 ClassDecl 翻译成 Type 的 ClassLayout（算偏移 / 建 vtable）。
// ─── 静态常量成员（见 ClassDecl::staticConsts）──────────────────────────────
// demo: std::false_type::value ⇒ {value=0, type=bool}；true_type ⇒ {value=1, type=bool}
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

    // ── 前置声明标记（[dcl.type.elab]）──────────────────────────────────
    // `struct A;` 只声明"名字存在"、不给定义 ⇒ 本字段为 true。
    // 【为什么必须显式标记】空壳与"真的空类"（`struct A {};`）在字段/方法上都为空，
    //   靠内容区分不了；而两者语义不同：前者不该生成任何符号。
    // 【不标会怎样】Sema 给空壳也生成默认构造/析构 ⇒ CodeGen 发射两份 A_dtor
    //   （空壳发 A_A + A_dtor，真定义发 A_A_0 + A_dtor）⇒ 汇编期
    //   "symbol `A_dtor' is already defined"。
    bool isForwardDecl = false;

    // ── 静态常量成员（名 → 值）──────────────────────────────────────────
    // 【用途】std 垫片 false_type/true_type 的 ::value 由 SemanticAnalyzer::
    //   registerBuiltins 直接注入（Parser 不解析类内 `static const int value = 1;`）。
    // 【理论】静态数据成员不占对象内存、无偏移量，是编译期常量（[class.static.data]）；
    //   查找走"类作用域 + 基类链"（[class.member.lookup]），与 findField 是两条路径。
    //   clang 对照：VarDecl 且 isStaticDataMember()，取值走常量求值。
    // ★ 带类型：false_type::value 的类型是 bool 而非 int，差别可观察（本项目下
    //   `int i = Trait<T>::value;` 报错，int→bool 未开放为隐式转换）—— 折叠时按这里的
    //   type 造对应字面量，而不是一律给 int。
    std::unordered_map<std::string, StaticConstMember> staticConsts;

    // ── 成员类型别名（名 → 目标类型）────────────────────────────────────
    // 【语法】`using type = T;` 与 `typedef T type;`（[dcl.typedef]）
    // 【理论】类型别名是纯编译期设施：不产生新类型、不占内存、不进符号表，只在编译期做
    //   一次名字替换 —— 它是 type_traits 全家桶的出口（每个元函数的"返回值"都是一条
    //   `using type = ...`）。目标可含模板形参，实例化时由 TemplateInstantiator 结构化
    //   替换（与字段/方法同一条路）。
    //   clang 对照：TypedefNameDecl / Sema::InstantiateTypedefNameDecl。
    std::unordered_map<std::string, TypePtr> typeAliases;
    std::vector<std::string> typeAliasOrder;   // 保持声明顺序，便于日志可观测

    // ── 成员模板（[temp.mem]）──────────────────────────────────────────────
    //   struct S { template <class T> T id(T x) { return x; } };
    // 每个元素是一个函数模板蓝图（funcTemplate 非空），与自由函数模板**同构** ——
    // 调用点用同一套合一算法推导，只是实例化出来的函数带 ownerClassName。
    std::vector<std::shared_ptr<TemplateDecl>> memberTemplates;

    ClassDecl() : Declaration(NodeKind::Class) {}

    // 便捷访问：第一个基类（无继承时返回空串）
    std::string firstBase() const {
        return baseClassNames.empty() ? "" : baseClassNames.front();
    }

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using ClassDeclPtr = std::shared_ptr<ClassDecl>;

// ─── 模板声明（代码蓝图） ─────────────────────────────────────────────────────
// Parser 遇 `template<typename T> class MyPtr {...}` 或 `template<typename T>
// T twice(T x) {...}` 时把整个声明"冻结"成蓝图，暂不解析内部语义；等实例化（类模板
// MyPtr<int> / 函数模板由实参推导驱动）时再克隆并做 T → int 的结构化替换。
//
// classTemplate / funcTemplate / aliasTemplate / guide 四槽位互斥：一个 TemplateDecl 只
// 承载一种蓝图。clang 里它们各是独立的 Decl（ClassTemplateDecl / FunctionTemplateDecl /
// TypeAliasTemplateDecl / CXXDeductionGuideDecl），本项目用"多槽位 + 判别方法"代替继承，
// 保持 AST 扁平、便于教学。
//
// demo: template<typename T> T twice(T x) { return x + x; } ⇒ TemplateDecl{typeParams=["T"],
//   classTemplate=nullptr, funcTemplate=FunctionDecl{name=twice, returnType=<TemplateParam T>,
//   parameters=[x:<TemplateParam T>], body=BlockStmt[ReturnStmt[BinaryExpr{Add,x,x}]]}}
// ─── 模板形参（类型形参 vs 非类型形参 NTTP）──────────────────────────────
// 对应 [temp.param]；clang: TemplateTypeParmDecl / NonTypeTemplateParmDecl
enum class TemplateParamKind {
    Type,     // typename T, class T
    NonType,  // int N, bool Flag 等非类型模板参数 (NTTP)
    Template, // template <class> class C —— 模板模板参数 [temp.param]/4
};

struct TemplateParam {
    TemplateParamKind kind = TemplateParamKind::Type;
    std::string       name;
    TypePtr           nonType = nullptr; // 非类型形参对应的类型（如 int）

    // ── kind == Template 时有效：被接受模板的【形参个数】───────────────
    // `template <template <class> class C>` ⇒ C.templateArity = 1。
    // 只记元数：本项目不做 [temp.arg.template]/2 的"逐位形参表至少一样特化"
    // 匹配，形态自检只要求"这一位得是个模板名"。
    size_t            templateArity = 0;

    // ── 默认模板实参（[temp.param]/12）──
    // demo: template<typename T, typename U = void> 里 U 的 `= void` ⇒ hasDefault = true
    // 用 hasDefault 而非 defaultArg.type == nullptr 判"有无默认"：显式写 `= void` 时
    //   type 非空但语义上仍是"有默认"，两者必须分开。
    // 【标准约束】默认实参只能给在主模板（及别名模板）的形参表；偏特化不得有默认实参；
    //   且一旦某位形参有默认值，其后的每一位都必须有。
    TemplateArg       defaultArg;
    bool              hasDefault = false;

    // ── 无名形参（[temp.param]/3）──
    // demo: template <typename T, typename = void> 第 2 位形参只用默认值参与 void_t 推导。
    // 内部给一个合成名（"$unnamed0"）当替换表的键 —— 表总得有键，而 '$' 不是合法标识符
    //   字符，保证用户代码写不出同名引用，语义上等价于"这个名字不可见"。
    bool              isUnnamed = false;

    SourceLocation    location;
};

// ── 模板形参以【指针】形式持有（对应 clang 的 TemplateParameterList）────────
// clang：TemplateParameterList 用 TrailingObjects 内联存 `NamedDecl*` 数组，节点由
//   TemplateTypeParmDecl::Create 分配在 ASTContext 的 BumpPtrAllocator（arena）上，
//   永不移动、永不单独释放 —— 故缓存形参指针永远安全。
// 本项目无 arena，用 shared_ptr 拿到同样两条性质：① 节点不随容器扩容而搬家（vector
//   扩容搬的是【指针值】，不是节点本身）；② 生命周期覆盖全部引用方。
// ★ 别改成值语义 `std::vector<TemplateParam>` —— 元素住在 vector 堆块里，解析期一路
//   push_back 会 reallocate，先前取得的 `const TemplateParam*` 立刻失效：
//   demo：`template<class T, class U>` 解析到 T 时又 push_back(U) ⇒ 指向 T 的指针作废，
//   而模板形参作用域的查询恰发生在 push_back 进行中（见 parser.h 的帧）。
using TemplateParamPtr = std::shared_ptr<TemplateParam>;

// ─── 模板声明的种类（[temp.class.spec] / [temp.expl.spec]）──────────────────
// 一个类模板可以有三种"版本"，同名共存，靠实参匹配择优：
//
//   template<class T, class U = void> struct Box {...};   ⇒ Primary      主模板
//   template<class T> struct Box<T*, T> {...};            ⇒ PartialSpec  偏特化
//   template<> struct Box<int*, int> {...};               ⇒ ExplicitSpec 全特化
//
// 【择优顺序】[temp.class.spec.match] + [temp.expl.spec]/6：① 全特化精确匹配（命中即用）
//   ② 偏特化逐个推导，取匹配成功者 ③ 都不中 → 主模板 + 默认实参补全。
// clang 对照：ClassTemplateDecl 另挂偏特化链表与实例化集合，是不同 AST 节点；本项目用
//   "同结构 + 判别枚举"保持 AST 扁平。
enum class TemplateSpecKind {
    Primary,      // 主模板
    PartialSpec,  // 偏特化：形参表非空，且 specPattern 中含模板参数（如 T*）
    ExplicitSpec, // 全特化：形参表为空 template<>，specPattern 全为具体类型
};

// ─────────────────────────────────────────────────────────────────────────────
// ─── 推导指引（deduction guide，[temp.deduct.guide]）─────────────────────────
// 对应 clang: CXXDeductionGuideDecl。
// 语法：['template' '<' params '>'] Name '(' params ')' '->' Name '<' args '>' ';'
// demo: template<class T> MyPtr(T) -> MyPtr<T>;   // 从构造实参反推 T
//       Box(int) -> Box<int>;                     // 非模板指引：写死映射
//
// CTAD 默认只会"拿构造函数当指引用"，可构造函数写不出所有想要的映射 —— 推导指引是用户
//   改写映射规则的钩子。★ 它长得像函数但【不产生任何代码】：没有函数体、没有符号、
//   不参与重载决议，只参与 CTAD 那一次推导，用完即弃。
struct DeductionGuideDecl : Declaration {
    std::vector<TemplateParamPtr> templateParams;  // 指引自身的模板形参（可为空）
    std::string                guideName;       // 被指引的类模板名（如 "MyPtr"）
    std::vector<Parameter>     parameters;      // 指引的形参表（推导模式 P）
    std::vector<TypePtr>       targetArgs;      // `->` 右侧的实参（含模板形参）

    DeductionGuideDecl() : Declaration(NodeKind::DeductionGuide) {}

    void accept(AstVisitor& v) override { v.visit(*this); }
};
using DeductionGuideDeclPtr = std::shared_ptr<DeductionGuideDecl>;

struct TemplateDecl : Declaration {
    std::vector<std::string>   typeParams;     // 模板参数名列表（如 ["T", "N"]，向后兼容）
    std::vector<TemplateParamPtr> templateParams; // 结构化模板形参列表（含类型/非类型区分）
    ClassDeclPtr               classTemplate;  // 类模板蓝图（与 funcTemplate 互斥）
    FuncDeclPtr                funcTemplate;   // 函数模板蓝图（S1+）
    TypeAliasDeclPtr           aliasTemplate;  // 别名模板蓝图：template<T> using X = ...;
    DeductionGuideDeclPtr      guide;          // 推导指引蓝图：template<T> X(T) -> X<T>;

    // ── 特化支持（[temp.class.spec] / [temp.expl.spec]）──
    // specKind    : 本声明是主模板 / 偏特化 / 全特化
    // specPattern : 特化形参模式 = 模板名后尖括号里的那串实参；主模板无尖括号 ⇒ 空
    //   Box<T*, T>     ⇒ [T*, T]      （含模板参数 ⇒ 偏特化；pattern[0]=Pointer(T)）
    //   Box<int*, int> ⇒ [int*, int]  （全具体 ⇒ 全特化）
    //   enable_if<true, T> ⇒ [true, T] ★ 非类型模式位
    // [temp.class.spec] 不区分模式位的形态 —— 类型与值都能出现在尖括号里，
    // 故元素是 TemplateArg（tagged）而非裸 TypePtr。此前只收类型，导致
    // `enable_if<true, T>` 这类"用 NTTP 选中特化"的惯用法根本写不出来。
    TemplateSpecKind           specKind = TemplateSpecKind::Primary;
    std::vector<TemplateArg>   specPattern;

    TemplateDecl() : Declaration(NodeKind::Template) {}

    bool isPrimary()      const { return specKind == TemplateSpecKind::Primary; }
    bool isPartialSpec()  const { return specKind == TemplateSpecKind::PartialSpec; }
    bool isExplicitSpec() const { return specKind == TemplateSpecKind::ExplicitSpec; }
    bool isSpecialization() const { return specKind != TemplateSpecKind::Primary; }

    bool isClassTemplate()    const { return classTemplate != nullptr; }
    bool isFunctionTemplate() const { return funcTemplate != nullptr; }
    // ── 别名模板 [temp.alias] ──
    // template<class T> using Vec = MyPtr<T>;
    // ★ 别名【不是新类型】，只是既有类型的另一个名字：Vec<int> 和 MyPtr<int> 是同一个
    //   类型，不产生包装类、不产生新符号 —— 故它没有"实例化"，只有"替换"（换掉形参）。
    // clang 对照：TypeAliasTemplateDecl + AliasTemplateSpecializationType（为诊断保留
    //   一层 sugar，本实现直接解糖到最终类型）。
    bool isAliasTemplate()    const { return aliasTemplate != nullptr; }
    // ── 推导指引（[temp.deduct.guide]）──
    // 【它不是"模板"】借用 TemplateDecl 只因语法上共用 `template<...>` 前缀外壳；指引没有
    //   实体、没有符号、不被实例化 —— 它只是 CTAD 可问到的一条"映射规则"（故也有
    //   templateParams）。
    bool isDeductionGuide()   const { return guide != nullptr; }
    // ── 成员模板（[temp.mem]）──
    // 与自由函数模板同构（funcTemplate 非空），区别只在"出生地"：它定义在类体内、
    // 实例化出的函数带 ownerClassName（有隐式 this）。调用点由 Sema 在对象的类里
    // 查到它，再走与自由函数模板完全相同的推导 + 实例化路径。
    bool isMemberTemplate = false;
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
// demo: 含 1 函数 + 1 类的源文件 ⇒ TranslationUnit{ declarations=[FunctionDecl, ClassDecl] }
struct TranslationUnit {
    std::vector<DeclPtr> declarations;  // 所有顶层声明
};

} // namespace minicc
