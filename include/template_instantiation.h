#pragma once
// =============================================================================
// 阶段 4：模板实例化引擎 (Template Instantiation Engine)
// =============================================================================
// 核心职责：
//   1. 识别代码中对模板的使用（如 MyPtr<int>）
//   2. 深拷贝模板 AST 蓝图
//   3. 将所有模板参数占位符（T）替换为真实类型（int）
//   4. 生成全局唯一的符号名（Name Mangling）
//   5. 将实例化后的类注册到全局类型表中
//
// 模板的本质：编译期的"复制粘贴"——但不是简单的文本替换，
// 而是 AST 层面的结构化克隆与类型替换。
// =============================================================================

#include "ast.h"
#include "type.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// NameMangler：符号修饰器
// ─────────────────────────────────────────────────────────────────────────────
// 为什么需要 Name Mangling？
//   C++ 支持函数重载和模板，但汇编器/链接器只认唯一的名字。
//   Name Mangling 将函数的完整签名编码为一个全球唯一的字符串。
//
// 例如（简化版 GCC ABI）：
//   MyPtr<int>        → _Z5MyPtrIiE
//   MyClass::foo(int) → _ZN7MyClass3fooEi
// =============================================================================
class NameMangler {
public:
    // 对模板类实例化生成符号名
    // 例: MyPtr<int> → _Z5MyPtrIiE
    static std::string mangleTemplateInstance(
        const std::string& templateName,
        const std::vector<TypePtr>& typeArgs);

    // 对函数生成符号名
    // 例: foo(int, double) → _Z3foo id
    static std::string mangleFunction(
        const std::string& funcName,
        const std::string& className,
        const std::vector<Parameter>& params);

    // 生成 RTTI type_info 的符号名
    // 例: MyClass → _ZTI7MyClass
    static std::string mangleRTTI(const std::string& className);

    // 生成 vtable 的符号名
    // 例: MyClass → _ZTV7MyClass
    static std::string mangleVTable(const std::string& className);

private:
    // 将类型编码为 mangling 字符串
    static std::string encodeType(TypePtr type);
};

// ─────────────────────────────────────────────────────────────────────────────
// TemplateInstantiator：模板实例化引擎
// ─────────────────────────────────────────────────────────────────────────────
class TemplateInstantiator {
public:
    // 实例化一个模板类
    // templateDecl: 模板蓝图
    // typeArgs: 实际的类型参数（如 [int]）
    // 返回：实例化后的 ClassDecl
    ClassDeclPtr instantiate(
        TemplateDeclPtr templateDecl,
        const std::vector<TypePtr>& typeArgs);

    // 获取所有已实例化的类
    const std::vector<ClassDeclPtr>& getInstantiatedClasses() const {
        return m_instantiatedClasses;
    }

private:
    std::vector<ClassDeclPtr> m_instantiatedClasses;

    // 模板参数名 → 实际类型的映射
    using TypeSubstitution = std::unordered_map<std::string, TypePtr>;

    // ── AST 深拷贝与类型替换 ──
    TypePtr substituteType(TypePtr type, const TypeSubstitution& subst);
    ExprPtr cloneExpr(ExprPtr expr, const TypeSubstitution& subst);
    StmtPtr cloneStmt(StmtPtr stmt, const TypeSubstitution& subst);
    FuncDeclPtr cloneMethod(FuncDeclPtr method, const TypeSubstitution& subst,
                            const std::string& newClassName);
    FieldInfo cloneField(const FieldInfo& field, const TypeSubstitution& subst);
};

} // namespace minicc
