// =============================================================================
// 阶段 4：模板实例化引擎实现
// =============================================================================
// 模板实例化是 C++ 编译期最强大的特性之一。
// 它在编译期"无中生有"地生成全新的类和函数代码。
//
// 工作流程：
//   1. 遇到 MyPtr<int> 使用
//   2. 找到 MyPtr 的模板蓝图（阶段2存储的）
//   3. 深拷贝整个 AST（类声明、字段、方法）
//   4. 将蓝图中所有的 T 替换为 int
//   5. 生成唯一符号名 _Z5MyPtrIiE
//   6. 注册为一个新的真实类
// =============================================================================

#include "template_instantiation.h"
#include <format>
#include <iostream>

namespace minicc {

// ═════════════════════════════════════════════════════════════════════════════
// NameMangler 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 类型编码：将类型转为 mangling 字符串
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::encodeType(TypePtr type) {
    if (!type) return "v"; // void

    switch (type->kind) {
        case TypeKind::Void:   return "v";
        case TypeKind::Bool:   return "b";
        case TypeKind::Int:    return "i";
        case TypeKind::Double: return "d";
        case TypeKind::Pointer:
            return "P" + encodeType(type->pointeeType);
        case TypeKind::LValueReference:
            return "R" + encodeType(type->referencedType);   // GCC ABI: R = lvalue ref
        case TypeKind::RValueReference:
            return "O" + encodeType(type->referencedType);   // GCC ABI: O = rvalue ref
        case TypeKind::Const:
            return "K" + encodeType(type->innerType);        // GCC ABI: K = const
        case TypeKind::Class:
            return std::format("{}{}", type->name.size(), type->name);
        case TypeKind::TemplateParam:
            return type->templateParamName;
        case TypeKind::Auto:
            return "Da"; // 不应该出现（auto 应该在阶段3已被消除）
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板实例化符号名
// 格式: _Z + 模板名长度 + 模板名 + I + 参数编码... + E
// 例: MyPtr<int> → _Z5MyPtrIiE
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleTemplateInstance(
    const std::string& templateName,
    const std::vector<TypePtr>& typeArgs) {

    std::string result = std::format("_Z{}{}I", templateName.size(), templateName);

    for (auto& arg : typeArgs) {
        result += encodeType(arg);
    }

    result += "E";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数符号名
// 格式: _Z + [N + 类名长度 + 类名] + 函数名长度 + 函数名 + 参数编码... [+ E]
// 例: MyClass::foo(int) → _ZN7MyClass3fooEi
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleFunction(
    const std::string& funcName,
    const std::string& className,
    const std::vector<Parameter>& params) {

    std::string result = "_Z";

    if (!className.empty()) {
        result += "N";
        result += std::format("{}{}", className.size(), className);
    }

    result += std::format("{}{}", funcName.size(), funcName);

    for (auto& param : params) {
        result += encodeType(param.type);
    }

    if (!className.empty()) {
        result += "E";
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// RTTI 符号名
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleRTTI(const std::string& className) {
    return std::format("_ZTI{}{}", className.size(), className);
}

// ─────────────────────────────────────────────────────────────────────────────
// vtable 符号名
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleVTable(const std::string& className) {
    return std::format("_ZTV{}{}", className.size(), className);
}

// ═════════════════════════════════════════════════════════════════════════════
// TemplateInstantiator 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 模板实例化：将蓝图克隆并替换
// ─────────────────────────────────────────────────────────────────────────────
ClassDeclPtr TemplateInstantiator::instantiate(
    TemplateDeclPtr templateDecl,
    const std::vector<TypePtr>& typeArgs) {

    // 1. 构建类型替换映射
    //    例: { "T" → Int }
    TypeSubstitution subst;
    for (size_t i = 0; i < templateDecl->typeParams.size()
         && i < typeArgs.size(); i++) {
        subst[templateDecl->typeParams[i]] = typeArgs[i];
    }

    // 2. 生成实例化后的类名
    std::string instanceName = templateDecl->classTemplate->name;
    for (auto& arg : typeArgs) {
        instanceName += "_" + arg->toString();
    }

    std::cout << std::format("\n  ╔══ Template Instantiation ═════════════════════════╗\n");
    std::cout << std::format("  ║ Blueprint: {} <{}>\n",
        templateDecl->classTemplate->name,
        templateDecl->typeParams.empty() ? "?" :
            [&]() { std::string s; for (size_t i = 0; i < templateDecl->typeParams.size(); i++) {
                if (i > 0) s += ", "; s += templateDecl->typeParams[i]; } return s; }());
    std::cout << std::format("  ║ Instance:  {}\n", instanceName);

    // 打印替换映射
    std::cout << "  ║ Substitution map: { ";
    for (auto& [param, type] : subst) {
        std::cout << std::format("'{}' → '{}', ", param, type->toString());
    }
    std::cout << "}\n";

    // 3. 深拷贝类声明
    auto newClass = std::make_shared<ClassDecl>();
    newClass->name = instanceName;
    newClass->baseClassName = templateDecl->classTemplate->baseClassName;
    newClass->location = templateDecl->location;

    // 4. 克隆字段（替换类型中的模板参数）
    std::cout << "  ║ ── Field Substitution ──\n";
    for (auto& field : templateDecl->classTemplate->fields) {
        std::cout << std::format("  ║   field '{}' : {} → ",
            field.name, field.type ? field.type->toString() : "?");
        auto cloned = cloneField(field, subst);
        std::cout << std::format("{}\n", cloned.type ? cloned.type->toString() : "?");
        newClass->fields.push_back(cloned);
    }

    // 5. 克隆方法（替换类型和函数体中的模板参数）
    std::cout << "  ║ ── Method Substitution ──\n";
    for (auto& method : templateDecl->classTemplate->methods) {
        std::cout << std::format("  ║   method '{}' : ", method->name);

        // 打印参数类型替换
        for (size_t i = 0; i < method->parameters.size(); i++) {
            auto& param = method->parameters[i];
            if (i > 0) std::cout << ", ";
            std::cout << std::format("{}:{}", param.name,
                param.type ? param.type->toString() : "?");
        }
        std::cout << std::format(" → {}\n",
            method->returnType ? method->returnType->toString() : "void");

        newClass->methods.push_back(
            cloneMethod(method, subst, instanceName));
    }

    // 6. 生成 mangled name
    std::string mangledName = NameMangler::mangleTemplateInstance(
        templateDecl->classTemplate->name, typeArgs);

    std::cout << std::format("  ║ Mangled: {} → {}\n", instanceName, mangledName);
    std::cout << std::format("  ╚═══════════════════════════════════════════════════╝\n");

    m_instantiatedClasses.push_back(newClass);
    return newClass;
}

// ─────────────────────────────────────────────────────────────────────────────
// 类型替换：将模板参数替换为实际类型
// ─────────────────────────────────────────────────────────────────────────────
// 这是模板实例化的核心操作：
//   如果类型是 TemplateParam 且名字在替换表中 → 返回实际类型
//   如果类型是 Pointer(T*) → 递归替换 T
//   如果类型是 LValueReference(T&) → 递归替换 T
//   如果类型是 RValueReference(T&&) → 递归替换 T + 引用折叠
//   如果类型是 Const(const T) → 递归替换 T
//   如果类型是 Class 且名字匹配模板参数 → 返回实际类型
//   否则 → 返回原类型
//
// ★ 引用折叠（Reference Collapsing）规则 ★
//   C++ 标准规定，当引用嵌套时按以下规则折叠：
//     T&  &   → T&    （左值引用 + 左值引用 → 左值引用）
//     T&  &&  → T&    （左值引用 + 右值引用 → 左值引用）
//     T&& &   → T&    （右值引用 + 左值引用 → 左值引用）
//     T&& &&  → T&&   （右值引用 + 右值引用 → 右值引用）
//   简言之：只要有一个是左值引用，结果就是左值引用。
//
//   这就是"万能引用"(Forwarding Reference)的原理：
//     template<typename T>
//     void foo(T&& x);
//
//     foo(42);      → T = int,    T&& = int&&    (右值引用)
//     foo(var);     → T = int&,   T&& = int& && → int&  (折叠为左值引用)
// ─────────────────────────────────────────────────────────────────────────────
TypePtr TemplateInstantiator::substituteType(
    TypePtr type, const TypeSubstitution& subst) {

    if (!type) return nullptr;

    // ── Case 1: 模板参数 → 直接替换 ──
    if (type->isTemplateParam()) {
        auto it = subst.find(type->templateParamName);
        if (it != subst.end()) {
            std::cout << std::format("    [subst] ★ TemplateParam '{}' → '{}' (direct replacement)\n",
                type->templateParamName, it->second->toString());
            return it->second;
        }
        std::cout << std::format("    [subst] TemplateParam '{}' not in substitution map, keep as-is\n",
            type->templateParamName);
        return type;
    }

    // ── Case 2: 指针类型 T* → 递归替换内部类型 ──
    if (type->isPointer() && type->pointeeType) {
        std::cout << std::format("    [subst] Pointer({}*) → recursing into pointee...\n",
            type->pointeeType->toString());
        TypePtr newPointee = substituteType(type->pointeeType, subst);
        if (newPointee != type->pointeeType) {
            auto result = Type::makePointer(newPointee);
            std::cout << std::format("    [subst] ★ Pointer substituted: {}* → {}*\n",
                type->pointeeType->toString(), newPointee->toString());
            return result;
        }
    }

    // ── Case 3: 左值引用 T& → 递归替换 + 引用折叠 ──
    if (type->isLValueReference() && type->referencedType) {
        std::cout << std::format("    [subst] LValueRef({}&) → recursing into referenced type...\n",
            type->referencedType->toString());
        TypePtr newInner = substituteType(type->referencedType, subst);

        // 引用折叠：如果替换后的类型本身也是引用，需要折叠
        if (newInner->isLValueReference() || newInner->isRValueReference()) {
            // T& & → T&  或  T&& & → T&
            std::cout << std::format("    [subst] ★ Reference collapsing: {}& → {} (& wins)\n",
                newInner->toString(), newInner->toString());
            return newInner; // & 总是赢
        }

        if (newInner != type->referencedType) {
            auto result = Type::makeLValueReference(newInner);
            std::cout << std::format("    [subst] ★ LValueRef substituted: {}& → {}&\n",
                type->referencedType->toString(), newInner->toString());
            return result;
        }
    }

    // ── Case 4: 右值引用 T&& → 递归替换 + 引用折叠 ──
    // ★ 这是"万能引用"(Forwarding Reference)的关键路径 ★
    // 当 T 是模板参数时，T&& 是万能引用：
    //   T = int   → int&&   (右值引用)
    //   T = int&  → int& && → int&  (折叠为左值引用)
    //   T = int&& → int&& && → int&& (折叠为右值引用)
    if (type->isRValueReference() && type->referencedType) {
        std::cout << std::format("    [subst] RValueRef({}&&) → recursing into referenced type...\n",
            type->referencedType->toString());
        TypePtr newInner = substituteType(type->referencedType, subst);

        // 引用折叠
        if (newInner->isLValueReference()) {
            // int& && → int&（左值引用永远赢）
            std::cout << std::format("    [subst] ★ Reference collapsing: {}&& → {} (lvalue ref wins!)\n",
                newInner->toString(), newInner->toString());
            return newInner;
        }
        if (newInner->isRValueReference()) {
            // int&& && → int&&（右值引用 + 右值引用 = 右值引用）
            std::cout << std::format("    [subst] ★ Reference collapsing: {}&& → {} (rvalue ref stays)\n",
                newInner->toString(), newInner->toString());
            return newInner;
        }

        // 普通类型：T&& → actualType&&
        if (newInner != type->referencedType) {
            auto result = Type::makeRValueReference(newInner);
            std::cout << std::format("    [subst] ★ RValueRef substituted: {}&& → {}&&\n",
                type->referencedType->toString(), newInner->toString());
            return result;
        }
    }

    // ── Case 5: const T → 递归替换内部类型 ──
    if (type->isConst() && type->innerType) {
        std::cout << std::format("    [subst] Const(const {}) → recursing into inner type...\n",
            type->innerType->toString());
        TypePtr newInner = substituteType(type->innerType, subst);
        if (newInner != type->innerType) {
            auto result = Type::makeConst(newInner);
            std::cout << std::format("    [subst] ★ Const substituted: const {} → const {}\n",
                type->innerType->toString(), newInner->toString());
            return result;
        }
    }

    // ── Case 6: 类类型中的模板参数名（简化处理）──
    // 因为 Parser 在解析类型 T 时创建的是 Class("T") 而非 TemplateParam("T")
    // 所以这里也需要检查类名是否在替换表中
    if (type->isClass()) {
        auto it = subst.find(type->name);
        if (it != subst.end()) {
            std::cout << std::format("    [subst] ★ Class '{}' matches template param → '{}'\n",
                type->name, it->second->toString());
            return it->second;
        }
    }

    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆字段
// ─────────────────────────────────────────────────────────────────────────────
FieldInfo TemplateInstantiator::cloneField(
    const FieldInfo& field, const TypeSubstitution& subst) {

    FieldInfo newField;
    newField.name = field.name;
    newField.type = substituteType(field.type, subst);
    newField.offset = field.offset;
    newField.size = field.size;
    newField.access = field.access;
    return newField;
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆方法（深拷贝函数声明和函数体）
// ─────────────────────────────────────────────────────────────────────────────
FuncDeclPtr TemplateInstantiator::cloneMethod(
    FuncDeclPtr method, const TypeSubstitution& subst,
    const std::string& newClassName) {

    auto newMethod = std::make_shared<FunctionDecl>();
    newMethod->name = method->name;
    newMethod->isVirtual = method->isVirtual;
    newMethod->isOverride = method->isOverride;
    newMethod->ownerClassName = newClassName;
    newMethod->location = method->location;

    // 替换返回类型
    std::cout << std::format("    [clone:method] '{}' return type: {} → ",
        method->name, method->returnType ? method->returnType->toString() : "void");
    newMethod->returnType = substituteType(method->returnType, subst);
    std::cout << std::format("{}\n", newMethod->returnType ? newMethod->returnType->toString() : "void");

    // 克隆参数
    for (auto& param : method->parameters) {
        Parameter newParam;
        newParam.name = param.name;
        std::cout << std::format("    [clone:method]   param '{}' : {} → ",
            param.name, param.type ? param.type->toString() : "?");
        newParam.type = substituteType(param.type, subst);
        std::cout << std::format("{}\n", newParam.type ? newParam.type->toString() : "?");
        newMethod->parameters.push_back(newParam);
    }

    // 克隆函数体
    if (method->body) {
        auto newBody = std::make_shared<BlockStmt>();
        newBody->location = method->body->location;
        for (auto& stmt : method->body->statements) {
            newBody->statements.push_back(cloneStmt(stmt, subst));
        }
        newMethod->body = newBody;
    }

    return newMethod;
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆表达式（深拷贝并替换类型）
// ─────────────────────────────────────────────────────────────────────────────
ExprPtr TemplateInstantiator::cloneExpr(
    ExprPtr expr, const TypeSubstitution& subst) {

    if (!expr) return nullptr;

    // 整数字面量
    if (auto e = std::dynamic_pointer_cast<IntLiteralExpr>(expr)) {
        auto cloned = std::make_shared<IntLiteralExpr>(e->value);
        cloned->location = e->location;
        return cloned;
    }

    // 布尔字面量
    if (auto e = std::dynamic_pointer_cast<BoolLiteralExpr>(expr)) {
        auto cloned = std::make_shared<BoolLiteralExpr>(e->value);
        cloned->location = e->location;
        return cloned;
    }

    // 字符串字面量
    if (auto e = std::dynamic_pointer_cast<StringLiteralExpr>(expr)) {
        auto cloned = std::make_shared<StringLiteralExpr>(e->value);
        cloned->location = e->location;
        return cloned;
    }

    // 变量引用
    if (auto e = std::dynamic_pointer_cast<VarExpr>(expr)) {
        auto cloned = std::make_shared<VarExpr>(e->name);
        cloned->location = e->location;
        // 检查变量名是否是模板参数
        auto it = subst.find(e->name);
        if (it != subst.end()) {
            // 变量名就是模板参数名（不太常见，但需要处理）
        }
        return cloned;
    }

    // 二元表达式
    if (auto e = std::dynamic_pointer_cast<BinaryExpr>(expr)) {
        auto cloned = std::make_shared<BinaryExpr>(
            e->op, cloneExpr(e->left, subst), cloneExpr(e->right, subst));
        cloned->location = e->location;
        return cloned;
    }

    // 一元表达式
    if (auto e = std::dynamic_pointer_cast<UnaryExpr>(expr)) {
        auto cloned = std::make_shared<UnaryExpr>(
            e->op, cloneExpr(e->operand, subst));
        cloned->location = e->location;
        return cloned;
    }

    // 函数调用
    if (auto e = std::dynamic_pointer_cast<CallExpr>(expr)) {
        std::vector<ExprPtr> clonedArgs;
        for (auto& arg : e->arguments) {
            clonedArgs.push_back(cloneExpr(arg, subst));
        }
        auto cloned = std::make_shared<CallExpr>(
            cloneExpr(e->callee, subst), std::move(clonedArgs));
        cloned->location = e->location;
        return cloned;
    }

    // 成员访问
    if (auto e = std::dynamic_pointer_cast<MemberExpr>(expr)) {
        auto cloned = std::make_shared<MemberExpr>(
            cloneExpr(e->object, subst), e->memberName, e->isArrow);
        cloned->location = e->location;
        return cloned;
    }

    // this
    if (auto e = std::dynamic_pointer_cast<ThisExpr>(expr)) {
        auto cloned = std::make_shared<ThisExpr>();
        cloned->location = e->location;
        return cloned;
    }

    // new
    if (auto e = std::dynamic_pointer_cast<NewExpr>(expr)) {
        auto cloned = std::make_shared<NewExpr>(e->className);
        cloned->location = e->location;
        for (auto& arg : e->constructorArgs) {
            cloned->constructorArgs.push_back(cloneExpr(arg, subst));
        }
        return cloned;
    }

    // nullptr
    if (auto e = std::dynamic_pointer_cast<NullptrLiteralExpr>(expr)) {
        auto cloned = std::make_shared<NullptrLiteralExpr>();
        cloned->location = e->location;
        return cloned;
    }

    return nullptr; // 未处理的表达式类型
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆语句（深拷贝并替换类型）
// ─────────────────────────────────────────────────────────────────────────────
StmtPtr TemplateInstantiator::cloneStmt(
    StmtPtr stmt, const TypeSubstitution& subst) {

    if (!stmt) return nullptr;

    // 变量声明
    if (auto s = std::dynamic_pointer_cast<VarDeclStmt>(stmt)) {
        auto cloned = std::make_shared<VarDeclStmt>(
            s->name,
            substituteType(s->declaredType, subst),
            cloneExpr(s->initializer, subst));
        cloned->location = s->location;
        return cloned;
    }

    // 表达式语句
    if (auto s = std::dynamic_pointer_cast<ExprStmt>(stmt)) {
        auto cloned = std::make_shared<ExprStmt>(cloneExpr(s->expr, subst));
        cloned->location = s->location;
        return cloned;
    }

    // 赋值语句
    if (auto s = std::dynamic_pointer_cast<AssignStmt>(stmt)) {
        auto cloned = std::make_shared<AssignStmt>(
            cloneExpr(s->target, subst),
            cloneExpr(s->value, subst));
        cloned->location = s->location;
        return cloned;
    }

    // return 语句
    if (auto s = std::dynamic_pointer_cast<ReturnStmt>(stmt)) {
        auto cloned = std::make_shared<ReturnStmt>(
            cloneExpr(s->value, subst));
        cloned->location = s->location;
        return cloned;
    }

    // if 语句
    if (auto s = std::dynamic_pointer_cast<IfStmt>(stmt)) {
        auto cloned = std::make_shared<IfStmt>(
            cloneExpr(s->condition, subst),
            cloneStmt(s->thenBranch, subst),
            s->elseBranch ? cloneStmt(s->elseBranch, subst) : nullptr);
        cloned->location = s->location;
        return cloned;
    }

    // while 语句
    if (auto s = std::dynamic_pointer_cast<WhileStmt>(stmt)) {
        auto cloned = std::make_shared<WhileStmt>(
            cloneExpr(s->condition, subst),
            cloneStmt(s->body, subst));
        cloned->location = s->location;
        return cloned;
    }

    // 代码块
    if (auto s = std::dynamic_pointer_cast<BlockStmt>(stmt)) {
        auto cloned = std::make_shared<BlockStmt>();
        cloned->location = s->location;
        for (auto& inner : s->statements) {
            cloned->statements.push_back(cloneStmt(inner, subst));
        }
        return cloned;
    }

    return nullptr; // 未处理的语句类型
}

} // namespace minicc
