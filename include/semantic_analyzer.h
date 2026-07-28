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

#include "ast.h"
#include "type.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// SymbolKind：符号的种类
// ─────────────────────────────────────────────────────────────────────────────
enum class SymbolKind : uint8_t {
    Variable,       // 变量
    Parameter,      // 函数参数
    Function,       // 函数
    ClassField,     // 类字段
    ClassMethod,    // 类方法
    Type,           // 类型名
};

const char* symbolKindName(SymbolKind k);

// ─────────────────────────────────────────────────────────────────────────────
// Symbol：符号表中的条目
// ─────────────────────────────────────────────────────────────────────────────
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
// ─────────────────────────────────────────────────────────────────────────────
class Scope {
public:
    explicit Scope(Scope* parent = nullptr, int depth = 0,
                   const std::string& name = "")
        : m_parent(parent), m_depth(depth), m_name(name) {}

    bool define(const std::string& name, Symbol sym);
    Symbol* lookup(const std::string& name);
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
    Scope* m_parent;
    int    m_depth;
    std::string m_name;
    std::unordered_map<std::string, Symbol> m_symbols;
};

// ─────────────────────────────────────────────────────────────────────────────
// SymbolTable：全局符号表管理器
// ─────────────────────────────────────────────────────────────────────────────
// 管理所有层级的作用域，追踪符号的注册和查找过程。
// ─────────────────────────────────────────────────────────────────────────────
class SymbolTable {
public:
    SymbolTable();

    void enterScope(const std::string& name = "");
    void exitScope();

    bool define(const std::string& name, Symbol sym);
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
    std::unique_ptr<Scope> m_globalScope;
    Scope* m_currentScope;
    int    m_currentDepth = 0;

    // 保存所有创建的作用域（防止内存泄漏）
    std::vector<std::unique_ptr<Scope>> m_allScopes;
};

// ─────────────────────────────────────────────────────────────────────────────
// SemanticAnalyzer：语义分析器
// ─────────────────────────────────────────────────────────────────────────────
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
    std::unordered_map<std::string, ClassDeclPtr>  m_classDecls;    // 类名 → 声明

    // ── 栈帧管理 ──
    int m_stackOffset = 0;  // 当前栈帧偏移量

    // ── 推导追踪 ──
    int m_inferDepth = 0;   // 类型推导嵌套深度（用于缩进输出）

    // ── 声明处理 ──
    void processDecl(DeclPtr decl);
    void processClassDecl(ClassDeclPtr decl);
    void registerFunction(FuncDeclPtr decl);
    void analyzeFunctionBody(FuncDeclPtr decl);
    void processTemplateDecl(TemplateDeclPtr decl);

    // ── 语句处理 ──
    void processStmt(StmtPtr stmt);
    void processBlockStmt(std::shared_ptr<BlockStmt> block);
    void processVarDecl(std::shared_ptr<VarDeclStmt> decl);
    void processIfStmt(std::shared_ptr<IfStmt> stmt);
    void processWhileStmt(std::shared_ptr<WhileStmt> stmt);
    void processReturnStmt(std::shared_ptr<ReturnStmt> stmt);
    void processAssignStmt(std::shared_ptr<AssignStmt> stmt);
    void processExprStmt(std::shared_ptr<ExprStmt> stmt);

    // ── 表达式类型推导 ──
    TypePtr inferType(ExprPtr expr);
    TypePtr inferIntLiteral(std::shared_ptr<IntLiteralExpr> expr);
    TypePtr inferBoolLiteral(std::shared_ptr<BoolLiteralExpr> expr);
    TypePtr inferStringLiteral(std::shared_ptr<StringLiteralExpr> expr);
    TypePtr inferNullptrLiteral(std::shared_ptr<NullptrLiteralExpr> expr);
    TypePtr inferVar(std::shared_ptr<VarExpr> expr);
    TypePtr inferBinary(std::shared_ptr<BinaryExpr> expr);
    TypePtr inferUnary(std::shared_ptr<UnaryExpr> expr);
    TypePtr inferCall(std::shared_ptr<CallExpr> expr);
    TypePtr inferMember(std::shared_ptr<MemberExpr> expr);
    TypePtr inferNew(std::shared_ptr<NewExpr> expr);
    TypePtr inferThis(std::shared_ptr<ThisExpr> expr);

    // ── 类内存布局计算 ──
    void computeClassLayout(ClassDeclPtr decl);
    void injectVTableAndRTTI(TypePtr classType);
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
