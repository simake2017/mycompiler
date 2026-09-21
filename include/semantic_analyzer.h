#pragma once
// =============================================================================
// 阶段 3：语义分析与类型系统 (Semantic Analysis)（理论见 docs/learn/29）
// =============================================================================
// 职责：
//   1. 构建符号表（Symbol Table）：记录每个变量、函数、类的类型信息
//   2. 类型检查：确保所有操作在类型上合法
//   3. auto 类型推导：把 auto 占位符换成真实类型
//   4. 类内存布局与虚表：字段偏移量、vtable 结构、RTTI 注入
//
// 哲学：编译期看"符号"（类型名、字段名），运行期看"偏移量"（内存布局）—— 本阶段的
//   任务就是把"符号"转化为"偏移量"。三遍扫描（见 analyze）：注册类/模板 → 注册函数名 → 分析函数体。
//
// 管线：源码 → Preprocessor → Lexer → Parser → 【SemanticAnalyzer】 → 模板推导/
//   实例化 → CodeGen(x86-64 .s)
//   输入：TranslationUnit（AST，此时类型只是语法标记，名字均未决议）
//   输出：① 标注 resolvedType 的 AST（auto 已抹去，每个表达式类型已定）
//         ② 符号表快照（Scope 作用域链 + 栈偏移，可 dump 观察）
//         ③ 类布局表（字段偏移 / vtable / RTTI，CodeGen 直接消费）
//
// ── 标准章节 → 本文件 ──────────────────────────────────────────────────
//   [basic.scope] 符号表与作用域链：名字从最内层作用域逐层向外解析（Scope / SymbolTable）
//   [expr]        类型检查与值类别：每个表达式有类型与左/右值性（inferType 系列）
//   [over.match]  重载决议：候选 → 可行 → 最优（resolveTemplateCall / isAtLeastAsSpecialized）
//   [temp.names]  两阶段查找（简化）：蓝图先注册不查体，实例化后才用具体类型检查函数体
//
// ── clang 模块对照（教学级简化）────────────────────────────────────────
//   lib/Sema/SemaDecl.cpp      声明处理      → processClassDecl / registerFunction
//   lib/Sema/SemaExpr.cpp      表达式类型检查 → inferType 系列 / Scope::lookup
//   lib/Sema/SemaOverload.cpp  重载决议与偏序 → resolveTemplateCall / isAtLeastAsSpecialized
// =============================================================================

#include "ast.h"
#include "ast_visitor.h"
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
// 作用域中同一个名字可能对应不同实体（变量 x / 函数 x / 类型 X），kind 告诉查找方
//   如何解释这条符号、允许怎样的使用方式。
// demo: `int x = 1;` 入表 → { name="x", kind=Variable, type=int, stack@-8 }
//       `int f(int a)` 入表 → { name="f", kind=Function, type=int(返回类型) }
// 注：ClassField / ClassMethod 为预留种类 —— 本实现中类成员不直接进 Scope，
//   字段挂在 ClassLayout.fields，方法挂在 m_classDecls（见 SemanticAnalyzer）。
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
// 一条 Symbol 回答三个问题：这个名字是什么（kind）、什么类型（type）、数据在哪里
//   （局部 → stackOffset；全局/成员 → 由布局表或 CodeGen 决定）。
// demo: `int x = 42;` → { name="x", type=int, kind=Variable, isLocal=true, stackOffset=-8 }
//       成员函数注册时隐式加入的 this →
//       { name="this", type=Animal*, kind=Parameter, isLocal=true, stackOffset=-8 }
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
// 符号表是编译器在"编译期"记住所有名字及其含义的核心数据结构；每个作用域一张，
//   作用域可嵌套（函数内的代码块）。查找时从当前作用域向外逐层搜索，直到命中或到达
//   全局作用域。理论依据：[basic.scope] 作用域树 + [basic.scope.scope] 可见性规则。
// 作用域链（f 内又有一个 `{ }` 块时）：
//     global(depth=0) ← f(depth=1)[basic.scope.function] ← block(depth=2)[basic.scope.block]
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
// SymbolTable：全局符号表管理器（所有层级作用域的注册与查找）
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
// 设计决策——"名字→单符号"教学模型：Scope/SymbolTable 保持一个名字只对应一个符号
//   （不支持重载的符号表），真正的重载集（函数模板候选集）单独挂在
//   m_functionTemplateCandidates。对照 clang：普通名字查找走 DeclContext::lookup，
//   重载集是挂在 DeclContext 上的 Decl 链，二者也是分开的。
// ── 本类如何做 AST 分派（两种手法，各有其位）────────────────────────────────
// ★ 判据（本项目唯一权威表述）：**handler 只要引用 → 访问者（accept 虚分派）；
//   还要 shared_ptr 所有权或返回值 → 标签分派（switch）**。clang 同此分法：
//   RecursiveASTVisitor 只服务遍历，类型计算走 dyn_cast/switch。
// ① 【访问者】语句处理链（processStmt）走 accept 虚表分派：handler 签名统一是
//    void visit(Stmt&)，不需要所有权也不返回值 —— 正是访问者的形状。
// ② 【NodeKind 标签分派】声明链（processDecl）与类型推导（inferType）用 switch：
//    · 声明链 handler 需要 shared_ptr 所有权（m_classDecls / m_globalVars 等注册表存的
//      就是它），而 visit 只拿得到引用 —— 从引用还原 shared_ptr 不安全，硬套访问者
//      就得引入隐藏的"当前节点暂存槽"，反而更难读；
//    · inferType / isLValueExpr 是【取值型】递归，而 visit 返回 void。
class SemanticAnalyzer : public DecltypeEvaluator,
                         public MemberTypeResolver,
                         public AliasTemplateResolver,
                         public AstVisitor {
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

    // 获取所有全局变量（供代码生成阶段使用）
    const std::vector<GlobalVarDeclPtr>& getGlobalVars() const {
        return m_globalVars;
    }

    // 获取模板声明（供模板实例化阶段使用）
    const std::vector<TemplateDeclPtr>& getTemplates() const {
        return m_templates;
    }

    // 获取类声明表（类名 → 蓝图），含模板实例化产出的实例类。
    // 【为什么开放】类模板偏序裁决（[temp.class.order]）的结果无法从实例类【名字】上
    //   分辨 —— 主模板与任一派生偏特化实例化后都叫 Box_int_ptr。要断言"到底选了哪一条"，
    //   必须能看到实例类的成员函数体（如 tag() 返回的常量）。
    //   与项目"可讲解、可观测优先"的定位一致。
    const std::unordered_map<std::string, ClassDeclPtr>& getClassDecls() const {
        return m_classDecls;
    }

    // 获取函数模板候选集（供 S2+ 实参推导使用）
    const std::unordered_map<std::string, std::vector<TemplateDeclPtr>>&
    getFunctionTemplateCandidates() const {
        return m_functionTemplateCandidates;
    }

    // 获取类模板注册表（类模板名 → 蓝图）
    const std::unordered_map<std::string, TemplateDeclPtr>&
    getClassTemplates() const {
        return m_classTemplates;
    }

    // 获取符号表（供调试输出）
    SymbolTable& getSymbolTable() { return m_symbolTable; }

    // ── 诊断可视化（--dump-* 系列）──
    // dumpHierarchy: 类层次结构图（继承树 + 每个类详情框 + typeinfo 链）
    // dumpLayout: 三层内存布局详图（对象 → vtable(含[-2][-1]) → typeinfo 链）
    // 由 main.cpp 根据命令行 --dump-hierarchy / --dump-layout 标志调用。
    void dumpHierarchy(const std::unordered_map<std::string, TypePtr>& classTypes);
    void dumpLayout(const std::unordered_map<std::string, TypePtr>& classTypes);

private:
    // ── 符号表管理 ──
    SymbolTable m_symbolTable;

    // ── 全局注册表 ──
    std::unordered_map<std::string, TypePtr>      m_classTypes;    // 类名 → Type
    std::unordered_map<std::string, FuncDeclPtr>   m_functionMap;   // 函数名 → 声明
    std::vector<FuncDeclPtr>                       m_functions;     // 所有函数（按顺序）
    std::vector<TemplateDeclPtr>                   m_templates;     // 模板蓝图
    // 函数模板候选集（S1+）：函数名 → 同名函数模板列表。不进 Scope/SymbolTable
    //   （那里保持"名字→单符号"的教学模型），调用点的重载决议（S6）遍历此候选集
    //   做推导与排序。对照 clang：重载集挂在 DeclContext 上，而非普通名字查找表。
    std::unordered_map<std::string, std::vector<TemplateDeclPtr>>
                                                   m_functionTemplateCandidates;
    // 类模板注册表：类模板名 → 蓝图。与函数模板候选集对称：m_templates 保留全部
    //   蓝图的有序列表（供 main.cpp Phase 4 遍历），此表供按名 O(1) 查找
    //   （resolveType / getOrInstantiateClass）。
    // 对照 clang：类模板名经 DeclContext::lookup 命中 ClassTemplateDecl，
    //   而实例化产物是 ClassTemplateSpecializationDecl，二者分开。
    // 重名语义：emplace 不覆盖，取先注册者。
    std::unordered_map<std::string, TemplateDeclPtr> m_classTemplates;

    // ── 类模板特化注册表（[temp.class.spec] / [temp.expl.spec]）──
    // 同名主模板可带若干偏特化与全特化，它们与主模板一样按名字索引；数量极少
    //   （教学代码里通常个位数），故用 vector 线性扫描。
    // 对照 clang：ClassTemplateDecl 持有 PartialSpecialization 链表 +
    //   Specializations 集合（lookupSpecialization），二者与主模板分开存放。
    // 全特化的两个 vector 分开存是刻意的：两者的匹配算法完全不同（全特化 = 逐位类型
    //   相等；偏特化 = 用实参推导模式），混在一起没法写。
    std::unordered_map<std::string, std::vector<TemplateDeclPtr>> m_partialSpecs;
    std::unordered_map<std::string, std::vector<TemplateDeclPtr>> m_explicitSpecs;

    // ── 别名模板注册表（[temp.alias]）──
    // 别名模板名 → 蓝图。单独一张表而不混进 m_classTemplates，因为两者的【消费方式
    //   根本不同】：类模板名要"实例化"（造新类、发新符号），别名模板名只要"替换"
    //   （解糖成既有类型，零新符号）。混在一起会让 resolveType 不得不在同一个分支里
    //   靠 isAliasTemplate() 二次分派，反而更容易写错。
    // 对照 clang：TypeAliasTemplateDecl 与 ClassTemplateDecl 是两种 Decl，
    //   在 DeclContext 里同名不同种（clang 靠 Decl 类型而非 flag 区分）。
    std::unordered_map<std::string, TemplateDeclPtr> m_aliasTemplates;

    // ── 推导指引注册表（[temp.deduct.guide]）──
    // 类模板名 → 该模板的指引列表。CTAD 时**指引优先于构造函数**：只要用户写了指引，
    //   就按指引推（这正是指引存在的意义 —— 覆盖默认规则）。
    // 对照 clang：ClassTemplateDecl::getDeductionGuides() 是一个独立的小集合，隐式指引
    //   （从构造函数合成）与显式指引并排在候选列表里，显式优先。
    std::unordered_map<std::string, std::vector<DeductionGuideDeclPtr>> m_deductionGuides;
    // 递归别名防护：正在展开的别名名集合（X<T> 的底层又写 X<...> 时死循环）。
    // 对照 clang：err_alias_template_extra_headers 之外的
    //   "alias template is not a type" / 递归深度上限诊断。
    std::vector<std::string> m_expandingAliases;
    // ── 函数模板实例化（S5）──
    TemplateInstantiator m_instantiator;
    std::unordered_map<std::string, FuncDeclPtr> m_templateInstanceCache; // mangled 名 → 实例

    // ── 类模板按需实例化（P3）──
    // resolveType 遇到带实参的类类型（Box<int>）时调用：查缓存 →
    // instantiate() 深拷贝蓝图 → processClassDecl 完整注册实例类。
    // 返回实例类型（如 Box_int）；已实例化过则直接返回缓存。
    // 对照 clang：Sema::InstantiateClass（[temp.inst] 隐式实例化点）。
    TypePtr getOrInstantiateClass(TypePtr templateIdType, SourceLocation loc);
    // 类模板实例缓存：「模板名<实参串>」→ 实例类型。
    // [temp.inst]/3：同一实参组合只实例化一次，重复使用直接命中。
    std::unordered_map<std::string, TypePtr> m_classInstanceCache;

    // ── 模板实参校验（[temp.arg]）──
    // 逐位比对结构化形参表 templateParams（带 kind）与实际实参：① 个数一致；
    //   ② 形态匹配（类型形参收类型实参 / 非类型形参收值实参）；③ NTTP 的值类型受支持。
    // ★ 用 templateParams 而非退化的 typeParams —— 只有前者知道哪位是 NTTP。
    // 对照 clang：Sema::CheckTemplateArgumentList（SemaTemplate.cpp）。
    void checkTemplateArguments(const TemplateDeclPtr& blueprint,
                                const TypePtr& templateIdType,
                                SourceLocation loc);

    // ── 类模板特化择优（[temp.class.spec.match]）──
    // 从「全特化 / 偏特化 / 主模板」中选出实参表该用哪个版本：
    //   ① 全特化 specPattern 与实参逐位类型相等 → 命中即用
    //   ② 偏特化：用实参推导 specPattern（合一算法）→ 全位成功即匹配
    //   ③ 都不中 → 主模板
    // 返回 {选中的声明, 特化路径的替换表（主模板为空）, 是否命中特化}。
    // 不做 [temp.class.order] 偏序裁决：多个偏特化同时匹配时取先注册者。
    std::tuple<TemplateDeclPtr, TemplateInstantiator::TypeSubstitution, bool>
    selectClassTemplate(const TemplateDeclPtr& primary,
                        const std::vector<TemplateArg>& args,
                        SourceLocation loc);

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
    std::unordered_map<std::string, EnumDeclPtr>   m_enumDecls;     // 枚举名 → 声明
    std::vector<GlobalVarDeclPtr>                  m_globalVars;    // 全局变量声明列表

    // ── 栈帧管理 ──
    int m_stackOffset = 0;  // 当前栈帧偏移量

    // ── 推导追踪 ──
    int m_inferDepth = 0;   // 类型推导嵌套深度（用于缩进输出）

    // ── 声明处理 ──
    // 顶层声明分发（按 Decl 动态类型派发到下面的具体处理器）
    void processDecl(DeclPtr decl);
    // 类注册：合并继承字段/vtable → 收集成员 → 布局 → 入符号表
    void processClassDecl(ClassDeclPtr decl);

    // ── 继承图打印（可观测性：编译期输出整棵类型继承树）──
    // Pass 1 注册完成后调用：以无基类的类为根，按 baseClassName 建树，
    // ASCII 树形打印；标注 [polymorphic] 表示该类带虚函数表（可参与
    // 虚调用与 dynamic_cast）。
    void printInheritanceGraph();
    // 全局变量声明处理
    void processGlobalVarDecl(GlobalVarDeclPtr decl);
    // 枚举声明处理
    void processEnumDecl(EnumDeclPtr decl);
    // 命名空间声明处理
    void processNamespaceDecl(NamespaceDeclPtr decl);
    // 类型别名声明处理
    void processTypeAliasDecl(TypeAliasDeclPtr decl);
    // Pass 2：函数名 → 符号表 + mangled 名（先注册以支持递归/前向引用）
    // ── 通用深度遍历：对每个函数声明执行 action（含类内方法，并递归进命名空间）──
    // Pass 2（注册符号）与 Pass 3（分析函数体）的走查形状完全相同，区别只在
    // action —— 提取到一处，免得两份拷贝各自演进（命名空间递归这种易漏点
    // 只需修一次）。详见 .cpp 里对"为什么必须递归"的说明。
    void forEachFunctionDecl(const std::vector<DeclPtr>& decls,
                             const std::function<void(FuncDeclPtr)>& action);

    void registerFunction(FuncDeclPtr decl);
    // Pass 3：进入函数作用域，注册参数/this，逐语句分析函数体
    void analyzeFunctionBody(FuncDeclPtr decl);
    // 模板蓝图注册（只存不查体——两阶段查找第一阶段 [temp.names] 简化）
    void processTemplateDecl(TemplateDeclPtr decl);

    // ── P4 内建外部函数原型 ──
    // memcpy/realloc/malloc/free：只在语义层登记"有这些名字、几个形参、
    // 返回什么"（无 body），Codegen 的 generate() 因 body 为空自动跳过
    // 发射——调用点按普通函数发射 callq，链接期由 libc 提供实现。
    // 对照 clang：Builtins::Info 表 + __builtin_* 语义内建。
    void registerBuiltins();

    // ── 语句处理 ──
    // 语句分发器：accept 走虚表分派到下方 visit 重载
    void processStmt(const StmtPtr& stmt);
    // { } 块 → 新建块作用域 [basic.scope.block]
    void visit(BlockStmt& block) override;
    // delete 语句处理
    void visit(DeleteStmt& stmt) override;
    // 变量声明：auto 推导/类型检查/分配栈槽/入符号表
    void visit(VarDeclStmt& decl) override;
    // if：条件须 bool|int（语境转换简化），分支各建作用域
    void visit(IfStmt& stmt) override;
    // while：条件类型检查，循环体建作用域
    void visit(WhileStmt& stmt) override;
    // return：返回值与声明返回类型兼容性检查 [stmt.return]
    void visit(ReturnStmt& stmt) override;
    // 赋值：推导左右类型（教学级不查左值可赋值性）
    void visit(AssignStmt& stmt) override;
    // 表达式语句：推导类型、丢弃值（副作用语句，如函数调用）
    void visit(ExprStmt& stmt) override;

    // ── 类型与别名解析 ──
    TypePtr resolveType(TypePtr type);

    // ── decltype 求值（实现 DecltypeEvaluator 接口，理论见 docs/learn/20）──
    // [dcl.type.decltype] 两套规则：decltype(e) 里 e 是【未加括号】的 id-expression
    //   或成员访问 → 取【声明类型】（static type）；decltype((e)) 加了括号 →
    //   取【表达式类型】，左值表达式带 &。
    // demo: int a;  decltype(a) → int（声明类型）│ decltype((a)) → int&（左值）
    // 抛 SubstitutionFailure —— 表达式不合法（immediate context），调用方按 SFINAE
    //   处理（移出候选集），而非报错。
    // 对照 clang：Sema::ActOnDecltypeExpression + BuildDecltypeType。
    TypePtr evaluateDecltype(const ExprPtr& expr, bool paren) override;

    // ── 成员类型查表（实现 MemberTypeResolver 接口）────────────────────
    // 依赖类型名 `typename T::type` 在替换阶段（直接上下文）的回调：把限定者解析成
    //   具体类，再查它的 typeAliases；查不到抛 SubstitutionFailure（软失败，由
    //   Sfinae::attempt 吸收），而不是像 resolveType 那样硬报错。
    // 对照 clang：Sema::SubstType 对 DependentNameType 的重新查找。
    TypePtr resolveMemberType(const TypePtr& qualifier,
                              const std::string& member) override;

    // 去类 qual 的别名表里找成员类型：找到返回（未解糖的）目标，找不到返回 nullptr。
    // 【与 resolveMemberType 的分工】本函数只做"查"，不决定失败是软是硬——
    //   resolveType（非直接上下文）拿到 nullptr 就 error，
    //   resolveMemberType（直接上下文）拿到 nullptr 就 Sfinae::fail。
    TypePtr findMemberType(const TypePtr& qual, const std::string& member);

    // ── 别名模板展开（实现 AliasTemplateResolver 接口，理论见 docs/learn/27）──
    // `Vec<int>`（template<class T> using Vec = MyPtr<T>;）在替换阶段走到这里：
    //   按形参表逐位绑定实参 → 对底层类型做结构替换 → 解糖成最终类型。
    //   返回 nullptr 表示该名字不是别名模板（调用方按类模板继续处理）。
    // 【不实例化】[temp.alias]/1：别名不是新类型，故这里【只替换不实例化】，也不产生
    //   任何新符号 —— Vec<int> 与 MyPtr<int> 是同一个类。
    // 对照 clang：Sema::CheckAliasTemplateId + Type::getCanonicalType 解糖。
    TypePtr expandAliasTemplate(const std::string& name,
                                const std::vector<TemplateArg>& args) override;
    // 纯查询：名字是否登记为别名模板（推导器逐位匹配时用，无副作用）。
    bool isAliasTemplate(const std::string& name) const override;

    // ── 类模板实参推导 CTAD（[dcl.type.class.deduct]，理论见 docs/learn/28）──
    // 把 `MyPtr m(7);` 里的裸模板名 `MyPtr` 推成 `MyPtr<int>`：输入变量声明语句
    //   （声明类型 + 构造实参表）；返回推导成功的 Class(name, args)，不适用/推不出
    //   则原样返回输入类型（调用方按原路径继续，该报的错照报）。
    // 两条来源：① 用户写的推导指引 ② 构造函数的隐式指引（算法见实现处注释）。
    TypePtr deduceClassTemplateArgs(const VarDeclStmt& decl);
    // 登记一条推导指引（模板形态由 processTemplateDecl 调用，独立形态由 processDecl 调用）。
    void registerDeductionGuide(const std::shared_ptr<struct DeductionGuideDecl>& g);
    // 把替换表套用到类型上（推导指引的 `-> X<T>` 右侧用）。
    TypePtr substituteInType(const TypePtr& type,
                             const std::unordered_map<std::string, TypePtr>& subst);

    // 表达式是否是左值（[expr.prim]/[basic.lval] 的值类别判定，简化版）。
    // 只覆盖本项目能构造出的形态——decltype((e)) 要靠它决定是否加 &。
    bool isLValueExpr(const ExprPtr& expr) const;

    // ── 静态常量成员折叠（Cls<Args>::value）──────────────────────────
    // 把类型限定访问（MemberExpr::isTypeAccess）在编译期求值成字面量，使运行期无需
    //   真的去内存里读（静态成员根本没有对象内存）。
    // 返回：可折叠时给出新的字面量表达式；否则原样返回。
    // 对照 clang：Sema::BuildDeclarationNameExpr 命中静态数据成员后，在常量求值
    //   上下文里由 Expr::EvaluateAsInt 折叠。
    ExprPtr foldStaticConst(ExprPtr expr);
    // 沿【继承链】查找静态常量（[class.member.lookup]）。
    // 返回：找到则填 outValue 并返回 true。
    bool lookupStaticConst(const std::string& className,
                           const std::string& member, int64_t& outValue);
    // lookupStaticConst 的副产物：命中常量的类型（决定折叠成 bool 还是 int 字面量）
    TypePtr m_lastStaticConstType;

    // ── 类模板偏序裁决（[temp.class.order]，理论见 docs/learn/21）────────
    // 判定偏特化 a 是否"至少与 b 同样特化"。算法与 [temp.func.order] 同源，只是数据
    //   来源换成 specPattern：
    //   ① 把 a 的模式里的形参名换成【唯一合成名】（避免 a、b 用同名形参时互相误绑定
    //      —— clang 用 UniqueSynthesizedType 做同一件事）
    //   ② 拿它当【实参】去推导 b 的模式
    //   ③ 推得通 ⇒ b 覆盖 a 的全部输入域 ⇒ a 接受的类型更少 ⇒ a 更特化
    // demo: a = S<T*>，b = S<T**> —— 用 a 推 b：P=T** 配 A=$a* ⇒ 指针层数对不上 ✗；
    //   用 b 推 a：P=T* 配 A=$b** ⇒ 剥一层 ⇒ T := $b* ✓ ⇒ b（T**）更特化，胜出。
    // 对照 clang：SemaOverload.cpp → IsAtLeastAsSpecialized（类模板走
    //   Sema::getMoreSpecializedPartialSpecialization，最终同一套比较逻辑）。
    bool classSpecAtLeastAsSpecialized(const TemplateDeclPtr& a,
                                       const TemplateDeclPtr& b);

    // 深拷贝类型并把其中的模板形参名按 prefix+序号 重命名。
    // 目的：让"某个特化自己的形参"在偏序比较中表现为【不透明的合成类型】，
    // 而不是一个可能与其他特化同名撞车的变量。
    TypePtr renameTemplateParams(const TypePtr& t, const std::string& prefix);

    // 日志辅助：把模式/类型列表渲染成 "T*, T" 这样的可读串
    std::string patternToString(const std::vector<TypePtr>& pattern);
    std::string typeListToString(const std::vector<TypePtr>& types);

    // ── 表达式类型推导 ──
    // 总入口：按节点动态类型分派到 inferXxx，结果写回 expr->resolvedType
    TypePtr inferType(ExprPtr expr);
    // 整数字面量 → int
    TypePtr inferIntLiteral(IntLiteralExpr& expr);
    // 布尔字面量 → bool
    TypePtr inferBoolLiteral(BoolLiteralExpr& expr);
    // 字符串字面量 → 指针类型（简化的 char* 表示）
    TypePtr inferStringLiteral(StringLiteralExpr& expr);
    // nullptr → void*（简化）
    TypePtr inferNullptrLiteral(NullptrLiteralExpr& expr);
    // 名字决议核心：作用域链 → 类字段 → 函数名，三级查找
    TypePtr inferVar(VarExpr& expr);
    // 二元运算：比较→bool；算术→常用算术转换(int/double)简化
    TypePtr inferBinary(BinaryExpr& expr);
    // 一元运算：! → bool；- 保持类型
    TypePtr inferUnary(UnaryExpr& expr);
    // 函数调用：记录实参左值性 → 普通函数 → 模板推导决议（S2~S6）
    TypePtr inferCall(CallExpr& expr);
    // 成员访问 obj.x / p->x：字段查布局表(得偏移)，方法查类声明
    TypePtr inferMember(MemberExpr& expr);
    // new C：类名存在性检查 → 返回 C*
    TypePtr inferNew(NewExpr& expr);
    // this：仅限成员函数内，类型为属主类指针 [class.this]
    TypePtr inferThis(ThisExpr& expr);
    // dynamic_cast<T*>(e)：检查目标类存在且源是类指针 → 返回 T* [expr.dynamic.cast]
    TypePtr inferDynamicCast(DynamicCastExpr& expr);
    // 下标 v[i]（读值）：类必须提供 at() 约定方法 → 返回 at() 的返回类型
    // （[expr.sub] 的糖化：operator[] 重载降级为成员方法调用）
    TypePtr inferIndex(IndexExpr& expr);

    // ── 类内存布局计算 ──
    // 逐字段对齐排布，算 offset/size/totalSize（_vptr 恒在偏移 0）
    void computeClassLayout(ClassDeclPtr decl);
    // 生成 RTTI 符号名(_ZTI 风格)并固化 vtable 槽位索引
    void injectVTableAndRTTI(TypePtr classType);
    // 向上取整到 alignment 的倍数：alignTo(12,8)=16
    uint32_t alignTo(uint32_t offset, uint32_t alignment);
    // 类型的对齐要求（字节），对照 clang Context.getTypeInfoInChars().Align：
    //   标量 = min(size, 8)（int→4, double→8, bool→1, 指针/引用→8）
    //   类类型 = 各成员 alignOf 的递归最大值（封顶 8）——与 size 无关！
    //   例：Five{bool×5} size=5 但 align=1，绝不能用 size 当 align。
    uint32_t alignOf(const TypePtr& type);

    // ── 推导辅助 ──
    std::string inferIndent() const;
    [[noreturn]] void error(const std::string& msg, SourceLocation loc);

    // ── 当前上下文 ──
    std::string m_currentClassName;   // 当前正在处理的类名
    // 当前所处的命名空间（"A" / "A::B" / 空）。用于命名空间内的【非限定
    // 名字查找】回退："N" 作用域里写 `S` 时按 "N::S" 再查一次。
    // 本实现没有作用域链，只记最近一层 —— 见 processNamespaceDecl 的注释。
    // 对照 clang：DeclContext 链上的 lookup。
    std::string m_currentNamespace;
    TypePtr     m_currentReturnType;  // 当前函数的返回类型
    std::string m_currentFuncName;    // 当前函数名
};

} // namespace minicc
