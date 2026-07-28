#pragma once
// =============================================================================
// 阶段 5：代码生成器 (Code Generator)
// =============================================================================
// 核心职责：将树状的 AST "拍平"为线性的 x86-64 汇编指令。
//
// 这是"运行期看偏移量"的最终体现：
//   - obj.field   → [rbp + offset]（直接用数字偏移量访问内存）
//   - ptr->vfunc() → 三部曲：读 vptr → 加偏移 → 跳转
//   - 类型和字段名在此阶段彻底消失，只剩下地址和数字
//
// 目标架构：x86-64 (System V AMD64 ABI)
// 输出格式：AT&T 语法的汇编文件（.s）
// =============================================================================

#include "ast.h"
#include "type.h"
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace minicc {

class CodeGen {
public:
    CodeGen();

    // 生成整个编译单元的汇编代码
    std::string generate(const TranslationUnit& unit,
                         const std::unordered_map<std::string, TypePtr>& classTypes,
                         const std::vector<FuncDeclPtr>& functions);

private:
    std::ostringstream m_code;      // .text 段（代码）
    std::ostringstream m_data;      // .data 段（全局数据）
    std::ostringstream m_rodata;    // .rodata 段（只读数据）
    int                m_labelCounter = 0;

    // 当前函数的局部变量偏移量追踪
    std::unordered_map<std::string, int> m_localVars;
    int m_currentStackOffset = 0;

    // 当前类上下文
    std::string m_currentClassName;
    TypePtr     m_currentClassType;
    const std::unordered_map<std::string, TypePtr>* m_classTypes = nullptr;

    // ── 顶层生成 ──
    void emitFunction(FuncDeclPtr func);
    void emitVTable(const std::string& className, TypePtr classType);
    void emitRTTI(const std::string& className, TypePtr classType);
    void emitStringLiterals();

    // ── 语句生成 ──
    void emitStmt(StmtPtr stmt);
    void emitBlockStmt(std::shared_ptr<BlockStmt> block);
    void emitVarDecl(std::shared_ptr<VarDeclStmt> decl);
    void emitAssign(std::shared_ptr<AssignStmt> stmt);
    void emitReturn(std::shared_ptr<ReturnStmt> stmt);
    void emitIf(std::shared_ptr<IfStmt> stmt);
    void emitWhile(std::shared_ptr<WhileStmt> stmt);
    void emitExprStmt(std::shared_ptr<ExprStmt> stmt);

    // ── 表达式生成 ──
    // 每个 emit 函数将表达式的值计算到 rax 寄存器中
    void emitExpr(ExprPtr expr);
    void emitIntLiteral(std::shared_ptr<IntLiteralExpr> expr);
    void emitBoolLiteral(std::shared_ptr<BoolLiteralExpr> expr);
    void emitStringLiteral(std::shared_ptr<StringLiteralExpr> expr);
    void emitVar(std::shared_ptr<VarExpr> expr);
    void emitBinary(std::shared_ptr<BinaryExpr> expr);
    void emitUnary(std::shared_ptr<UnaryExpr> expr);
    void emitCall(std::shared_ptr<CallExpr> expr);
    void emitMember(std::shared_ptr<MemberExpr> expr);
    void emitNew(std::shared_ptr<NewExpr> expr);
    void emitThis(std::shared_ptr<ThisExpr> expr);

    // ── 虚函数调用（核心！） ──
    // ptr->vfunc(args) 的汇编三部曲：
    //   (a) 从 ptr 读出 _vptr 机器地址
    //   (b) 加上虚函数在表中的 Index 偏移量
    //   (c) 跳转至该地址执行
    void emitVirtualCall(const std::string& className,
                         const std::string& methodName,
                         const std::vector<ExprPtr>& args,
                         uint32_t vtableIndex);

    // ── 辅助 ──
    std::string newLabel(const std::string& prefix = "L");
    void emit(const std::string& line);
    void emitData(const std::string& line);
    void emitRodata(const std::string& line);
    void emitComment(const std::string& comment);

    // 字符串字面量收集
    std::vector<std::pair<std::string, std::string>> m_stringLiterals; // label → value
    std::string addStringLiteral(const std::string& value);
};

} // namespace minicc
