// =============================================================================
// tests/unit/obs_helpers.h —— 单测观测工具（各阶段日志捕获 + AST 递归嵌套打印）
// =============================================================================
// 供 tests/unit/test_*.cpp 共享，观测风格对齐 test_preprocessor.cpp：
//   ① StdoutCapture        捕获 Lexer/Parser/Sema 各阶段的 std::cout 中文日志流水
//   ② explainPipelineLine  给每行阶段日志附中文解释
//   ③ dumpExprTree/Stmt    递归嵌套打印 AST（├─/└─ 树形），把递归下降解析
//                          的递归过程（嵌套结构）直接可视化
//   ④ dumpWithExplanation  总输出可视化 box：[输入] → [trace+解释] → [输出]
// =============================================================================
#pragma once

#include "ast.h"
#include "type.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace minicc {

// 捕获各阶段通过 std::cout 写出的日志流水。
// 实现：把 std::cout 重定向到临时 stringstream，析构时恢复。
class StdoutCapture {
    std::streambuf* old_;
    std::stringstream buf_;
public:
    StdoutCapture() : old_(nullptr), buf_() { old_ = std::cout.rdbuf(buf_.rdbuf()); }
    ~StdoutCapture() { std::cout.rdbuf(old_); }
    std::string str() const { return buf_.str(); }
};

// 给一行阶段日志加中文注释（[parse] / [sema] 流水）。匹配不上返回空串。
inline std::string explainPipelineLine(const std::string& line) {
    if (line.find("[parse:type]") != std::string::npos) {
        if (line.find("const prefix") != std::string::npos)
            return "识别 const 前缀（[dcl.type.cv]），推迟到最后再应用";
        if (line.find("suffix *") != std::string::npos)
            return "指针后缀：在基类型上叠一层 *（[dcl.ptr]）";
        if (line.find("suffix &&") != std::string::npos)
            return "右值引用后缀 &&（[dcl.ref]）";
        if (line.find("suffix &") != std::string::npos)
            return "左值引用后缀 &（[dcl.ref]）";
        if (line.find("const applied") != std::string::npos)
            return "const 应用到完整类型上";
        if (line.find("★ final type") != std::string::npos)
            return "★ 类型解析完成，得到最终类型";
        if (line.find("base =") != std::string::npos)
            return "确定基类型（int/double/bool/void/auto/类名）";
        return "类型解析子过程";
    }
    if (line.find("[parse:template]") != std::string::npos)
        return "模板声明解析（[temp]）";
    if (line.find("[parse]") != std::string::npos) {
        if (line.find("template-id") != std::string::npos)
            return "识别模板实参列表 <...>（template-id，[temp.names]）";
        return "解析器流水";
    }
    if (line.find("┌─ Scope") != std::string::npos)
        return "进入新作用域（符号表压栈）";
    if (line.find("└─") != std::string::npos)
        return "离开作用域（符号表弹栈）";
    if (line.find("Pass 1") != std::string::npos)
        return "三遍扫描之一：注册类/模板/全局变量";
    if (line.find("Pass 2") != std::string::npos)
        return "三遍扫描之二：注册函数（支持递归调用）";
    if (line.find("Pass 3") != std::string::npos)
        return "三遍扫描之三：分析函数体（类型推导 + 名字决议）";
    if (line.find("[register]") != std::string::npos)
        return "登记符号到符号表";
    return "";
}

// ── AST 递归嵌套打印（核心可视化：递归下降解析的递归过程） ─────────────────

inline const char* binaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";   case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";   case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";   case BinaryOp::Eq:  return "==";
        case BinaryOp::Neq: return "!=";  case BinaryOp::Lt:  return "<";
        case BinaryOp::Gt:  return ">";   case BinaryOp::Le:  return "<=";
        case BinaryOp::Ge:  return ">=";  case BinaryOp::And: return "&&";
        case BinaryOp::Or:  return "||";
    }
    return "?";
}

// 附加语义阶段推导出的类型（阶段 2 后为空，阶段 3 后填充）
inline std::string typeSuffix(const ExprPtr& e) {
    if (e && e->resolvedType) return "  : " + e->resolvedType->toString();
    return "";
}

inline void dumpExprTree(const ExprPtr& e, const std::string& prefix, bool isLast);

// 通用：打印节点自身标签后递归其子节点（嵌套递归过程的核心）
inline void dumpExprNode(const ExprPtr& e, const std::string& prefix, bool isLast,
                         const std::string& label,
                         const std::vector<std::pair<std::string, ExprPtr>>& children) {
    std::printf("%s%s─ %s%s\n", prefix.c_str(), isLast ? "└" : "├",
                label.c_str(), typeSuffix(e).c_str());
    std::string childPrefix = prefix + (isLast ? "   " : "│  ");
    for (size_t i = 0; i < children.size(); ++i) {
        const auto& [role, child] = children[i];
        if (!child) {
            std::printf("%s%s─ %s: (空)\n", childPrefix.c_str(),
                        i + 1 == children.size() ? "└" : "├", role.c_str());
            continue;
        }
        dumpExprTree(child, childPrefix, i + 1 == children.size());
    }
}

inline void dumpExprTree(const ExprPtr& e, const std::string& prefix, bool isLast) {
    if (!e) { std::printf("%s%s─ (空)\n", prefix.c_str(), isLast ? "└" : "├"); return; }
    switch (e->kind) {
    case NodeKind::IntLiteral: {
        auto n = std::static_pointer_cast<IntLiteralExpr>(e);
        dumpExprNode(e, prefix, isLast, "IntLiteral(" + std::to_string(n->value) + ")", {});
        break;
    }
    case NodeKind::BoolLiteral: {
        auto n = std::static_pointer_cast<BoolLiteralExpr>(e);
        dumpExprNode(e, prefix, isLast, std::string("BoolLiteral(") + (n->value ? "true" : "false") + ")", {});
        break;
    }
    case NodeKind::StringLiteral: {
        auto n = std::static_pointer_cast<StringLiteralExpr>(e);
        dumpExprNode(e, prefix, isLast, "StringLiteral(\"" + n->value + "\")", {});
        break;
    }
    case NodeKind::NullptrLiteral:
        dumpExprNode(e, prefix, isLast, "NullptrLiteral", {});
        break;
    case NodeKind::Var: {
        auto n = std::static_pointer_cast<VarExpr>(e);
        std::string label = "Var(" + n->name + ")";
        if (!n->explicitTemplateArgs.empty())
            label += "  ⟨显式模板实参 ×" + std::to_string(n->explicitTemplateArgs.size()) + "⟩";
        dumpExprNode(e, prefix, isLast, label, {});
        break;
    }
    case NodeKind::Binary: {
        auto n = std::static_pointer_cast<BinaryExpr>(e);
        // 优先级爬升的直接体现：Add 的 right 是更深一层的 Mul —— 嵌套即递归
        dumpExprNode(e, prefix, isLast,
                     std::string("Binary(") + binaryOpName(n->op) + ")",
                     {{"left", n->left}, {"right", n->right}});
        break;
    }
    case NodeKind::Unary: {
        auto n = std::static_pointer_cast<UnaryExpr>(e);
        dumpExprNode(e, prefix, isLast,
                     std::string("Unary(") + (n->op == UnaryOp::Neg ? "-" : "!") + ")",
                     {{"operand", n->operand}});
        break;
    }
    case NodeKind::Call: {
        auto n = std::static_pointer_cast<CallExpr>(e);
        std::vector<std::pair<std::string, ExprPtr>> children;
        children.emplace_back("callee", n->callee);
        for (size_t i = 0; i < n->arguments.size(); ++i)
            children.emplace_back("arg[" + std::to_string(i) + "]", n->arguments[i]);
        dumpExprNode(e, prefix, isLast,
                     "Call(" + std::to_string(n->arguments.size()) + " 个实参)", children);
        break;
    }
    case NodeKind::Member: {
        auto n = std::static_pointer_cast<MemberExpr>(e);
        dumpExprNode(e, prefix, isLast,
                     std::string("Member(") + (n->isArrow ? "->" : ".") + n->memberName + ")",
                     {{"object", n->object}});
        break;
    }
    case NodeKind::New: {
        auto n = std::static_pointer_cast<NewExpr>(e);
        std::vector<std::pair<std::string, ExprPtr>> children;
        for (size_t i = 0; i < n->constructorArgs.size(); ++i)
            children.emplace_back("ctor-arg[" + std::to_string(i) + "]", n->constructorArgs[i]);
        dumpExprNode(e, prefix, isLast, "New(" + n->className + ")", children);
        break;
    }
    case NodeKind::This:
        dumpExprNode(e, prefix, isLast, "This", {});
        break;
    case NodeKind::Delete: {
        auto n = std::static_pointer_cast<DeleteExpr>(e);
        dumpExprNode(e, prefix, isLast, n->isArray ? "Delete[]" : "Delete",
                     {{"pointer", n->pointerExpr}});
        break;
    }
    default:
        dumpExprNode(e, prefix, isLast, "<未知表达式节点>", {});
        break;
    }
}

// 语句级递归打印（用于完整函数体 / 语句层测试）
inline void dumpStmtTree(const StmtPtr& s, const std::string& prefix, bool isLast) {
    if (!s) return;
    auto say = [&](const std::string& label) {
        std::printf("%s%s─ %s\n", prefix.c_str(), isLast ? "└" : "├", label.c_str());
    };
    std::string childPrefix = prefix + (isLast ? "   " : "│  ");
    switch (s->kind) {
    case NodeKind::VarDecl: {
        auto n = std::static_pointer_cast<VarDeclStmt>(s);
        say("VarDecl(" + n->name + " : " +
            (n->declaredType ? n->declaredType->toString() : "?") + ")");
        if (n->initializer) dumpExprTree(n->initializer, childPrefix, true);
        break;
    }
    case NodeKind::Assign: {
        auto n = std::static_pointer_cast<AssignStmt>(s);
        say("Assign");
        if (n->target) dumpExprTree(n->target, childPrefix, !n->value);
        if (n->value)  dumpExprTree(n->value, childPrefix, true);
        break;
    }
    case NodeKind::Return: {
        auto n = std::static_pointer_cast<ReturnStmt>(s);
        say("Return");
        if (n->value) dumpExprTree(n->value, childPrefix, true);
        break;
    }
    case NodeKind::ExprStmt: {
        auto n = std::static_pointer_cast<ExprStmt>(s);
        say("ExprStmt");
        if (n->expr) dumpExprTree(n->expr, childPrefix, true);
        break;
    }
    case NodeKind::DeleteStmt: {
        auto n = std::static_pointer_cast<DeleteStmt>(s);
        say(n->isArray ? "DeleteStmt[]" : "DeleteStmt");
        if (n->pointerExpr) dumpExprTree(n->pointerExpr, childPrefix, true);
        break;
    }
    case NodeKind::Block: {
        auto n = std::static_pointer_cast<BlockStmt>(s);
        say("Block(" + std::to_string(n->statements.size()) + " 条语句)");
        for (size_t i = 0; i < n->statements.size(); ++i)
            dumpStmtTree(n->statements[i], childPrefix, i + 1 == n->statements.size());
        break;
    }
    default:
        say("<语句>");
        break;
    }
}

// ── 总输出：对齐 test_preprocessor.cpp 的 dumpWithExplanation 可视化 box ────
//   what   : 本用例主题（显示在块标题）
//   input  : 被测表达式/源码片段
//   trace  : StdoutCapture 捕获到的各阶段日志流水
//   dumper : 「输出」块打印回调（嵌套递归打印 AST 树）
inline void dumpWithExplanation(const char* what, const std::string& input,
                                const std::string& trace,
                                const std::function<void()>& dumper) {
    std::printf("\n=================================================================\n");
    std::printf("── %s\n", what);
    std::printf("=================================================================\n");
    std::printf("[输入]\n%s\n", input.c_str());
    std::printf("-----------------------------------------------------------------\n");
    std::printf("[各阶段 trace（每行日志后附中文解释）]\n");
    std::printf("=================================================================\n");
    size_t pos = 0;
    while (pos < trace.size()) {
        auto eol = trace.find('\n', pos);
        std::string line = trace.substr(pos, (eol == std::string::npos ? std::string::npos : eol - pos));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            std::string hint = explainPipelineLine(line);
            if (!hint.empty()) std::printf("      %s   # %s\n", line.c_str(), hint.c_str());
            else               std::printf("      %s\n", line.c_str());
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    std::printf("=================================================================\n");
    std::printf("[输出：AST 递归嵌套打印]\n");
    dumper();
    std::printf("=================================================================\n\n");
}

} // namespace minicc
