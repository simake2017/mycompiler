#pragma once
// =============================================================================
// 阶段 3：语义分析与类型系统 (Semantic Analysis)
// =============================================================================
// 核心职责：
//   1. 构建符号表（Symbol Table）：记录每个变量、函数、类的类型信息
//   2. 类型检查：确保所有操作在类型上合法
//   3. auto 类型推导：将 auto 占位符替换为真实类型
//   4. 类的内存布局计算：字段偏移量、vtable 结构、RTTI 注入
//   5. 虚函数表构建：为每个含虚函数的类生成 vtable 结构
//
// 哲学：编译期看"符号"（类型名、字段名），运行期看"偏移量"（内存布局）。
// 此阶段的任务就是将"符号"转化为"偏移量"。
// =============================================================================
// 管线位置
// =============================================================================
//   源码 → Preprocessor → Lexer → Parser → 【SemanticAnalyzer】 → 模板推导/
//                                            实例化 → CodeGen(x86-64 .s)
//   输入：TranslationUnit（AST，此时类型只是语法标记，名字均未决议）
//   输出：① 标注 resolvedType 的 AST（auto 已抹去，每个表达式类型已定）
//         ② 符号表快照（Scope 作用域链 + 栈偏移，可 dump 观察）
//         ③ 类布局表（字段偏移 / vtable / RTTI，CodeGen 直接消费）
//
// 理论背景（C++ 标准章节 → 本头文件对应物）
//   [basic.scope]   符号表与作用域链：名字从最内层作用域逐层向外解析
//                   （Scope / SymbolTable）
//   [expr]          类型检查与值类别：每个表达式有类型与左/右值性
//                   （SemanticAnalyzer::inferType 系列）
//   [over.match]    重载决议：候选 → 可行 → 最优
//                   （resolveTemplateCall / isAtLeastAsSpecialized）
//   [temp.names]    两阶段查找（简化）：模板蓝图先注册不查体，实例化后才
//                   用具体类型检查函数体
//
// clang 模块对照（教学级简化）
//   lib/Sema/SemaDecl.cpp      声明处理      → processClassDecl / registerFunction
//   lib/Sema/SemaExpr.cpp      表达式类型检查 → inferType 系列 / Scope::lookup
//   lib/Sema/SemaOverload.cpp  重载决议与偏序 → resolveTemplateCall /
//                                              isAtLeastAsSpecialized
// =============================================================================

#include "ast.h"
#include "type.h"
#include "template_instantiation.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// SymbolKind：符号的种类
// ─────────────────────────────────────────────────────────────────────────────
// 作用域中同一个名字可能对应不同实体（变量 x / 函数 x / 类型 X），
// kind 告诉查找方如何解释这条符号、允许怎样的使用方式。
// 示例：`int x = 1;` 入表 → { name="x", kind=Variable, type=int, stack@-8 }
//       `int f(int a)` 入表 → { name="f", kind=Function, type=int(返回类型) }
// 注：ClassField / ClassMethod 为预留种类——本实现中类成员不直接进 Scope，
//     字段挂在 ClassLayout.fields，方法挂在 m_classDecls（见 SemanticAnalyzer）。
enum class SymbolKind : uint8_t {
    Variable,         // 变量
    Parameter,        // 函数参数
    Function,         // 函数
    FunctionTemplate, // 函数模板（S1+：蓝图，调用时才推导实例化）
    ClassField,       // 类字段
    ClassMethod,      // 类方法
    Type,             // 类型名
};

const char* symbolKindName(SymbolKind k);

// ─────────────────────────────────────────────────────────────────────────────
// Symbol：符号表中的条目
// ─────────────────────────────────────────────────────────────────────────────
// 一条 Symbol 回答三个问题：这个名字是什么（kind）、什么类型（type）、
// 数据在哪里（局部 → stackOffset；全局/成员 → 由布局表或 CodeGen 决定）。
// 示例：函数体内 `int x = 42;` 写入的条目：
//   { name="x", type=int, kind=Variable, isLocal=true, stackOffset=-8 }
// 示例：成员函数注册时隐式加入的 this：
//   { name="this", type=Animal*, kind=Parameter, isLocal=true, stackOffset=-8 }
struct Symbol {
    std::string  name;
    TypePtr      type;
    SymbolKind   kind = SymbolKind::Variable;
    bool         isLocal = true;         // 是否是局部变量
    int          stackOffset = 0;        // 在栈帧中的偏移量（局部变量）
    std::string  ownerClass;             // 所属类名（如果是类成员）
    SourceLocation definedAt;            // 定义位置
};

// ─────────────────────────────────────────────────────────────────────────────
// Scope：作用域（支持嵌套）
// ─────────────────────────────────────────────────────────────────────────────
// 符号表是编译器在"编译期"记住所有名字及其含义的核心数据结构。
// 每个作用域有一个符号表，作用域可以嵌套（函数内的代码块）。
// 查找符号时，从当前作用域向外逐层搜索，直到找到或到达全局作用域。
//
// 理论依据：[basic.scope] 作用域树 + [basic.scope.scope] 名字可见性规则。
// 作用域链 ASCII 图（f 内又有一个 `{ }` 块时）：
//
//     global(depth=0, "global")
//        ↑ parent
//     f(depth=1, "f")          ← 函数作用域 [basic.scope.function]
//        ↑ parent
//     block(depth=2, "block")  ← 块作用域   [basic.scope.block]
//
// 在 block 层 lookup("t")：block ✗ → f ✓ 命中（由内向外）
// 在 f     层 lookup("u")（u 声明于 block 内）：✗ —— 可见性单向，外不见内
// ─────────────────────────────────────────────────────────────────────────────
class Scope {
public:
    explicit Scope(Scope* parent = nullptr, int depth = 0,
                   const std::string& name = "")
        : m_parent(parent), m_depth(depth), m_name(name) {}

    // 向本层插入一条符号。若本层已有同名符号返回 false（重声明检查，
    // [basic.scope.scope]：同一作用域内不得重复声明同一名字），
    // 调用方（processVarDecl）据此报 "already declared"
    bool define(const std::string& name, Symbol sym);
    // 沿作用域链从本层逐层向外找名字，命中即返回，直到全局作用域为止
    Symbol* lookup(const std::string& name);
    // 只查本层、不上链（需要"仅限同一作用域"语义时使用）
    Symbol* lookupLocal(const std::string& name);
    Scope* parent() { return m_parent; }
    int depth() const { return m_depth; }
    const std::string& name() const { return m_name; }

    // 获取本作用域中的所有符号（用于 dump）
    const std::unordered_map<std::string, Symbol>& symbols() const {
        return m_symbols;
    }

    // dump 当前作用域的符号表
    void dump(int indent = 0) const;

private:
    // 父作用域指针（链首为全局作用域，其 parent 为 nullptr）
    Scope* m_parent;
    // 嵌套深度：global=0，函数=1，函数内块=2……（用于日志缩进）
    int    m_depth;
    // 作用域名（"global" / 函数名 / "block" / "if-then" 等，仅用于 dump）
    std::string m_name;
    // 本层符号表：名字 → Symbol（哈希表，O(1) 本层查找）
    std::unordered_map<std::string, Symbol> m_symbols;
};

// ─────────────────────────────────────────────────────────────────────────────
// SymbolTable：全局符号表管理器
// ─────────────────────────────────────────────────────────────────────────────
// 管理所有层级的作用域，追踪符号的注册和查找过程。
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    // 构造时创建全局作用域（作用域链的根，depth=0，名字 "global"）
    SymbolTable();

    // 压入新作用域：新 Scope 的 parent 指向当前作用域，depth+1。
    // 时机：进入函数（analyzeFunctionBody）、进入 { } 块（processBlockStmt）、
    //       if/while 分支
    void enterScope(const std::string& name = "");
    // 弹回父作用域：子作用域中声明的名字从此不可见
    // （Scope 对象本身保留在 m_allScopes 中，供 dump/调试）
    void exitScope();

    // 委托当前作用域：在当前层插入符号
    bool define(const std::string& name, Symbol sym);
    // 委托当前作用域：从当前层沿链向外查找（见 Scope::lookup）
    Symbol* lookup(const std::string& name);

    Scope* currentScope() { return m_currentScope; }
    int currentDepth() const { return m_currentDepth; }

    // dump 整个符号表（从全局到当前）
    void dump() const;

    // dump 当前作用域
    void dumpCurrentScope() const;

    // 获取全局作用域
    Scope* globalScope() { return m_globalScope.get(); }

private:
    // 全局作用域（唯一由 unique_ptr 直接持有；其余存 m_allScopes）
    std::unique_ptr<Scope> m_globalScope;
    // 指向当前所在作用域（enterScope/exitScope 移动此指针）
    Scope* m_currentScope;
    // 当前嵌套深度（= m_currentScope->depth()，冗余保存便于日志）
    int    m_currentDepth = 0;

    // 保存所有创建的作用域（防止内存泄漏）
    std::vector<std::unique_ptr<Scope>> m_allScopes;
};

// ─────────────────────────────────────────────────────────────────────────────
// SemanticAnalyzer：语义分析器
// ─────────────────────────────────────────────────────────────────────────────
// 三遍扫描（见 analyze）：注册类/模板 → 注册函数名 → 分析函数体。
// 产物供后续阶段直接消费：
//   TemplateInstantiation ← getTemplates()（模板蓝图）
//   TemplateDeducer(S2~S6) ← getFunctionTemplateCandidates()（候选集）
//   CodeGen               ← getFunctions() + getClassTypes()（布局表）
//
// 设计决策——"名字→单符号"教学模型：
//   Scope/SymbolTable 保持一个名字只对应一个符号（不支持重载的符号表），
//   真正的重载集（函数模板候选集）单独挂在 m_functionTemplateCandidates。
//   对照 clang：普通名字查找走 DeclContext::lookup，重载集是挂在
//   DeclContext 上的 Decl 链，二者也是分开的。
class SemanticAnalyzer {
public:
    SemanticAnalyzer();

    // 分析整个编译单元
    void analyze(TranslationUnit& unit);

    // 获取所有类的布局信息（供代码生成阶段使用）
    const std::unordered_map<std::string, TypePtr>& getClassTypes() const {
        return m_classTypes;
    }

    // 获取所有函数（供代码生成阶段使用）
    const std::vector<FuncDeclPtr>& getFunctions() const {
        return m_functions;
    }

    // 获取模板声明（供模板实例化阶段使用）
    const std::vector<TemplateDeclPtr>& getTemplates() const {
        return m_templates;
    }

    // 获取函数模板候选集（供 S2+ 实参推导使用）
    const std::unordered_map<std::string, std::vector<TemplateDeclPtr>>&
    getFunctionTemplateCandidates() const {
        return m_functionTemplateCandidates;
    }

    // 获取符号表（供调试输出）
    SymbolTable& getSymbolTable() { return m_symbolTable; }

private:
    // ── 符号表管理 ──
    SymbolTable m_symbolTable;

    // ── 全局注册表 ──
    std::unordered_map<std::string, TypePtr>      m_classTypes;    // 类名 → Type
    std::unordered_map<std::string, FuncDeclPtr>   m_functionMap;   // 函数名 → 声明
    std::vector<FuncDeclPtr>                       m_functions;     // 所有函数（按顺序）
    std::vector<TemplateDeclPtr>                   m_templates;     // 模板蓝图
    // 函数模板候选集（S1+）：函数名 → 同名函数模板列表。
    // 不进 Scope/SymbolTable（那里保持"名字→单符号"的教学模型），
    // 调用点的重载决议（S6）遍历此候选集做推导与排序。
    // 对照 clang：重载集挂在 DeclContext 上，而非普通名字查找表。
    std::unordered_map<std::string, std::vector<TemplateDeclPtr>>
                                                   m_functionTemplateCandidates;
    // ── 函数模板实例化（S5）──
    TemplateInstantiator m_instantiator;
    std::unordered_map<std::string, FuncDeclPtr> m_templateInstanceCache; // mangled 名 → 实例

    // 模板调用解析（S2~S6）：返回调用结果类型，失败返回 nullptr（交回原有报错路径）
    TypePtr resolveTemplateCall(const std::string& funcName,
                                std::shared_ptr<VarExpr> calleeVar,
                                const std::vector<TypePtr>& argTypes,
                                const std::vector<bool>& argIsLValue,
                                const std::vector<TypePtr>& explicitArgs,
                                SourceLocation loc);
    FuncDeclPtr getOrInstantiateFunction(TemplateDeclPtr tmpl,
                                         const std::vector<TypePtr>& args);
    // S6 偏序：a 是否至少与 b 同样特化（deduction-based，[temp.func.order] 简化）
    bool isAtLeastAsSpecialized(TemplateDeclPtr a, TemplateDeclPtr b);
    std::unordered_map<std::string, ClassDeclPtr>  m_classDecls;    // 类名 → 声明

    // ── 栈帧管理 ──
    int m_stackOffset = 0;  // 当前栈帧偏移量

    // ── 推导追踪 ──
    int m_inferDepth = 0;   // 类型推导嵌套深度（用于缩进输出）

    // ── 声明处理 ──
    // 顶层声明分发（按 Decl 动态类型派发到下面的具体处理器）
    void processDecl(DeclPtr decl);
    // 类注册：合并继承字段/vtable → 收集成员 → 布局 → 入符号表
    //（demo：Dog:Animal 继承时 vtable 槽位复用，见 cpp 实现处）
    void processClassDecl(ClassDeclPtr decl);
    // Pass 2：函数名 → 符号表 + mangled 名（先注册以支持递归/前向引用）
    void registerFunction(FuncDeclPtr decl);
    // Pass 3：进入函数作用域，注册参数/this，逐语句分析函数体
    void analyzeFunctionBody(FuncDeclPtr decl);
    // 模板蓝图注册（只存不查体——两阶段查找第一阶段 [temp.names] 简化）
    void processTemplateDecl(TemplateDeclPtr decl);

    // ── 语句处理 ──
    // 语句分发器（dynamic_pointer_cast 逐一尝试，教学版双分派）
    void processStmt(StmtPtr stmt);
    // { } 块 → 新建块作用域 [basic.scope.block]
    void processBlockStmt(std::shared_ptr<BlockStmt> block);
    // 变量声明：auto 推导/类型检查/分配栈槽/入符号表
    void processVarDecl(std::shared_ptr<VarDeclStmt> decl);
    // if：条件须 bool|int（语境转换简化），分支各建作用域
    void processIfStmt(std::shared_ptr<IfStmt> stmt);
    // while：条件类型检查，循环体建作用域
    void processWhileStmt(std::shared_ptr<WhileStmt> stmt);
    // return：返回值与声明返回类型兼容性检查 [stmt.return]
    void processReturnStmt(std::shared_ptr<ReturnStmt> stmt);
    // 赋值：推导左右类型（教学级不查左值可赋值性）
    void processAssignStmt(std::shared_ptr<AssignStmt> stmt);
    // 表达式语句：推导类型、丢弃值（副作用语句，如函数调用）
    void processExprStmt(std::shared_ptr<ExprStmt> stmt);

    // ── 表达式类型推导 ──
    // 总入口：按节点动态类型分派到 inferXxx，结果写回 expr->resolvedType
    TypePtr inferType(ExprPtr expr);
    // 整数字面量 → int
    TypePtr inferIntLiteral(std::shared_ptr<IntLiteralExpr> expr);
    // 布尔字面量 → bool
    TypePtr inferBoolLiteral(std::shared_ptr<BoolLiteralExpr> expr);
    // 字符串字面量 → 指针类型（简化的 char* 表示）
    TypePtr inferStringLiteral(std::shared_ptr<StringLiteralExpr> expr);
    // nullptr → void*（简化）
    TypePtr inferNullptrLiteral(std::shared_ptr<NullptrLiteralExpr> expr);
    // 名字决议核心：作用域链 → 类字段 → 函数名，三级查找
    TypePtr inferVar(std::shared_ptr<VarExpr> expr);
    // 二元运算：比较→bool；算术→常用算术转换(int/double)简化
    TypePtr inferBinary(std::shared_ptr<BinaryExpr> expr);
    // 一元运算：! → bool；- 保持类型
    TypePtr inferUnary(std::shared_ptr<UnaryExpr> expr);
    // 函数调用：记录实参左值性 → 普通函数 → 模板推导决议（S2~S6）
    TypePtr inferCall(std::shared_ptr<CallExpr> expr);
    // 成员访问 obj.x / p->x：字段查布局表(得偏移)，方法查类声明
    TypePtr inferMember(std::shared_ptr<MemberExpr> expr);
    // new C：类名存在性检查 → 返回 C*
    TypePtr inferNew(std::shared_ptr<NewExpr> expr);
    // this：仅限成员函数内，类型为属主类指针 [class.this]
    TypePtr inferThis(std::shared_ptr<ThisExpr> expr);

    // ── 类内存布局计算 ──
    // 逐字段对齐排布，算 offset/size/totalSize（_vptr 恒在偏移 0）
    void computeClassLayout(ClassDeclPtr decl);
    // 生成 RTTI 符号名(_ZTI 风格)并固化 vtable 槽位索引
    void injectVTableAndRTTI(TypePtr classType);
    // 向上取整到 alignment 的倍数：alignTo(12,8)=16
    uint32_t alignTo(uint32_t offset, uint32_t alignment);

    // ── 推导辅助 ──
    std::string inferIndent() const;
    [[noreturn]] void error(const std::string& msg, SourceLocation loc);

    // ── 当前上下文 ──
    std::string m_currentClassName;   // 当前正在处理的类名
    TypePtr     m_currentReturnType;  // 当前函数的返回类型
    std::string m_currentFuncName;    // 当前函数名
};

} // namespace minicc
