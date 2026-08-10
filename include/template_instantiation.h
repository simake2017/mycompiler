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
//
// 管线位置（阶段 4，推导 S5）：
//   Parser(蓝图) → SemanticAnalyzer(发现实例化需求) → TemplateDeducer(推导实参)
//   → 【本文件：TemplateInstantiator 按实参克隆 + 替换，产出具体类/函数】
//   → SemanticAnalyzer(对实例做类型检查) → CodeGen
//
// 对应 C++ 标准章节：
//   [temp.inst]   模板实例化的时机与规则（隐式/按需实例化）
//   [temp.subst]  模板实参替换：把模板参数替换为实参类型
//   [dcl.ref]     引用折叠规则（万能引用实例化的关键）
// 对照 clang：
//   lib/Sema/SemaTemplateInstantiate.cpp —— 声明级实例化（本文件 instantiate*）
//   lib/Sema/TreeTransform.h             —— AST 递归重建（本文件 cloneExpr/cloneStmt）
//   区别：clang 用 SubstTemplateTypeParmType 类型节点记录替换，minicc 在克隆时直接换类型
//
// 实例化 = 结构化替换（MyPtr<int>）：
//   蓝图 template<typename T> class MyPtr          实例 MyPtr_int
//   ┌──────────────────────────────┐  {T→int}  ┌──────────────────────────────┐
//   │ field data : T*              │ ────────▶ │ field data : int*            │
//   │ method get() : T&            │           │ method get() : int&          │
//   └──────────────────────────────┘           └──────────────────────────────┘
//   符号名：_Z5MyPtrIiE（Itanium ABI mangling）
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
//
// 编码规则对齐 Itanium C++ ABI（GCC/Clang 所用）：
//   _Z 前缀、<长度><名字> 的 source-name、I…E 模板实参表、N…E 嵌套限定名
// =============================================================================
class NameMangler {
public:
    // 对模板类实例化生成符号名
    // 例: MyPtr<int> → _Z5MyPtrIiE
    // 函数模板实例同样走这里：twice<int> → _Z5twiceIiE（与源码名区分，避免链接冲突）
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
    // 模板参数名 → 实际类型的映射（公开：推导引擎构造后传入）
    using TypeSubstitution = std::unordered_map<std::string, TypePtr>;

    // 【做什么】类模板实例化 [temp.inst]：深拷贝蓝图 + 结构化替换 [temp.subst]
    // 【demo】MyPtr<int>：替换表 {T→int}，字段 T* data → int* data，
    //         方法中每个 T 换成 int，新类名 MyPtr_int，符号 _Z5MyPtrIiE
    //
    // 实例化一个模板类
    // templateDecl: 模板蓝图
    // typeArgs: 实际的类型参数（如 [int]）
    // 返回：实例化后的 ClassDecl
    ClassDeclPtr instantiate(
        TemplateDeclPtr templateDecl,
        const std::vector<TypePtr>& typeArgs);

    // 实例化一个函数模板（S5）
    // typeArgs 由推导引擎（S2~S4）产出；克隆蓝图函数并替换所有 T，
    // 生成 mangled 符号名（_Z5twiceIiE 风格）。
    // 【理论】实例化 = 结构化替换 substitution：把推导得到的 {T := int} 应用到
    //         蓝图的每个类型位置（返回类型/形参/函数体局部变量），而非文本替换
    // 【demo】twice(3) 推导出 T := int ⇒ 实例化 twice<int>：
    //         void twice(T x) → void twice(int x)，函数体中的 T 同步替换，符号 _Z5twiceIiE
    FuncDeclPtr instantiateFunction(
        TemplateDeclPtr templateDecl,
        const std::vector<TypePtr>& typeArgs);

    // 获取所有已实例化的类
    const std::vector<ClassDeclPtr>& getInstantiatedClasses() const {
        return m_instantiatedClasses;
    }

    // 获取所有已实例化的函数（S5）
    const std::vector<FuncDeclPtr>& getInstantiatedFunctions() const {
        return m_instantiatedFunctions;
    }

    // 类型替换（公开：推导引擎替换返回类型时复用同一套规则，含引用折叠）
    // 【理论】[temp.subst] 的核心操作：遍历类型树，每个模板参数叶节点换成实参类型，
    //         复合节点（指针/引用/const）递归重建；引用节点重建时执行引用折叠 [dcl.ref]
    // 【demo】substituteType(T,  {T := int})  → int
    //         substituteType(T&, {T := int})  → int&
    //         substituteType(T&&,{T := int&}) → int& && 折叠为 int&（万能引用落地）
    TypePtr substituteType(TypePtr type, const TypeSubstitution& subst);

private:
    std::vector<ClassDeclPtr> m_instantiatedClasses;
    std::vector<FuncDeclPtr>  m_instantiatedFunctions;

    // ── AST 深拷贝与类型替换 ──
    // 对应 clang TreeTransform 的递归重建思想：逐节点克隆，类型位置套用 substituteType；
    // 克隆产物不携带旧的语义分析结果，交回语义分析器重新检查（两阶段查找的第二阶段）
    ExprPtr cloneExpr(ExprPtr expr, const TypeSubstitution& subst);
    StmtPtr cloneStmt(StmtPtr stmt, const TypeSubstitution& subst);
    FuncDeclPtr cloneMethod(FuncDeclPtr method, const TypeSubstitution& subst,
                            const std::string& newClassName);
    FieldInfo cloneField(const FieldInfo& field, const TypeSubstitution& subst);
};

} // namespace minicc
