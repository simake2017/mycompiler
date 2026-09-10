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
// 管线位置
// =============================================================================
//   源码 → Preprocessor → Lexer → Parser → 【SemanticAnalyzer】 → 模板推导/
//                                            实例化 → CodeGen(x86-64 .s)
//   输入：TranslationUnit（AST，类型只是语法标记，名字均未决议）
//   输出：① 标注 resolvedType 的 AST（auto 已抹去，每个表达式类型已定）
//         ② 符号表快照（Scope 作用域链 + 栈偏移，全阶段中文日志可观测）
//         ③ 类布局表（字段偏移 / vtable / RTTI，CodeGen 直接消费）
// =============================================================================
// 理论背景（C++ 标准章节 → 本文件实现位置）
// =============================================================================
//   [basic.scope]   符号表与作用域链：名字从最内层逐层向外查找
//                   → Scope::lookup / SymbolTable::enterScope/exitScope
//   [expr]          类型检查与值类别：inferType 自底向上给每个表达式定型，
//                   inferCall 记录实参左值性供引用绑定使用
//   [over.match]    重载决议三步（候选→可行→最优）
//                   → resolveTemplateCall + isAtLeastAsSpecialized
//   [temp.names]    两阶段查找（简化）：Pass 1 注册模板蓝图不查体；
//                   S5 实例化后再 analyzeFunctionBody(实例) 做第二阶段检查
//   [dcl.init.ref]  引用绑定规则：本文件 inferCall 记录 argIsLValue，
//                   真正的绑定检查（非 const T& 不绑右值、T&& 万能引用折叠）
//                   在 TemplateDeducer 中执行
// =============================================================================
// clang 模块对照（教学级简化）
// =============================================================================
//   clang lib/Sema/SemaDecl.cpp      → processClassDecl / registerFunction /
//                                      processVarDecl（ActOnDeclarableType 等）
//   clang lib/Sema/SemaExpr.cpp      → inferType 系列（ActOnCallExpr、
//                                      BuildMemberExpr、LookupName 等）
//   clang lib/Sema/SemaOverload.cpp  → resolveTemplateCall（AddTemplateOverload-
//                                      Candidate）/ isAtLeastAsSpecialized
// =============================================================================

#include "semantic_analyzer.h"
#include "template_deduction.h"
#include <format>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <functional>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// SymbolKind 名称
// ─────────────────────────────────────────────────────────────────────────────
// 仅供日志/dump 输出：把枚举翻译成可读字符串。
// 示例：dump 符号表时打印 `x  Variable  int  stack@-8`，其中 "Variable"
//       即由本函数产出。
const char* symbolKindName(SymbolKind k) {
    switch (k) {
        case SymbolKind::Variable:    return "Variable";
        case SymbolKind::Parameter:   return "Parameter";
        case SymbolKind::Function:    return "Function";
        case SymbolKind::FunctionTemplate: return "FunctionTemplate";
        case SymbolKind::ClassField:  return "ClassField";
        case SymbolKind::ClassMethod: return "ClassMethod";
        case SymbolKind::Type:        return "Type";
    }
    return "Unknown";
}

// ═════════════════════════════════════════════════════════════════════════════
// Scope 实现
// ═════════════════════════════════════════════════════════════════════════════
// 向本层符号表插入一条符号。emplace 失败（本层已有同名）返回 false ——
// 这是重声明检查 [basic.scope.scope] 的落点。
// 示例：当前块已声明 `int x;`，再次 `int x;` → define("x",…) 返回 false，
//       processVarDecl 报 "Variable 'x' already declared in this scope"。
// 注意：外层同名不拦截（内层遮蔽外层是合法的）——遮蔽由 lookup 的顺序实现。
bool Scope::define(const std::string& name, Symbol sym) {
    auto [it, inserted] = m_symbols.emplace(name, std::move(sym));
    return inserted;
}

// 作用域链查找（[basic.scope.scope] 名字可见性的核心算法）：
// 从本层开始，未命中则递归父作用域，直到全局作用域为止。
// 示例：f(depth=1) 内 block(depth=2) 中查找 `x`：
//     block ✗ → f ✓ 返回（内层遮蔽外层的同名符号——先查到谁就用谁）
//     若各层皆无 → nullptr → 上层转查类字段/函数名（见 inferVar）
Symbol* Scope::lookup(const std::string& name) {
    auto it = m_symbols.find(name);
    if (it != m_symbols.end()) return &it->second;
    if (m_parent) return m_parent->lookup(name);
    return nullptr;
}

// 只查本层、不上链：用于需要"仅限同一作用域"语义的场合（对比 lookup）。
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
// 构造：创建全局作用域（链根，depth=0，名字 "global"），类名/函数名等
// 顶层符号都注册在这里。
SymbolTable::SymbolTable()
    : m_globalScope(std::make_unique<Scope>(nullptr, 0, "global"))
    , m_currentScope(m_globalScope.get())
    , m_currentDepth(0) {}

// 压栈进入新作用域：新 Scope 的 parent 指向当前作用域。
// 示例：analyzeFunctionBody 调 enterScope("f") → processBlockStmt 再调
//       enterScope("block")，形成 global ← f ← block 链。
// 所有权：newScope 移交 m_allScopes 统一持有（退出作用域 ≠ 销毁对象，
// 对象保留到编译结束，供 dump 调试）。
void SymbolTable::enterScope(const std::string& name) {
    m_currentDepth++;
    auto newScope = std::make_unique<Scope>(m_currentScope, m_currentDepth, name);
    m_currentScope = newScope.get();
    m_allScopes.push_back(std::move(newScope));
}

// 弹栈回到父作用域：子作用域中的名字从此不可见（对象仍保留在 m_allScopes）。
// 防御：global 的 parent 为 nullptr，多退一步也不会崩溃。
void SymbolTable::exitScope() {
    if (m_currentScope && m_currentScope->parent()) {
        m_currentScope = m_currentScope->parent();
        m_currentDepth--;
    }
}

// 委托当前作用域：符号插入当前层（跨层插入不存在——可见性规则使然）。
bool SymbolTable::define(const std::string& name, Symbol sym) {
    return m_currentScope->define(name, std::move(sym));
}

// 委托当前作用域：从当前层沿链向外查（实现见 Scope::lookup）。
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
// ─────────────────────────────────────────────────────────────────────────────
// 类型兼容性判断（带隐式转换）
// ─────────────────────────────────────────────────────────────────────────────
// [conv.lval] 左值到右值转换：int& 可当 int 用 —— 比较前剥掉双方顶层引用/const；
// 另含 int → double 隐式提升（[conv.promo]）。
// 这是函数模板可用的前提之一：实例化后的类型常带引用包装（T& / T&& / const T&）。
// 示例：typeCompatible(int, int&)    = true  （int& → int，[conv.lval]）
//       typeCompatible(int, const int) = true（剥顶层 const）
//       typeCompatible(double, int)  = true  （int → double，[conv.promo]）
//       typeCompatible(int, bool)    = false （无此隐式转换规则 → 报错）
TypePtr SemanticAnalyzer::resolveType(TypePtr type) {
    if (!type) return nullptr;
    if (type->isClass()) {
        // ── P3：模板 id（Box<int>）→ 按需实例化，产出具体实例类型 ──
        // 判定条件：携带非空实参，且该名字是已登记的类模板蓝图
        // （类模板注册表 m_classTemplates，O(1) 查找）。
        // （普通已实例化类如 Box_int 实参为空，走下面的符号表替换。）
        if (!type->templateArgs.empty()) {
            if (m_classTemplates.count(type->name)) {
                // 实参先递归解析（实参本身可能是模板 id：Box<Box<int>>）
                TypePtr tid = Type::makeClass(type->name);
                tid->templateArgs.reserve(type->templateArgs.size());
                for (auto& arg : type->templateArgs) {
                    tid->templateArgs.push_back(resolveType(arg));
                }
                return getOrInstantiateClass(tid, SourceLocation{});
            }
        } else if (m_classTemplates.count(type->name)) {
            // ── 裸类模板名（[temp.arg.explicit]）──
            // `Box b;` 不带实参：蓝图不是类型，不能当类名解析。
            // 对照 clang：use of class template 'Box' requires template
            // arguments。此处早期报错，而不是放行未解析类型到使用期
            // 才报 'Class not declared'（诊断不指向根因）。
            error(std::format("'{}' is a class template; provide template "
                              "arguments (e.g. {}<int>)",
                              type->name, type->name), SourceLocation{});
        }
        Symbol* sym = m_symbolTable.lookup(type->name);
        // Parser 遇到 `Dog*` 这类书写名时会临时 new 一个 Class("Dog")，
        // 其 classLayout 是空的；真正带字段/偏移/虚表布局的类型在
        // processClassDecl 阶段已登记进符号表（kind=Type）。
        // 只要查到的注册类型与来者不是同一个对象，就用注册版替换——
        // 否则后面 inferMember 的 classLayout.findField 会在空布局上查无此字段。
        if (sym && sym->kind == SymbolKind::Type && sym->type &&
            sym->type != type) { // wangyang **** 指针不相同说明指向的不是同
            return sym->type;
        }
    }
    if (type->isPointer()) {
        auto base = resolveType(type->pointeeType);
        if (base != type->pointeeType) return Type::makePointer(base);
    }
    if (type->isReference()) {
        auto base = resolveType(type->referencedType);
        if (base != type->referencedType) return Type::makeLValueReference(base);
    }
    if (type->isRValueReference()) {
        auto base = resolveType(type->referencedType);
        if (base != type->referencedType) return Type::makeRValueReference(base);
    }
    if (type->isConst()) {
        auto base = resolveType(type->innerType);
        if (base != type->innerType) return Type::makeConst(base);
    }
    return type;
}

static bool typeCompatible(const TypePtr& expected, const TypePtr& got) {
    if (!expected || !got) return false;
    TypePtr e = expected, g = got;
    while (e->isReference()) e = e->referencedType;
    while (g->isReference()) g = g->referencedType;
    if (e->isConst()) e = e->innerType;
    if (g->isConst()) g = g->innerType;
    if (e->equals(g)) return true;
    return e->isDouble() && g->isInt();
}

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
// 为什么必须三遍？因为名字可以先使用后声明：
//   Pass 1 先注册类型 → 类字段/继承引用不受声明顺序影响；
//   Pass 2 先注册函数名 → `int f() { return g(); }` 即使写在 `int g()`
//   之前，Pass 3 查体时也能在 m_functionMap 中查到 g（递归同理）。
// 对照 clang：也是先把所有 Decl 挂进 DeclContext（建名字索引），
// 函数体延迟到 ActOnTopLevelDecl/完整定义后再逐一分析。
// 限制：Pass 1 单遍处理继承，基类必须先于派生类出现在源码中
//      （不支持前向声明，[class] 教学级简化）。
void SemanticAnalyzer::processDecl(DeclPtr decl) {
    // wangyang 这两行是一样的，模板方法，实际是要求<> 中写明实际类型的，只是这里能推到出来，所以省略了
    // if (auto cls = std::dynamic_pointer_cast<ClassDecl, Declaration>(decl)) {
    // 这里如果是返回空指针，就不会往下走了
    if (auto cls = std::dynamic_pointer_cast<ClassDecl>(decl)) {
        processClassDecl(cls);
    } else if (auto tmpl = std::dynamic_pointer_cast<TemplateDecl>(decl)) {
        processTemplateDecl(tmpl);
    } else if (auto gvar = std::dynamic_pointer_cast<GlobalVarDecl>(decl)) {
        processGlobalVarDecl(gvar);
    } else if (auto enm = std::dynamic_pointer_cast<EnumDecl>(decl)) {
        processEnumDecl(enm);
    } else if (auto ns = std::dynamic_pointer_cast<NamespaceDecl>(decl)) {
        processNamespaceDecl(ns);
    } else if (auto ta = std::dynamic_pointer_cast<TypeAliasDecl>(decl)) {
        processTypeAliasDecl(ta);
    }
}

void SemanticAnalyzer::analyze(TranslationUnit& unit) {

    // P4：libc 内建原型先登记——早于 Pass 1，全程可见
    registerBuiltins();

    std::cout << "\n  ┌──── Pass 1: 注册类、模板、全局变量与命名空间 ────\n";
    for (auto& decl : unit.declarations) {
        processDecl(decl);
    }

    // Pass 1 完成后所有类的继承关系已就绪 → 打印继承图（可观测性）
    printInheritanceGraph();

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

// ─────────────────────────────────────────────────────────────────────────────
// 继承图打印（可观测性设施：把 Pass 1 收集到的继承关系渲染成 ASCII 树）
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：以"无基类的类"为根，沿 baseClassNames 向下展开，树形打印全部类。
// 理论：类继承关系构成一片森林（多继承场景下是 DAG；本项目简化为
//       主基类链 forest）。真编译器在 Sema 内部维护 CXXRecordDecl 的
//       getBases() 边表，本函数用 ClassDecl.firstBase() 即时重建同样的图。
// 标注：[polymorphic] = 带虚函数表 → 对象含 _vptr，可参与虚调用与
//       dynamic_cast 的运行时类型检查（typeinfo 继承链由 CodeGen 发射）。
// 输出示例（对应类 Animal/ Dog : Animal / Cat : Animal）：
//   │ ═══ 类型继承图（inheritance graph）═══
//   │ Animal [polymorphic]
//   │ ├── Dog [polymorphic]
//   │ └── Cat [polymorphic]
void SemanticAnalyzer::printInheritanceGraph() {
    // ① 邻接表：父类名 → 子类名列表（m_classDecls 是 Pass 1 的成果）
    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots; // 无基类的根类
    for (auto& [name, decl] : m_classDecls) {
        if (decl->baseClassNames.empty()) {
            roots.push_back(name);
        } else {
            for (auto& bn : decl->baseClassNames)
                children[bn].push_back(name);
        }
    }

    if (m_classDecls.empty()) return; // 无类可打印，静默跳过

    auto isPoly = [this](const std::string& cls) {
        auto it = m_classTypes.find(cls);
        return it != m_classTypes.end() && it->second->classLayout.hasVTable;
    };

    std::cout << "  │ ═══ 类型继承图（inheritance graph）═══\n";

    // ② 深度优先递归：prefix 记录竖线骨架，branch 决定本节点的分叉符号
    std::function<void(const std::string&, const std::string&, const std::string&)> dfs =
        [&](const std::string& cls, const std::string& prefix, const std::string& branch) {
            std::cout << std::format("  │ {}{}{}{}\n", prefix, branch, cls,
                isPoly(cls) ? " [polymorphic]" : "");
            auto it = children.find(cls);
            if (it == children.end()) return;
            auto& kids = it->second;
            std::sort(kids.begin(), kids.end()); // 排序保证输出可复现
            for (size_t i = 0; i < kids.size(); ++i) {
                bool last = (i + 1 == kids.size());
                // 本层分叉符号 + 下探一层的骨架（最后子节点以下空格延续）
                dfs(kids[i],
                    prefix + (branch.empty() ? "" : (last ? "    " : "│   ")),
                    last ? "└── " : "├── ");
            }
        };

    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        dfs(r, "", "");
    }
}

void SemanticAnalyzer::processGlobalVarDecl(GlobalVarDeclPtr decl) {
    decl->declaredType = resolveType(decl->declaredType);
    if (decl->initializer) {
        TypePtr initType = inferType(decl->initializer);
        if (decl->declaredType->isAuto()) {
            decl->declaredType = initType;
        } else if (!typeCompatible(decl->declaredType, initType)) {
            error(std::format("Cannot initialize global variable '{}' of type '{}' with value of type '{}'",
                decl->name, decl->declaredType->toString(), initType->toString()), decl->location);
        }
    }
    m_globalVars.push_back(decl);

    Symbol sym;
    sym.name = decl->name;
    sym.type = decl->declaredType;
    sym.kind = SymbolKind::Variable;
    sym.isLocal = false;
    sym.definedAt = decl->location;
    m_symbolTable.globalScope()->define(decl->name, sym);

    std::cout << std::format("  [register] global variable '{}' : {}\n",
        decl->name, decl->declaredType ? decl->declaredType->toString() : "auto");
}

void SemanticAnalyzer::processEnumDecl(EnumDeclPtr decl) {
    m_enumDecls[decl->name] = decl;
    TypePtr enumType = decl->underlyingType ? decl->underlyingType : Type::makeInt();

    for (auto& item : decl->items) {
        if (item.valueExpr) {
            inferType(item.valueExpr);
        }
        if (!decl->isScoped) {
            Symbol sym;
            sym.name = item.name;
            sym.type = enumType;
            sym.kind = SymbolKind::Variable;
            sym.isLocal = false;
            sym.definedAt = item.location;
            m_symbolTable.globalScope()->define(item.name, sym);
        }
        if (!decl->name.empty()) {
            Symbol scopedSym;
            scopedSym.name = decl->name + "::" + item.name;
            scopedSym.type = enumType;
            scopedSym.kind = SymbolKind::Variable;
            scopedSym.isLocal = false;
            scopedSym.definedAt = item.location;
            m_symbolTable.globalScope()->define(scopedSym.name, scopedSym);
        }
    }
    std::cout << std::format("  [register] enum '{}' ({} items)\n", decl->name, decl->items.size());
}

void SemanticAnalyzer::processNamespaceDecl(NamespaceDeclPtr decl) {
    std::cout << std::format("  [namespace] enter namespace '{}'\n", decl->name);
    for (auto& innerDecl : decl->declarations) {
        if (auto func = std::dynamic_pointer_cast<FunctionDecl>(innerDecl)) {
            func->name = decl->name + "::" + func->name;
            registerFunction(func);
        } else if (auto cls = std::dynamic_pointer_cast<ClassDecl>(innerDecl)) {
            cls->name = decl->name + "::" + cls->name;
            processClassDecl(cls);
        } else if (auto gvar = std::dynamic_pointer_cast<GlobalVarDecl>(innerDecl)) {
            gvar->name = decl->name + "::" + gvar->name;
            processGlobalVarDecl(gvar);
        } else if (auto enm = std::dynamic_pointer_cast<EnumDecl>(innerDecl)) {
            enm->name = decl->name + "::" + enm->name;
            processEnumDecl(enm);
        } else {
            processDecl(innerDecl);
        }
    }
}

void SemanticAnalyzer::processTypeAliasDecl(TypeAliasDeclPtr decl) {
    Symbol sym;
    sym.name = decl->aliasName;
    sym.type = decl->underlyingType;
    sym.kind = SymbolKind::Type;
    sym.isLocal = false;
    sym.definedAt = decl->location;
    m_symbolTable.globalScope()->define(decl->aliasName, sym);
    std::cout << std::format("  [register] type alias '{}' = {}\n",
        decl->aliasName, decl->underlyingType ? decl->underlyingType->toString() : "?");
}

// ═════════════════════════════════════════════════════════════════════════════
// 类声明处理
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：建类类型 → 合并基类字段/vtable（继承）→ 收集自身字段/方法并建
// vtable → 计算内存布局 → 注入 _vptr/RTTI → 注册进全局符号表。
// 理论依据：
//   [class.derived] 单继承 public 布局——基类子对象位于派生类成员之前；
//   [class.virtual] 虚函数"同槽位覆盖"——override 替换基类同名虚函数的
//   vtable 槽位而不是新增，这是动态分派的根基。
//
// 示例：class Animal { virtual int speak(); }
//       class Dog : public Animal { virtual int speak(); }
//   处理 Animal：vtable = [ 0: Animal_speak ]
//   处理 Dog：  先拷贝基类表 [ 0: Animal_speak ]
//               → 发现同名 override → 槽 0 改写为 Dog_speak（索引不变！）
//   Dog 对象内存：            Dog 的 vtable：
//     +0: _vptr ────────────→ [0] = &Dog_speak
//     +8: 自身字段 ……         [-1]= &type_info(_ZTI3Dog)（vtable 前一槽）
//   运行期 Animal* p = new Dog; p->speak() →
//     callq *(%rdi) 按索引 0 间接跳转 → 落到 Dog_speak（动态分派）
void SemanticAnalyzer::processClassDecl(ClassDeclPtr decl) {
    std::cout << std::format("  [register] class '{}' ", decl->name);
    if (!decl->baseClassNames.empty()) {
        std::cout << ": public ";
        for (size_t i = 0; i < decl->baseClassNames.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << decl->baseClassNames[i];
        }
    }
    std::cout << "\n";

    // 为本类新建类型对象：classLayout（字段/偏移/vtable）都挂在它上面，
    // 完成后注册进 m_classTypes，全局唯一（"编译期看符号"的那个符号）。
    // 注意：必须【立刻】挂到 decl->classType 上——后面的
    // computeClassLayout(decl) 从 decl->classType 取类型计算偏移；
    // 若拖到函数末尾才赋值，布局会算在一个 hasVTable=false 的
    // 临时孤儿类型上：字段偏移漏掉 _vptr、totalSize=0，
    // CodeGen 按 0 字节 malloc，运行期写必然越界。
    TypePtr classType = Type::makeClass(decl->name);
    decl->classType = classType;

    // ── 处理继承（多继承：[class.mi] Itanium 主基类优化模型）──
    // 策略：第一个多态基类 = 主基类（共享主表+字段无限定），
    //       其余多态基类 = 次基类（独立次表 entries，字段带 "BaseName." 前缀）。
    //       非多态基类的字段也带 "BaseName." 前缀（避免多基类同名冲突）。
    std::vector<std::string> allBaseFieldNames; // 所有基类字段名（用于自身字段限定）
    bool hasPrimaryVTable = false;
    // 注意：本循环【不计算】子对象偏移——offset 统一由循环后的 [relocate]
    // 阶段按 Itanium 规则摆放（primary 恒占 0，其余从 primary 尾部对齐累加）。
    // 历史教训：曾在循环里边扫边放（currentOffset 累加），当非多态基类声明在
    // 多态基类之前时，A 先占 0、primary P 又写死 0 → 子对象重叠（A.x 压 _vptr）；
    // 且 primary 分支的 currentOffset 是"重置"而非累加，会把先摆的进度悄悄丢弃。

    if (!decl->baseClassNames.empty()) {
        // 保存自身字段（Parser 已将它们填入 decl->fields），继承处理中重建顺序
        std::vector<FieldInfo> ownFields = std::move(decl->fields);
        decl->fields.clear();

        for (size_t baseIdx = 0; baseIdx < decl->baseClassNames.size(); ++baseIdx) {
            const std::string& baseName = decl->baseClassNames[baseIdx];
            auto baseIt = m_classTypes.find(baseName);
            if (baseIt == m_classTypes.end()) { // wangyang 这里就是要 必须先声明base 类才可以，必须要按照顺序去初始化才可以
                error(std::format("Base class '{}' not found", baseName), decl->location);
            }
            TypePtr baseType = baseIt->second;

            // 收集基类字段名（用于自身字段限定）
            for (auto& bf : baseType->classLayout.fields) {
                std::string rawName = bf.name;
                auto dot = rawName.find('.');
                if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                allBaseFieldNames.push_back(rawName);
            }

            if (!hasPrimaryVTable && baseType->classLayout.hasVTable) { // wangyang 第一个有虚函数的类才算是主基类
                // ═══ 主基类（primary base）═══
                // 合并字段到主字段列表 + 合并 vtable 到主表
                // 字段名不加限定前缀（保持单继承兼容）
                for (auto& baseField : baseType->classLayout.fields) {
                    FieldInfo fi = baseField;
                    if (fi.sourceClass.empty()) fi.sourceClass = baseName;
                    decl->fields.push_back(fi);
                }
                for (auto& baseEntry : baseType->classLayout.vtableEntries) {
                    classType->classLayout.vtableEntries.push_back(baseEntry);
                }
                classType->classLayout.hasVTable = true;
                hasPrimaryVTable = true;

                BaseSubobject primarySub;
                primarySub.baseClassName = baseName;
                primarySub.hasVTable = true;
                primarySub.isPrimary = true;
                primarySub.vtableSegmentOffset = 0;
                // offset 不在此设置：[relocate] 阶段统一置 0 并摆放其余子对象
                classType->classLayout.bases.push_back(primarySub);

                std::cout << std::format("    ↳ [primary] '{}': {} fields, {} vtable entries\n",
                    baseName, baseType->classLayout.fields.size(),
                    baseType->classLayout.vtableEntries.size());
            } else if (baseType->classLayout.hasVTable) {
                // ═══ 次基类（secondary base）═══
                // 字段带 "BaseName." 限定前缀；vtable 条目存入独立次表 entries
                for (auto& baseField : baseType->classLayout.fields) {
                    FieldInfo fi = baseField;
                    // 提取裸名（已有前缀则去掉）
                    std::string rawName = fi.name;
                    auto dot = rawName.find('.');
                    if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                    fi.name = baseName + "." + rawName;
                    fi.sourceClass = baseName;
                    decl->fields.push_back(fi);
                }

                BaseSubobject secSub;
                secSub.baseClassName = baseName;
                secSub.hasVTable = true;
                secSub.isPrimary = false;
                // offset 由 [relocate] 阶段统一摆放
                // 次表条目（独立于主表，覆写时设 thunkAdjust）
                for (auto& baseEntry : baseType->classLayout.vtableEntries) { // wangyang 次基类的vtable entry 不是放到 classLayout里面的
                    VTableEntry secEntry = baseEntry;
                    secEntry.mangledName = baseName + "_" + (baseEntry.baseFunctionName.empty()
                        ? std::string(baseEntry.mangledName.substr(baseEntry.mangledName.find('_') + 1))
                        : baseEntry.baseFunctionName);
                    secEntry.baseFunctionName = baseEntry.baseFunctionName.empty()
                        ? baseEntry.mangledName.substr(baseEntry.mangledName.find('_') + 1)
                        : baseEntry.baseFunctionName;
                    secSub.entries.push_back(secEntry);
                }
                classType->classLayout.bases.push_back(secSub);

                std::cout << std::format("    ↳ [secondary] '{}': {} fields, {} vtable entries\n",
                    baseName, baseType->classLayout.fields.size(),
                    secSub.entries.size());
            } else {
                // ═══ 非多态基类 ═══
                // 字段带限定前缀，无 vtable 贡献
                for (auto& baseField : baseType->classLayout.fields) {
                    FieldInfo fi = baseField;
                    std::string rawName = fi.name;
                    auto dot = rawName.find('.');
                    if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                    fi.name = baseName + "." + rawName;
                    fi.sourceClass = baseName;
                    decl->fields.push_back(fi);
                }

                BaseSubobject nonPolySub;
                nonPolySub.baseClassName = baseName;
                nonPolySub.hasVTable = false;
                nonPolySub.isPrimary = false;
                // offset 由 [relocate] 阶段统一摆放
                classType->classLayout.bases.push_back(nonPolySub);

                std::cout << std::format("    ↳ [non-poly] '{}': {} fields\n",
                    baseName, baseType->classLayout.fields.size());
            }
        }

        // ── 统一摆放子对象偏移（Itanium [class.mi]：primary 恒占 offset 0）──
        // 这是子对象偏移的【唯一】计算点（上方循环只收集字段与 vtable 条目，
        // 不写 offset，避免"边扫边放"产生两个 offset=0 的历史 bug）。
        // Itanium 规则：primary = 第一个【多态】基类（与声明顺序无关），
        // 恒放 offset 0；其余子对象（含声明在 primary 之前的非多态基类）
        // 一律从 primary 子对象尾部开始依次对齐摆放。
        // 简化声明：全部基类都非多态时，第一个基类视作 primary（占 0），
        // 保持与 computeClassLayout 的字段放置规则一致。
        if (!classType->classLayout.bases.empty()) {
            size_t primaryIdx = 0;
            bool anyPoly = false;
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i) {
                if (classType->classLayout.bases[i].hasVTable) {
                    primaryIdx = i;
                    anyPoly = true;
                    break;
                }
            }
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i)
                classType->classLayout.bases[i].isPrimary = (i == primaryIdx);

            // place 恒从 primary 子对象尾部起算——无论 primary 是多态基类
            // （真 primary）还是全非多态时的首个基类（视作 primary）。
            // 历史 bug：这里曾用 if (anyPoly) 守卫，全非多态时 place 停在 0，
            // 第二个基类被 alignTo(0,8)=0 放到 offset 0，与首个基类字段重叠
            // （D : A{int x}, B{int y} → x@0 与 y@0 互踩，构造 B 覆盖 x）。
            uint32_t place = 0;
            {
                auto pIt = m_classTypes.find(
                    classType->classLayout.bases[primaryIdx].baseClassName);
                if (pIt != m_classTypes.end())
                    place = pIt->second->classLayout.totalSize; // primary 尾部
            }
            (void)anyPoly; // anyPoly 仅保留语义说明作用，不再参与 place 计算
            for (size_t i = 0; i < classType->classLayout.bases.size(); ++i) {
                auto& sub = classType->classLayout.bases[i];
                if (sub.isPrimary) { sub.offset = 0; continue; }
                sub.offset = alignTo(place, 8); // wangyang ****这里非常关键, 这里会对结束位置再做一次偏移，彻底锁死对应的位置
                auto bIt = m_classTypes.find(sub.baseClassName);
                place = sub.offset + (bIt != m_classTypes.end()
                    ? bIt->second->classLayout.totalSize : 0);
                std::cout << std::format("    ↳ [relocate] '{}' → offset {} (primary='{}' 占 0)\n",
                    sub.baseClassName, sub.offset,
                    classType->classLayout.bases[primaryIdx].baseClassName);
            }
        }

        // 追加自身字段到最后
        for (auto& f : ownFields) {
            decl->fields.push_back(f);
        }
    }

    // 自身字段与基类字段同名时加限定前缀（避免覆写基类字段名）
    size_t inheritedCount = 0;
    for (auto& baseName : decl->baseClassNames) {
        auto baseIt = m_classTypes.find(baseName);
        if (baseIt != m_classTypes.end())
            inheritedCount += baseIt->second->classLayout.fields.size();
    }
    for (size_t i = inheritedCount; i < decl->fields.size(); ++i) {
        std::string rawName = decl->fields[i].name;
        for (auto& bfn : allBaseFieldNames) {
            if (rawName == bfn) {
                decl->fields[i].name = decl->name + "." + rawName;
                decl->fields[i].sourceClass = decl->name;
                break;
            }
        }
    }

    // ── 注册字段到符号表 ──
    // 注意此处遍历的是"合并后"的字段列表（基类字段已在继承处理时插到头部），
    // 逐一转成 FieldInfo 挂进布局表；具体 offset/size 稍后由
    // computeClassLayout 统一计算。
    for (auto& field : decl->fields) {
        // 字段类型必须先 resolveType：Parser 造的 Class("Five") 是占位类型
        // （classLayout 全空，totalSize=0），不换成注册版本会导致
        //   ① sizeInBytes()=0 → 字段占 0 字节，后续字段重叠、malloc 分配不足；
        //   ② inferMember 拿空布局查 o->f.a → "No member 'a'"。
        // 对照 clang（RecordLayoutBuilder.cpp:1850 LayoutField）：成员布局信息
        // 经 Context.getTypeInfoInChars(D->getType()) 一次取回 TI.Width/TI.Align，
        // 类型永远是 complete 的注册版（ASTContext::getASTRecordLayout 按需递归
        // 构建+缓存），不存在占位类型。
        field.type = resolveType(field.type);

        FieldInfo fi;
        fi.name = field.name;
        fi.type = field.type;
        fi.access = field.access;
        classType->classLayout.fields.push_back(fi);

        std::cout << std::format("    field: {} : {}\n",
            field.name, field.type ? field.type->toString() : "?");
    }

    // ── 合成默认构造与析构（若未显式提供）──
    bool hasExplicitCtor = false;
    bool hasExplicitDtor = false;
    for (auto& method : decl->methods) {
        if (std::dynamic_pointer_cast<ConstructorDecl>(method) || method->name == decl->name) {
            hasExplicitCtor = true;
        }
        if (std::dynamic_pointer_cast<DestructorDecl>(method) || method->name == "~" + decl->name) {
            hasExplicitDtor = true;
        }
    }

    if (!hasExplicitCtor) {
        auto defaultCtor = std::make_shared<ConstructorDecl>();
        defaultCtor->name = decl->name;
        defaultCtor->ownerClassName = decl->name;
        defaultCtor->returnType = Type::makeVoid();
        defaultCtor->isDefaultCtor = true;
        defaultCtor->body = std::make_shared<BlockStmt>();
        decl->methods.push_back(defaultCtor);
    }
    if (!hasExplicitDtor) {
        auto defaultDtor = std::make_shared<DestructorDecl>();
        defaultDtor->name = "~" + decl->name;
        defaultDtor->ownerClassName = decl->name;
        defaultDtor->returnType = Type::makeVoid();
        defaultDtor->isDefaultDtor = true;
        defaultDtor->body = std::make_shared<BlockStmt>();
        decl->methods.push_back(defaultDtor);
    }

    // ── 校验构造函数初始化列表 ──
    for (auto& method : decl->methods) {
        if (auto ctor = std::dynamic_pointer_cast<ConstructorDecl>(method)) {
            for (auto& init : ctor->initList) {
                bool found = false;
                // 检查是否匹配任一基类名
                for (auto& bn : decl->baseClassNames) {
                    if (init.memberName == bn) { found = true; break; }
                }
                // 检查是否匹配自身字段（含限定名）
                for (auto& f : decl->fields) {
                    if (f.name == init.memberName) {
                        // 去掉限定名再比
                        found = true; break;
                    }
                    // 匹配裸名
                    std::string rawName = f.name; // wangyang 有可能因为上面操作 成了 "a.name"这种结构
                    auto dot = rawName.find('.');
                    if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                    if (rawName == init.memberName) { found = true; break; }
                }
                if (!found) {
                    error(std::format("Member '{}' not found in class '{}' during initializer list",
                        init.memberName, decl->name), init.location);
                }
            }
        }
    }

    // ── 注册方法，检查虚函数 ──
    for (auto& method : decl->methods) {
        method->ownerClassName = decl->name;

        std::string methodNameInVTable = method->name.starts_with("~") ? "dtor" : method->name;

        // Override 检测：即使没有 virtual 关键字，也检查是否覆写基类虚函数
        // （C++ 标准：覆写虚函数不需要重新声明 virtual）
        bool overriddenInPrimary = false;
        bool overriddenInSecondary = false;

        // 查主表
        for (auto& entry : classType->classLayout.vtableEntries) {
            std::string entryFuncName = entry.baseFunctionName.empty()
                ? entry.mangledName.substr(entry.mangledName.find('_') + 1)
                : entry.baseFunctionName;
            if (entryFuncName == methodNameInVTable) {
                entry.mangledName = decl->name + "_" + methodNameInVTable; // wangyang 这里会对继承的多态函数修饰
                entry.baseFunctionName = methodNameInVTable;
                entry.isOverridden = true;
                overriddenInPrimary = true;
                // 覆写基类虚函数 → 自身也变成虚函数
                method->isVirtual = true;
                break;
            }
        }

        // 查次表 entries（多继承时次基类的覆写）
        // 注意：即使主表已命中，也要查次表——同名函数可能同时存在于主表和次表
        // （如 whoAmI 同时在 A 和 B 中），需要同时更新两处的覆写
        for (auto& base : classType->classLayout.bases) {
            if (base.isPrimary || !base.hasVTable) continue;
            for (auto& entry : base.entries) {
                std::string entryFuncName = entry.baseFunctionName.empty()
                    ? entry.mangledName.substr(entry.mangledName.find('_') + 1)
                    : entry.baseFunctionName;
                if (entryFuncName == methodNameInVTable) {
                    entry.mangledName = decl->name + "_" + methodNameInVTable; // wangyang 这里会覆盖掉次基类  原先的名字
                    entry.baseFunctionName = methodNameInVTable;
                    entry.isOverridden = true;
                    overriddenInSecondary = true; // 覆写了，那么下面就不用在写了
                    // 覆写基类虚函数 → 自身也变成虚函数
                    method->isVirtual = true;
                    // thunk 调整量：-(次基类子对象偏移)
                    entry.thunkAdjust = -(int)base.offset;
                    std::cout << std::format("      ↳ override in secondary '{}': thunkAdjust={}\n",
                        base.baseClassName, entry.thunkAdjust);
                    break;
                }
            }
        }

        std::cout << std::format("    method: {}() → {}{}\n",
            method->name,
            method->returnType ? method->returnType->toString() : "?",
            method->isVirtual ? " [virtual]" :
            method->isOverride ? " [override]" : "");

        if (method->isVirtual) {
            classType->classLayout.hasVTable = true;

            if (!overriddenInPrimary && !overriddenInSecondary) { // wangyang **既没有覆盖主虚函数 又没有覆盖次虚函数
                VTableEntry entry;
                entry.mangledName = decl->name + "_" + methodNameInVTable; // wangyang 这里会修饰为当前 类名_方法名
                entry.baseFunctionName = methodNameInVTable;
                entry.index = static_cast<uint32_t>(
                    classType->classLayout.vtableEntries.size());
                classType->classLayout.vtableEntries.push_back(entry); // wangyang 也就是说classLayout 只会放 主基类和自己的虚函数
            }
        }
        std::cout << std::format("  ◀◀ END override check for '{}' (class '{}')\n",
            method->name, decl->name);
    }

    // ── 计算内存布局 ──
    computeClassLayout(decl);

    // ── 注入 vtable 和 RTTI ──
    if (classType->classLayout.hasVTable) {
        injectVTableAndRTTI(classType);
    }

    // 写回最终字段列表：decl->fields（基类+自身，且已被 computeClassLayout
    // 填好 offset/size）整体覆盖前面逐步 push 的版本，作为布局的权威结果。
    classType->classLayout.fields = decl->fields; // wangyang **这里时会做一个最终的回填

    // ── 注册到全局符号表 ──
    // 三处登记：m_classTypes（类型+布局）、m_classDecls（AST 声明）、
    // 符号表（kind=Type，供名字查找）。
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
// [basic.align] 的对齐规则（教学简化版）：
//   每个字段的对齐要求 = min(字段大小, 8)（8 字节封顶，无 packed/pragma）；
//   字段偏移 = 当前偏移向上取整到该对齐；整体大小 = 向上取整到最大对齐。
// 示例（含虚函数的类，字段 age:int、weight:double）：
//     +0   _vptr   (8B，hasVTable 时恒在偏移 0，隐藏指针)
//     +8   age     (4B，alignTo(8,4)=8)
//     +16  weight  (8B，alignTo(12,8)=16 —— 12~15 是填充 padding)
//     totalSize = alignTo(24,8) = 24
// 简化：字段按声明顺序逐一排布，不做字段重排/空基类优化。
void SemanticAnalyzer::computeClassLayout(ClassDeclPtr decl) {
    TypePtr classType = decl->classType;
    if (!classType) {
        classType = Type::makeClass(decl->name);
        decl->classType = classType;
    }

    uint32_t offset = 0;
    uint32_t maxAlign = 1;
    bool hasVTable = classType->classLayout.hasVTable;

    // 如果有虚函数，先放 _vptr
    if (hasVTable) {
        offset = 8;
        maxAlign = 8;
    }

    // ── 多继承布局：子对象偏移由 processClassDecl 的 [relocate] 阶段统一摆放 ──
    // 那里按 Itanium 规则已完成：primary（第一个多态基类，全非多态时取首个基类）
    // 占 offset 0，其余子对象从 primary 尾部依次对齐摆放。本函数只负责：
    //   ① 从 bases 里读出 primary 的名字（决定字段走"直接复用偏移"分支）；
    //   ② 逐字段放置（见下）。
    // 历史 bug：这里曾用 bi==0 判定主基类，与 processClassDecl 的
    // "第一个多态基类"标准打架——当非多态基类声明在前时，两边选出不同的
    // primary，导致 A.offset 与 P.offset 同为 0，A.x 压在 _vptr 上。
    std::string currentPrimaryBase;
    for (auto& base : classType->classLayout.bases) {
        if (base.isPrimary) {
            currentPrimaryBase = base.baseClassName; // 找到主基类
            break;
        }
    }

    // ── 逐字段放置 ──
    // 字段顺序：[主基类字段...][次基类1字段...][次基类2字段...][自身字段...]
    // 策略：对每个次基类字段，复用基类自身的偏移（field 在基类布局中的 offset），
    //   加上该基类在 D 中的子对象起始偏移（base.offset），即可得到在 D 中的绝对偏移。
    //   主基类字段 offset=0（共享 _vptr），直接复用基类偏移。
    //   自身字段从最后一个基类子对象尾部继续累加。
    uint32_t currentFieldOffset = offset;  // 自身字段起始偏移（动态推进）
    std::string prevSrcClass;

    // 先计算自身字段的起始偏移（所有基类子对象之后）
    if (!decl->baseClassNames.empty()) {
        for (auto& base : classType->classLayout.bases) {
            auto baseIt = m_classTypes.find(base.baseClassName);
            if (baseIt != m_classTypes.end()) {
                uint32_t end = base.offset + baseIt->second->classLayout.totalSize;
                if (end > currentFieldOffset) currentFieldOffset = end; //wangyang 这里是一种覆盖设置 offset，前面 已经设置过每个 class 的base offset ,每个layout 已经自己对齐过了
            }
        }
        currentFieldOffset = alignTo(currentFieldOffset, 8);// 最后对齐到8 就可以了
    }

    for (auto& field : decl->fields) {
        std::string srcClass = field.sourceClass;
        if (srcClass.empty()) srcClass = decl->name;

        if (srcClass == currentPrimaryBase) {
            // 主基类字段：直接用基类布局中的偏移（主基类 offset=0，共享 _vptr）
            // 查找该字段在基类布局中的原始偏移
            auto baseIt = m_classTypes.find(srcClass);
            if (baseIt != m_classTypes.end()) {
                std::string rawName = field.name;
                auto dot = rawName.find('.');
                if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                for (auto& bf : baseIt->second->classLayout.fields) {
                    std::string bfRaw = bf.name;
                    auto bfdot = bfRaw.find('.');
                    if (bfdot != std::string::npos) bfRaw = bfRaw.substr(bfdot + 1);
                    if (bfRaw == rawName) {
                        field.offset = bf.offset; // wangyang 字段的对齐，已经在自身计算的时候对齐过了
                        field.size = bf.size;
                        break;
                    }
                }
            }
        } else if (srcClass != decl->name) {
            // 次基类字段：base.offset + 基类内部偏移
            for (auto& base : classType->classLayout.bases) {
                if (base.baseClassName == srcClass) {
                    auto baseIt = m_classTypes.find(srcClass);
                    if (baseIt != m_classTypes.end()) {
                        std::string rawName = field.name;
                        auto dot = rawName.find('.');
                        if (dot != std::string::npos) rawName = rawName.substr(dot + 1);
                        for (auto& bf : baseIt->second->classLayout.fields) {
                            std::string bfRaw = bf.name;
                            auto bfdot = bfRaw.find('.');
                            if (bfdot != std::string::npos) bfRaw = bfRaw.substr(bfdot + 1);
                            if (bfRaw == rawName) {
                                field.offset = base.offset + bf.offset; // wangyang 这里会添加这个类的基础偏移位置
                                field.size = bf.size;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        } else {
            // 自身字段：从基类子对象尾部继续
            // ── size 与 align 分离（对照 clang LayoutField 的 TI.Width / TI.Align）──
            // fieldSize = 字段占用字节数；fieldAlign = 字段的对齐要求。
            // 关键：类类型字段的 align ≠ size——Five{bool×5} size=5 但 align=1，
            // 若拿 size 当 align 会推出 alignTo(4,5)=9 这种非 2 幂的错位布局。
            uint32_t fieldAlign = alignOf(field.type);   // 对齐要求（成员 align 递归 max）
            uint32_t fieldSize = field.type->sizeInBytes(); // 实际占用字节
            // wangyang 对齐的精髓就是 起始地址要能够 整除 filedAlign
            currentFieldOffset = alignTo(currentFieldOffset, fieldAlign);
            field.offset = currentFieldOffset;
            field.size = fieldSize;
            currentFieldOffset += fieldSize; // 加上这个字段的长度，等于当前位置
            maxAlign = std::max(maxAlign, fieldAlign);
        }
    }

    offset = currentFieldOffset;

    classType->classLayout.totalSize = alignTo(offset, maxAlign); // wangyang 最终长度也会进行一个对齐
    if (decl->fields.empty() && hasVTable) {
        classType->classLayout.totalSize = 8;
    }
}

// 注入 RTTI 符号并固化 vtable 索引（Itanium/GCC ABI 教学版）：
//   vtable 符号在内存中的布局：
//       vtable[-1] = &type_info   ← RTTI，位于 vtable 起始地址的前一槽
//       vtable[0]  = &第一个虚函数
//       vtable[1]  = &第二个虚函数 ……
//   RTTI 符号名按 GCC mangling：_ZTI + 类名长度 + 类名，
//   例：Animal → _ZTI6Animal（对照：vtable 本身是 _ZTV 前缀）。
//   最后统一重排 index：继承+override 过程中槽位可能乱序，
//   这里按最终顺序重新编号，CodeGen 按 index 生成间接跳转。
void SemanticAnalyzer::injectVTableAndRTTI(TypePtr classType) {
    classType->classLayout.rttiMangledName =
        "_ZTI" + std::to_string(classType->name.size()) + classType->name;

    for (size_t i = 0; i < classType->classLayout.vtableEntries.size(); i++) {
        classType->classLayout.vtableEntries[i].index = static_cast<uint32_t>(i);
    }

    // ── 多继承：计算次表段在 _ZTV 符号内的字节偏移 ──
    // _ZTV 布局：[主表头 16B][主表槽位...][次表头 16B][次表槽位...]...
    // 每段表头 = 16B（8B offset-to-top + 8B RTTI ptr）
    // 主表段 vtableSegmentOffset = 0（vptr 直接指主表头后 16B 处）
    uint32_t segmentOffset = 16 + static_cast<uint32_t>(
        classType->classLayout.vtableEntries.size()) * 8;
    for (auto& base : classType->classLayout.bases) {
        if (base.isPrimary || !base.hasVTable) continue;
        base.vtableSegmentOffset = segmentOffset;
        segmentOffset += 16 + static_cast<uint32_t>(base.entries.size()) * 8;
        std::cout << std::format("    [MI] secondary '{}' vtableSegmentOffset={}\n",
            base.baseClassName, base.vtableSegmentOffset);
    }
}

// 向上取整到 alignment 的倍数：alignTo(12,8)=16，alignTo(16,8)=16，
// alignTo(5,4)=8。等价于位运算 (offset+align-1) & ~(align-1) 的算术写法；
// alignment==0 时原样返回，防御除零。
uint32_t SemanticAnalyzer::alignTo(uint32_t offset, uint32_t alignment) {
    if (alignment == 0) return offset;
    return (offset + alignment - 1) / alignment * alignment;
}

// 类型的对齐要求（字节）。对照 clang：ASTContext::getTypeInfoInChars 对
// record 返回其 ASTRecordLayout 的 Alignment（= 成员 align 的递归最大值，
// 见 RecordLayoutBuilder.cpp UpdateAlignment），对标量返回 TargetInfo ABI 表值。
// 教学简化：标量 = min(size,8)（int→4 double→8 bool→1 指针/引用→8，与 ABI 表一致）；
// 类类型 = 各成员 alignOf 递归 max（封顶 8）；无成员的空类 → align 1。
// 绝不能拿 sizeInBytes 当 align：Five{bool×5} size=5 align=1，
// Five{int,int} size=8 align=4——size/align 是两个独立维度。
uint32_t SemanticAnalyzer::alignOf(const TypePtr& type) {
    if (!type) return 1;
    switch (type->kind) {
        case TypeKind::Const:
            return type->innerType ? alignOf(type->innerType) : 1;
        case TypeKind::Class: {
            uint32_t a = 1;
            for (auto& f : type->classLayout.fields)
                a = std::max(a, alignOf(f.type)); // 会找到 最大 的字段
            return std::min(a, 8u);
        }
        default: {
            uint32_t s = type->sizeInBytes();
            return s == 0 ? 1u : std::min(s, 8u); // Void/未知类型兜底 1
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 注册函数（Pass 2）
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把函数声明写进三处——
//   ① m_functionMap：名字 → 声明（调用点查这个，fullName 与裸名都登记）
//   ② m_functions：按声明顺序的列表（CodeGen 逐个输出汇编）
//   ③ 符号表：kind=Function（名字查找可见）
// 并生成 mangled 名：普通函数 = 函数名本身；成员函数 = 类名_函数名
// （CodeGen 的汇编标号、vtable 条目都用它）。
// 简化说明：符号表"一名一槽"，不支持普通函数重载（[over] 教学级简化），
// 同名后注册者覆盖前者；函数模板重载集另存 m_functionTemplateCandidates。
// 示例：Animal::speak(int) → mangledName="Animal_speak"，符号表条目
//       { name="speak", kind=Function, type=int, ownerClass="Animal" }
void SemanticAnalyzer::registerFunction(FuncDeclPtr decl) {
    decl->returnType = resolveType(decl->returnType);
    for (auto& param : decl->parameters) {
        param.type = resolveType(param.type);
    }

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
        if (decl->name.starts_with("~")) {
            decl->mangledName = decl->ownerClassName + "_dtor";
        } else {
            decl->mangledName = decl->ownerClassName + "_" + decl->name;
            // 构造函数/方法重载：同名多个时追加参数个数区分
            // 检查是否已有同名方法（当前方法尚未加入 m_functions 时已经 push_back 了，
            // 所以计数要 -1）
            size_t sameNameCount = 0;
            for (auto& f : m_functions) {
                if (f.get() != decl.get()
                    && f->ownerClassName == decl->ownerClassName
                    && f->name == decl->name) {
                    ++sameNameCount;
                }
            }
            if (sameNameCount > 0 || decl->parameters.size() > 0) {
                decl->mangledName += "_" + std::to_string(decl->parameters.size());
            }
        }
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

// ─────────────────────────────────────────────────────────────────────────────
// 内建外部函数原型注册（P4）
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：给 libc 的四个内存管理函数登记"语义外壳"——名字、形参表、返回类型，
//   body 一律 nullptr。三处登记缺一不可：
//     ① m_functionMap：调用点按裸名查找命中（与自由函数同一条路径）
//     ② m_functions：CodeGen 的 generate() 遍历它，但 `if (func->body)`
//        守卫自动跳过发射 → .s 里不会出现这些函数的汇编体
//     ③ 符号表：kind=Function，任何作用域可见
//   于是用户写 `int* p = malloc(8);`：语义检查认识名字、实参个数匹配，
//   调用点按普通函数发射 `callq malloc`，链接期由 libc 提供真实现。
//
// 对照 clang：内建也走"先有声明再使用"——Builtins::Info 大表给出每个
//   __builtin_* 的类型串（ASTContext::getBuiltinType 翻译成 C 原型），
//   语义检查照常进行；区别只在真 clang 多数内建会在 IR 层替换为
//   llvm.memcpy 等 intrinsic，而本项目直接调 libc 符号，链接器兜底。
//
// 签名取舍（教学级简化）：minicc 无 void/size_t 语义（只有 int 与指针），
//   故一律 int* 表示"一块内存"、int 表示"字节数"：
//     malloc(int)             → int*   （emitNew 已有 `callq malloc` 先例）
//     free(int*)              → void
//     memcpy(int*,int*,int)   → int*   （返回目标指针，对齐 libc 语义）
//     realloc(int*,int)       → int*   （原块扩容/搬家，返回新块首址）
// 对照 CodeGen::emitDelete 末尾的 `callq free`——那里是编译器自己合成的
//   调用，这里则把同一个符号暴露给用户代码，闭环自洽。
void SemanticAnalyzer::registerBuiltins() {
    // 登记一个原型：构造无 body 的 FunctionDecl 并写入三处
    auto declare = [this](const std::string& name, TypePtr ret,
                          std::vector<TypePtr> paramTypes) {
        auto decl = std::make_shared<FunctionDecl>();
        decl->name = name;
        decl->returnType = std::move(ret);
        decl->body = nullptr;  // 纯声明 → CodeGen 跳过发射，链接期由 libc 提供
        for (size_t i = 0; i < paramTypes.size(); i++) {
            Parameter p;
            p.name = "arg" + std::to_string(i);
            p.type = std::move(paramTypes[i]);
            decl->parameters.push_back(std::move(p));
        }
        decl->mangledName = name;          // 外部符号：不修饰，直接用原名
        m_functionMap[name] = decl;
        m_functions.push_back(decl);

        Symbol sym;
        sym.name = name;
        sym.type = decl->returnType;
        sym.kind = SymbolKind::Function;
        sym.isLocal = false;
        m_symbolTable.globalScope()->define(name, sym);  // 显式进全局作用域
    };

    TypePtr intTy  = Type::makeInt();
    TypePtr intPtr = Type::makePointer(intTy);
    TypePtr voidTy = Type::makeVoid();

    declare("malloc", intPtr, {intTy});               // malloc(int n)
    declare("free", voidTy, {intPtr});                // free(int* p)
    declare("memcpy", intPtr, {intPtr, intPtr, intTy}); // memcpy(dst, src, n)
    declare("realloc", intPtr, {intPtr, intTy});      // realloc(p, n)

    std::cout << "  [builtin] ✔ registered libc prototypes: "
                 "malloc(int)→int*, free(int*), "
                 "memcpy(int*,int*,int)→int*, realloc(int*,int)→int*    "
                 "[P4: no body — codegen skips, linker binds libc]\n";
}

// ═════════════════════════════════════════════════════════════════════════════
// 分析函数体（Pass 3）—— 类型推导与符号决议的主战场
// ═════════════════════════════════════════════════════════════════════════════
// 函数作用域 [basic.scope.function] 的构建流程：
//   1. enterScope(函数名)          → 建立本函数作用域
//   2. 参数入符号表                → 每个参数占一个 8 字节栈槽
//      （教学模型：统一 8 字节槽，不区分 int/double/指针；栈向低地址生长，
//        偏移依次为 -8, -16, -24 …）
//   3. 成员函数追加 this           → [class.this]：隐式首参，
//      类型 = 指向属主类的指针
//   4. 逐条 processStmt(函数体)    → 体内局部变量继续 -8/槽
// 示例：int add(int a, int b) { int s = a + b; return s; }
//   符号表：a:Parameter stack@-8 | b:Parameter stack@-16 | s:Variable stack@-24
// 本函数也被 S5 复用：模板实例化出的函数在此做"两阶段查找的第二阶段"
// （用具体类型查体），因此有下面的上下文保存/恢复。
void SemanticAnalyzer::analyzeFunctionBody(FuncDeclPtr decl) {
    if (!decl->body) return;

    std::string funcLabel = decl->ownerClassName.empty()
        ? decl->name
        : decl->ownerClassName + "::" + decl->name;

    std::cout << std::format("\n  ╔══ Function Body: {} ══╗\n", funcLabel);

    m_symbolTable.enterScope(funcLabel);
    // ── 保存外层上下文 ──
    // 模板实例化（S5）会在分析外层函数期间嵌套调用本函数分析实例体，
    // 不保存/恢复会把外层的 m_currentReturnType 等踩掉（真实 bug：
    // main 分析中途实例化 void 函数后，main 的 return 被按 void 检查）。
    int         savedStackOffset = m_stackOffset;
    std::string savedClassName   = m_currentClassName;
    TypePtr     savedReturnType  = m_currentReturnType;
    std::string savedFuncName    = m_currentFuncName;
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

    // ── 恢复外层上下文 ──
    m_stackOffset       = savedStackOffset;
    m_currentClassName  = savedClassName;
    m_currentReturnType = savedReturnType;
    m_currentFuncName   = savedFuncName;

    std::cout << std::format("  ╚══ End {} ══╝\n\n", funcLabel);
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板声明处理
// ─────────────────────────────────────────────────────────────────────────────
// 注册模板蓝图（[temp] 教学模型：蓝图 = 等待实参的 AST 半成品）。
// 两个分支：
//   类模板：只存储蓝图，不在语义分析阶段展开。显式实参的收集与检查
//          （[temp.arg.explicit] 简化：只支持显式实参、个数按 typeParams
//          对齐）在语义分析之后由驱动程序（main.cpp 阶段 4）逐组实参调用
//          TemplateInstantiator::instantiate 完成。
//   函数模板（S1）：注册进候选集 m_functionTemplateCandidates 等待调用点
//          推导，不查函数体（理由见函数体内注释——两阶段查找第一阶段）。
void SemanticAnalyzer::processTemplateDecl(TemplateDeclPtr decl) {
    std::cout << std::format("  [register] template <");
    for (size_t i = 0; i < decl->typeParams.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << "typename " << decl->typeParams[i];
    }

    if (decl->isClassTemplate()) {
        std::cout << std::format("> {} (class blueprint stored, not analyzed)\n",
            decl->templateName());
        // ── 类模板注册表（与函数模板候选集对称）──
        // 名字→蓝图 O(1) 查找；emplace 不覆盖，重名取先注册者
        // （与旧线性扫描 m_templates 取第一个命中的语义一致）。
        auto [it, inserted] =
            m_classTemplates.emplace(decl->templateName(), decl);
        std::cout << std::format("    ↳ class template '{}' registered{}\n",
            decl->templateName(),
            inserted ? "" : " (duplicate name, first registration wins)");
    } else {
        // ── 函数模板（S1）──
        // 只注册蓝图，不分析函数体：模板体中的 T 是"依赖类型"，
        // 要到实例化时（S5，由调用点推导驱动）才能做类型检查。
        // 这正是两阶段名称查找的第一阶段：非依赖名现在查，依赖名推迟。
        auto& func = decl->funcTemplate;
        std::string paramStr;
        for (size_t i = 0; i < func->parameters.size(); i++) {
            if (i > 0) paramStr += ", ";
            paramStr += func->parameters[i].type->toString() + " "
                      + func->parameters[i].name;
        }
        std::cout << std::format("> {}({}) → {} (function blueprint stored, awaiting call-site deduction)\n",
            func->name, paramStr,
            func->returnType ? func->returnType->toString() : "void");

        auto& candidates = m_functionTemplateCandidates[func->name];
        candidates.push_back(decl);
        std::cout << std::format("    ↳ overload candidate set '{}' size = {}\n",
            func->name, candidates.size());
    }

    m_templates.push_back(decl);
}

// ═════════════════════════════════════════════════════════════════════════════
// 语句处理
// ═════════════════════════════════════════════════════════════════════════════
// 语句分发器：用 dynamic_pointer_cast 逐一尝试具体语句类型
// （教学版"双分派"；clang 用更高效的 StmtVisitor 按 StmtClass 枚举跳转）。
// 未匹配任何已知类型的节点被静默跳过。
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
    else if (auto del = std::dynamic_pointer_cast<DeleteStmt>(stmt))
        processDeleteStmt(del);
}

void SemanticAnalyzer::processDeleteStmt(std::shared_ptr<DeleteStmt> stmt) {
    TypePtr ptrType = inferType(stmt->pointerExpr);
    if (!ptrType || !ptrType->isPointer()) {
        error("delete operand must be a pointer", stmt->location);
    }
    std::cout << std::format("  [delete] delete {}{}\n",
        stmt->isArray ? "[] " : "", ptrType->toString());
}

// 复合语句 `{ … }` → 新建块作用域（[basic.scope.block]）：
// 块内声明的变量在 exitScope 后不再可见。
// 示例：while 体内 `{ int t = 0; … }` —— t 只在块内可见，块外引用 t
//       会报 Undefined variable。
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
//
// 完整示例 `auto x = 42;`：
//   declaredType=auto → inferType(IntLiteral(42)) → int
//   → 改写 AST：decl->declaredType := int（后续阶段再也看不到 auto）
//   → 分配栈槽 stack@-8，符号表写入 { x, int, Variable, stack@-8 }
// 非 auto 但有初值时做类型检查：
//   `int x = y;`（y:int&）→ typeCompatible 剥引用放行（[conv.lval]）
//   `int x = 3.14;`        → 无兼容规则 → 报 Type mismatch
// ═════════════════════════════════════════════════════════════════════════════
void SemanticAnalyzer::processVarDecl(std::shared_ptr<VarDeclStmt> decl) {
    decl->declaredType = resolveType(decl->declaredType);
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
            // ── 类型检查（含 [conv.lval] 引用剥除 / 数值提升）──
            if (!type->equals(initType)) {
                if (typeCompatible(type, initType)) {
                    std::cout << std::format("  [conv] {} : {} ⟶ {} (implicit: ref-strip / promotion)\n",
                        decl->name,
                        initType ? initType->toString() : "?", type->toString());
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
    // 分配栈槽：统一 8 字节/变量（教学简化），栈向低地址生长，
    // 该偏移与 CodeGen 的栈帧布局一一对应。
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

// if 语句：条件必须可语境转换为 bool（[stmt.select]），
// 教学级简化为只接受 bool | int（不做指针/类的隐式转换链）。
// then / else 分支各自进入独立作用域（[basic.scope.block]），
// 分支内声明的变量互不可见。
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

// while 语句：推导条件类型（教学级未强制 bool|int，比 if 宽松），
// 循环体进入独立作用域。注意：循环体只静态分析一遍——动态的反复执行
// 是运行期的事，语义分析只保证"每一遍都类型合法"。
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

// return 语句 [stmt.return]：返回值类型必须与函数声明的返回类型兼容
// （typeCompatible 负责剥引用/顶层 const、int→double 提升）。
// 示例：int f() 中 `return x;`（x:int&）✓ 引用剥除后匹配；
//       int f() 中 `return 3.14;` ✗ 报 Return type mismatch。
// 反向情况：void 函数带返回值会被 typeCompatible(void, T)=false 拦下；
// 但"非 void 函数漏写 return / 并非所有路径都有 return"不做流分析
// （clang 在 CheckReturnVal 之外还有 CFG 流敏感检查，此处为教学级简化）。
void SemanticAnalyzer::processReturnStmt(std::shared_ptr<ReturnStmt> stmt) {
    if (stmt->value) {
        m_inferDepth++;
        TypePtr retType = inferType(stmt->value);
        m_inferDepth--;

        std::cout << std::format("  [return] type: {} (expected: {})\n",
            retType ? retType->toString() : "?",
            m_currentReturnType ? m_currentReturnType->toString() : "?");

        // 类型检查（含 [conv.lval] 引用剥除 / 数值提升）
        TypePtr expType = resolveType(m_currentReturnType);
        if (retType && expType
            && !typeCompatible(expType, retType)) {
            error(std::format(
                "Return type mismatch: expected '{}', got '{}'",
                expType->toString(), retType->toString()),
                stmt->location);
        }
    } else {
        std::cout << "  [return] void\n";
    }
}

// 赋值语句：分别推导左值（target）与右值（value）的类型并打日志。
// 教学级简化：不检查"左侧必须是可修改左值"、不检查左右类型兼容性
// （clang 在 SemaExpr.cpp 的 CheckAssignmentOperands 中完成这两件事）；
// 左值性目前只在 inferCall 的模板推导路径中发挥作用。
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

// 表达式语句：只求类型不求值——值被丢弃，语句的意义在副作用
// （典型如 `foo();`）。推导本身会触发符号决议与类型检查。
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
//
// 理论依据：语法制导的类型推导（[expr] 类型规则即属性文法中的
// 综合属性）——每个节点的类型由其子节点自底向上合成。
// 推导结果写回 expr->resolvedType 这一 AST 注解，供模板实例化与
// CodeGen 直接消费（clang 对应物：Expr::setType / getType）。
// 分派方式与 processStmt 相同：dynamic_pointer_cast 逐一尝试。
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
    else if (auto e = std::dynamic_pointer_cast<DynamicCastExpr>(expr))
        type = inferDynamicCast(e);
    else if (auto e = std::dynamic_pointer_cast<IndexExpr>(expr))
        type = inferIndex(e);
    else if (auto e = std::dynamic_pointer_cast<DeleteExpr>(expr)) {
        inferType(e->pointerExpr);
        type = Type::makeVoid();
    }

    expr->resolvedType = type;
    return type;
}

// ─── 字面量推导（最简单：类型由字面量本身决定）───
// 与标准的差异（教学简化）：
//   整数字面量   → int（标准有 int/long/long long 试配序列，[lex.icon]）
//   字符串字面量 → 指针类型（标准是 const char[N] 数组退化为 const char*，
//                 [lex.string]；本类型系统无 char 类型，以 int* 承载地址）
//   nullptr     → void*（标准有独立类型 std::nullptr_t，[lex.nullptr]）
//   bool 字面量  → bool（与标准一致，[lex.bool]）

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
// 示例（类 C 的成员函数内，块中引用名字 `x`）：
//   ① 作用域链：block ✗ → 函数作用域 ✗ → global ✗
//   ② 类字段：C 的布局表 findField("x") ✓ → 返回字段类型
//      （即隐式 this->x，"编译期看符号"在此兑现为字段偏移的线索）
//   ③ 若①②皆失：查 m_functionMap —— x 是函数名则返回其返回类型
//   全失 → "Undefined variable 'x'"
// 命中类类型变量时，会把符号表里的类型替换为 m_classTypes 中带完整
// 布局信息的版本（符号表只记名字→类型，布局细节在注册表里）。
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
// 两条规则分支：
//   比较/逻辑（== != < > <= >= && ||）→ 结果恒为 bool
//     （[expr.rel]/[expr.eq]/[expr.log.and]/[expr.log.or]）
//   算术（+ - * /）→ 常用算术转换 [expr.arith.conv] 的大幅简化：
//     任一侧 double → double（int 提升）；否则 int × int → int
// 示例：x:int + y:double → double；a:int < b:int → bool
// 简化：不检查操作数类型组合的合法性（如 bool+bool 也放行），
//       clang 在 SemaExpr.cpp 的 CheckBinOp 中逐组合校验。
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

// 一元表达式 [expr.unary.op]：
//   逻辑非 ! → 结果恒为 bool（操作数被语境转换为 bool）
//   算术取负 - → 保持操作数类型（简化：不校验操作数是否为数值类型）
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
// 重载决议 [over.match] 的三阶段（本实现简化流）：
//   ① 候选 candidates：同名普通函数（m_functionMap）与同名函数模板
//      候选集（m_functionTemplateCandidates）
//   ② 可行 viable：参数个数匹配；模板须推导成功（resolveTemplateCall）
//   ③ 最优 best：非模板优先于模板（[over.match.best] 规则简化）；
//      模板之间按偏序取最特化（S6 isAtLeastAsSpecialized）
//
// 示例：`twice(42)`，同时存在 twice(int) 与 template<T> T twice(T)：
//   ① m_functionMap 命中 twice(int)，参数个数匹配 → 直接返回 int
//      （非模板胜出，模板候选根本不参与）
//   若只有模板 → 推导 T:=int → 实例化 _Z5twiceIiE → 返回 int
//
// 方法调用（callee 是 MemberExpr）只走普通函数路径，
// 并置 isMethodCall 标记供 CodeGen 处理隐式 this。
TypePtr SemanticAnalyzer::inferCall(std::shared_ptr<CallExpr> expr) {
    // ── 确定 callee 名字与形态 ──
    std::string funcName;
    std::shared_ptr<VarExpr> calleeVar =
        std::dynamic_pointer_cast<VarExpr>(expr->callee);
    if (calleeVar) {
        funcName = calleeVar->name;
    } else if (auto mem = std::dynamic_pointer_cast<MemberExpr>(expr->callee)) {
        funcName = mem->memberName;
        mem->isMethodCall = true;
        // 修复（对齐 HEAD 行为）：先推导 callee 成员表达式，
        // 给 mem->object 标注 resolvedType（如 shape → Shape*）。
        // CodeGen::emitCall 依赖该类型解引用出类名、查 vtable 条目，
        // 从而把 shape->area() 降级为经 _vptr 的间接调用；
        // 否则下方 m_functionMap 命中方法名后提前返回，
        // 对象类型永远不会被推导，虚调用退化为直接 callq。
        // 注意：只对 MemberExpr 补推导——普通函数/模板的 VarExpr callee
        // 若在此推导会因符号表查无此"变量"而误报 Undefined variable。
        m_inferDepth++;
        inferType(mem);
        m_inferDepth--;
    }

    // ── 推导实参类型（并记录左值性：变量/成员访问是左值）──
    // 左值性是推导的输入：T& 绑定检查、万能引用折叠（S2）
    std::vector<TypePtr> argTypes;
    std::vector<bool>    argIsLValue;
    for (auto& arg : expr->arguments) {
        m_inferDepth++;
        TypePtr t = inferType(arg);
        m_inferDepth--;
        argTypes.push_back(t);
        // ★ 值类别（lvalue/rvalue）判定——简化模型：
        //   变量引用 / 成员访问 → 有确定内存地址 → 左值；
        //   字面量 / 算术结果 / 调用返回值 → 右值。
        //   该标记随 argTypes 传给 TemplateDeducer 执行引用绑定检查
        //   （[dcl.init.ref]）：非 const T& 拒绝绑定右值实参；
        //   T&& 为万能引用，经引用折叠后左右值皆可绑定（S2 实现）。
        bool isLValue = std::dynamic_pointer_cast<VarExpr>(arg)
                     || std::dynamic_pointer_cast<MemberExpr>(arg);
        argIsLValue.push_back(isLValue);
        std::cout << std::format("{}  arg: {}{}\n", inferIndent(),
            t ? t->toString() : "?", isLValue ? " (lvalue)" : " (rvalue)");
    }

    // ── 成员方法调用（P3 修复）：按"对象类 → 方法表"查，不走全局名字表 ──
    // ★ wangyang: Box_int::get 与 Box_double::get 这类同名方法（模板实例、
    // 或多类同名成员）在全局 m_functionMap 里互相覆盖（registerFunction 以
    // 裸名作键），直接查名字表会随机命中别的类的方法，返回类型/参数全错。
    // 对照 clang：成员调用走 UnqualifiedIdExpr 的类作用域限定查找
    // （BuildMemberCallExpr → LookupMember），普通名字查找只是兜底。
    // 前提：先补推导 callee 成员表达式，拿到 mem->object 的 resolvedType。
    if (auto mem = std::dynamic_pointer_cast<MemberExpr>(expr->callee)) {
        if (!mem->object->resolvedType) {
            m_inferDepth++;
            inferType(mem->object);
            m_inferDepth--;
        }
        TypePtr objType = mem->object->resolvedType;
        if (objType && objType->isPointer()) objType = objType->pointeeType;
        if (objType && objType->isClass()) {
            // 搜索对象类及其所有基类（BFS）的方法
            std::vector<std::string> searchQueue = {objType->name};
            std::set<std::string> searched;
            while (!searchQueue.empty()) {
                std::string clsName = searchQueue.back(); searchQueue.pop_back();
                if (!searched.insert(clsName).second) continue;
                auto classIt = m_classDecls.find(clsName);
                if (classIt != m_classDecls.end()) {
                    for (auto& method : classIt->second->methods) {
                        if (method->name == funcName &&
                            method->parameters.size() == argTypes.size()) {
                            std::cout << std::format(
                                "{}[call] {}.{}({} args) → {}    [class-scoped member call{}]\n",
                                inferIndent(), objType->name, funcName, argTypes.size(),
                                method->returnType ? method->returnType->toString() : "?",
                                clsName != objType->name ? std::format(" via '{}'", clsName) : "");
                            return method->returnType;
                        }
                    }
                    // 搜基类
                    for (auto& bn : classIt->second->baseClassNames)
                        searchQueue.insert(searchQueue.begin(), bn);
                }
            }
        }
        // 类里没查到 → 落入下方既有路径（兼容既有行为与报错）
    }

    // ── 普通函数/方法（既有路径；S6：非模板优先于模板）──
    auto it = m_functionMap.find(funcName);
    if (it != m_functionMap.end()) {
        if (it->second->parameters.size() == argTypes.size()) {
            std::cout << std::format(
                "{}[call] {}({} args) → {}    [symbol resolved, non-template preferred]\n",
                inferIndent(), funcName, argTypes.size(),
                it->second->returnType ? it->second->returnType->toString() : "?");
            return it->second->returnType;
        }
        std::cout << std::format(
            "{}[call] non-template '{}' arg count mismatch ({} vs {}), keep searching\n",
            inferIndent(), funcName, it->second->parameters.size(), argTypes.size());
    }

    // ── 函数模板路径（S2~S6）：仅对非成员调用 ──
    if (calleeVar) {
        TypePtr resolved = resolveTemplateCall(
            funcName, calleeVar, argTypes, argIsLValue,
            calleeVar->explicitTemplateArgs, expr->location);
        if (resolved) return resolved;
    }

    // ── 兜底：原有 callee 推导路径（可能报 Undefined variable）──
    m_inferDepth++;
    TypePtr calleeType = inferType(expr->callee);
    m_inferDepth--;
    std::cout << std::format("{}[call] {}() → {} (callee type)\n",
        inferIndent(), funcName,
        calleeType ? calleeType->toString() : "?");
    return calleeType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数模板调用解析（S2~S6 全链路）
// ─────────────────────────────────────────────────────────────────────────────
// 流程（对照 clang: AddTemplateOverloadCandidate → 推导 → 排序）：
//   1. 取同名候选集
//   2. 逐候选推导（S2 核心，含 S3 显式前缀 / S4 不可推导上下文检查）
//   3. 可行候选按偏序排序选最特化（S6）
//   4. 实例化赢家（S5，带缓存），把 callee 重写为 mangled 符号
//
// 示例：twice(42)，候选 { template<T> T twice(T) }：
//   候选 1 个 → deducer.deduce：P=T, A=int ⇒ T := int ✓ → 可行
//   → 实例化 twice<int> → mangled _Z5twiceIiE
//   → AST 中 callee 名字被原地改写为 _Z5twiceIiE（CodeGen 直接 callq）
//   → 返回实例的返回类型 int
// 全部候选推导失败 → 按 [over.match.viable] 报 "no matching function"。
// 返回 nullptr 表示"没有模板候选"，交回 inferCall 的兜底路径。
TypePtr SemanticAnalyzer::resolveTemplateCall(
    const std::string& funcName,
    std::shared_ptr<VarExpr> calleeVar,
    const std::vector<TypePtr>& argTypes,
    const std::vector<bool>& argIsLValue,
    const std::vector<TypePtr>& explicitArgs,
    SourceLocation loc) {

    auto candIt = m_functionTemplateCandidates.find(funcName);
    if (candIt == m_functionTemplateCandidates.end() || candIt->second.empty())
        return nullptr;

    auto& candidates = candIt->second;
    std::cout << std::format(
        "{}[call] {} — {} function template candidate(s), overload resolution begins\n",
        inferIndent(), funcName, candidates.size());

    TemplateDeducer deducer;
    struct Viable { TemplateDeclPtr decl; DeductionResult result; };
    std::vector<Viable> viables;

    for (auto& cand : candidates) {
        std::string plist;
        for (size_t i = 0; i < cand->typeParams.size(); i++) {
            if (i > 0) plist += ", ";
            plist += cand->typeParams[i];
        }
        std::cout << std::format("{}  candidate: template <{}> {}\n",
            inferIndent(), plist, funcName);
        auto r = deducer.deduce(cand, argTypes, argIsLValue, explicitArgs);
        if (r.success) {
            viables.push_back({cand, r});
        } else {
            std::cout << std::format("{}  candidate rejected: {}\n",
                inferIndent(), r.failureReason);
        }
    }

    if (viables.empty()) {
        error(std::format(
            "no matching function for call to '{}' (no template candidate deduces successfully)",
            funcName), loc);
    }

    // ── S6：偏序 —— 选最特化的可行候选 ──
    size_t winnerIdx = 0;
    for (size_t i = 1; i < viables.size(); i++) {
        if (isAtLeastAsSpecialized(viables[i].decl, viables[winnerIdx].decl)) {
            winnerIdx = i;
        }
    }
    if (viables.size() > 1) {
        std::cout << std::format(
            "{}  [overload] {} viable candidate(s), partial ordering picks #{}\n",
            inferIndent(), viables.size(), winnerIdx);
    }

    auto& winner = viables[winnerIdx];

    // ── S5：实例化（带缓存）──
    FuncDeclPtr instance =
        getOrInstantiateFunction(winner.decl, winner.result.deducedArgs);

    // 把 callee 重写为 mangled 符号，codegen 直接 callq
    calleeVar->name = instance->mangledName;
    std::cout << std::format("{}[call] {} → {} (template resolved) → {}\n",
        inferIndent(), funcName, instance->mangledName,
        instance->returnType ? instance->returnType->toString() : "void");

    return instance->returnType;
}

// ─────────────────────────────────────────────────────────────────────────────
// 实例化函数模板（S5）：带缓存，避免同一 <实参> 重复实例化
// ─────────────────────────────────────────────────────────────────────────────
// [temp.inst]：同一模板 + 同一实参列表，全局只实例化一次。
// 缓存键 = mangled 名，例：twice<int> → _Z5twiceIiE
// （_Z + 名字长度 5 + twice + I + i(int 的编码) + E）。
// 新实例诞生后的三步：
//   ① m_functionMap[mangled] = 实例（后续调用直接命中）
//   ② push 进 m_functions（CodeGen 按序输出汇编）
//   ③ analyzeFunctionBody(实例)：用具体类型检查函数体 ——
//      这就是两阶段查找 [temp.names] 的第二阶段
//      （第一阶段 = processTemplateDecl 只注册蓝图不查体）。
// ─────────────────────────────────────────────────────────────────────────────
// P3：类模板按需实例化（[temp.inst] 隐式实例化点）
// ─────────────────────────────────────────────────────────────────────────────
// 触发时机：类型位置（变量声明/形参/返回类型）出现 Box<int> 这类模板 id，
// resolveType 发现"带实参的类名命中类模板蓝图"即调入本函数。
// 三步走（对照 clang：Sema::ActOnTag → InstantiateClass → 成员延迟分析）：
//   ① instantiate()    深拷贝蓝图 + 结构化替换 → 实例 ClassDecl（如 Box_int）
//   ② processClassDecl 实例类按普通类走完整注册（构造/析构合成、布局、
//                      vtable/RTTI、符号表）
//   ③ 实例方法注册 + 函数体分析 —— 即两阶段查找的第二阶段：
//      蓝图期无法检查的依赖类型，在实参落地后做真正的类型检查。
// 缓存先行：先写缓存再析方法体，方法体若再引用同一实例（递归/互用）直接命中。
TypePtr SemanticAnalyzer::getOrInstantiateClass(
    TypePtr templateIdType, SourceLocation loc) {

    // ── 缓存键：模板名 + 实参可读串，如 "Box<int>" ──
    std::string key = templateIdType->name + "<";
    for (size_t i = 0; i < templateIdType->templateArgs.size(); i++) {
        if (i > 0) key += ",";
        key += templateIdType->templateArgs[i]
                   ? templateIdType->templateArgs[i]->toString() : "?";
    }
    key += ">";

    auto cached = m_classInstanceCache.find(key);
    if (cached != m_classInstanceCache.end()) {
        std::cout << std::format(
            "  [instantiate:class] cache hit: {} (skip re-instantiation)\n", key);
        return cached->second;
    }

    // ── 查蓝图（类模板注册表 O(1)；重名取先注册者，emplace 不覆盖）──
    TemplateDeclPtr blueprint;
    if (auto it = m_classTemplates.find(templateIdType->name);
        it != m_classTemplates.end()) {
        blueprint = it->second;
    }
    if (!blueprint) {
        error(std::format("'{}' is not a class template", templateIdType->name), loc);
    }
    if (blueprint->typeParams.size() != templateIdType->templateArgs.size()) {
        error(std::format(
            "class template '{}' expects {} type argument(s), but {} given",
            templateIdType->name, blueprint->typeParams.size(),
            templateIdType->templateArgs.size()), loc);
    }

    std::cout << std::format(
        "\n  [instantiate:class] ★ on-demand instantiation: {}\n", key);

    // ── ① 深拷贝蓝图 + 结构化替换 ── // wangyang **** 这里就是我一直想要的部分，对template 进行实例化解析
    ClassDeclPtr instance =
        m_instantiator.instantiate(blueprint, templateIdType->templateArgs);

    // ── ② 实例类完整注册（与源码中手写的类一视同仁）──
    processClassDecl(instance);

    TypePtr instanceType = m_classTypes[instance->name];

    // 实例类符号同时登记进全局作用域：实例化可能在某函数体分析中途触发，
    // processClassDecl 会把符号 define 进该函数作用域——兄弟函数不可见。
    // 全局作用域冗余登记一份，保证任何位置 lookup("Box_int") 都命中。
    Symbol globalSym;
    globalSym.name = instance->name;
    globalSym.type = instanceType;
    globalSym.kind = SymbolKind::Type;
    globalSym.isLocal = false;
    globalSym.definedAt = instance->location;
    m_symbolTable.globalScope()->define(instance->name, globalSym);

    // 缓存先行写入（方法体分析可能再次引用本实例——递归/互用场景）
    m_classInstanceCache[key] = instanceType;

    // ── ③ 实例方法：注册（等价 Pass 2）+ 函数体分析（等价 Pass 3）──
    for (auto& method : instance->methods) {
        registerFunction(method);
    }
    for (auto& method : instance->methods) {
        analyzeFunctionBody(method);
    }

    std::cout << std::format(
        "  [instantiate:class] ✔ {} ready ({} bytes, {} method(s))\n",
        instance->name, instanceType->classLayout.totalSize,
        instance->methods.size());
    return instanceType;
}

FuncDeclPtr SemanticAnalyzer::getOrInstantiateFunction(
    TemplateDeclPtr tmpl, const std::vector<TypePtr>& args) {
    std::string mangled =
        NameMangler::mangleTemplateInstance(tmpl->funcTemplate->name, args);

    auto it = m_templateInstanceCache.find(mangled);
    if (it != m_templateInstanceCache.end()) {
        std::cout << std::format("  [instantiate] cache hit: {} (skip re-instantiation)\n",
            mangled);
        return it->second;
    }

    FuncDeclPtr instance = m_instantiator.instantiateFunction(tmpl, args);
    m_templateInstanceCache[mangled] = instance;

    // 注册进 codegen 函数列表；函数体在具体类型下做两阶段查找的第二阶段
    m_functionMap[mangled] = instance;
    m_functions.push_back(instance);
    analyzeFunctionBody(instance);
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// S6 偏序：a 是否"至少与 b 同样特化"（deduction-based，[temp.func.order] 简化）
// ─────────────────────────────────────────────────────────────────────────────
// 标准规则方向（易错点）：用 a 自己的参数类型当合成实参，去推导 b。
// 若 b 能推导成功，说明 b 覆盖 a 的全部输入域 → a 接受的类型更少 → a 更特化。
// 例：po(T) vs po(T*)
//   用 T* 当实参推导 po(T) → T := T* 成功 → po(T*) 更特化 ✓
// 对照 clang：lib/Sema/SemaOverload.cpp → IsAtLeastAsSpecialized
bool SemanticAnalyzer::isAtLeastAsSpecialized(TemplateDeclPtr a, TemplateDeclPtr b) {
    TemplateDeducer deducer;
    std::vector<TypePtr> synthArgs;
    std::vector<bool>    synthLValue;
    for (auto& p : a->funcTemplate->parameters) {
        synthArgs.push_back(p.type);   // a 的模板参数名在此扮演"唯一合成类型"
        synthLValue.push_back(true);
    }

    std::cout << std::format("  [overload] partial ordering check (deduction-based)...\n");
    auto r = deducer.deduce(b, synthArgs, synthLValue);
    std::cout << std::format("  [overload]   ⇒ {}\n",
        r.success ? "前者至少与后者同样特化（前者胜）"
                  : "无特化关系（保留原候选）");
    return r.success;
}

// ─────────────────────────────────────────────────────────────────────────────
// 成员访问推导 —— "编译期看符号 → 运行期看偏移量"的转换点
// ─────────────────────────────────────────────────────────────────────────────
// 成员访问 [expr.ref] 的两条形态：
//   p->x（isArrow）：p 是指针 → 先取 pointeeType 解引用 → 再查成员
//   obj.x         ：obj 是类对象本身 → 直接查成员
// 成员解析两步走：
//   ① 字段：查布局表 findField（线性扫描，含继承字段）→ 返回字段类型，
//      日志同时给出 offset/size —— CodeGen 拿 offset 直接寻址，
//      这就是"符号 → 偏移量"的兑现时刻
//   ② 方法：查 m_classDecls 的方法表 → 返回方法返回类型
// 示例：p:Animal*，p->age → findField("age") → int (offset=8, size=4)
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

// 下标表达式 v[i]（读值形态）：
//   [expr.sub] 的糖化——minicc 没有运算符重载，IndexExpr 在此被
//   "语义化"为成员调用约定：容器类必须提供 at() 方法，
//   v[i] 读值 ≡ v.at(i)（CodeGen 按此发射）。
//   校验两件事：① object 是类类型；② 该类存在形参合法的 at() 方法。
//   结果类型 = at() 的返回类型（Vector::at → int，Map::at → value 类型）。
TypePtr SemanticAnalyzer::inferIndex(std::shared_ptr<IndexExpr> expr) {
    m_inferDepth++;
    TypePtr objType = inferType(expr->object);
    m_inferDepth--;

    if (!objType) {
        error("Cannot subscript expression of null type", expr->location);
    }

    // 支持容器指针：p[i] ≡ (*p)[i]（与 MemberExpr 的 -> 解引用同一约定）
    TypePtr actualType = objType;
    if (objType->isPointer()) {
        actualType = objType->pointeeType;
    }

    if (!actualType || !actualType->isClass()) {
        error(std::format("Cannot apply subscript to non-class type '{}'",
            actualType ? actualType->toString() : "?"), expr->location);
    }

    // 查约定方法 at()：必须存在且恰好接收 1 个形参
    auto classIt = m_classDecls.find(actualType->name);
    if (classIt == m_classDecls.end()) {
        error(std::format("Class '{}' not declared", actualType->name),
              expr->location);
    }

    std::shared_ptr<FunctionDecl> atMethod;  // 形参校验与结果类型共用
    for (auto& method : classIt->second->methods) {
        if (method->name == "at" && method->parameters.size() == 1) {
            atMethod = method;
            break;
        }
    }
    if (!atMethod) {
        error(std::format(
            "Class '{}' has no at() method —— subscript requires the "
            "at()/set() convention (see docs/learn/12)", actualType->name),
            expr->location);
    }

    // 下标表达式推导 + 与 at() 形参类型校验
    TypePtr idxType = inferType(expr->index);
    if (idxType && !typeCompatible(atMethod->parameters[0].type, idxType)) {
        error(std::format(
            "Subscript type '{}' does not match {}.at() parameter '{}'",
            idxType->toString(), actualType->name,
            atMethod->parameters[0].type->toString()), expr->location);
    }

    std::cout << std::format("{}[index] {}[{}] → {}    (sugar for {}.at(i))\n",
        inferIndent(), actualType->name,
        idxType ? idxType->toString() : "?",
        atMethod->returnType ? atMethod->returnType->toString() : "?",
        actualType->name);

    return atMethod->returnType;
}

// new 表达式 [expr.new]（大幅简化）：
//   只检查类名是否已注册；返回"指向该类的指针"类型；
//   分配字节数 = computeClassLayout 算出的 totalSize（CodeGen 据此调 malloc）。
//   不调用构造函数（构造/析构特性尚未实现，见 ROADMAP 主线 A）。
TypePtr SemanticAnalyzer::inferNew(std::shared_ptr<NewExpr> expr) {
    // ★ wangyang: P3 —— new Box<int>()：先按需实例化，className 原地改写
    // 为实例名（如 Box_int）。之后全管线（本函数查表、CodeGen::emitNew）
    // 只认实例名，模板痕迹在语义阶段一次性抹平。
    if (!expr->templateArgs.empty()) {
        TypePtr tid = Type::makeClass(expr->className);
        for (auto& arg : expr->templateArgs) {
            tid->templateArgs.push_back(resolveType(arg));
        }
        TypePtr instance = getOrInstantiateClass(tid, expr->location);
        std::cout << std::format("  [new] template-id new {} → new {} (instantiated)\n",
            expr->className, instance->name);
        expr->className = instance->name;
        expr->templateArgs.clear();
    }

    auto it = m_classTypes.find(expr->className);
    if (it == m_classTypes.end()) {
        error(std::format("Unknown class '{}'", expr->className), expr->location);
    }

    // 推导构造函数实参类型
    std::vector<TypePtr> argTypes;
    for (auto& arg : expr->constructorArgs) {
        argTypes.push_back(inferType(arg));
    }

    // 查找匹配的构造函数
    auto classIt = m_classDecls.find(expr->className);
    if (classIt != m_classDecls.end()) {
        bool foundMatch = false;
        for (auto& method : classIt->second->methods) {
            auto ctor = std::dynamic_pointer_cast<ConstructorDecl>(method);
            if (!ctor && method->name != expr->className) continue;
            if (method->parameters.size() == argTypes.size()) {
                bool match = true;
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    if (!typeCompatible(method->parameters[i].type, argTypes[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    foundMatch = true;
                    break;
                }
            }
        }
        if (!foundMatch && (!argTypes.empty() || !classIt->second->methods.empty())) {
            if (!argTypes.empty()) {
                error(std::format("No matching constructor for class '{}' with {} arguments",
                    expr->className, argTypes.size()), expr->location);
            }
        }
    }

    std::cout << std::format("{}[new] {} → {}*    (size={} bytes, args={})\n",
        inferIndent(), expr->className, expr->className,
        it->second->classLayout.totalSize, argTypes.size());

    return Type::makePointer(it->second);
}

// this 表达式 [class.this]：
//   只能出现在成员函数内（否则报错）；类型为"指向属主类的指针"
//   （标准中 this 是 prvalue）。对应的 this 符号已由 analyzeFunctionBody
//   注册为隐式参数；此处只负责给 ThisExpr 节点定型。
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
// dynamic_cast<T*>(expr)：运行时类型检查转型 [expr.dynamic.cast]
// ─────────────────────────────────────────────────────────────────────────────
// 检查规则（对应真 C++ 的子集——只支持"类指针 → 类指针"）：
//   1. 目标类型必须是已声明的类名；
//   2. 源表达式必须是"指向类的指针"（如 Base*）；
//   3. 编译期静态检查：源类与目标类必须存在继承关联
//      （目标 ⊆ 源的祖先链，或源 ⊆ 目标的祖先链），否则 clang 直接报错
//      "cannot cast 'A *' to 'B *' via dynamic_cast"；
//   4. 成败取决于运行时实际类型，编译期不裁决——这正是 dynamic_cast 与
//      static_cast 的本质区别（static_cast 完全由静态类型推导）。
// 结果类型：T*（指针类型）。
TypePtr SemanticAnalyzer::inferDynamicCast(std::shared_ptr<DynamicCastExpr> expr) {
    // 1. 目标类必须已声明
    auto targetIt = m_classTypes.find(expr->targetClassName);
    if (targetIt == m_classTypes.end()) {
        error(std::format("Unknown class '{}' in dynamic_cast", expr->targetClassName),
            expr->location);
    }

    // 2. 源表达式必须是"指向类的指针"
    TypePtr srcType = inferType(expr->operand);
    if (!srcType || !srcType->isPointer()
        || !srcType->pointeeType || !srcType->pointeeType->isClass()) {
        error("dynamic_cast operand must be a pointer to a class",
            expr->location);
    }
    std::string srcClass = srcType->pointeeType->name;

    // 3. 静态可达性检查：真 C++ 只要求两类"同属一个继承体系"
    //    （[expr.dynamic.cast]：源与目标须关联，兄弟类互转也合法，
    //    成败交由运行时的实际类型裁决）。因此这里的编译期判据是：
    //    存在公共祖先（含自身等同：a == b）。
    //    hasCommonAncestor(a, b)：收集 a 的祖先集，沿 b 链上溯查找交集。
    auto hasCommonAncestor = [this](const std::string& a, const std::string& b) {
        // BFS 收集 a 的全部祖先（遍历所有 baseClassNames，不再只走 firstBase）
        std::set<std::string> anc;
        std::vector<std::string> work = {a};
        while (!work.empty()) {
            std::string cur = work.back(); work.pop_back();
            if (!anc.insert(cur).second) continue;  // 已访问
            auto declIt = m_classDecls.find(cur);
            if (declIt != m_classDecls.end()) {
                for (auto& bn : declIt->second->baseClassNames)
                    work.push_back(bn);
            }
        }
        // BFS 检查 b 的祖先是否有交集
        work.push_back(b);
        std::set<std::string> visited;
        while (!work.empty()) {
            std::string cur = work.back(); work.pop_back();
            if (!visited.insert(cur).second) continue;
            if (anc.count(cur)) return true;
            auto declIt = m_classDecls.find(cur);
            if (declIt != m_classDecls.end()) {
                for (auto& bn : declIt->second->baseClassNames)
                    work.push_back(bn);
            }
        }
        return false;
    };
    if (!hasCommonAncestor(expr->targetClassName, srcClass)) {
        error(std::format("Cannot dynamic_cast '{}*' to '{}*': unrelated class types",
            srcClass, expr->targetClassName), expr->location);
    }

    std::cout << std::format("{}[dynamic_cast] {}* → {}*    (runtime RTTI check)\n",
        inferIndent(), srcClass, expr->targetClassName);
    return Type::makePointer(targetIt->second);
}

// ─────────────────────────────────────────────────────────────────────────────
// 错误处理
// ─────────────────────────────────────────────────────────────────────────────
// 统一错误出口：携带源码位置（行/列）抛出异常，由 main() 捕获打印。
// 教学级采用 fail-fast 策略：遇到第一个语义错误即终止，
// 不做错误恢复/继续收集（clang 的 DiagnosticsEngine 支持跳过错误继续）。
[[noreturn]] void SemanticAnalyzer::error(const std::string& msg, SourceLocation loc) {
    throw std::runtime_error(
        std::format("[Semantic Error] {}: {}", loc.toString(), msg));
}

// ═════════════════════════════════════════════════════════════════════════════
// 诊断可视化：--dump-hierarchy / --dump-layout
// ═════════════════════════════════════════════════════════════════════════════
// 由 main.cpp 根据命令行标志调用。正常编译路径不受影响。
//
// typeinfo 形态判断规则（Itanium ABI 三种形态）：
//   bases.empty()       → 'C' (__class_type_info,     无基类, 链终点)
//   bases.size() == 1   → 'S' (__si_class_type_info,  单继承, base 指针)
//   bases.size() > 1    → 'V' (__vmi_class_type_info, 多继承, base 数组)
// ─────────────────────────────────────────────────────────────────────────────

static char typeinfoForm(const ClassLayout& layout) {
    if (layout.bases.empty()) return 'C';
    if (layout.bases.size() == 1) return 'S';
    return 'V';
}

static const char* typeinfoName(char form) {
    switch (form) {
        case 'C': return "__class_type_info";
        case 'S': return "__si_class_type_info";
        case 'V': return "__vmi_class_type_info";
        default:  return "?";
    }
}

// 递归打印 typeinfo 链：当前类 → base → base.base → ...
static void printRTTIChain(
    const std::string& className,
    const std::unordered_map<std::string, TypePtr>& classTypes,
    const std::string& indent,
    std::set<std::string>& visited)
{
    std::string mangled = std::format("_ZTI{}{}", className.length(), className);

    if (visited.count(className)) {
        std::cout << std::format("{}{} → (已访问，跳过)\n", indent, mangled);
        return;
    }
    visited.insert(className);

    auto it = classTypes.find(className);
    if (it == classTypes.end()) {
        std::cout << std::format("{}{} → (未知类)\n", indent, mangled);
        return;
    }

    auto& layout = it->second->classLayout;
    char form = typeinfoForm(layout);

    std::cout << std::format("{}{} ['{}' {}]", indent, mangled, form, typeinfoName(form));

    if (layout.bases.empty()) {
        std::cout << " → 终止\n";
        return;
    }

    if (layout.bases.size() == 1) {
        auto& base = layout.bases[0];
        std::cout << std::format(" ──base──→\n");
        printRTTIChain(base.baseClassName, classTypes, indent, visited);
    } else {
        std::cout << std::format(" (base_count={})\n", layout.bases.size());
        for (size_t i = 0; i < layout.bases.size(); ++i) {
            auto& base = layout.bases[i];
            bool last = (i + 1 == layout.bases.size());
            std::string branch = last ? "└── " : "├── ";
            std::string childIndent = indent + (last ? "    " : "│   ");
            std::cout << std::format("{}{}bases[{}] @offset={}: ",
                indent, branch, i, base.offset);
            printRTTIChain(base.baseClassName, classTypes, childIndent, visited);
        }
    }
}

void SemanticAnalyzer::dumpHierarchy(
    const std::unordered_map<std::string, TypePtr>& classTypes)
{
    if (classTypes.empty()) return;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  类层次结构图 (Class Hierarchy Diagram)                         ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // ── ① 继承树 ──
    std::cout << "\n  ┌─ 继承树 (Inheritance Tree) ──────────────────────────────────\n";

    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots;
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.bases.empty()) {
            roots.push_back(name);
        } else {
            for (auto& base : type->classLayout.bases)
                children[base.baseClassName].push_back(name);
        }
    }

    auto isPoly = [](const TypePtr& t) { return t->classLayout.hasVTable; };
    auto getSize = [](const TypePtr& t) { return t->classLayout.totalSize; };

    std::set<std::string> treeVisited;
    std::function<void(const std::string&, const std::string&, const std::string&)> dfs =
        [&](const std::string& cls, const std::string& prefix, const std::string& branch) {
            auto it = classTypes.find(cls);
            if (it == classTypes.end()) return;
            auto& t = it->second;
            bool dup = !treeVisited.insert(cls).second;
            if (dup) {
                std::cout << std::format("  │ {}{}{}  [{}{}B]  ↑ (已展开)\n",
                    prefix, branch, cls,
                    isPoly(t) ? "polymorphic, " : "",
                    getSize(t));
                return;
            }
            std::cout << std::format("  │ {}{}{}  [{}{}B]\n",
                prefix, branch, cls,
                isPoly(t) ? "polymorphic, " : "",
                getSize(t));
            auto ci = children.find(cls);
            if (ci == children.end()) return;
            auto& kids = ci->second;
            std::sort(kids.begin(), kids.end());
            for (size_t i = 0; i < kids.size(); ++i) {
                bool last = (i + 1 == kids.size());
                dfs(kids[i],
                    prefix + (branch.empty() ? "" : (last ? "    " : "│   ")),
                    last ? "└── " : "├── ");
            }
        };

    std::sort(roots.begin(), roots.end());
    for (auto& r : roots) {
        dfs(r, "", "");
    }
    std::cout << "  └───────────────────────────────────────────────────────────────\n";

    // ── ② 每个类的详情框 ──
    // 按类名字母顺序打印（确保每个类只打印一次）
    std::vector<std::string> ordered;
    for (auto& [name, type] : classTypes) {
        ordered.push_back(name);
    }
    std::sort(ordered.begin(), ordered.end());

    for (auto& name : ordered) {
        auto it = classTypes.find(name);
        if (it == classTypes.end()) continue;
        auto& layout = it->second->classLayout;
        char form = typeinfoForm(layout);

        std::cout << std::format("\n  ┌─ {} ─────────────────────────────────────────────────\n", name);

        // typeinfo 形态
        std::cout << std::format("  │  typeinfo : {} ['{}'", typeinfoName(form), form);
        if (form == 'C') std::cout << " 无基类";
        else if (form == 'S') std::cout << " 单继承";
        else if (form == 'V') std::cout << " 多继承";
        std::cout << "]\n";

        // size
        std::cout << std::format("  │  size     : {} bytes\n", layout.totalSize);

        // 基类
        if (!layout.bases.empty()) {
            for (auto& base : layout.bases) {
                std::cout << std::format("  │  基类     : {} ({}{}, offset={})\n",
                    base.baseClassName,
                    base.isPrimary ? "主基类" : "次基类",
                    base.hasVTable ? ", 多态" : "",
                    base.offset);
            }
        }

        // 字段（区分继承 vs 自有）
        if (!layout.fields.empty()) {
            bool hasInherited = false, hasOwn = false;
            for (auto& f : layout.fields) {
                if (f.sourceClass.empty() || f.sourceClass == name) hasOwn = true;
                else hasInherited = true;
            }
            if (hasInherited) {
                std::cout << "  │  继承字段 :\n";
                for (auto& f : layout.fields) {
                    if (!f.sourceClass.empty() && f.sourceClass != name) {
                        std::cout << std::format("  │    +{:<4} {:<12} : {} ({})  ← {}\n",
                            f.offset, f.name,
                            f.type ? f.type->toString() : "?",
                            f.size, f.sourceClass);
                    }
                }
            }
            if (hasOwn) {
                std::cout << "  │  自有字段 :\n";
                for (auto& f : layout.fields) {
                    if (f.sourceClass.empty() || f.sourceClass == name) {
                        std::cout << std::format("  │    +{:<4} {:<12} : {} ({})\n",
                            f.offset, f.name,
                            f.type ? f.type->toString() : "?",
                            f.size);
                    }
                }
            }
        }

        // 虚函数
        if (layout.hasVTable && !layout.vtableEntries.empty()) {
            std::cout << "  │  虚函数   :\n";
            for (auto& entry : layout.vtableEntries) {
                std::cout << std::format("  │    [{}] {} {}",
                    entry.index, entry.mangledName,
                    entry.isOverridden ? "(override)" : "");
                if (entry.thunkAdjust != 0)
                    std::cout << std::format("  thunk={}", entry.thunkAdjust);
                std::cout << "\n";
            }
            std::cout << std::format("  │  RTTI     : {}\n", layout.rttiMangledName);
        }

        // RTTI 链
        std::cout << "  │  RTTI 链  :\n";
        std::set<std::string> visited;
        printRTTIChain(name, classTypes, "  │    ", visited);

        std::cout << "  └───────────────────────────────────────────────────────────────\n";
    }
}

void SemanticAnalyzer::dumpLayout(
    const std::unordered_map<std::string, TypePtr>& classTypes)
{
    if (classTypes.empty()) return;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  类内存布局详图 (Memory Layout Detail)                          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // 按继承顺序：先打印无基类的，再打印有基类的
    std::vector<std::string> ordered;
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.bases.empty())
            ordered.push_back(name);
    }
    for (auto& [name, type] : classTypes) {
        if (!type->classLayout.bases.empty())
            ordered.push_back(name);
    }
    std::sort(ordered.begin(), ordered.end());

    for (auto& name : ordered) {
        auto it = classTypes.find(name);
        if (it == classTypes.end()) continue;
        auto& layout = it->second->classLayout;

        std::cout << std::format("\n  ━━ {} ({} bytes) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n",
            name, layout.totalSize);

        // ── 第一层：对象内存 ──
        std::cout << std::format("  ┌─ {} 对象 ({}B) ─────────────────────────────────┐\n",
            name, layout.totalSize);
        if (layout.hasVTable) {
            std::cout << std::format("  │  +{:<3} _vptr ────────────────────────┐\n", 0);
        }
        for (auto& f : layout.fields) {
            std::string src = "";
            if (!f.sourceClass.empty() && f.sourceClass != name)
                src = std::format("  ← {}", f.sourceClass);
            std::cout << std::format("  │  +{:<3} {:<10} : {} ({}B){}\n",
                f.offset, f.name,
                f.type ? f.type->toString() : "?",
                f.size, src);
        }
        std::cout << "  └──────────────────────────────────────────────────────┘\n";

        if (!layout.hasVTable) continue;

        // ── 第二层：vtable ──
        std::string vtblName = std::format("_ZTV{}{}", name.length(), name);
        std::string rttiName = std::format("_ZTI{}{}", name.length(), name);
        std::cout << std::format("                                        │\n");
        std::cout << std::format("                                        ▼\n");
        std::cout << std::format("  ┌─ {} ──────────────────────────────────────────┐\n", vtblName);
        std::cout << std::format("  │  [-2]  offset-to-top = 0    ─→ 归顶: top = obj + 0\n");
        std::cout << std::format("  │  [-1]  typeinfo ptr ──────────┐  ─→ {} (RTTI)\n", rttiName);
        std::cout << std::format("  │  ── ↑ vptr 指向此处 ───────── │ ──\n");
        for (auto& entry : layout.vtableEntries) {
            std::cout << std::format("  │  [{:<2}]  {}{}\n",
                entry.index, entry.mangledName,
                entry.isOverridden ? "  (override)" : "");
        }
        std::cout << std::format("  └────────────────────────────── │ ──────────────────────────┘\n");

        // ── 第三层：typeinfo 链 ──
        std::cout << std::format("                                  │\n");
        std::cout << std::format("                                  ▼\n");

        char form = typeinfoForm(layout);
        std::string rttiBoxName = std::format("_ZTI{}{}", name.length(), name);
        std::cout << std::format("  ┌─ {} ({}) ────────────────────────────────────┐\n",
            rttiBoxName, form);
        std::cout << std::format("  │  +0   vptr  → 形态标记 '{}'\n", form);
        std::cout << std::format("  │  +8   name  → \"{}\" (mangled: {}{})\n",
            name, name.length(), name);

        if (form == 'V') {
            // VMI 类型：只列出 bases 数组，不递归展开（避免深层嵌套太复杂）
            std::cout << std::format("  │  +16  base_count = {}\n", layout.bases.size());
            for (size_t i = 0; i < layout.bases.size(); ++i) {
                auto& base = layout.bases[i];
                std::cout << std::format("  │  bases[{}]: {} @offset={} [{}]\n",
                    i, base.baseClassName, base.offset,
                    base.isPrimary ? "primary" : "secondary");
            }
            std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
        } else {
            // 'S'（单继承）或 'C'（无基类）：沿主基类链递归展开
            std::set<std::string> layoutVisited;
            layoutVisited.insert(name);
            std::string currentName = name;
            const ClassLayout* currentLayout = &layout;

            while (true) {
                if (currentLayout->bases.empty()) {
                    // 'C' 类型：链终止
                    std::cout << std::format("  │  (无 +16 字段 — 链终止)\n");
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }
                // 'S' 类型：有 +16 base 指针
                std::cout << std::format("  │  +16  base  ──────────────────────┐\n");
                std::cout << std::format("  └───────────────────────────────────── │ ─────┘\n");
                std::cout << std::format("                                        │\n");
                std::cout << std::format("                                        ▼\n");

                auto& base = currentLayout->bases[0];
                auto baseIt = classTypes.find(base.baseClassName);
                if (baseIt == classTypes.end()) {
                    std::cout << std::format("  ┌─ _ZTI{}{} (?) ──────────────────────────────────┐\n",
                        base.baseClassName.length(), base.baseClassName);
                    std::cout << std::format("  │  (基类不在当前翻译单元中)\n");
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }
                if (!layoutVisited.insert(base.baseClassName).second) {
                    std::cout << std::format("  ┌─ _ZTI{}{} → (已访问，跳过) ──────────────────────┐\n",
                        base.baseClassName.length(), base.baseClassName);
                    std::cout << std::format("  └───────────────────────────────────────────────────┘\n");
                    break;
                }

                auto& baseLayout = baseIt->second->classLayout;
                char baseForm = typeinfoForm(baseLayout);
                std::string baseRttiName = std::format("_ZTI{}{}",
                    base.baseClassName.length(), base.baseClassName);
                std::cout << std::format("  ┌─ {} ({}) ────────────────────────────────────┐\n",
                    baseRttiName, baseForm);
                std::cout << std::format("  │  +0   vptr  → 形态标记 '{}'\n", baseForm);
                std::cout << std::format("  │  +8   name  → \"{}\"\n", base.baseClassName);

                currentName = base.baseClassName;
                currentLayout = &baseLayout;
            }
        }
    }
}

} // namespace minicc
