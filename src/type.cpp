// =============================================================================
// 类型系统实现（include/type.h 声明的 Type 成员）
// =============================================================================
// 工厂方法、sizeInBytes、equals、toString、stripReferences/stripConst。
// 目录分工：
//   · 引用折叠与结构替换（substituteType）在 src/template_instantiation.cpp；
//   · 类型 → Itanium mangling 编码（NameMangler::encodeType）也在那里。
//   本文件的 toString 面向"人读"，encodeType 面向"链接器"。
// =============================================================================

#include "type.h"
#include <format>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// TemplateArg：模板实参的人读形态
// ─────────────────────────────────────────────────────────────────────────────
// 按 kind 分派：类型实参取类型名，非类型实参（NTTP）取十进制数字 —— 这是"值 vs 类型"
// 在字符串层面的体现（把 4 当成类型打印只会得到无意义的 "?"）。
// demo: ofType(makeInt()).toString() ⇒ "int"；ofValue(4).toString() ⇒ "4"
std::string TemplateArg::toString() const {
    if (kind == TemplateArgKind::Type) return type ? type->toString() : "?";
    return std::to_string(value);
}

// ─────────────────────────────────────────────────────────────────────────────
// 工厂方法：创建各种类型实例
// ─────────────────────────────────────────────────────────────────────────────

// void 类型。无入参。demo：void f(); 的返回类型 → Type{kind=Void, name="void"}
TypePtr Type::makeVoid() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Void;
    t->name = "void";
    return t;
}

// bool 类型。无入参。demo：bool b; 的声明类型 → Type{kind=Bool, name="bool"}，encodeType→"b"
TypePtr Type::makeBool() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Bool;
    t->name = "bool";
    return t;
}

// int 类型。无入参。demo：int x; 的声明类型 → Type{kind=Int, name="int"}，encodeType→"i"
TypePtr Type::makeInt() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Int;
    t->name = "int";
    return t;
}

// double 类型。无入参。demo：double d; → Type{kind=Double, name="double"}，encodeType→"d"
TypePtr Type::makeDouble() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Double;
    t->name = "double";
    return t;
}

// 指针类型 T*。入参 pointee = 被指向的内层类型。
// demo：makePointer(Int) → Pointer(Int)，toString="int*"，encodeType→"Pi"
//       makePointer(Pointer(Int)) → int**，encodeType→"PPi"
TypePtr Type::makePointer(TypePtr pointee) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Pointer;
    t->name = pointee->toString() + "*";
    t->pointeeType = std::move(pointee);
    return t;
}

// ── 引用折叠（[dcl.ref]/6）：★ 在【构造点】就规范化，不留给调用方 ──
// 四行合一（只要有一层是左值引用，结果就是左值引用）：
//     T&  &  → T&      T&  && → T&      T&& &  → T&      T&& && → T&&
// 【为什么必须放在这里】折叠不是"替换时顺手做的一步"，而是【引用类型的不变量】：造引用
//   的地方有四处 —— 模板替换（substituteType）、实参推导的万能引用 bind、Parser 的声明符、
//   Sema。只在其中一条路径上折叠，别处造出的嵌套引用就无人收拾：推导 `T := A&` 时 A 本身
//   已是引用（变量的声明类型）⇒ 得到非法的 `int& &` 结构，观测量是同一函数被实例化出两个
//   符号（_Z2idIRiE / _Z2idIRRiE）。放进工厂函数后，四"处"变成零"处"。
// 对照 clang：唯一折叠点是 Sema::BuildReferenceType（clang/lib/Sema/SemaType.cpp:1887），
//   canonical type 在构造时即算好（ASTContext::getLValueReferenceType，ASTContext.cpp:4163）
//   —— 同一个思想：规范化在类型诞生的那一刻完成。
// ★ 本实现无 canonical type 概念（日志/符号/比较吃同一份结构），故直接返回折叠后的规范
//   形式，而非 clang 那样保留拼写形式的嵌套节点。
//
// 左值引用 T&。入参 referenced = 被引用的内层类型。
// demo：makeLValueReference(Int) → LValueReference(Int)，toString="int&"，encodeType→"Ri"
TypePtr Type::makeLValueReference(TypePtr referenced) {
    // T& & → T&：内层已是左值引用，直接复用（结构共享，不造新节点）
    if (referenced->isLValueReference()) return referenced;
    // T&& & → T&：'&' 赢 —— 剥掉内层的 '&&'，结果仍是左值引用
    // （递归调用而非直接取 referencedType，是为了对"万一存在的更深处嵌套"自愈）
    if (referenced->isRValueReference()) return Type::makeLValueReference(referenced->referencedType);

    auto t = std::make_shared<Type>();
    t->kind = TypeKind::LValueReference;
    t->name = referenced->toString() + "&";
    t->referencedType = std::move(referenced);
    return t;
}

// 右值引用 T&&。入参 referenced = 被引用的内层类型。
// demo：makeRValueReference(Int) → RValueReference(Int)，toString="int&&"，encodeType→"Oi"
// 当 T 是模板参数时 T&& 是"万能引用"（[temp.deduct.call]）。
TypePtr Type::makeRValueReference(TypePtr referenced) {
    // [dcl.ref]/6：右值引用套在【任何】引用上，结果都是那个内层引用本身
    //   T&  && → T&      T&& && → T&&
    // 即"内层是什么就还是什么"—— 故直接返回内层，无需区分左右值。
    // （注意与左值引用的区别：那个是"剥壳取左值"，这个是"原样返回"。）
    if (referenced->isReference()) return referenced;

    auto t = std::make_shared<Type>();
    t->kind = TypeKind::RValueReference;
    t->name = referenced->toString() + "&&";
    t->referencedType = std::move(referenced);
    return t;
}

// const 限定 const T。入参 inner = 被限定的内层类型。
// demo：makeConst(Int) → Const(Int)，toString="const int"，encodeType→"Ki"
//       makeConst(LValueReference(Int)) → const int&，encodeType→"KRi"
// ★ const 落在哪一层由【源码位置】决定（[dcl.type.cv]，见 type.h"组合规则"与 docs/learn/23）：
//   makeConst(Int)              → Const(Int)           "const int"    （说明符侧，限定基类型）
//   makeConst(Pointer(Int))     → Const(Pointer(Int))  "int* const"   （声明符侧）
//   makePointer(makeConst(Int)) → Pointer(Const(Int))  "const int*"   （说明符侧）
//   两种结构打印不同、mangling 不同、偏特化匹配结果也不同 —— 不可互换。
TypePtr Type::makeConst(TypePtr inner) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Const;
    t->name = "const " + inner->toString();
    t->innerType = std::move(inner);
    return t;
}

// 类类型。入参 name = 类名；同时初始化 classLayout.className。
// demo：makeClass("Point") → Class{ name="Point", classLayout.className="Point" }
// encodeType→"5Point"（长度5 + 名字）。字段偏移/vtable 由语义阶段填充。
TypePtr Type::makeClass(const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Class;
    t->name = name;
    t->classLayout.className = name;
    return t;
}

// 模板参数占位类型。入参 paramName = 模板参数名。
// demo：makeTemplateParam("T") → TemplateParam{ templateParamName="T" }
// 出现在模板蓝图中；实例化时被 substituteType 替换为实际类型。
TypePtr Type::makeTemplateParam(const std::string& paramName) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::TemplateParam;
    t->name = paramName;
    t->templateParamName = paramName;
    return t;
}

// auto 占位类型。无入参。demo：auto x = ...; → Type{kind=Auto, name="auto"}
// 语义阶段用初始化表达式类型回填后即被消除（阶段3之后不应再出现 auto）。
TypePtr Type::makeAuto() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Auto;
    t->name = "auto";
    return t;
}

// decltype(expr) 半成品类型。入参 expr = 操作数；paren = 原文是否写成 decltype((e))。
// demo：decltype(x) → Type{kind=Decltype, decltypeExpr=VarExpr{x}, decltypeParen=false}
// 【不立即求值】：表达式原样留存，等 substituteType 阶段再求
// （模板模式里 T 未知，此刻无法求）。对照 clang：DecltypeType。
TypePtr Type::makeDecltype(DecltypeExprPtr expr, bool paren) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Decltype;
    t->name = "decltype";          // 占位名；求值后整个节点被具体类型取代
    t->decltypeExpr = std::move(expr);
    t->decltypeParen = paren;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// sizeInBytes：获取类型占用的字节数
// ─────────────────────────────────────────────────────────────────────────────
// "运行期看偏移量"的基础。简化模型按 64 位系统：void=0，bool=1，int=4，double=8，
// 指针与引用=8（引用本质是指针），class 取 classLayout.totalSize；
// TemplateParam / Auto / Decltype 大小未知，返回 0（正常流程下不应活到 CodeGen）。
uint32_t Type::sizeInBytes() const {
    switch (kind) {
        case TypeKind::Void:   return 0;
        case TypeKind::Bool:   return 1;
        case TypeKind::Int:    return 4;
        case TypeKind::Double: return 8;
        case TypeKind::Pointer: return 8;
        case TypeKind::LValueReference: return 8;  // 引用本质是指针，占 8 字节
        case TypeKind::RValueReference: return 8;  // 同上
        case TypeKind::Const:  return innerType ? innerType->sizeInBytes() : 0;
        case TypeKind::Class:  return classLayout.totalSize;
        case TypeKind::TemplateParam: return 0; // 模板参数大小未知
        case TypeKind::Auto:   return 0; // auto 大小未知，等待推导
        case TypeKind::Decltype: return 0; // decltype 未求值，大小未知
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// equals：类型比较
// ─────────────────────────────────────────────────────────────────────────────
// 结构化递归比较：kind 相同才继续，复合类型递归比较内层。用于语义检查（实参/形参匹配）
// 与函数模板推导中"多处推导结果须一致"的判断（合一算法 unify 成功后的等价检查）。
// demo: int equals int ⇒ true；int equals double ⇒ false；Pi equals Pi ⇒ true（递归比 pointee）
bool Type::equals(const TypePtr& other) const {
    if (!other) return false;
    if (kind != other->kind) return false;

    switch (kind) {
        case TypeKind::Void:
        case TypeKind::Bool:
        case TypeKind::Int:
        case TypeKind::Double:
            return true; // 基础类型只比较 kind

        case TypeKind::Pointer:
            return pointeeType && other->pointeeType
                && pointeeType->equals(other->pointeeType);

        case TypeKind::LValueReference:
        case TypeKind::RValueReference:
            return referencedType && other->referencedType
                && referencedType->equals(other->referencedType);

        case TypeKind::Const:
            return innerType && other->innerType
                && innerType->equals(other->innerType);

        case TypeKind::Class:
            // ★ 嵌套类型名（S<int>::type / T::type）不能只比 name ——
            //   两个不同限定者的 "type" 会撞成相等（Box<int>::type 与
            //   Box<double>::type 是不同类型）。必须连限定者一起递归比。
            if (isNestedName() != other->isNestedName()) return false;
            if (isNestedName()) {
                return name == other->name
                    && nestedQualifier->equals(other->nestedQualifier);
            }
            return name == other->name; // 类类型按名字比较

        case TypeKind::TemplateParam:
            return templateParamName == other->templateParamName;

        case TypeKind::Auto:
            return true; // auto 与 auto 相等

        case TypeKind::Decltype:
            // 未求值的 decltype 之间无从比较（操作数是表达式，不是类型）。
            // 正常流程下不该走到这里：substituteType / resolveType 之后
            // Decltype 节点应已被求值成具体类型并整体替换掉。
            return false;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// toString：类型的可读字符串表示
// ─────────────────────────────────────────────────────────────────────────────
// 递归拼接，用于中文日志与报错信息。与 NameMangler::encodeType 的分工：toString 面向
// "人读"，encodeType 面向"链接器"。
// demo: Const(LValueReference(Int)).toString() ⇒ "const int&"
std::string Type::toString() const {
    switch (kind) {
        case TypeKind::Void:   return "void";
        case TypeKind::Bool:   return "bool";
        case TypeKind::Int:    return "int";
        case TypeKind::Double: return "double";
        case TypeKind::Pointer:
            return pointeeType ? pointeeType->toString() + "*" : "?*";
        case TypeKind::LValueReference:
            return referencedType ? referencedType->toString() + "&" : "?&";
        case TypeKind::RValueReference:
            return referencedType ? referencedType->toString() + "&&" : "?&&";
        case TypeKind::Const: {
            if (!innerType) return "const ?";
            // ★ 'const' 印在【哪一侧】由它限定的东西决定（[dcl.type.cv]）：
            //     Const(Int)          → "const int"       限定基类型
            //     Pointer(Const(Int)) → "const int*"      指针【指向】const
            //     Const(Pointer(Int)) → "int* const"      const 的【指针】
            // ★ toString() 不是单射 —— 若一律输出 "const " + inner->toString()，
            //   Pointer(Const(Int)) 与 Const(Pointer(Int)) 会印成同一个串。危害不止日志难读：
            //   ① getOrInstantiateClass 的【缓存键】以 toString 为输入 ⇒ C<const int*> 与
            //      C<int* const> 共用一条缓存，后者的实例被前者的实例顶掉；
            //   ② 实例名清洗（template_instantiation.cpp Step 2）同样吃 toString。
            //   即"类型树的形状"与"人读串"必须是单射（同族教训见 docs/learn/23）。
            //   对照 clang：TypePrinter::printQualifiedType 按 TypeClass 分派，
            //   指针的 const 走 printPointer 输出 "int *const"。
            if (innerType->isPointer()) {
                return (innerType->pointeeType ? innerType->pointeeType->toString() : "?")
                       + "* const";
            }
            if (innerType->isLValueReference()) {
                return (innerType->referencedType ? innerType->referencedType->toString() : "?")
                       + "& const";
            }
            if (innerType->isRValueReference()) {
                return (innerType->referencedType ? innerType->referencedType->toString() : "?")
                       + "&& const";
            }
            return "const " + innerType->toString();
        }
        case TypeKind::Class: {
            // 嵌套类型名印成"限定者::成员"（S<int>::type），与源码书写一致。必须与普通类名
            // 区分开 —— 否则 S<int>::type 会与类 "type" 同名（人读串不单射，见 docs/learn/23）。
            if (isNestedName()) {
                return (nestedQualifier ? nestedQualifier->toString() : "?")
                       + "::" + name;
            }
            // P3 / NTTP：模板 id（Box<int>、Buf<4>）带上实参打印——人读形态对齐源码书写
            // TemplateArg::toString 已按 kind 分派：类型实参取类型名，值实参取数字。
            if (templateArgs.empty()) return name;
            std::string s = name + "<";
            for (size_t i = 0; i < templateArgs.size(); i++) {
                if (i > 0) s += ", ";
                s += templateArgs[i].toString();
            }
            return s + ">";
        }
        case TypeKind::TemplateParam: return templateParamName;
        case TypeKind::Auto:   return "auto";
        case TypeKind::Decltype:
            // 未求值形态：打印成 decltype(...)，括号数按原文还原，便于日志观察
            return decltypeParen ? "decltype((...))" : "decltype(...)";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// stripReferences / stripConst：去除修饰，获取"裸类型"
// ─────────────────────────────────────────────────────────────────────────────
// 推导时常用：先剥掉实参/形参的引用与 const，再比较"裸类型"（对应 [temp.deduct.call]
// 对实参类型做的退化调整）。
// demo: stripReferences(RValueReference(Int)) ⇒ Int；stripReferences(Int) ⇒ nullptr
//       stripConst(Const(Int)) ⇒ Int；stripConst(Int) ⇒ nullptr
// （nullptr = "无可剥的修饰"，调用方据此回退到原类型。）
TypePtr Type::stripReferences() const {
    if (isLValueReference() || isRValueReference()) {
        return referencedType ? referencedType : nullptr;
    }
    // 非引用类型 ⇒ nullptr（调用方据此回退到原类型）
    return nullptr;
}

TypePtr Type::stripConst() const {
    if (isConst()) {
        return innerType ? innerType : nullptr;
    }
    return nullptr;
}

} // namespace minicc
