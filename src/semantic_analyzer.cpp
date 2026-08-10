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
            // 类方法同样注册为函数（fullName = 类名::方法名），
            // 供方法调用解析与 CodeGen 输出。
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
    if (!decl->baseClassName.empty()) {
        std::cout << std::format(": public {}", decl->baseClassName);
    }
    std::cout << "\n";

    // 为本类新建类型对象：classLayout（字段/偏移/vtable）都挂在它上面，
    // 完成后注册进 m_classTypes，全局唯一（"编译期看符号"的那个符号）。
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
        // 注意插到字段列表【头部】——基类子对象必须排在派生类成员之前
        // （[class.mem] 对象布局顺序），多级继承时顺序为
        // 祖父字段…基类字段…自身字段。
        for (auto& baseField : baseType->classLayout.fields) {
            decl->fields.insert(decl->fields.begin(), baseField);
        }

        // 继承基类的虚函数
        // 整表拷贝过来：派生类的 override 将在下面"同槽位改写"，
        // 未 override 的槽位原样保留（仍指向基类实现）。
        for (auto& baseEntry : baseType->classLayout.vtableEntries) {
            classType->classLayout.vtableEntries.push_back(baseEntry);
        }

        // 基类有 vtable（含虚函数）→ 派生类对象也必须带 _vptr，
        // 即使派生类自己没声明任何新虚函数。
        if (baseType->classLayout.hasVTable) {
            classType->classLayout.hasVTable = true;
        }

        std::cout << std::format("    ↳ Inherited {} fields, {} vtable entries from '{}'\n",
            baseType->classLayout.fields.size(),
            baseType->classLayout.vtableEntries.size(),
            decl->baseClassName);
    }

    // ── 注册字段到符号表 ──
    // 注意此处遍历的是"合并后"的字段列表（基类字段已在继承处理时插到头部），
    // 逐一转成 FieldInfo 挂进布局表；具体 offset/size 稍后由
    // computeClassLayout 统一计算。
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

            // override 检测（[class.virtual]，教学简化：用 mangled 名子串
            // 匹配同名虚函数；标准按签名+const 限定精确比对）：
            // 命中基类槽位 → 原位改写 mangledName，索引保持不变 ——
            // 这正是动态分派的关键：调用方统一按槽位索引间接跳转，
            // 基类指针也能落到派生类实现。
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
                // 非 override → 本类新声明的虚函数：追加到 vtable 末尾，
                // 索引 = 当前表长（之后 injectVTableAndRTTI 会统一重排）。
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

    // 写回最终字段列表：decl->fields（基类+自身，且已被 computeClassLayout
    // 填好 offset/size）整体覆盖前面逐步 push 的版本，作为布局的权威结果。
    classType->classLayout.fields = decl->fields;

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
}

// 向上取整到 alignment 的倍数：alignTo(12,8)=16，alignTo(16,8)=16，
// alignTo(5,4)=8。等价于位运算 (offset+align-1) & ~(align-1) 的算术写法；
// alignment==0 时原样返回，防御除零。
uint32_t SemanticAnalyzer::alignTo(uint32_t offset, uint32_t alignment) {
    if (alignment == 0) return offset;
    return (offset + alignment - 1) / alignment * alignment;
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
        if (retType && m_currentReturnType
            && !typeCompatible(m_currentReturnType, retType)) {
            error(std::format(
                "Return type mismatch: expected '{}', got '{}'",
                m_currentReturnType->toString(), retType->toString()),
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

// new 表达式 [expr.new]（大幅简化）：
//   只检查类名是否已注册；返回"指向该类的指针"类型；
//   分配字节数 = computeClassLayout 算出的 totalSize（CodeGen 据此调 malloc）。
//   不调用构造函数（构造/析构特性尚未实现，见 ROADMAP 主线 A）。
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
// 错误处理
// ─────────────────────────────────────────────────────────────────────────────
// 统一错误出口：携带源码位置（行/列）抛出异常，由 main() 捕获打印。
// 教学级采用 fail-fast 策略：遇到第一个语义错误即终止，
// 不做错误恢复/继续收集（clang 的 DiagnosticsEngine 支持跳过错误继续）。
[[noreturn]] void SemanticAnalyzer::error(const std::string& msg, SourceLocation loc) {
    throw std::runtime_error(
        std::format("[Semantic Error] {}: {}", loc.toString(), msg));
}

} // namespace minicc
