// =============================================================================
// 阶段 3：语义分析器实现 —— 符号表构建、类型推导、内存布局
// =============================================================================
// 这是编译器中最核心、最精彩的阶段。它做了四件关键的事：
//
// 1. 【符号表构建】为每个名字（变量、函数、类）建立类型映射
// 2. 【符号决议】在表达式中查找名字，绑定到对应的符号
// 3. 【auto 推导】把 auto 占位符"当场抹去"，替换为真实类型
// 4. 【内存布局】计算每个类的字段偏移量，注入 _vptr 和 RTTI
//
// "编译期看符号，运行期看偏移量"——此阶段就是完成这个转换的分水岭。
// =============================================================================

#include "semantic_analyzer.h"
#include <format>
#include <iostream>
#include <algorithm>
#include <cassert>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// SymbolKind 名称
// ─────────────────────────────────────────────────────────────────────────────
const char* symbolKindName(SymbolKind k) {
    switch (k) {
        case SymbolKind::Variable:    return "Variable";
        case SymbolKind::Parameter:   return "Parameter";
        case SymbolKind::Function:    return "Function";
        case SymbolKind::ClassField:  return "ClassField";
        case SymbolKind::ClassMethod: return "ClassMethod";
        case SymbolKind::Type:        return "Type";
    }
    return "Unknown";
}

// ═════════════════════════════════════════════════════════════════════════════
// Scope 实现
// ═════════════════════════════════════════════════════════════════════════════
bool Scope::define(const std::string& name, Symbol sym) {
    auto [it, inserted] = m_symbols.emplace(name, std::move(sym));
    return inserted;
}

Symbol* Scope::lookup(const std::string& name) {
    auto it = m_symbols.find(name);
    if (it != m_symbols.end()) return &it->second;
    if (m_parent) return m_parent->lookup(name);
    return nullptr;
}

Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = m_symbols.find(name);
    return (it != m_symbols.end()) ? &it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scope::dump：打印当前作用域中的所有符号
// ─────────────────────────────────────────────────────────────────────────────
void Scope::dump(int indent) const {
    std::string pad(indent * 2, ' ');
    std::cout << std::format("{}┌─ Scope[{}] '{}' (depth={}) ─────────────────\n",
        pad, m_depth, m_name.empty() ? "anonymous" : m_name, m_depth);

    if (m_symbols.empty()) {
        std::cout << std::format("{}│  (empty)\n", pad);
    }

    for (auto& [name, sym] : m_symbols) {
        std::string typeStr = sym.type ? sym.type->toString() : "?";
        std::string offsetStr = sym.isLocal
            ? std::format("  stack@{}", sym.stackOffset) : "";

        std::cout << std::format("{}│  {:<12} {:<10} {:<12}{}{}\n",
            pad,
            name,
            symbolKindName(sym.kind),
            typeStr,
            offsetStr,
            sym.ownerClass.empty() ? "" : std::format(" [{}]", sym.ownerClass));
    }

    std::cout << std::format("{}└──────────────────────────────────────\n", pad);
}

// ═════════════════════════════════════════════════════════════════════════════
// SymbolTable 实现
// ═════════════════════════════════════════════════════════════════════════════
SymbolTable::SymbolTable()
    : m_globalScope(std::make_unique<Scope>(nullptr, 0, "global"))
    , m_currentScope(m_globalScope.get())
    , m_currentDepth(0) {}

void SymbolTable::enterScope(const std::string& name) {
    m_currentDepth++;
    auto newScope = std::make_unique<Scope>(m_currentScope, m_currentDepth, name);
    m_currentScope = newScope.get();
    m_allScopes.push_back(std::move(newScope));
}

void SymbolTable::exitScope() {
    if (m_currentScope && m_currentScope->parent()) {
        m_currentScope = m_currentScope->parent();
        m_currentDepth--;
    }
}

bool SymbolTable::define(const std::string& name, Symbol sym) {
    return m_currentScope->define(name, std::move(sym));
}

Symbol* SymbolTable::lookup(const std::string& name) {
    return m_currentScope->lookup(name);
}

void SymbolTable::dump() const {
    std::cout << "\n  ╔══════════════════════════════════════════════╗\n";
    std::cout << "  ║         SYMBOL TABLE (符号表快照)            ║\n";
    std::cout << "  ╚══════════════════════════════════════════════╝\n";
    m_globalScope->dump(2);
    std::cout << "\n";
}

void SymbolTable::dumpCurrentScope() const {
    m_currentScope->dump(2);
}

// ═════════════════════════════════════════════════════════════════════════════
// SemanticAnalyzer 构造
// ═════════════════════════════════════════════════════════════════════════════
SemanticAnalyzer::SemanticAnalyzer() = default;

// ─────────────────────────────────────────────────────────────────────────────
// 推导缩进辅助
// ─────────────────────────────────────────────────────────────────────────────
std::string SemanticAnalyzer::inferIndent() const {
    return std::string(m_inferDepth * 2, ' ');
}

// ─────────────────────────────────────────────────────────────────────────────
// 分析入口：三遍扫描
// ─────────────────────────────────────────────────────────────────────────────
// 第一遍：收集类和模板声明 → 建立类型注册表
// 第二遍：注册所有函数名 → 支持递归调用
// 第三遍：分析函数体 → 类型检查、auto 推导
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::analyze(TranslationUnit& unit) {

    std::cout << "\n  ┌──── Pass 1: 注册类与模板 ────────────────────────\n";
    for (auto& decl : unit.declarations) {
        if (auto cls = std::dynamic_pointer_cast<ClassDecl>(decl)) {
            processClassDecl(cls);
        } else if (auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(decl)) {
            processTemplateDecl(tmpl);
        }
    }

    std::cout << "\n  ┌──── Pass 2: 注册所有函数（支持递归）──────────────\n";
    for (auto& decl : unit.declarations) {
        if (auto func = std::dynamic_pointer_cast<FunctionDecl>(decl)) {
            registerFunction(func);
        } else if (auto cls = std::dynamic_pointer_cast<ClassDecl>(decl)) {
            for (auto& method : cls->methods) {
                registerFunction(method);
            }
        }
    }

    // dump 全局符号表
    m_symbolTable.dump();

    std::cout << "  ┌──── Pass 3: 分析函数体（类型推导 + 符号决议）────\n";
    for (auto& decl : unit.declarations) {
        if (auto func = std::dynamic_pointer_cast<FunctionDecl>(decl)) {
            analyzeFunctionBody(func);
        } else if (auto cls = std::dynamic_pointer_cast<ClassDecl>(decl)) {
            for (auto& method : cls->methods) {
                analyzeFunctionBody(method);
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 类声明处理
// ═════════════════════════════════════════════════════════════════════════════
void SemanticAnalyzer::processClassDecl(ClassDeclPtr decl) {
    std::cout << std::format("  [register] class '{}' ", decl->name);
    if (!decl->baseClassName.empty()) {
        std::cout << std::format(": public {}", decl->baseClassName);
    }
    std::cout << "\n";

    TypePtr classType = Type::makeClass(decl->name);

    // ── 处理继承 ──
    if (!decl->baseClassName.empty()) {
        auto baseIt = m_classTypes.find(decl->baseClassName);
        if (baseIt == m_classTypes.end()) {
            error(std::format("Base class '{}' not found", decl->baseClassName),
                  decl->location);
        }

        TypePtr baseType = baseIt->second;

        // 继承基类的字段
        for (auto& baseField : baseType->classLayout.fields) {
            decl->fields.insert(decl->fields.begin(), baseField);
        }

        // 继承基类的虚函数
        for (auto& baseEntry : baseType->classLayout.vtableEntries) {
            classType->classLayout.vtableEntries.push_back(baseEntry);
        }

        if (baseType->classLayout.hasVTable) {
            classType->classLayout.hasVTable = true;
        }

        std::cout << std::format("    ↳ Inherited {} fields, {} vtable entries from '{}'\n",
            baseType->classLayout.fields.size(),
            baseType->classLayout.vtableEntries.size(),
            decl->baseClassName);
    }

    // ── 注册字段到符号表 ──
    for (auto& field : decl->fields) {
        FieldInfo fi;
        fi.name = field.name;
        fi.type = field.type;
        fi.access = field.access;
        classType->classLayout.fields.push_back(fi);

        std::cout << std::format("    field: {} : {}\n",
            field.name, field.type ? field.type->toString() : "?");
    }

    // ── 注册方法，检查虚函数 ──
    for (auto& method : decl->methods) {
        method->ownerClassName = decl->name;

        std::cout << std::format("    method: {}() → {}{}\n",
            method->name,
            method->returnType ? method->returnType->toString() : "?",
            method->isVirtual ? " [virtual]" :
            method->isOverride ? " [override]" : "");

        if (method->isVirtual) {
            classType->classLayout.hasVTable = true;

            bool overridden = false;
            for (auto& entry : classType->classLayout.vtableEntries) {
                if (entry.mangledName.find(method->name) != std::string::npos) {
                    entry.mangledName = decl->name + "_" + method->name;
                    entry.isOverridden = true;
                    overridden = true;
                    break;
                }
            }

            if (!overridden) {
                VTableEntry entry;
                entry.mangledName = decl->name + "_" + method->name;
                entry.index = static_cast<uint32_t>(
                    classType->classLayout.vtableEntries.size());
                classType->classLayout.vtableEntries.push_back(entry);
            }
        }
    }

    // ── 计算内存布局 ──
    computeClassLayout(decl);

    // ── 注入 vtable 和 RTTI ──
    if (classType->classLayout.hasVTable) {
        injectVTableAndRTTI(classType);
    }

    classType->classLayout.fields = decl->fields;

    // ── 注册到全局符号表 ──
    m_classTypes[decl->name] = classType;
    m_classDecls[decl->name] = decl;
    decl->classType = classType;

    Symbol classSym;
    classSym.name = decl->name;
    classSym.type = classType;
    classSym.kind = SymbolKind::Type;
    classSym.isLocal = false;
    classSym.definedAt = decl->location;
    m_symbolTable.define(decl->name, classSym);

    // ── 打印内存布局 ──
    std::cout << std::format("    ══ Memory Layout of '{}' ══\n", decl->name);
    std::cout << std::format("    Total size: {} bytes\n", classType->classLayout.totalSize);
    if (classType->classLayout.hasVTable) {
        std::cout << "    +0: _vptr (8 bytes, hidden) → vtable\n";
    }
    for (auto& f : classType->classLayout.fields) {
        std::cout << std::format("    +{}: {} : {} ({} bytes)\n",
            f.offset, f.name, f.type ? f.type->toString() : "?", f.size);
    }
    if (classType->classLayout.hasVTable) {
        std::cout << std::format("    vtable: {} entries",
            classType->classLayout.vtableEntries.size());
        std::cout << std::format(", RTTI: {}\n", classType->classLayout.rttiMangledName);
    }
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 内存布局计算
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::computeClassLayout(ClassDeclPtr decl) {
    TypePtr classType = decl->classType;
    if (!classType) {
        classType = Type::makeClass(decl->name);
        decl->classType = classType;
    }

    uint32_t offset = 0;
    uint32_t maxAlign = 1;

    // 如果有虚函数，先放 _vptr
    if (classType->classLayout.hasVTable) {
        offset = 8;
        maxAlign = 8;
    }

    for (auto& field : decl->fields) {
        uint32_t fieldAlign = std::max(1u,
            std::min(field.type->sizeInBytes(), 8u));
        uint32_t fieldSize = field.type->sizeInBytes();

        offset = alignTo(offset, fieldAlign);
        field.offset = offset;
        field.size = fieldSize;
        offset += fieldSize;
        maxAlign = std::max(maxAlign, fieldAlign);
    }

    classType->classLayout.totalSize = alignTo(offset, maxAlign);
    if (decl->fields.empty() && classType->classLayout.hasVTable) {
        classType->classLayout.totalSize = 8;
    }
}

void SemanticAnalyzer::injectVTableAndRTTI(TypePtr classType) {
    classType->classLayout.rttiMangledName =
        "_ZTI" + std::to_string(classType->name.size()) + classType->name;

    for (size_t i = 0; i < classType->classLayout.vtableEntries.size(); i++) {
        classType->classLayout.vtableEntries[i].index = static_cast<uint32_t>(i);
    }
}

uint32_t SemanticAnalyzer::alignTo(uint32_t offset, uint32_t alignment) {
    if (alignment == 0) return offset;
    return (offset + alignment - 1) / alignment * alignment;
}

// ─────────────────────────────────────────────────────────────────────────────
// 注册函数（Pass 2）
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::registerFunction(FuncDeclPtr decl) {
    std::string fullName = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    m_functionMap[fullName] = decl;
    m_functionMap[decl->name] = decl;
    m_functions.push_back(decl);

    // 设置 mangled name
    if (decl->ownerClassName.empty()) {
        decl->mangledName = decl->name;
    } else {
        decl->mangledName = decl->ownerClassName + "_" + decl->name;
    }

    // 注册到符号表
    Symbol sym;
    sym.name = decl->name;
    sym.type = decl->returnType;
    sym.kind = SymbolKind::Function;
    sym.isLocal = false;
    sym.ownerClass = decl->ownerClassName;
    sym.definedAt = decl->location;
    m_symbolTable.define(decl->name, sym);

    std::string paramStr;
    for (size_t i = 0; i < decl->parameters.size(); i++) {
        if (i > 0) paramStr += ", ";
        paramStr += decl->parameters[i].type->toString()
                  + " " + decl->parameters[i].name;
    }
    std::cout << std::format("  [register] {}({}) → {}    [mangled: {}]\n",
        fullName, paramStr,
        decl->returnType ? decl->returnType->toString() : "void",
        decl->mangledName);
}

// ═════════════════════════════════════════════════════════════════════════════
// 分析函数体（Pass 3）—— 类型推导与符号决议的主战场
// ═════════════════════════════════════════════════════════════════════════════
void SemanticAnalyzer::analyzeFunctionBody(FuncDeclPtr decl) {
    if (!decl->body) return;

    std::string funcLabel = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    std::cout << std::format("\n  ╔══ Function Body: {} ══╗\n", funcLabel);

    m_symbolTable.enterScope(funcLabel);
    m_stackOffset = 0;
    m_currentClassName = decl->ownerClassName;
    m_currentReturnType = decl->returnType;
    m_currentFuncName = decl->name;

    // ── 注册参数到符号表 ──
    for (auto& param : decl->parameters) {
        m_stackOffset -= 8;

        Symbol sym;
        sym.name = param.name;
        sym.type = param.type;
        sym.kind = SymbolKind::Parameter;
        sym.isLocal = true;
        sym.stackOffset = m_stackOffset;
        sym.definedAt = decl->location;
        m_symbolTable.define(param.name, sym);

        std::cout << std::format("  [param] {} : {}    stack@{}\n",
            param.name, param.type->toString(), m_stackOffset);
    }

    // ── 注册 this 指针（如果是成员函数）──
    if (!decl->ownerClassName.empty()) {
        auto classIt = m_classTypes.find(decl->ownerClassName);
        if (classIt != m_classTypes.end()) {
            m_stackOffset -= 8;

            Symbol thisSym;
            thisSym.name = "this";
            thisSym.type = Type::makePointer(classIt->second);
            thisSym.kind = SymbolKind::Parameter;
            thisSym.isLocal = true;
            thisSym.stackOffset = m_stackOffset;
            m_symbolTable.define("this", thisSym);

            std::cout << std::format("  [param] this : {}*    stack@{}\n",
                decl->ownerClassName, m_stackOffset);
        }
    }

    // ── 分析函数体（不创建新的 block scope，直接在函数 scope 中）──
    for (auto& stmt : decl->body->statements) {
        processStmt(stmt);
    }

    // ── dump 函数作用域符号表 ──
    std::cout << std::format("\n  ── Symbol Table after {} ──\n", funcLabel);
    m_symbolTable.dumpCurrentScope();

    m_symbolTable.exitScope();

    std::cout << std::format("  ╚══ End {} ══╝\n\n", funcLabel);
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板声明处理
// ─────────────────────────────────────────────────────────────────────────────
void SemanticAnalyzer::processTemplateDecl(TemplateDeclPtr decl) {
    std::cout << std::format("  [register] template <");
    for (size_t i = 0; i < decl->typeParams.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "typename " << decl->typeParams[i];
    }
    std::cout << std::format("> {} (blueprint stored, not analyzed)\n",
        decl->classTemplate->name);

    m_templates.push_back(decl);
}

// ═════════════════════════════════════════════════════════════════════════════
// 语句处理
// ═════════════════════════════════════════════════════════════════════════════
void SemanticAnalyzer::processStmt(StmtPtr stmt) {
    if (auto block = std::dynamic_pointer_cast<BlockStmt>(stmt))
        processBlockStmt(block);
    else if (auto var = std::dynamic_pointer_cast<VarDeclStmt>(stmt))
        processVarDecl(var);
    else if (auto ifStmt = std::dynamic_pointer_cast<IfStmt>(stmt))
        processIfStmt(ifStmt);
    else if (auto whileStmt = std::dynamic_pointer_cast<WhileStmt>(stmt))
        processWhileStmt(whileStmt);
    else if (auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmt))
        processReturnStmt(ret);
    else if (auto assign = std::dynamic_pointer_cast<AssignStmt>(stmt))
        processAssignStmt(assign);
    else if (auto expr = std::dynamic_pointer_cast<ExprStmt>(stmt))
        processExprStmt(expr);
}

void SemanticAnalyzer::processBlockStmt(std::shared_ptr<BlockStmt> block) {
    m_symbolTable.enterScope("block");
    for (auto& stmt : block->statements) {
        processStmt(stmt);
    }
    m_symbolTable.exitScope();
}

// ═════════════════════════════════════════════════════════════════════════════
// 变量声明 —— auto 推导的核心战场
// ═════════════════════════════════════════════════════════════════════════════
// 当遇到 `auto x = expr;` 时：
//   1. declaredType 是 Auto 占位符
//   2. 推导 initializer (expr) 的类型
//   3. 将 declaredType **永久替换**为推导出的真实类型
//   4. auto 从此在 AST 中彻底消失
//
// 这就是"编译期抹去 auto"的本质——auto 只是语法糖。
// ═════════════════════════════════════════════════════════════════════════════
void SemanticAnalyzer::processVarDecl(std::shared_ptr<VarDeclStmt> decl) {
    TypePtr type = decl->declaredType;

    if (decl->initializer) {
        // ── 推导初始化表达式的类型 ──
        std::cout << std::format("  [var decl] {} : {} = ",
            decl->name, type->toString());

        m_inferDepth++;
        TypePtr initType = inferType(decl->initializer);
        m_inferDepth--;

        std::cout << std::format("{}    ⟹ inferred: {}\n",
            inferIndent(), initType ? initType->toString() : "?");

        // ─── auto 类型推导 ───
        if (type->isAuto()) {
            if (!initType || initType->isAuto() || initType->isVoid()) {
                error(std::format("Cannot deduce auto type for '{}'", decl->name),
                      decl->location);
            }

            // ★★★ 核心：永久替换 auto 为真实类型 ★★★
            decl->declaredType = initType;
            type = initType;

            std::cout << std::format("  [auto] ★ {} : auto ⟹ {}   ← auto 被永久替换!\n",
                decl->name, type->toString());
        }
        else {
            // ── 类型检查 ──
            if (!type->equals(initType)) {
                if (type->isDouble() && initType && initType->isInt()) {
                    std::cout << std::format("  [coerce] {} : int → double (implicit)\n",
                        decl->name);
                }
                else if (type->isPointer() && initType && initType->isPointer()) {
                    std::cout << std::format("  [poly] {} : {} → {} (polymorphic)\n",
                        decl->name, initType->toString(), type->toString());
                }
                else {
                    error(std::format(
                        "Type mismatch in '{}': declared '{}', got '{}'",
                        decl->name, type->toString(), initType->toString()),
                        decl->location);
                }
            }
        }
    } else if (type->isAuto()) {
        error(std::format("auto variable '{}' must have an initializer", decl->name),
              decl->location);
    } else {
        std::cout << std::format("  [var decl] {} : {} (no initializer)\n",
            decl->name, type->toString());
    }

    // ── 注册到符号表 ──
    m_stackOffset -= 8;

    Symbol sym;
    sym.name = decl->name;
    sym.type = type;
    sym.kind = SymbolKind::Variable;
    sym.isLocal = true;
    sym.stackOffset = m_stackOffset;
    sym.definedAt = decl->location;

    if (!m_symbolTable.define(decl->name, sym)) {
        error(std::format("Variable '{}' already declared in this scope", decl->name),
              decl->location);
    }

    std::cout << std::format("  [symbol] ✚ {} : {}    stack@{}   ← 加入符号表\n",
        decl->name, type->toString(), m_stackOffset);
}

void SemanticAnalyzer::processIfStmt(std::shared_ptr<IfStmt> stmt) {
    std::cout << std::format("  [if] condition:\n");
    m_inferDepth++;
    TypePtr condType = inferType(stmt->condition);
    m_inferDepth--;
    std::cout << std::format("  [if] condition type: {}\n",
        condType ? condType->toString() : "?");

    if (condType && !condType->isBool() && !condType->isInt()) {
        error("If condition must be bool or int", stmt->location);
    }

    m_symbolTable.enterScope("if-then");
    processStmt(stmt->thenBranch);
    m_symbolTable.exitScope();

    if (stmt->elseBranch) {
        m_symbolTable.enterScope("if-else");
        processStmt(stmt->elseBranch);
        m_symbolTable.exitScope();
    }
}

void SemanticAnalyzer::processWhileStmt(std::shared_ptr<WhileStmt> stmt) {
    std::cout << std::format("  [while] condition:\n");
    m_inferDepth++;
    TypePtr condType = inferType(stmt->condition);
    m_inferDepth--;
    std::cout << std::format("  [while] condition type: {}\n",
        condType ? condType->toString() : "?");

    m_symbolTable.enterScope("while-body");
    processStmt(stmt->body);
    m_symbolTable.exitScope();
}

void SemanticAnalyzer::processReturnStmt(std::shared_ptr<ReturnStmt> stmt) {
    if (stmt->value) {
        m_inferDepth++;
        TypePtr retType = inferType(stmt->value);
        m_inferDepth--;

        std::cout << std::format("  [return] type: {} (expected: {})\n",
            retType ? retType->toString() : "?",
            m_currentReturnType ? m_currentReturnType->toString() : "?");

        // 类型检查
        if (retType && m_currentReturnType && !m_currentReturnType->equals(retType)) {
            if (!(m_currentReturnType->isDouble() && retType->isInt())) {
                error(std::format(
                    "Return type mismatch: expected '{}', got '{}'",
                    m_currentReturnType->toString(), retType->toString()),
                    stmt->location);
            }
        }
    } else {
        std::cout << "  [return] void\n";
    }
}

void SemanticAnalyzer::processAssignStmt(std::shared_ptr<AssignStmt> stmt) {
    std::cout << "  [assign] lhs:\n";
    m_inferDepth++;
    TypePtr targetType = inferType(stmt->target);
    m_inferDepth--;

    std::cout << "  [assign] rhs:\n";
    m_inferDepth++;
    TypePtr valueType = inferType(stmt->value);
    m_inferDepth--;

    std::cout << std::format("  [assign] {} ⟵ {} \n",
        targetType ? targetType->toString() : "?",
        valueType ? valueType->toString() : "?");
}

void SemanticAnalyzer::processExprStmt(std::shared_ptr<ExprStmt> stmt) {
    m_inferDepth++;
    inferType(stmt->expr);
    m_inferDepth--;
}

// ═════════════════════════════════════════════════════════════════════════════
// 表达式类型推导 (inferType) —— 符号决议的核心
// ═════════════════════════════════════════════════════════════════════════════
// 对每个表达式节点：
//   1. 递归推导子表达式的类型
//   2. 查找符号表（变量引用）
//   3. 检查类型合法性
//   4. 将推导结果写回 expr->resolvedType
//
// 推导过程有详细的缩进输出，可以清晰看到每一步。
// ═════════════════════════════════════════════════════════════════════════════
TypePtr SemanticAnalyzer::inferType(ExprPtr expr) {
    if (!expr) return nullptr;

    TypePtr type = nullptr;

    if (auto e = std::dynamic_pointer_cast<IntLiteralExpr>(expr))
        type = inferIntLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<BoolLiteralExpr>(expr))
        type = inferBoolLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<StringLiteralExpr>(expr))
        type = inferStringLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<NullptrLiteralExpr>(expr))
        type = inferNullptrLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<VarExpr>(expr))
        type = inferVar(e);
    else if (auto e = std::dynamic_pointer_cast<BinaryExpr>(expr))
        type = inferBinary(e);
    else if (auto e = std::dynamic_pointer_cast<UnaryExpr>(expr))
        type = inferUnary(e);
    else if (auto e = std::dynamic_pointer_cast<CallExpr>(expr))
        type = inferCall(e);
    else if (auto e = std::dynamic_pointer_cast<MemberExpr>(expr))
        type = inferMember(e);
    else if (auto e = std::dynamic_pointer_cast<NewExpr>(expr))
        type = inferNew(e);
    else if (auto e = std::dynamic_pointer_cast<ThisExpr>(expr))
        type = inferThis(e);

    expr->resolvedType = type;
    return type;
}

// ─── 字面量推导（最简单：类型由字面量本身决定）───

TypePtr SemanticAnalyzer::inferIntLiteral(std::shared_ptr<IntLiteralExpr> expr) {
    std::cout << std::format("{}[infer] IntLiteral({}) → int\n",
        inferIndent(), expr->value);
    return Type::makeInt();
}

TypePtr SemanticAnalyzer::inferBoolLiteral(std::shared_ptr<BoolLiteralExpr> expr) {
    std::cout << std::format("{}[infer] BoolLiteral({}) → bool\n",
        inferIndent(), expr->value ? "true" : "false");
    return Type::makeBool();
}

TypePtr SemanticAnalyzer::inferStringLiteral(std::shared_ptr<StringLiteralExpr>) {
    std::cout << std::format("{}[infer] StringLiteral → char*\n", inferIndent());
    return Type::makePointer(Type::makeInt());
}

TypePtr SemanticAnalyzer::inferNullptrLiteral(std::shared_ptr<NullptrLiteralExpr>) {
    std::cout << std::format("{}[infer] nullptr → void*\n", inferIndent());
    return Type::makePointer(Type::makeVoid());
}

// ─────────────────────────────────────────────────────────────────────────────
// 变量引用的符号决议
// ─────────────────────────────────────────────────────────────────────────────
// 这是"符号决议"的核心：给定一个名字，在符号表中查找它的类型。
//
// 查找顺序：
//   1. 当前作用域的局部变量
//   2. 外层作用域（逐层向外）
//   3. 当前类的字段（如果在类方法中）
//   4. 全局函数名
//
// 如果找不到 → 编译错误："Undefined variable"
// ─────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferVar(std::shared_ptr<VarExpr> expr) {
    // 1. 查符号表（从当前作用域向外搜索）
    Symbol* sym = m_symbolTable.lookup(expr->name);
    if (sym) {
        TypePtr resolvedType = sym->type;

        // 如果是类类型，返回注册表中的完整类型（包含布局信息）
        if (resolvedType && resolvedType->isClass()) {
            auto classIt = m_classTypes.find(resolvedType->name);
            if (classIt != m_classTypes.end()) {
                resolvedType = classIt->second;
            }
        }

        std::cout << std::format(
            "{}[resolve] '{}' → {}    (kind={}, {})\n",
            inferIndent(),
            expr->name,
            resolvedType ? resolvedType->toString() : "?",
            symbolKindName(sym->kind),
            sym->isLocal ? std::format("stack@{}", sym->stackOffset) : "global");

        return resolvedType;
    }

    // 2. 如果在类方法中，查类的字段
    if (!m_currentClassName.empty()) {
        auto classIt = m_classTypes.find(m_currentClassName);
        if (classIt != m_classTypes.end()) {
            auto field = classIt->second->classLayout.findField(expr->name);
            if (field) {
                std::cout << std::format(
                    "{}[resolve] '{}' → {}    (class field, offset={})\n",
                    inferIndent(), expr->name,
                    field->type ? field->type->toString() : "?",
                    field->offset);
                return field->type;
            }
        }
    }

    // 3. 检查是否是函数名
    auto funcIt = m_functionMap.find(expr->name);
    if (funcIt != m_functionMap.end()) {
        std::cout << std::format("{}[resolve] '{}' → function\n",
            inferIndent(), expr->name);
        return funcIt->second->returnType;
    }

    error(std::format("Undefined variable '{}'", expr->name), expr->location);
}

// ─────────────────────────────────────────────────────────────────────────────
// 二元表达式推导
// ─────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferBinary(std::shared_ptr<BinaryExpr> expr) {
    std::cout << std::format("{}[infer] BinaryExpr(op) left:\n", inferIndent());

    m_inferDepth++;
    TypePtr leftType = inferType(expr->left);
    std::cout << std::format("{}[infer] BinaryExpr(op) right:\n", inferIndent());
    TypePtr rightType = inferType(expr->right);
    m_inferDepth--;

    // 比较运算符返回 bool
    if (expr->op == BinaryOp::Eq || expr->op == BinaryOp::Neq
        || expr->op == BinaryOp::Lt || expr->op == BinaryOp::Gt
        || expr->op == BinaryOp::Le || expr->op == BinaryOp::Ge
        || expr->op == BinaryOp::And || expr->op == BinaryOp::Or) {

        std::cout << std::format(
            "{}[infer] {} {} {} → bool    (comparison)\n",
            inferIndent(),
            leftType ? leftType->toString() : "?",
            "op",
            rightType ? rightType->toString() : "?");
        return Type::makeBool();
    }

    // 算术运算符
    if (leftType && rightType) {
        TypePtr resultType;
        if (leftType->isDouble() || rightType->isDouble()) {
            resultType = Type::makeDouble();
        } else if (leftType->isInt() && rightType->isInt()) {
            resultType = Type::makeInt();
        } else {
            resultType = Type::makeInt(); // 默认
        }

        std::cout << std::format(
            "{}[infer] {} op {} → {}\n",
            inferIndent(),
            leftType->toString(),
            rightType->toString(),
            resultType->toString());
        return resultType;
    }

    error("Invalid binary operation types", expr->location);
}

TypePtr SemanticAnalyzer::inferUnary(std::shared_ptr<UnaryExpr> expr) {
    m_inferDepth++;
    TypePtr operandType = inferType(expr->operand);
    m_inferDepth--;

    if (expr->op == UnaryOp::Not) {
        std::cout << std::format("{}[infer] !{} → bool\n",
            inferIndent(), operandType ? operandType->toString() : "?");
        return Type::makeBool();
    }

    std::cout << std::format("{}[infer] -{} → {}\n",
        inferIndent(),
        operandType ? operandType->toString() : "?",
        operandType ? operandType->toString() : "?");
    return operandType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数调用推导
// ─────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferCall(std::shared_ptr<CallExpr> expr) {
    // 推导 callee 类型
    m_inferDepth++;
    TypePtr calleeType = inferType(expr->callee);
    m_inferDepth--;

    // 查找函数声明
    std::string funcName;
    if (auto var = std::dynamic_pointer_cast<VarExpr>(expr->callee)) {
        funcName = var->name;
    } else if (auto mem = std::dynamic_pointer_cast<MemberExpr>(expr->callee)) {
        funcName = mem->memberName;
        mem->isMethodCall = true;
    }

    auto it = m_functionMap.find(funcName);
    if (it != m_functionMap.end()) {
        // 检查参数数量
        std::cout << std::format("{}[call] {}({} args) → {}    [symbol resolved]\n",
            inferIndent(), funcName, expr->arguments.size(),
            it->second->returnType ? it->second->returnType->toString() : "?");

        // 推导参数类型
        for (size_t i = 0; i < expr->arguments.size(); i++) {
            m_inferDepth++;
            TypePtr argType = inferType(expr->arguments[i]);
            m_inferDepth--;
            std::cout << std::format("{}  arg[{}]: {}\n",
                inferIndent(), i, argType ? argType->toString() : "?");
        }

        return it->second->returnType;
    }

    std::cout << std::format("{}[call] {}() → {} (callee type)\n",
        inferIndent(), funcName,
        calleeType ? calleeType->toString() : "?");
    return calleeType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 成员访问推导 —— "编译期看符号 → 运行期看偏移量"的转换点
// ─────────────────────────────────────────────────────────────────────────────
TypePtr SemanticAnalyzer::inferMember(std::shared_ptr<MemberExpr> expr) {
    m_inferDepth++;
    TypePtr objType = inferType(expr->object);
    m_inferDepth--;

    if (!objType) {
        error("Cannot access member of null type", expr->location);
    }

    // 解引用指针
    TypePtr actualType = objType;
    if (expr->isArrow && objType->isPointer()) {
        actualType = objType->pointeeType;
    }

    if (!actualType || !actualType->isClass()) {
        error(std::format("Cannot access member '{}' on non-class type '{}'",
            expr->memberName, actualType ? actualType->toString() : "?"),
            expr->location);
    }

    // 查找字段
    auto fieldInfo = actualType->classLayout.findField(expr->memberName);
    if (fieldInfo) {
        std::cout << std::format(
            "{}[member] {}.{} → {}    (offset={}, size={})\n",
            inferIndent(),
            actualType->name, expr->memberName,
            fieldInfo->type ? fieldInfo->type->toString() : "?",
            fieldInfo->offset, fieldInfo->size);
        return fieldInfo->type;
    }

    // 查找方法
    auto classIt = m_classDecls.find(actualType->name);
    if (classIt != m_classDecls.end()) {
        for (auto& method : classIt->second->methods) {
            if (method->name == expr->memberName) {
                std::cout << std::format(
                    "{}[member] {}.{}() → {}    (method{})\n",
                    inferIndent(),
                    actualType->name, expr->memberName,
                    method->returnType ? method->returnType->toString() : "?",
                    method->isVirtual ? ", virtual" : "");
                return method->returnType;
            }
        }
    }

    error(std::format("No member '{}' in class '{}'",
        expr->memberName, actualType->name), expr->location);
}

TypePtr SemanticAnalyzer::inferNew(std::shared_ptr<NewExpr> expr) {
    auto it = m_classTypes.find(expr->className);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", expr->className), expr->location);
    }

    std::cout << std::format("{}[new] {} → {}*    (size={} bytes)\n",
        inferIndent(), expr->className, expr->className,
        it->second->classLayout.totalSize);

    return Type::makePointer(it->second);
}

TypePtr SemanticAnalyzer::inferThis(std::shared_ptr<ThisExpr>) {
    if (m_currentClassName.empty()) {
        error("'this' used outside of class method", SourceLocation{});
    }

    auto it = m_classTypes.find(m_currentClassName);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", m_currentClassName), SourceLocation{});
    }

    std::cout << std::format("{}[this] → {}*\n",
        inferIndent(), m_currentClassName);
    return Type::makePointer(it->second);
}

// ─────────────────────────────────────────────────────────────────────────────
// 错误处理
// ─────────────────────────────────────────────────────────────────────────────
[[noreturn]] void SemanticAnalyzer::error(const std::string& msg, SourceLocation loc) {
    throw std::runtime_error(
        std::format("[Semantic Error] {}: {}", loc.toString(), msg));
}

} // namespace minicc
