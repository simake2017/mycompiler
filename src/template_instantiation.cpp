// =============================================================================
// 阶段 4：模板实例化引擎实现（理论见 docs/learn/05、13、18、27）
// =============================================================================
// 实例化 = 结构化替换 [temp.subst]：深拷贝蓝图 AST，逐【类型位置】换实参；复合类型
//   （T*/T&/const T）由 substituteType 递归处理，不是文本替换。
// 蓝图写法 ⇒ 实例（MyPtr<int>，{T := int}）：
//   class MyPtr<T>   ⇒ 深拷贝类 AST ⇒ 新类 MyPtr_int ⇒ 符号 _Z5MyPtrIiE ⇒ 注册为真实类
//   T* data          ⇒ int* data              │ T& get() ⇒ int& get()
//   return N;（NTTP）⇒ return 4;（表达式位置在 cloneExpr 里替换）
//   Vec<T>（别名）    ⇒ 只解糖成 MyPtr<T>，零新类、零新符号（[temp.alias]/1）
//
//   管线位置：Sema 发现 MyPtr<int> 的使用（或推导出函数模板实参）后调用本模块；
//             产出的具体类/函数注册进全局表，供后续语义检查与 CodeGen 使用。
//   标准章节：[temp.inst] 隐式实例化的触发（用到才实例化）│ [temp.subst] 实参替换
//             [dcl.ref]/6 引用折叠 │ [temp.alias]/1 别名只解糖不实例化
//   对照 clang：SemaTemplateInstantiate.cpp ≈ instantiate*，TreeTransform.h ≈ cloneExpr/cloneStmt
// =============================================================================

#include "template_instantiation.h"
#include <format>
#include <iostream>

namespace minicc {

// ═════════════════════════════════════════════════════════════════════════════
// NameMangler 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 类型编码：类型树 → Itanium ABI mangling 片段（理论见 docs/learn/13）
// ─────────────────────────────────────────────────────────────────────────────
// 【编码表】v=void b=bool i=int d=double P=指针 R=左值引用 O=右值引用 K=const
//           类名 = <长度><名字>（如 7MyClass）；模板参数名原样输出（实例化后不应出现）
// demo: int* ⇒ Pi │ const int ⇒ Ki │ MyClass ⇒ 7MyClass
std::string NameMangler::encodeType(TypePtr type,
                                    const std::vector<std::string>& typeParams) {
    if (!type) return "v"; // void

    switch (type->kind) {
        case TypeKind::Void:   return "v";
        case TypeKind::Bool:   return "b";
        case TypeKind::Int:    return "i";
        case TypeKind::Double: return "d";
        case TypeKind::Pointer:
            return "P" + encodeType(type->pointeeType, typeParams);
        case TypeKind::LValueReference:
            return "R" + encodeType(type->referencedType, typeParams); // GCC ABI: R = lvalue ref
        case TypeKind::RValueReference:
            return "O" + encodeType(type->referencedType, typeParams); // GCC ABI: O = rvalue ref
        case TypeKind::Const:
            return "K" + encodeType(type->innerType, typeParams);      // GCC ABI: K = const
        case TypeKind::Class:
            return std::format("{}{}", type->name.size(), type->name);
        case TypeKind::TemplateParam:
            // ★ 仅在函数模板实例的签名里走这段（typeParams 非空）：
            //   形参编成 Itanium <template-param>，它【引用】模板实参表的第 n 项
            //   —— 序号 0 ⇒ T_，序号 1 ⇒ T0_，序号 2 ⇒ T1_（编码值比序号少一）。
            //   类模板路径传的是空表 ⇒ 落到下面原样输出，既有符号不变。
            for (size_t i = 0; i < typeParams.size(); i++) {
                if (typeParams[i] == type->templateParamName) {
                    return i == 0 ? std::string("T_")
                                  : std::format("T{}_", i - 1);
                }
            }
            return type->templateParamName;
        case TypeKind::Auto:
            return "Da"; // 不应该出现（auto 应该在阶段3已被消除）
        case TypeKind::Decltype:
            // ★ 不应该出现：decltype 必须已在替换阶段（Case 1.5）求值成具体类型。
            //   漏求值会产出形如 `_ZN…7decltype…` 的非法符号，链接期才炸、极难排查，
            //   故就地报出挡在编译期（clang 不会有此状态，Sema 层必已解析完毕）。
            std::cerr << std::format(
                "[内部错误] decltype 类型节点未在替换阶段求值就进入了 mangling：{}\n",
                type->toString());
            return "Dt";   // Itanium ABI 的 decltype 编码（正常路径不会走到）
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板实例化符号名（理论见 docs/learn/13）
// 格式: _Z + 模板名长度 + 模板名 + I + 参数编码... + E
// demo: MyPtr<int> ⇒ _Z5MyPtrIiE │ twice<int> ⇒ _Z5twiceIiE（类/函数模板实例共用）
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleTemplateInstance(
    const std::string& templateName,
    const std::vector<TemplateArg>& args) {

    std::string result = std::format("_Z{}{}I", templateName.size(), templateName);

    for (auto& arg : args) {
        // 按 [temp.arg] 的形态分派：类型实参 → 直接编码类型（Box<int> ⇒ …IiE）；
        // 非类型实参（NTTP）→ <expr-primary> L<类型编码><值>E（Buf<4> ⇒ …ILi4EE）。
        if (arg.isType()) {
            result += encodeType(arg.type);
        }
        else if (arg.isTemplate()) {
            // ── 模板模板实参 [temp.arg.template] ──
            // 实参是【模板名】：Itanium 按 <name> 直接编码（与类类型同形）。
            // demo: Wrap<Box, int> ⇒ _Z4WrapI3BoxiE
            // ★ 不能落到下面的 NTTP 分支 —— 那会把它编成 Li0E（值 0 的 expr-primary），
            //   产出一个"看着像模像样"却与 clang 完全不同的符号。
            // 对照 clang：ItaniumMangle 的 TemplateTemplateArg 走 mangleName 路径。
            result += encodeType(Type::makeClass(arg.templateName));
        }
        else {
            // <expr-primary>：L 开头 E 收尾，中间是「类型编码 + 值」。
            // ★ 负数按 Itanium 规则编成 n<绝对值>（-4 ⇒ "n4"）—— '-' 不是合法的
            //   mangling 字符。
            result += "L";
            // 按实参的【形态】编码：bool ⇒ b、int ⇒ i（缺失时按 int 兜底）
            result += arg.valueType ? encodeType(arg.valueType) : "i";
            result += (arg.value < 0)
                          ? std::format("n{}", -arg.value)
                          : std::to_string(arg.value);
            result += "E";                       // 收 <expr-primary>
        }
    }

    result += "E";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数模板实例符号名（[temp]，Itanium ABI §5.1.8 <bare-function-type>）
// 格式: _Z + 名 + I<模板实参>E + <返回类型> + <各参数类型>
// demo: template<class T> T twice(T x)      以 T=int 实例化 ⇒ _Z5twiceIiET_T_
//                                                             └┬┘ └┬┘
//                                                         返回 T_  参数 T_
//       template<class T> T pick(T a, int n)              ⇒ _Z4pickIiET_i
//       template<class T> void f(T a, T b)                ⇒ _Z1fIiEvT_T0_
// ★ 与类模板实例唯一的差别就是这个末尾段：函数模板的返回类型也是签名的一部分
//   （无法从名字反推），必须编进去；参数表同理。少了它，`template<class T>
//   T f(T)` 与 `template<class T> T f(T, int)` 在同一次实例化下会撞成同一符号。
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleFunctionTemplateInstance(
    const std::string& funcName,
    const std::vector<TemplateArg>& args,
    const TypePtr& returnType,
    const std::vector<Parameter>& params,
    const std::vector<std::string>& typeParams) {

    std::string result = mangleTemplateInstance(funcName, args);

    // <bare-function-type>：返回类型在前，各参数类型依次跟上。
    result += encodeType(returnType, typeParams);
    for (auto& param : params) {
        result += encodeType(param.type, typeParams);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数符号名（理论见 docs/learn/13）
// 格式: _Z + [N + 类名长度 + 类名] + 函数名长度 + 函数名 + 参数编码... [+ E]
// demo: MyClass::foo(int) ⇒ _ZN7MyClass3fooEi │ twice(int) ⇒ _Z5twicei
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
// RTTI 符号名（[class.rtti]，理论见 docs/learn/11）
// ─────────────────────────────────────────────────────────────────────────────
// 格式 _ZTI + 类名；demo: MyClass ⇒ _ZTI7MyClass
// ★ 命名空间类的名字带 "::"，直接拼进符号标签会让汇编器报 "junk at end of line"，
//   故统一净化（产生端与调用端都是本函数，天然一致）。⚠ 净化后 "N::S" 与 "N_S"
//   会撞名 —— 教学实现接受（见 docs/learn/26）。
static std::string symbolSafe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '$') ? c : '_';
    }
    return out;
}

std::string NameMangler::mangleRTTI(const std::string& className) {
    const std::string safe = symbolSafe(className);   // 先净化再算长度，自洽
    return std::format("_ZTI{}{}", safe.size(), safe);
}

// ─────────────────────────────────────────────────────────────────────────────
// vtable 符号名（理论见 docs/learn/11）
// ─────────────────────────────────────────────────────────────────────────────
// 格式 _ZTV + 类名（只有虚函数的类才生成）；demo: MyClass ⇒ _ZTV7MyClass
std::string NameMangler::mangleVTable(const std::string& className) {
    const std::string safe = symbolSafe(className);
    return std::format("_ZTV{}{}", safe.size(), safe);
}

// ═════════════════════════════════════════════════════════════════════════════
// TemplateInstantiator 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 类模板实例化 [temp.inst]（理论见 docs/learn/19、27）
// ─────────────────────────────────────────────────────────────────────────────
// 实例化 = 结构化替换：深拷贝蓝图 AST，逐【类型位置】替换；流程对应函数体内的
// 0 → 6 编号步骤（含 2a 撞名守卫 / 5.5 类内别名替换）。
// 实例名清洗：把实参的【人读串】洗成合法的汇编符号字符。
// ★ 必须尽量【单射】：'*' 与 '&' 各配一个字母（Box<int*>→Box_intP、Box<int&>→Box_intR），
//   否则两种不同实参洗出同一个符号。',' '<' '>' → '_' 仍非单射，
//   由调用点的撞名守卫兜底（宁可报错，也绝不产出重复符号）。
// 类模板实例与成员模板实例共用本函数。
// 对照 clang：Itanium ABI 的 <substitution> 是另一种思路（去重而非清洗），
//   本实现只求"符号合法且够用"，人读名与真正的编码（NameMangler）是两件事。
std::string sanitizeSymbolChars(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (c == ' ') continue;                       // 空格剔除
        switch (c) {
            case '*': out += 'P'; break;              // Pointer   Box<int*>  → Box_intP
            case '&': out += 'R'; break;              // Reference Box<int&>  → Box_intR
            case '-': out += 'N'; break;              // Negative  Buf<-3>    → Buf_N3
            case '+': out += 'A'; break;              // 目前进不来（实参只认整数字面量）
            case ',': case '<': case '>':
                out += '_'; break;
            default:  out += c;   break;
        }
    }
    return out;
}

ClassDeclPtr TemplateInstantiator::instantiate(
    TemplateDeclPtr templateDecl,
    const std::vector<TemplateArg>& args,
    const TypeSubstitution* substOverride) {

    // ── 0. 形参表选取 + 实参个数校验（[temp.arg.explicit]）──
    // ★ 必须用 templateParams（带 kind 的结构化表），不能用 typeParams（裸名字表）：
    //   后者把 `template<int N>` 的 N 也记成类型形参名，分不出哪一位是 NTTP。
    const auto& params = templateDecl->templateParams;

    // ── 两条路径：主模板 vs 特化（[temp.class.spec] / [temp.expl.spec]）──
    // 主模板：args[i] ↔ params[i] 逐位对应，按 kind 分派 + 校验。
    // 特化  ：替换表由 Sema 的偏特化匹配（TemplateDeducer::matchPattern）给出，与特化
    //         自己的形参表【不】逐位对应（Box<T*,T> 1 个形参 ↔ Box<double*,double>
    //         2 个实参），故直接采用外部传入的替换表，跳过逐位与个数校验。
    // args 在两条路径下都只用于「实例名生成 + mangling」，语义是使用点的实参。
    TypeSubstitution subst;
    if (substOverride) {
        subst = *substOverride;
        std::cout << std::format(
            "  [subst:map] (specialization) substitution supplied by pattern matching: ");
        if (subst.empty()) {
            // 全特化：形参表为空，没有需要绑定的未知量，替换表本就该是空的 ——
            // 特化体里的类型全是具体类型，逐字克隆即可。
            std::cout << "(none — fully concrete body, verbatim clone)";
        }
        for (auto& [k, v] : subst) std::cout << std::format("{} := {} ", k, v.toString());
        std::cout << "\n";
    }
    else {
        if (args.size() != params.size()) {
            throw std::runtime_error(std::format(
                "[Instantiate Error] template '{}' expects {} argument(s), got {}",
                templateDecl->templateName(),
                params.size(), args.size()));
        }

        // ── 1. 构建替换映射：按 kind 分派，类型实参进 type 槽，值实参进 value 槽 ──
        //    例: template<class T>      + Box<int> → { "T" → TemplateArg{Type,int} }
        //        template<int N>        + Buf<4>   → { "N" → TemplateArg{Integral,4} }
        //        template<class T, int N> + Pair<int,8>
        //                                      → { "T"→{Type,int}, "N"→{Integral,8} }
        //    混排天然可处理——因为分派依据是每一位形参自己的 kind，而非实参长相。
        for (size_t i = 0; i < params.size(); i++) {
            const TemplateParam& p = *params[i];

            // 形态自检（SemanticAnalyzer::checkTemplateArguments 已把过关，
            // 此处是 TemplateInstantiator 被直接调用时的兜底）
            if (p.kind == TemplateParamKind::Type && args[i].isValue()) {
                throw std::runtime_error(std::format(
                    "[Instantiate Error] template argument {} for '{}' ('{}') must be "
                    "a type, but '{}' is a value",
                    i + 1, templateDecl->templateName(), p.name, args[i].toString()));
            }
            if (p.kind == TemplateParamKind::NonType && args[i].isType()) {
                throw std::runtime_error(std::format(
                    "[Instantiate Error] template argument {} for '{}' ('{}') must be "
                    "a value, but '{}' is a type",
                    i + 1, templateDecl->templateName(), p.name, args[i].toString()));
            }

            subst[p.name] = args[i];
            std::cout << std::format(
                "  [subst:map] '{}' ({}) := {}\n", p.name,
                p.kind == TemplateParamKind::Type ? "type" : "non-type",
                args[i].toString());
        }
    }

    // ── 2. 生成实例化后的类名 ──
    // 实例名直接进汇编符号（方法名 <类名>_<方法>），必须剔除空格与 '<' '>' ',' 等
    // 非法字符；NTTP 值实参走同一套清洗（Buf<4> ⇒ Buf_4），天然区分不同实例。
    // ★ 这里只是「人读的实例名」，真正的符号编码在 NameMangler。
    //   清洗规则见 sanitizeSymbolChars（与成员模板实例共用同一份，避免两处漂移）。
    std::string instanceName = templateDecl->classTemplate->name;
    for (auto& arg : args) {
        instanceName += "_" + sanitizeSymbolChars(arg.toString());
    }

    // ── 2a. ★ 撞名守卫 ──
    // 清洗非单射，极端情况下两个不同实参表会洗出同一实例名 ⇒ 两条实例产出同一批汇编
    // 符号，而 as 只会报 `symbol 'X' is already defined`，指不到根因 —— 这里拦一道
    // 并说清谁跟谁撞了。无损键与 Sema 的 m_classInstanceCache 同构，键相同即"本来就
    // 是同一条实例"，放行（单元测试会重放同一条）。
    {
        std::string lossless = templateDecl->classTemplate->name + "<";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) lossless += ",";
            lossless += args[i].toString();
        }
        lossless += ">";

        auto [it, inserted] = m_instanceNameOwner.emplace(instanceName, lossless);
        if (!inserted && it->second != lossless) {
            throw std::runtime_error(std::format(
                "[Instantiate Error] 实例名撞车：'{}' 与 '{}' 清洗后都得到汇编符号名 "
                "'{}'。两者是不同类型，必须产出不同符号 —— 请给实例名清洗规则"
                "（template_instantiation.cpp 的 Step 2a）补上区分字符。",
                it->second, lossless, instanceName));
        }
        if (inserted) {
            std::cout << std::format(
                "  [instantiate:name] '{}' → 汇编符号前缀 '{}'\n", lossless, instanceName);
        }
    }

    // ── 2b. 蓝图摘要打印 ──
    // ★ 形参表必须由 templateParams 渲染（才能把 NTTP 打成 `int N` 而非 `typename N`）。
    // 特化路径额外打印命中版本（主模板/偏特化/全特化），是"三者择优"的观测点。
    const std::string kindTag =
        templateDecl->isExplicitSpec() ? " [EXPLICIT SPECIALIZATION]"
      : templateDecl->isPartialSpec()  ? " [PARTIAL SPECIALIZATION]"
                                       : " [primary template]";
    std::cout << std::format("\n  ╔══ Template Instantiation ═════════════════════════╗\n");
    std::cout << std::format("  ║ Blueprint: {} <{}>{}\n",
        templateDecl->classTemplate->name,
        params.empty() ? "?" :
            [&]() { std::string s; for (size_t i = 0; i < params.size(); i++) {
                if (i > 0) s += ", ";
                s += (params[i]->kind == TemplateParamKind::Type)
                         ? "typename " + params[i]->name
                     : (params[i]->kind == TemplateParamKind::NonType)
                         ? (params[i]->nonType ? params[i]->nonType->toString() : "?")
                               + " " + params[i]->name
                         : "template <...> class " + params[i]->name;
            } return s; }(),
        kindTag);
    if (templateDecl->isSpecialization()) {
        std::cout << std::format("  ║ Pattern:   {}<{}>\n",
            templateDecl->classTemplate->name,
            [&] { std::string s;
                  for (size_t i = 0; i < templateDecl->specPattern.size(); i++) {
                      if (i > 0) s += ", ";
                      // 类型位印类型名、值位印值（TemplateArg::toString 已分派）
                      s += templateDecl->specPattern[i].toString();
                  } return s; }());
    }
    std::cout << std::format("  ║ Instance:  {}\n", instanceName);

    // 打印替换映射
    std::cout << "  ║ Substitution map: { ";
    for (auto& [param, arg] : subst) {
        std::cout << std::format("'{}' → '{}', ", param, arg.toString());
    }
    std::cout << "}\n";

    // 3. 深拷贝类声明
    auto newClass = std::make_shared<ClassDecl>();
    newClass->name = instanceName;
    newClass->baseClassNames = templateDecl->classTemplate->baseClassNames;
    newClass->location = templateDecl->location;

    // 4. 克隆字段：demo: T* data ⇒ int* data（substituteType 递归进复合类型）
    std::cout << "  ║ ── Field Substitution ──\n";
    for (auto& field : templateDecl->classTemplate->fields) {
        std::cout << std::format("  ║   field '{}' : {} → ",
            field.name, field.type ? field.type->toString() : "?");
        auto cloned = cloneField(field, subst);
        std::cout << std::format("{}\n", cloned.type ? cloned.type->toString() : "?");
        newClass->fields.push_back(cloned);
    }

    // 5. 克隆方法：返回类型/形参/体内局部变量声明中的 T 全部替换，表达式走 cloneExpr
    std::cout << "  ║ ── Method Substitution ──\n";
    const std::string& blueprintName = templateDecl->classTemplate->name;
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

        auto cloned = cloneMethod(method, subst, instanceName);
        // ★ 构造/析构函数名绑定在类名上：蓝图里的 Box/~Box 必须随实例改名，否则符号
        //   与 CodeGen 发射的调用符号（类名_类名）对不上 ⇒ undefined reference。
        //   对照 clang：实例化时按新类名重建 CXXConstructorDecl 的 DeclName。
        if (cloned->kind == NodeKind::Constructor && cloned->name == blueprintName) {
            cloned->name = instanceName;
        }
        if (cloned->kind == NodeKind::Destructor && cloned->name == "~" + blueprintName) {
            cloned->name = "~" + instanceName;
        }
        newClass->methods.push_back(cloned);
    }

    // 5.5 克隆类内类型别名（using X = T; / typedef T X;，理论见 docs/learn/24）
    // ★ 每个实例必须自带"已替换"的别名表，否则 Box<int>::type 会拿到蓝图里的裸 T。
    for (const auto& aliasName : templateDecl->classTemplate->typeAliasOrder) {
        auto src = templateDecl->classTemplate->typeAliases.find(aliasName);
        if (src == templateDecl->classTemplate->typeAliases.end()) continue;
        TypePtr sub = substituteType(src->second, subst);
        newClass->typeAliases[aliasName] = sub;
        newClass->typeAliasOrder.push_back(aliasName);
        std::cout << std::format("  ║   alias '{}' : {} → {}\n",
            aliasName,
            src->second ? src->second->toString() : "?",
            sub ? sub->toString() : "?");
    }

    // 6. 生成 mangled name
    std::string mangledName = NameMangler::mangleTemplateInstance(
        templateDecl->classTemplate->name, args);

    std::cout << std::format("  ║ Mangled: {} → {}\n", instanceName, mangledName);
    std::cout << std::format("  ╚═══════════════════════════════════════════════════╝\n");

    m_instantiatedClasses.push_back(newClass);
    return newClass;
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数模板实例化 S5（理论见 docs/learn/05）
// ─────────────────────────────────────────────────────────────────────────────
// 推导（S2~S4）给出 typeArgs 后：建替换表 → 复用 cloneMethod 深拷贝蓝图函数
//   → 生成 mangled 符号 → 交回 Sema 注册并用具体类型分析函数体（两阶段查找第二阶段）。
// demo: twice(3) ⇒ T := int ⇒ void twice(T x){...} 的每个 T 换成 int
//                   ⇒ 符号 _Z5twiceIiE ⇒ 进 m_instantiatedFunctions
// ─────────────────────────────────────────────────────────────────────────────
FuncDeclPtr TemplateInstantiator::instantiateFunction(
    TemplateDeclPtr templateDecl,
    const std::vector<TypePtr>& typeArgs,
    const std::string& ownerClassName) {

    // 推导引擎 S2~S4 只产出裸 TypePtr（函数模板的 NTTP 尚未实现），此处统一包成
    // TemplateArg::ofType 放进同一张替换表 —— 表本身类型/值两形态通用。
    TypeSubstitution subst;
    for (size_t i = 0; i < templateDecl->typeParams.size()
         && i < typeArgs.size(); i++) {
        subst[templateDecl->typeParams[i]] = TemplateArg::ofType(typeArgs[i]);
    }

    auto& blueprint = templateDecl->funcTemplate;

    // mangler 收 tagged 实参表，而这条路径手上只有裸类型，故再包一遍。
    std::vector<TemplateArg> targs;
    targs.reserve(typeArgs.size());
    for (auto& a : typeArgs) targs.push_back(TemplateArg::ofType(a));

    // ★ 函数模板实例必须编出完整的 <bare-function-type>（返回类型 + 参数表），
    //   不能只编模板实参 —— 那是类模板的编法（docs/BUGS.md B2）。
    //   这里用的是蓝图（尚未替换）的返回类型与参数表：形参在签名里保留为
    //   T_ / T0_… 形态，与 clang 的编法一致。
    //   demo: `template<class T> T twice(T x);` 蓝图签名 = T→T ⇒ 实例 twice<int>
    //         编出 _Z5twiceIiET_T_（_Z + 5twice + I i E 模板实参 + T_ 返回类型 + T_ 形参表）
    //   成员模板（[temp.mem]）另走一套：符号是 `类名_方法名` + 实参后缀 —— 与普通
    //   成员函数同款前缀，使 CodeGen 既有的调用约定（`Cls_method`）能对上，
    //   而不同实参的实例又靠 `_int` / `_double` 后缀区分开（自由函数模板的
    //   Itanium 名里已含实参，无此需要）。
    std::string mangled;
    if (ownerClassName.empty()) {
        mangled = NameMangler::mangleFunctionTemplateInstance(
            blueprint->name, targs,
            blueprint->returnType, blueprint->parameters,
            templateDecl->typeParams);
    } else {
        mangled = sanitizeSymbolChars(ownerClassName + "_" + blueprint->name);
        for (auto& a : targs) mangled += "_" + sanitizeSymbolChars(a.toString());
    }

    std::cout << std::format("\n  ╔══ Function Template Instantiation (S5) ═══════╗\n");
    std::cout << std::format("  ║ Blueprint: {} <{}>\n",
        blueprint->name,
        [&]() { std::string s;
            for (size_t i = 0; i < templateDecl->typeParams.size(); i++) {
                if (i > 0) s += ", ";
                s += templateDecl->typeParams[i];
            }
            return s.empty() ? "?" : s; }());
    std::cout << "  ║ Substitution: { ";
    for (auto& [p, t] : subst) {
        std::cout << std::format("{} → {}, ", p, t.toString());
    }
    std::cout << "}\n";

    // 复用方法克隆（ownerClassName = "" 表示自由函数；成员模板传所属类名，
    // 使实例带隐式 this —— CodeGen 与 Sema 都靠 ownerClassName 判"有没有 this"）
    FuncDeclPtr instance = cloneMethod(blueprint, subst, ownerClassName);
    instance->mangledName = mangled;

    std::cout << std::format("  ║ Symbol: {} → {}\n", blueprint->name, mangled);
    std::cout << std::format("  ╚═══════════════════════════════════════════════╝\n");

    m_instantiatedFunctions.push_back(instance);
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// 类型替换 substituteType —— [temp.subst] 的核心（理论见 docs/learn/02、05、27）
// ─────────────────────────────────────────────────────────────────────────────
// 深度优先遍历类型树：按节点种类分派，未命中或非依赖则原样返回。
//   扫到哪种类型节点 ⇒ 走哪个 Case ⇒ 结果（以 {T := int} 为例）：
//     TemplateParam("T") ⇒ Case 1   ⇒ int（不在表中则原样保留，如外层模板的形参）
//     decltype(e)        ⇒ Case 1.5 ⇒ 替换 e 后求值（延迟求值的兑现时刻）
//     T*                 ⇒ Case 2   ⇒ int*（pointee 无变化则复用原节点）
//     T&                 ⇒ Case 3   ⇒ int&；配 {T := int&} ⇒ int& & 折叠为 int&
//     T&&                ⇒ Case 4   ⇒ int&&；配 {T := int&} ⇒ int& && 折叠为 int&
//     const T            ⇒ Case 5   ⇒ const int
//     typename T::type   ⇒ Case 5.5 ⇒ 查类成员别名（查不到 ⇒ 软失败）
//     MyPtr<T>           ⇒ Case 5.2 ⇒ MyPtr<int>（实参位替换）
//     X<int>（别名 id）   ⇒ Case 5.8 ⇒ 解糖（展开失败 ⇒ 软失败）
//     Class("T")         ⇒ Case 6   ⇒ int（parseType 把裸 T 建成了 Class 节点）
//
// ★ 引用折叠 [dcl.ref]/6：T& & / T& && / T&& & ⇒ T&，T&& && ⇒ T&& —— 只要有一层
//   左值引用，结果就是左值引用。这是万能引用（T&& 形参）的根基：
//     foo(42)  ⇒ T = int,  T&& = int&&
//     foo(var) ⇒ T = int&, T&& = int& && ⇒ int&（折叠）
//   ★ 折叠只在 Type::makeLValueReference 里实现一份，调用点不得自行判断。
TypePtr TemplateInstantiator::substituteType(
    TypePtr type, const TypeSubstitution& subst) {

    if (!type) return nullptr;

    // ── Case 1: 模板参数 → 直接替换 ──
    // demo: T 配 {T := int} ⇒ int；不在表中则原样保留（如外层模板的形参）
    if (type->isTemplateParam()) {
        auto it = subst.find(type->templateParamName);
        if (it != subst.end()) {
            // ★ 类型位置只接受【类型】实参：命中值实参说明 NTTP 名被当类型用了，
            //   必须报硬错误而非把值硬塞成类型
            //   （clang: err_nontype_template_parameter_used_as_type）。
            if (it->second.isValue()) {
                Sfinae::fail(std::format(
                    "[Subst Error] non-type template parameter '{}' is used as a "
                    "type (its value is '{}')",
                    type->templateParamName, it->second.toString()));
            }
            std::cout << std::format("    [subst] ★ TemplateParam '{}' → '{}' (direct replacement)\n",
                type->templateParamName, it->second.type->toString());
            return it->second.type;
        }
        std::cout << std::format("    [subst] TemplateParam '{}' not in substitution map, keep as-is\n",
            type->templateParamName);
        return type;
    }

    // ── Case 1.5: decltype(expr) —— 延迟求值的兑现时刻（理论见 docs/learn/20）──
    // Parser 只留下"表达式 + 是否加括号"（那时 T 未知），替换阶段拿到实参才能求值：
    //   ① 先把实参替换进操作数表达式  ② 交给 DecltypeEvaluator 求值成具体类型
    // demo: template<class T> struct is_range<T, void_t<decltype(declval<T>().begin())>>
    //       T := vector<int> ⇒ ① declval<vector<int>>().begin() ② 求值 ⇒ int ✓
    //       T := int         ⇒ ② 求值失败 ⇒ 抛 SubstitutionFailure ⇒ 偏特化不匹配
    //                          ⇒ 回退主模板 false_type —— 这一抛一捕就是 SFINAE 全貌
    // 对照 clang：TreeTransform::TransformDecltypeType → BuildDecltypeType 重新求值。

    //template <typename T>  这里 decltype 是一个 class type
    // struct is_range<T, std::void_t<decltype(std::declval<T>().begin()),
    //                                decltype(std::declval<T>().end())>>
    //     : public std::true_type {};
    if (type->isDecltype()) {
        std::cout << "    [subst] decltype: 先替换操作数，再求值（两段式的第二段）\n";

        // ① 操作数表达式做替换
        ExprPtr concrete = cloneExpr(type->decltypeExpr, subst);

        // ② 求值
        if (!m_decltypeEval) {
            // 单元测试路径：没有挂求值器 → 原样保留（保持"延迟"可见）
            std::cout << "    [subst] decltype: no evaluator attached — stays deferred\n";
            return Type::makeDecltype(concrete, type->decltypeParen);
        }

        // 求值失败会抛 SubstitutionFailure —— 让它向上传播：
        // 那正是 SFINAE 的信号，由 matchPattern / 重载决议负责捕获。
        TypePtr result = m_decltypeEval->evaluateDecltype(concrete, type->decltypeParen);
        std::cout << std::format("    [subst] ★ decltype 求值完成 → {}\n",
            result ? result->toString() : "?");
        return result;
    }

    // ── Case 2: 指针 T* → 递归替换 pointee ──
    // demo: T* 配 {T := int} ⇒ int*；pointee 无变化则复用原节点（结构共享）
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
    // demo: T& 配 {T := int} ⇒ int&；配 {T := int&} ⇒ int& & 折叠为 int&
    if (type->isLValueReference() && type->referencedType) {
        std::cout << std::format("    [subst] LValueRef({}&) → recursing into referenced type...\n",
            type->referencedType->toString());
        TypePtr newInner = substituteType(type->referencedType, subst);

        // ★ 内层若已是引用，结果必须【重新构造】，不能直接把 newInner 返回：
        //   内层是右值引用时原样返回会得到 `int&&`，而 [dcl.ref]/6 要求 `int&`
        //   （"& 赢"只写在 Type::makeLValueReference 里，调用点不得自行判断）。
        if (newInner->isReference()) {
            TypePtr folded = Type::makeLValueReference(newInner);
            std::cout << std::format("    [subst] ★ Reference collapsing: {}& → {} (& wins)\n",
                newInner->toString(), folded->toString());
            return folded;
        }

        if (newInner != type->referencedType) {
            auto result = Type::makeLValueReference(newInner);
            std::cout << std::format("    [subst] ★ LValueRef substituted: {}& → {}&\n",
                type->referencedType->toString(), newInner->toString());
            return result;
        }
    }

    // ── Case 4: 右值引用 T&& → 递归替换 + 引用折叠 ──
    // ★ 万能引用（forwarding reference）的关键路径：
    //   T = int ⇒ int&& │ T = int& ⇒ int& && ⇒ int& │ T = int&& ⇒ int&& && ⇒ int&&
    // demo: T&& 配 {T := int&} ⇒ int& && ⇒ int&（左值引用赢，规则见 Case 3）
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

    // ── Case 5: const T → 递归替换 inner ──
    // demo: const T 配 {T := int} ⇒ const int
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

    // ── Case 5.5: 依赖类型名 typename T::type（[temp.res]/5，理论见 docs/learn/25）──
    // 限定者（T）要等替换才知道，成员名（type）是它里面的类型别名：
    //   Class{"type", nestedQualifier=TemplateParam("T")} → 替换限定者后查别名表。
    // ★ 失败必须【当场】抛：替换期正是 [temp.deduct]/8 的直接上下文，查不到即软失败
    //   被 Sfinae::attempt 吸收 —— void_t<typename T::type> 探测惯例全靠这一条。
    // 限定者替换后仍是形参（多层模板的内层）⇒ 原样返回留给外层替换（同 Case 1）。
    // 对照 clang：TransformDependentNameType → Sema::SubstType 重做限定名查找。
    if (type->isClass() && type->isNestedName() && type->nestedQualifier) {
        std::cout << std::format("    [subst] 依赖限定名 {}::{} → 先替换限定者...\n",
            type->nestedQualifier->toString(), type->name);
        TypePtr newQual = substituteType(type->nestedQualifier, subst);

        if (newQual && newQual->isTemplateParam()) {
            std::cout << std::format(
                "    [subst] 限定者 '{}' 仍是模板形参（外层模板的）⇒ 保持依赖\n",
                newQual->templateParamName);
            return type;
        }

        if (m_memberResolver) {
            // 查不到 / 限定者不是类 ⇒ resolveMemberType 抛 SubstitutionFailure
            TypePtr member = m_memberResolver->resolveMemberType(newQual, type->name);
            std::cout << std::format("    [subst] ★ {}::{} ⇒ {}\n",
                newQual ? newQual->toString() : "?", type->name,
                member ? member->toString() : "?");
            return member;
        }

        // 单元测试路径（没挂 resolver）：替换限定者后原样保留，保持"延迟"可见
        if (newQual != type->nestedQualifier) {
            auto result = Type::makeClass(type->name);
            result->nestedQualifier = newQual;
            std::cout << std::format("    [subst] 无成员查表器 ⇒ 保持延迟: {}\n",
                result->toString());
            return result;
        }
        return type;
    }

    // ── Case 5.2: 类模板 id 的实参替换（先于别名解糖）──
    // 【为什么必需】Case 1/6 只按【名字】查表，而 `MyPtr<T>` 的依赖藏在实参里 ——
    //   不替换就会拿形参 T 去实例化，造出假实例 MyPtr_T，方法返回类型跟着错。
    // demo: substituteType(MyPtr<T>, {T := int}) ⇒ MyPtr<int>
    //       substituteType(MyPtr<T>, {})          ⇒ 原样返回（保持依赖）
    // ★ 必须先做这一步再做别名解糖：别名 Vec<T> 的实参先变成 int，展开出的
    //   MyPtr<int> 才是具体的；顺序反了就会拿 T 去实例化。
    // 对照 clang：TreeTransform::TransformTemplateSpecializationType 逐项 Transform。
    TypePtr idNode = type;
    if (type->isClass() && !type->templateArgs.empty()) {
        std::vector<TemplateArg> newArgs;
        newArgs.reserve(type->templateArgs.size());
        bool changed = false;
        for (const auto& arg : type->templateArgs) {
            if (arg.isType() && arg.type) {
                // ── 实参位的"裸名字"其实是 NTTP 名 ⇒ 还原成值实参 ──
                // Parser 的实参分流只看 Token 形态（整数字面量 → 值，其余 → 类型），
                // 于是 `Buf<N>` 的 N（非类型形参名）被建成 Class("N") 类型节点。
                // ★ 替换表是唯一权威：N 在表里绑的是【值】⇒ 这里的裸 N 只能是值实参。
                // demo: template<int N> using BufA = Buf<N>; BufA<4> ⇒ Buf<4> ✓
                // 对照 clang：Parser 维护 TemplateParameterScope，无此还原步骤。
                auto sit = (arg.type->isClass() && arg.type->templateArgs.empty() &&
                            !arg.type->isNestedName())
                               ? subst.find(arg.type->name)
                               : subst.end();
                if (sit != subst.end() && sit->second.isValue()) {
                    std::cout << std::format(
                        "    [subst] ★ 实参 '{}' 在替换表里绑定的是值 ⇒ 还原为 NTTP 值实参 {}\n",
                        arg.type->name, sit->second.toString());
                    newArgs.push_back(sit->second);
                    changed = true;
                    continue;
                }

                TypePtr na = substituteType(arg.type, subst);
                if (na != arg.type) changed = true;
                newArgs.push_back(TemplateArg::ofType(na));
            } else {
                newArgs.push_back(arg);   // NTTP 值实参：值不随类型替换而变
            }
        }
        if (changed) {
            auto rebuilt = Type::makeClass(type->name);
            rebuilt->templateArgs    = newArgs;
            rebuilt->nestedQualifier = type->nestedQualifier;
            std::cout << std::format("    [subst] ★ 模板 id 实参替换: {} → {}\n",
                type->toString(), rebuilt->toString());
            idNode = rebuilt;
        }
    }

    // ── Case 5.3: 模板模板形参的使用点 `C<T>`（[temp.param]/4）──
    // 形参 C 绑的是【一个模板】而不是类型：把 C 换成被传进来的模板名，
    //   `C<T>` 于是成为 `Box<T>`（实参已在 Case 5.2 换过），交由外部的落地路径兑现。
    // ★ 与 Case 6（裸形参名 → 类型）的分工：这里换的是【模板 id 的名字段】，
    //   换完仍是"待实例化的半成品"，不是具体类型 —— 与模板体内的 `Box<T> inner;`
    //   走同一条兑现路径（所以不需要在这里调 resolveType，也不应该调：
    //   这一层不认识 Sema，硬调会引入双向依赖）。
    // 对照 clang：TreeTransform::TransformTemplateSpecializationType 里
    //   TemplateTemplateParmDecl 的替换发生在 TransformTemplateName。
    if (idNode->isClass() && !idNode->templateArgs.empty()) {
        auto it = subst.find(idNode->name);
        if (it != subst.end() && it->second.isTemplate()) {
            auto rebuilt = Type::makeClass(it->second.templateName);
            rebuilt->templateArgs    = idNode->templateArgs;
            rebuilt->nestedQualifier = idNode->nestedQualifier;
            std::cout << std::format(
                "    [subst] ★ 模板模板形参 '{}' → 模板 '{}'：{} → {}\n",
                idNode->name, it->second.templateName,
                idNode->toString(), rebuilt->toString());
            idNode = rebuilt;
        }
    }

    // ── Case 5.8: 别名模板 id X<int> / X<T>（[temp.alias]/1，理论见 docs/learn/27）──
    // X 是 `template<class U> using X = ...;` 声明的【别名】：不产生新类型、只产生
    // 新名字 —— 故这里是"解糖"（desugar）而非"实例化"，展开后不留痕迹。
    // ★ 必须在替换期展开并【当场】判成败：展开失败（如 enable_if 条件为假）要抛软
    //   失败让 Sfinae::attempt 吸收成"该候选被剔除"，拖到 Sema 解析期就成硬错误。
    // 与 Case 5.5 的区别：5.5 管 `typename T::type`（成员别名），本支管顶层 `X<T>`。
    // 对照 clang：Sema::SubstType 判 isTypeAlias() → CheckAliasTemplateId 取 sugar。
    if (idNode->isClass() && !idNode->templateArgs.empty() && m_aliasResolver) {
        std::vector<TemplateArg> substArgs;
        substArgs.reserve(idNode->templateArgs.size());
        for (const auto& arg : idNode->templateArgs)
            substArgs.push_back(arg.isType() && arg.type
                                    ? TemplateArg::ofType(substituteType(arg.type, subst))
                                    : arg);
        TypePtr expanded = m_aliasResolver->expandAliasTemplate(idNode->name, substArgs);
        if (expanded) {
            std::cout << std::format(
                "    [subst] ★ 别名模板 {}<...> 解糖 ⇒ {}\n",
                idNode->name, expanded->toString());
            return expanded;
        }
    }

    // 重建过模板 id（名字不是别名、实参已替换）⇒ 返回重建结果
    if (idNode != type) return idNode;

    // ── Case 6: 类类型节点里裸着的形参名（简化处理）──
    // demo: Class("T") 配 {T := int} ⇒ int（parseType 把裸 T 建成 Class 节点，故按名兜底）
    if (type->isClass()) {
        auto it = subst.find(type->name);
        if (it != subst.end() && it->second.isType()) {
            std::cout << std::format("    [subst] ★ Class '{}' matches template param → '{}'\n",
                type->name, it->second.type->toString());
            return it->second.type;
        }
        // 命中但值是 Integral：同样说明 NTTP 名被当类型用了（见 Case 1 的说明）。
        if (it != subst.end()) {
            // 同 Case 1：立即上下文内的替换失败 → 可恢复，发软失败信号
            Sfinae::fail(std::format(
                "[Subst Error] non-type template parameter '{}' is used as a type "
                "(its value is '{}')",
                type->name, it->second.toString()));
        }
    }

    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆字段
// ─────────────────────────────────────────────────────────────────────────────
// 类型过 substituteType，其余元信息（offset/size/access）原样复制。
// demo: MyPtr<int> 的 "data : T*" ⇒ "data : int*"
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
// 克隆方法 —— 声明重建，对应 clang TreeTransform
// ─────────────────────────────────────────────────────────────────────────────
// 返回类型、形参类型、函数体三处的 T 全部替换；ownerClassName 指向实例类名
// （自由函数传 ""，见 instantiateFunction）。克隆不携带旧的语义分析结果 —— 实例方法
// 交回 Sema 用具体类型重新检查（两阶段查找的第二阶段）。
FuncDeclPtr TemplateInstantiator::cloneMethod(
    FuncDeclPtr method, const TypeSubstitution& subst,
    const std::string& newClassName) {

    // ★ 必须按源节点种类重建节点类型：构造函数 → ConstructorDecl（保住 initList/
    //   isDefaultCtor），析构 → DestructorDecl（保住 isDefaultDtor）。一律建
    //   FunctionDecl 会让合成构造/析构失效，CodeGen 的 Class_Class 调用链接不到符号。
    //   对照 clang：TreeTransform 按 DeclKind 分派 TransformConstructorDecl/DestructorDecl。
    FuncDeclPtr newMethod;
    // 按种类分派（[class.copy.ctor]/[class.dtor]）：构造带初始化列表，析构带
    // isDefaultDtor 标志，普通方法走空壳。
    if (method->kind == NodeKind::Constructor) {
        auto ctor = std::static_pointer_cast<ConstructorDecl>(method);
        auto clonedCtor = std::make_shared<ConstructorDecl>();
        clonedCtor->isDefaultCtor = ctor->isDefaultCtor;
        // 初始化列表：成员名原样保留，实参表达式递归克隆
        for (auto& init : ctor->initList) {
            CtorInitializer clonedInit;
            clonedInit.memberName = init.memberName;
            clonedInit.location = init.location;
            for (auto& arg : init.arguments) {
                clonedInit.arguments.push_back(cloneExpr(arg, subst));
            }
            clonedCtor->initList.push_back(clonedInit);
        }
        newMethod = clonedCtor;
    } else if (method->kind == NodeKind::Destructor) {
        auto dtor = std::static_pointer_cast<DestructorDecl>(method);
        auto clonedDtor = std::make_shared<DestructorDecl>();
        clonedDtor->isDefaultDtor = dtor->isDefaultDtor;
        newMethod = clonedDtor;
    } else {
        newMethod = std::make_shared<FunctionDecl>();
    }
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
// 克隆表达式 —— 深拷贝结构，对应 clang TreeTransform::TransformExpr
// ─────────────────────────────────────────────────────────────────────────────
// 本项目表达式节点不存类型注解，实例的类型在语义分析中重新推出，故这里只递归克隆结构。
// 【覆盖率】ast.h 的 14 种 Expression 派生类已全覆盖：Int/Bool/String/Nullptr 字面量、
//   Var、Binary、Unary、Call、Member、Index、New、DynamicCast、This、Delete（漏一种
//   则 default 返回 nullptr，-Wswitch 会提醒补 case）。
ExprPtr TemplateInstantiator::cloneExpr(
    ExprPtr expr, const TypeSubstitution& subst) {

    if (!expr) return nullptr;

    // ★ 分派判据（详见 docs/learn/29）：clone* 是【取值型】递归，每个分支都要返回新建
    //   节点（ExprPtr/StmtPtr），而 AstVisitor::visit 返回 void ⇒ 走 NodeKind 标签分派。
    //   static_cast 安全：节点 kind 由构造函数设定，恒等于自身类型。
    switch (expr->kind) {
        case NodeKind::IntLiteral: {
            auto& e = static_cast<IntLiteralExpr&>(*expr);
            auto cloned = std::make_shared<IntLiteralExpr>(e.value);
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::BoolLiteral: {
            auto& e = static_cast<BoolLiteralExpr&>(*expr);
            auto cloned = std::make_shared<BoolLiteralExpr>(e.value);
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::StringLiteral: {
            auto& e = static_cast<StringLiteralExpr&>(*expr);
            auto cloned = std::make_shared<StringLiteralExpr>(e.value);
            cloned->location = e.location;
            return cloned;
        }

        // ── 变量引用 ──
        // ★ NTTP 的值替换发生在【表达式位置】（类型位置才归 substituteType）：模板体里
        //   裸的 NTTP 形参名由 Parser 建成 VarExpr，在这里换成整数字面量。
        // 扫到的 VarExpr ⇒ 结果：
        //   VarExpr{"N"} 替换表里绑的是【值】  ⇒ IntLiteralExpr{4}
        //       （template<int N> class Buf { int size() { return N; } } 的 Buf<4>
        //         ⇒ 实例方法体变成 `return 4;`，Sema 与 CodeGen 只见到常量）
        //   VarExpr{"x"} 绑的是类型或未命中 ⇒ 克隆名字本身
        // ★ explicitTemplateArgs 必须一并替换：declval<T>() 的 T 挂在 VarExpr{declval,[T]}
        //   上，只克隆名字会让 decltype 求值阶段看到裸 declval()，T 凭空丢失 ⇒ 探测链断裂。
        // 对照 clang：TreeTransform::TransformDeclRefExpr 换成已求值的 TemplateArgument
        //   （真 C++ 还要过 CheckTemplateArgument 做常量折叠，如 Buf<2+2> 折成 4，见 ROADMAP D）
        // ⚠ 已知边界：只按"名字命中 NTTP 形参名"判定，未做作用域检查 —— 模板体内同名
        //   局部变量遮蔽 NTTP 时会被误伤（clang 靠 DeclRefExpr 绑定的 ValueDecl* 区分）。
        case NodeKind::Var: {
            auto& e = static_cast<VarExpr&>(*expr);
            auto it = subst.find(e.name);
            if (it != subst.end() && it->second.isValue()) {
                std::cout << std::format(
                    "    [subst] ★ NTTP value substitution: VarExpr '{}' → IntLiteral {}\n",
                    e.name, it->second.value);
                auto lit = std::make_shared<IntLiteralExpr>(it->second.value);
                lit->location = e.location;
                return lit;
            }
            // 未命中，或命中类型形参（那是类型名不是变量）—— 克隆名字本身。
            // ★ 但 explicitTemplateArgs 必须一并替换：declval<T>() 的 T 挂在
            //   VarExpr{declval, [T]} 上，只克隆名字会让 decltype 求值阶段看到裸
            //   declval()，T 凭空丢失 ⇒ 探测链断裂。
            auto cloned = std::make_shared<VarExpr>(e.name);
            cloned->location = e.location;
            for (const auto& ta : e.explicitTemplateArgs) {
                if (ta.isType() && ta.type) {
                    cloned->explicitTemplateArgs.push_back(
                        TemplateArg::ofType(substituteType(ta.type, subst)));
                } else {
                    cloned->explicitTemplateArgs.push_back(ta);   // 值实参原样带过
                }
            }
            return cloned;
        }

        case NodeKind::Binary: {
            auto& e = static_cast<BinaryExpr&>(*expr);
            auto cloned = std::make_shared<BinaryExpr>(
                e.op, cloneExpr(e.left, subst), cloneExpr(e.right, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::Unary: {
            auto& e = static_cast<UnaryExpr&>(*expr);
            auto cloned = std::make_shared<UnaryExpr>(
                e.op, cloneExpr(e.operand, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::Call: {
            auto& e = static_cast<CallExpr&>(*expr);
            std::vector<ExprPtr> clonedArgs;
            for (auto& arg : e.arguments) {
                clonedArgs.push_back(cloneExpr(arg, subst));
            }
            auto cloned = std::make_shared<CallExpr>(
                cloneExpr(e.callee, subst), std::move(clonedArgs));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::Member: {
            auto& e = static_cast<MemberExpr&>(*expr);
            auto cloned = std::make_shared<MemberExpr>(
                cloneExpr(e.object, subst), e.memberName, e.isArrow);
            cloned->location = e.location;
            return cloned;
        }

        // 下标访问（容器封装的糖：模板体内 v[i] 也能实例化到具体类）
        case NodeKind::Index: {
            auto& e = static_cast<IndexExpr&>(*expr);
            auto cloned = std::make_shared<IndexExpr>(
                cloneExpr(e.object, subst), cloneExpr(e.index, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::This: {
            auto& e = static_cast<ThisExpr&>(*expr);
            auto cloned = std::make_shared<ThisExpr>();
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::New: {
            auto& e = static_cast<NewExpr&>(*expr);
            auto cloned = std::make_shared<NewExpr>(e.className);
            cloned->location = e.location;
            for (auto& arg : e.constructorArgs) {
                cloned->constructorArgs.push_back(cloneExpr(arg, subst));
            }
            return cloned;
        }

        // dynamic_cast：类名是字面名（非模板参数），原样克隆即可
        case NodeKind::DynamicCast: {
            auto& e = static_cast<DynamicCastExpr&>(*expr);
            auto cloned = std::make_shared<DynamicCastExpr>(
                e.targetClassName, cloneExpr(e.operand, subst));
            cloned->location = e.location;
            return cloned;
        }

        case NodeKind::NullptrLiteral: {
            auto& e = static_cast<NullptrLiteralExpr&>(*expr);
            auto cloned = std::make_shared<NullptrLiteralExpr>();
            cloned->location = e.location;
            return cloned;
        }

        // ── delete 表达式（ast.h 的 DeleteExpr 节点）──
        // 表达式位置的 `delete p`/`delete[] p`（如 `return delete p, 0;` 或作为子表达式）。
        // pointerExpr 递归克隆，isArray 原样带上。
        // ★ 必须深拷贝：蓝图 ExprPtr 被多实例共享时，任一实例改写都会污染其它实例。
        // demo: "delete data;" 配 {T := int} ⇒ "delete data;"（data 的 T*→int* 由 Sema 推出）
        case NodeKind::Delete: {
            auto& e = static_cast<DeleteExpr&>(*expr);
            auto cloned = std::make_shared<DeleteExpr>(
                cloneExpr(e.pointerExpr, subst), e.isArray);
            cloned->location = e.location;
            return cloned;
        }

        // 不会到达。留 default 是为了新增节点却忘了补 case 时由 -Wswitch 报出。
        default:
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆语句 —— 深拷贝，对应 clang TreeTransform::TransformStmt
// ─────────────────────────────────────────────────────────────────────────────
// ★ 必须深拷贝：蓝图 ClassDecl 全局只有一份，Box<int> 与 Box<double> 必须各持独立
//   AST 子树；浅拷贝会让后实例的替换覆盖掉先实例的语句（同一 StmtPtr 被两边共享）。
// ★ 替换只发生在【类型位置】：subst 是 {形参名 → 具体类型}，语句里的局部名
//   （item/ptr）原样保留，与模板参数无关。
// 【覆盖率】ast.h 的 8 种 Statement 已全覆盖：VarDecl/Expr/Assign/Return/If/While/
//   Block/DeleteStmt（漏一种则 default 返回 nullptr，该语句被静默丢弃）。
// demo: 蓝图 Box<T> 体配 {"T" → int} ⇒ "T item = x;" 变成 "int item = x;"
//                                    ⇒ "{ T a; T b; }" 变成 "{ int a; int b; }"
StmtPtr TemplateInstantiator::cloneStmt(
    StmtPtr stmt, const TypeSubstitution& subst) {

    if (!stmt) return nullptr;

    // ── 变量声明（本函数唯一直接碰类型的分支）──
    // declaredType 过 substituteType，initializer 过 cloneExpr（初始化式里可能嵌 new T(...)）。
    // demo: "T item = x;" 配 {T := int} ⇒ "int item = x;"
    // demo: "T* p = new T();" 配 {T := int} ⇒ "int* p = new int();"
    //       （declaredType 是 Pointer(T)，substituteType 会递归进 pointeeType）

    // 同 cloneExpr：取值型递归 ⇒ NodeKind 标签分派。
    switch (stmt->kind) {
        case NodeKind::VarDecl: {
            auto& s = static_cast<VarDeclStmt&>(*stmt);
            auto cloned = std::make_shared<VarDeclStmt>(
                s.name,
                substituteType(s.declaredType, subst),
                cloneExpr(s.initializer, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── 表达式语句 ──
        // 以分号结尾的裸表达式（f(x); / new T();）。本节点不含类型，替换全靠 cloneExpr。
        case NodeKind::ExprStmt: {
            auto& s = static_cast<ExprStmt&>(*stmt);
            auto cloned = std::make_shared<ExprStmt>(cloneExpr(s.expr, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── 赋值语句（C++ 里赋值是表达式 [expr.ass]，本项目简化为语句，ast.h 的 AssignStmt）──
        // target/value 两侧都过 cloneExpr；变量名不变，故通常无替换发生。
        // demo: "item = x;" ⇒ "item = x;"（结构深拷贝，内容不变）
        case NodeKind::Assign: {
            auto& s = static_cast<AssignStmt&>(*stmt);
            auto cloned = std::make_shared<AssignStmt>(
                cloneExpr(s.target, subst),
                cloneExpr(s.value, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── return 语句 ──
        // 返回值表达式过 cloneExpr；返回类型本身在 cloneMethod 里替换，不在这里。
        // demo: "return item;" ⇒ "return item;"
        case NodeKind::Return: {
            auto& s = static_cast<ReturnStmt&>(*stmt);
            auto cloned = std::make_shared<ReturnStmt>(
                cloneExpr(s.value, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── if 语句 ──
        // 两个递归点：condition 走 cloneExpr，then/else 分支走 cloneStmt（自递归）；
        // elseBranch 可空，需判空。
        // demo: "if (n > 0) { T a = n; } else { T a = 0; }"
        //    ⇒  "if (n > 0) { int a = n; } else { int a = 0; }"
        case NodeKind::If: {
            auto& s = static_cast<IfStmt&>(*stmt);
            auto cloned = std::make_shared<IfStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.thenBranch, subst),
                s.elseBranch ? cloneStmt(s.elseBranch, subst) : nullptr);
            cloned->location = s.location;
            return cloned;
        }

        // ── while 语句 ──
        // condition 走 cloneExpr，body 走 cloneStmt 自递归。
        // demo: "while (i < n) { T t = i; i = i + 1; }" ⇒ "while (i < n) { int t = i; i = i + 1; }"
        case NodeKind::While: {
            auto& s = static_cast<WhileStmt&>(*stmt);
            auto cloned = std::make_shared<WhileStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.body, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── 代码块 ──
        // { ... } 逐条递归 cloneStmt —— cloneMethod 拿到的 body 就是 BlockStmt，
        // 从这里往下扇出到每条语句。
        // demo: "{ T a; T b = a; return b; }" ⇒ "{ int a; int b = a; return b; }"
        case NodeKind::Block: {
            auto& s = static_cast<BlockStmt&>(*stmt);
            auto cloned = std::make_shared<BlockStmt>();
            cloned->location = s.location;
            for (auto& inner : s.statements) {
                cloned->statements.push_back(cloneStmt(inner, subst));
            }
            return cloned;
        }

        // ── delete 语句 ──
        // 语句位置的 `delete p;`/`delete[] p;`（析构 + free，[expr.delete]）。
        // pointerExpr 递归克隆；isArray 原样带上（决定 codegen 是否走数组释放）。
        // ★ 本分支必须存在：缺失时该语句落到末尾 return nullptr 被静默丢弃 —— 模板体内
        //   释放内存的语句实例化后凭空消失（不报错、内存泄漏）。
        // demo: "~Box() { delete data; }" 配 {T := int} ⇒ "~Box_int() { delete data; }"
        //       （data 是 int*，codegen 发 callq 析构 + callq free）
        case NodeKind::DeleteStmt: {
            auto& s = static_cast<DeleteStmt&>(*stmt);
            auto cloned = std::make_shared<DeleteStmt>(
                cloneExpr(s.pointerExpr, subst), s.isArray);
            cloned->location = s.location;
            return cloned;
        }

        // 不会到达。留 default 是为了新增节点却忘了补 case 时由 -Wswitch 报出。
        default:
            return nullptr;
    }
}

} // namespace minicc
