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
//
// 管线位置：语义分析发现 MyPtr<int> 的使用（或推导出函数模板实参）后调用本模块；
// 产出的具体类/函数注册进全局表，供后续语义检查与 CodeGen 使用。
//
// 对应 C++ 标准章节：
//   [temp.inst]  隐式实例化的触发与语义（用到才实例化）
//   [temp.subst] 模板实参替换（类型位置逐一替换，引用处折叠）
//   [dcl.ref]    引用折叠（T& && → T& 等四条规则）
// 对照 clang：
//   lib/Sema/SemaTemplateInstantiate.cpp —— 声明/类型实例化（本文件 instantiate*）
//   lib/Sema/TreeTransform.h             —— 表达式/语句递归重建（本文件 cloneExpr/cloneStmt）
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
// 【做什么】把类型树编码为 Itanium ABI 的 mangling 片段（复合类型递归编码）
// 【编码表】v=void b=bool i=int d=double P=指针 R=左值引用 O=右值引用 K=const
//           类名 = <长度><名字>（如 7MyClass）；模板参数名原样输出（实例化后不应出现）
// 【demo】int* → Pi；const int → Ki；MyClass → 7MyClass
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
        case TypeKind::Decltype:
            // 不应该出现：decltype 是"半成品类型"，必须在替换阶段（Case 1.5）
            // 求值成具体类型。若有节点活到 mangling，说明某条替换路径漏了求值 ——
            // 那会产出一个形如 `_ZN…7decltype…` 的非法符号，链接期才炸，极难排查。
            // 故在此就地报出，把问题挡在编译期。
            // 对照 clang：不会有这种状态，DecltypeType 在 Sema 层必被解析完毕。
            std::cerr << std::format(
                "[内部错误] decltype 类型节点未在替换阶段求值就进入了 mangling：{}\n",
                type->toString());
            return "Dt";   // Itanium ABI 的 decltype 编码（正常路径不会走到）
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// 模板实例化符号名
// 格式: _Z + 模板名长度 + 模板名 + I + 参数编码... + E
// 例: MyPtr<int> → _Z5MyPtrIiE
// 【做什么】类模板与函数模板实例共用：twice<int> → _Z5twiceIiE；
//           同一模板的不同实参实例符号互不相同，链接期不会冲突
// ─────────────────────────────────────────────────────────────────────────────
std::string NameMangler::mangleTemplateInstance(
    const std::string& templateName,
    const std::vector<TemplateArg>& args) {

    std::string result = std::format("_Z{}{}I", templateName.size(), templateName);

    for (auto& arg : args) {
        // 按 [temp.arg] 的形态分派编码：
        //   类型实参 → 直接编码类型（Box<int> → …IiE）
        //   非类型实参（NTTP）→ <expr-primary>：L <类型编码> <值> E
        //                       （Buf<4> → …ILi4EE）
        if (arg.isType()) {
            result += encodeType(arg.type);
        }
        else {
            // <expr-primary>：L 开头，E 收尾；中间是「类型编码 + 值」
            // 类型取 NTTP 形参声明的类型（本项目只支持 int → "i"）。
            // 负数按 Itanium 规则编码为 n<绝对值>（如 -4 → "n4"），
            // 避免 '-' 出现在符号名里（'-' 不是合法的 mangling 字符）。
            result += "L";
            result += "i";                       // 本项目 NTTP 只支持 int
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
// 函数符号名
// 格式: _Z + [N + 类名长度 + 类名] + 函数名长度 + 函数名 + 参数编码... [+ E]
// 例: MyClass::foo(int) → _ZN7MyClass3fooEi
// ─────────────────────────────────────────────────────────────────────────────
// 【demo】MyClass::foo(int)：_Z + N + 7MyClass + 3foo + i + E → _ZN7MyClass3fooEi
//         自由函数 twice(int)：_Z + 5twice + i → _Z5twicei
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
// 【做什么】type_info 对象的符号名（RTTI，[class.rtti]），格式 _ZTI + 类名
// 【demo】MyClass → _ZTI7MyClass
// ★ 命名空间里的类（N::S）名字带 "::"，直接拼进符号标签会让汇编器报
//   "junk at end of line"。这里统一净化：产生端（本函数）与引用端
//   （codegen 里所有 mangleRTTI 调用点）都是同一个函数，天然一致。
//   注意净化后 "N::S" 与 "N_S" 会撞名 —— 教学实现接受这一点（见 docs/learn/26）。
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
// vtable 符号名
// ─────────────────────────────────────────────────────────────────────────────
// 【做什么】虚函数表（vtable）的符号名，格式 _ZTV + 类名（有虚函数的类才生成）
// 【demo】MyClass → _ZTV7MyClass
std::string NameMangler::mangleVTable(const std::string& className) {
    const std::string safe = symbolSafe(className);
    return std::format("_ZTV{}{}", safe.size(), safe);
}

// ═════════════════════════════════════════════════════════════════════════════
// TemplateInstantiator 实现
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 模板实例化：将蓝图克隆并替换
// ─────────────────────────────────────────────────────────────────────────────
// 【做什么】类模板实例化 [temp.inst]：MyPtr<int> 的完整流程对应函数体内编号步骤
// 【理论】实例化 = 结构化替换 [temp.subst]：深拷贝蓝图 AST，把每个类型位置的 T 换成 int；
//         不是文本替换——复合类型（T*/T&/const T）由 substituteType 递归处理，
//         字段/方法/方法体同步重写（见文件头 ASCII 图）
ClassDeclPtr TemplateInstantiator::instantiate(
    TemplateDeclPtr templateDecl,
    const std::vector<TemplateArg>& args,
    const TypeSubstitution* substOverride) {

    // ── 0. 形参表选取 + 实参个数校验（[temp.arg.explicit]：实参与形参一一对应）──
    // ★ 关键：必须用 templateParams（带 kind 的结构化形参表），不能用 typeParams
    //   （退化的名字列表 vector<string>）。后者把 `template<int N>` 的 N 也当成
    //   一个"类型形参名"记着，根本分不出哪一位是 NTTP ——
    //   这正是"template<class T> 与 template<int N> 如何区分"的答案所在。
    //   两张表并存于 TemplateDecl，模板形参的形态只存在于 templateParams 里。
    const auto& params = templateDecl->templateParams;

    // ── 两条路径：主模板 vs 特化（[temp.class.spec] / [temp.expl.spec]）──
    // 主模板：实参与形参逐位对应，按 kind 分派 + 校验。
    // 特化  ：替换表由 Sema 的偏特化匹配（TemplateDeducer::matchPattern）得出，
    //         与特化自己的形参表【不是】逐位对应关系 ——
    //         例：偏特化 template<class T> struct Box<T*, T> 有 1 个形参，
    //             使用点 Box<double*, double> 有 2 个实参；{T := double} 只能
    //             由「模式 [T*,T] 匹配实参 [double*, double]」推导得到，
    //             不能像主模板那样 args[i] ↔ params[i] 对齐。
    //         故特化路径直接采用外部传入的替换表，跳过逐位校验与个数校验
    //         （个数与形态在 Sema 选择特化时已经把关）。
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
            const TemplateParam& p = params[i];

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
    // 实例名直接充当汇编符号的一部分（方法名 <类名>_<方法>）
    //    必须剔除空格——否则 `Box<const int&>` 会产出自带空格/逗号/尖括号的
    //    非法符号名。规则：空格丢弃，',' 与 '<'/'>'/'&'/'*' 转为下划线。
    //    （更严格的做法是 NameMangler 全权编码实例名；此处为教学可读性折中，
    //    形如 Box_const_int__。）
    //    NTTP 的值实参走同一套清洗：Buf<4> → Buf_4（数字无特殊字符，原样保留），
    //    于是 Buf<4> 与 Buf<8> 是不同实例、不同符号，天然区分。
    std::string instanceName = templateDecl->classTemplate->name;
    for (auto& arg : args) {
        instanceName += "_";
        for (char c : arg.toString()) {
            if (c == ' ') continue;                       // 空格剔除
            // ── 非法符号字符 → 字母（★ 必须【单射】：一字符对一字母）──
            //   ',' '<' '>' '&' '*' '-' 都进不了汇编标识符，必须换掉。
            //   ★ 换法有个坑：早期版本一律换成 '_'，于是
            //        Box<int*>  → Box_int_
            //        Box<int&>  → Box_int_     ← 撞了！
            //     两条实例的类名相同 ⇒ 汇编期 duplicate symbol
            //     （实测 as: `Box_int__dtor' is already defined）。
            //     而 clang 视 Box<int*> 与 Box<int&> 为两个不同类型，合法。
            //   改为给每个字符【各配一个字母】后单射成立，pointer/reference
            //   不再互撞；'*' 与 '&' 这两个最常撞车的符号是重点。
            //   （仍然非单射的是 ',' '<' '>' → '_'：它们只出现在复合实参里，
            //     要撞上需要用户真造出 `int_double` 这种类名 —— 由下面的
            //     撞名守卫兜底，宁可报错也不产出重复符号。）
            //   ★ 注意与 mangling 的区别：这里只是个「人读的实例名」，
            //     真正的符号名编码是 NameMangler（负数在那里编成 n3，见上）。
            switch (c) {
                case '*': instanceName += 'P'; break;  // Pointer   Box<int*>  → Box_intP
                case '&': instanceName += 'R'; break;  // Reference Box<int&>  → Box_intR
                case '-': instanceName += 'N'; break;  // Negative  Buf<-3>    → Buf_N3
                case '+': instanceName += 'A'; break;  // 目前进不来（实参只认整数字面量）
                case ',': case '<': case '>':
                    instanceName += '_'; break;
                default:  instanceName += c;   break;
            }
        }
    }

    // ── 2a. ★ 撞名守卫 ──
    // 【为什么需要】上面的清洗仍有非单射的角落（',' '<' '>' 都变 '_'，
    //   多实参之间也用 '_' 拼接），极端情况下两个【不同】的实参列表会洗出
    //   同一个实例名。那意味着两条实例产出同一批汇编符号 —— 而 as 的报错
    //   是 `symbol 'X' is already defined`，指不到"实例名撞车"这个根因。
    //   在这里拦一道，把根因说清楚：谁跟谁撞了、无损键分别是什么。
    // 无损键与 Sema 的 m_classInstanceCache 用同一套构造（名 + 实参 toString），
    //   所以"键相同"⇔"本来就是同一条实例"，那种情况放行（直接调用本类的
    //   单元测试会重放同一条）。
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
    // 用 templateParams 渲染形参表，才能把 NTTP 打成 `int N` 而非 `typename N`
    //（旧实现读 typeParams，只能一律按 typename 打印，对 NTTP 是错的）。
    // 特化路径额外打印被选中的是哪个版本（主模板/偏特化/全特化），
    // 这是"三者如何择优"最直观的观测点。
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
                s += (params[i].kind == TemplateParamKind::Type)
                         ? "typename " + params[i].name
                         : (params[i].nonType ? params[i].nonType->toString() : "?")
                               + " " + params[i].name;
            } return s; }(),
        kindTag);
    if (templateDecl->isSpecialization()) {
        std::cout << std::format("  ║ Pattern:   {}<{}>\n",
            templateDecl->classTemplate->name,
            [&] { std::string s;
                  for (size_t i = 0; i < templateDecl->specPattern.size(); i++) {
                      if (i > 0) s += ", ";
                      const auto& p = templateDecl->specPattern[i];
                      s += p ? p->toString() : "?";
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

    // 4. 克隆字段（替换类型中的模板参数）
    //    demo: T* data → int* data（substituteType 递归处理复合类型）
    std::cout << "  ║ ── Field Substitution ──\n";
    for (auto& field : templateDecl->classTemplate->fields) {
        std::cout << std::format("  ║   field '{}' : {} → ",
            field.name, field.type ? field.type->toString() : "?");
        auto cloned = cloneField(field, subst);
        std::cout << std::format("{}\n", cloned.type ? cloned.type->toString() : "?");
        newClass->fields.push_back(cloned);
    }

    // 5. 克隆方法（替换类型和函数体中的模板参数）
    //    返回类型/形参/体内局部变量声明中的 T 全部替换，表达式经 cloneExpr 深拷贝
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
        // 构造/析构函数的名字绑定在"类名"上——蓝图里它们叫
        // Box / ~Box，实例类叫 Box_int，必须跟着改名，否则 mangledName
        // 会变成 Box_int_Box，与 CodeGen 发射的调用符号（类名_类名）对不上，
        // 链接期 undefined reference。对照 clang：实例化时按新类名重建
        // CXXConstructorDecl 的 DeclName。
        if (cloned->kind == NodeKind::Constructor && cloned->name == blueprintName) {
            cloned->name = instanceName;
        }
        if (cloned->kind == NodeKind::Destructor && cloned->name == "~" + blueprintName) {
            cloned->name = "~" + instanceName;
        }
        newClass->methods.push_back(cloned);
    }

    // 5.5 克隆类内类型别名（using X = T; / typedef T X;）
    //     [temp.alias]：别名模板的每个实例都自带一份"已替换"的别名表，
    //     否则 Box<int>::type 会拿到蓝图里的裸 T（未替换的模板形参）。
    //     替换引擎与字段/方法同一套 substituteType，只是位置换成别名目标。
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
// 函数模板实例化（S5）
// ─────────────────────────────────────────────────────────────────────────────
// 推导引擎（S2~S4）给出 typeArgs 后：
//   1. 构建替换表 { T → int, ... }
//   2. 复用方法克隆引擎深拷贝蓝图函数（返回类型/参数/函数体全部替换）
//   3. 生成 mangled 符号（_Z5twiceIiE 风格）——与普通函数符号区分
//   克隆出的函数交回语义分析器：注册 + 用具体类型分析函数体（两阶段查找的第二阶段）。
// demo：twice(3) → 推导得 T := int → 本函数产出 twice<int>：
//   void twice(T x){...} 中每个 T 换成 int，符号 _Z5twiceIiE，注册进 m_instantiatedFunctions
// ─────────────────────────────────────────────────────────────────────────────
FuncDeclPtr TemplateInstantiator::instantiateFunction(
    TemplateDeclPtr templateDecl,
    const std::vector<TypePtr>& typeArgs) {

    // 函数模板路径的形参仍是裸 TypePtr（推导引擎 S2~S4 只产出类型，
    // 函数模板的非类型形参尚未实现），故此处统一包成 TemplateArg::ofType
    // 再放进同一张替换表 —— 表本身已是类型/值两形态通用的。
    TypeSubstitution subst;
    for (size_t i = 0; i < templateDecl->typeParams.size()
         && i < typeArgs.size(); i++) {
        subst[templateDecl->typeParams[i]] = TemplateArg::ofType(typeArgs[i]);
    }

    auto& blueprint = templateDecl->funcTemplate;

    // mangler 收的是 tagged 实参表，而函数模板这条路径手上只有裸类型，
    // 故此处再包一遍（替换表 subst 与 mangling 需要同一份形态信息）。
    std::vector<TemplateArg> targs;
    targs.reserve(typeArgs.size());
    for (auto& a : typeArgs) targs.push_back(TemplateArg::ofType(a));

    std::string mangled =
        NameMangler::mangleTemplateInstance(blueprint->name, targs);

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

    // 复用方法克隆（ownerClassName = "" 表示自由函数）
    FuncDeclPtr instance = cloneMethod(blueprint, subst, "");
    instance->mangledName = mangled;

    std::cout << std::format("  ║ Symbol: {} → {}\n", blueprint->name, mangled);
    std::cout << std::format("  ╚═══════════════════════════════════════════════╝\n");

    m_instantiatedFunctions.push_back(instance);
    return instance;
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
// 【做什么】[temp.subst] 的类型替换：深度优先遍历类型树，
//           模板参数叶节点按 subst 换为对应实参，复合节点递归重建；
//           引用节点重建时执行引用折叠（[dcl.ref]，规则见上方注释块）
TypePtr TemplateInstantiator::substituteType(
    TypePtr type, const TypeSubstitution& subst) {

    if (!type) return nullptr;

    // ── Case 1: 模板参数 → 直接替换 ──
    // demo：substituteType(T, {T := int}) → int；不在表中则原样保留（如外层模板的参数）
    if (type->isTemplateParam()) {
        auto it = subst.find(type->templateParamName);
        if (it != subst.end()) {
            // ★ 类型位置只接受【类型】实参。若命中一个值实参（Integral），
            //   说明用户把 NTTP 的形参名（如 int N 的 N）当类型用了——
            //   这是硬错误，必须报出来而不是把值硬塞成类型。
            //   Parser 已把 NTTP 名排除在模板形参作用域外（见 parseTemplateDecl
            //   只压 Type 形参），正常写法下走不到这里；这是兜底。
            //   对照 clang：err_nontype_template_parameter_used_as_type。
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

    // ── Case 1.5: decltype(expr) —— ★ 延迟求值的兑现时刻 ★ ──
    // 【为什么要在这里求】decltype 在 Parser 阶段只留下"表达式 + 是否加括号"两样
    //   东西，因为那时还是模板蓝图、T 未知。替换阶段拿到了具体实参，
    //   才是唯一能真正求值的时机。
    // 【两步】① 先把替换应用到操作数表达式上（T → int 等）
    //         ② 再交给 DecltypeEvaluator 求值成具体类型
    // 【demo】template<class T> struct is_range<T, void_t<decltype(declval<T>().begin())>>
    //         对 T := vector<int>：
    //           ① declval<T>().begin()  →  declval<vector<int>>().begin()
    //           ② 求值 → int（begin 的返回类型）
    //         若 T := int：① 替换后 declval<int>().begin()，② 求值失败 →
    //           抛 SubstitutionFailure → 偏特化不匹配 → 回退主模板 false_type
    //         —— 这一"抛—捕"就是 SFINAE 探测的全部机制。
    // 【对照 clang】TreeTransform::TransformDecltypeType →
    //   Sema::SubstType 中 DecltypeType 依赖分支 → BuildDecltypeType 重新求值。

    //template <typename T>  这里decltype 是一个class type
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

    // ── Case 2: 指针类型 T* → 递归替换内部类型 ──
    // demo：T* 配 {T := int} → int*；pointee 无变化则复用原节点（结构共享，省一次拷贝）
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
    // demo：T& 配 {T := int} → int&；配 {T := int&} → int& & 折叠为 int&
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
    // demo：万能引用落地——T&& 配 {T := int&} → int& && 折叠为 int&（左值引用赢）
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
    // demo：const T 配 {T := int} → const int
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

    // ── Case 5.5: 依赖类型名 typename T::type ──
    // 【是什么】限定者（T）要等替换才知道，成员名（type）是它里面的类型别名。
    //   实例化 Get<Plain> 时：typename T::type v; 的字段类型
    //     Class("type", nestedQualifier = TemplateParam("T"))
    //   经本分支 → 限定者替换成 Plain → 查 Plain 的别名表 → int。
    // 【为什么失败要在这里抛】替换期就是 [temp.deduct]/8 的直接上下文：
    //   `T := int` 时 `int::type` 不存在，必须【当场】软失败，
    //   由 Sfinae::attempt（偏特化匹配 / 重载决议）吸收 —— 这正是
    //     template<class T> using has_type = void_t<typename T::type>;
    //   这类探测惯例能工作的全部机制。拖到 Sema 解析期就成硬错误了。
    // 【为什么限定者仍是形参时原样返回】多层模板时内层替换只认得自己的形参，
    //   外层形参留待外层替换 —— 与 Case 1 的处理一致。
    // 对照 clang：TreeTransform::TransformDependentNameType →
    //   Sema::SubstType 里对依赖限定名重新做限定名查找；
    //   查不到即 Sema::SubstitutionFailure（可恢复）。
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
    // 【是什么】`MyPtr<T>` 这种"类模板 id"节点，它的依赖藏在【实参】里而不是
    //   名字里 —— Case 1/6 只按名字查替换表，`MyPtr` 这个名字不在表中，
    //   于是整个节点原样返回，实参里的 T 永远换不掉。
    //   表现出来就是：`template<class T> using Vec = MyPtr<T>;` 展开 Vec<int>
    //   得到的仍是 `MyPtr<T>`，后续按模板参数去实例化，造出一个名叫
    //   MyPtr_T 的假实例（实参是形参 T 本身！），方法返回类型跟着错。
    //   这是别名模板真正落地的最后一块拼图，也是嵌套模板
    //   （template<class T> struct W { Box<T> b; };）此前没有覆盖到的路径。
    // 【demo】substituteType(MyPtr<T>, {T := int}) → MyPtr<int>
    //         MyPtr<T> 配 {} （外层还没绑）→ 原样返回，保持依赖
    // 【为什么先做这一步再做别名解糖】别名 Vec<T> 的实参必须先变成 int，
    //   展开出来的 MyPtr<int> 才是具体的；顺序反了就会拿 T 去实例化。
    // 对照 clang：TreeTransform::TransformTemplateSpecializationType ——
    //   对 TemplateArgumentList 逐项 Transform，全具体才 rebuild。
    TypePtr idNode = type;
    if (type->isClass() && !type->templateArgs.empty()) {
        std::vector<TemplateArg> newArgs;
        newArgs.reserve(type->templateArgs.size());
        bool changed = false;
        for (const auto& arg : type->templateArgs) {
            if (arg.isType() && arg.type) {
                // ── 实参位的"裸名字"其实是 NTTP 名 ⇒ 还原成值实参 ──
                // 【为什么会错】Parser 的实参分流只看 Token 形态：整数字面量 → 值，
                //   其余 → 类型（见 parseTemplateArgumentList）。而 `Buf<N>` 里
                //   的 N 是【非类型形参名】，形态上却是标识符 —— 于是被建成
                //   Class("N") 类型节点，替换到这一步就会撞上
                //   "non-type template parameter 'N' is used as a type"。
                // 【凭什么能还原】替换表是唯一权威：N 在表里绑的是【值】，
                //   而类型位置的实参不可能绑值 —— 所以这里的裸 N 只能是值实参。
                //   Parser 缺的是"这个作用域里 N 是值"的知识，替换阶段恰好有。
                // 【demo】template<int N> using BufA = Buf<N>;
                //         BufA<4> ⇒ 替换表 {N := 值 4} ⇒ 还原成 Buf<4> ✓
                // 对照 clang：Parser 维护 TemplateParameterScope，NonTypeTemplateParm
                //   在实参位直接走常量表达式解析，不存在这一步还原。
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

    // ── Case 5.8: 别名模板 id X<int> / X<T>（[temp.alias]）──
    // 【是什么】X 是 `template<class U> using X = ...;` 声明的【别名】，
    //   它不产生新类型，只产生新名字 —— 所以这里做的是"解糖"（desugar），
    //   不是"实例化"：替换完实参就把它换成底层类型，中间不留任何痕迹。
    // 【为什么在替换阶段就要展开】这是 define-time / call-time 的分界：
    //   定义期（Sema 第一遍）算不动 X<T>，因为 T 还没绑；
    //   到了这里实参已具体，展开的【当场】就是 [temp.deduct]/8 的直接上下文 ——
    //   展开失败（如 enable_if 条件为假）必须当场抛软失败，才能被
    //   Sfinae::attempt 吸收成"该候选被剔除"，而不是拖成硬错误。
    // 【与 Case 5.5 的区别】5.5 处理 `typename T::type`（成员别名，限定者是
    //   形参）；这里处理 `X<T>`（顶层别名模板 id，名字自己就是别名）。
    // 对照 clang：Sema::SubstType 对 TemplateSpecializationType 判
    //   isTypeAlias() → Sema::CheckAliasTemplateId → 直接取底层的 sugar。
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

    // ── Case 6: 类类型中的模板参数名（简化处理）──
    // demo：Class("T") 配 {T := int} → int（parseType 把裸 T 建成了 Class 节点的兜底路径）
    // 因为 Parser 在解析类型 T 时创建的是 Class("T") 而非 TemplateParam("T")
    // 所以这里也需要检查类名是否在替换表中
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
// 【做什么】结构化替换落到字段层：类型过一遍 substituteType，其余元信息原样复制
// 【demo】MyPtr<int> 的字段 "data : T*" → "data : int*"
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
// 【做什么】方法级结构化替换：返回类型、形参类型、函数体三处的 T 全部替换；
//           ownerClassName 指向实例类名（自由函数传 ""，见 instantiateFunction）
// 【理论】对应 clang TreeTransform 的声明重建；克隆不携带旧的语义分析结果，
//           实例方法将交回语义分析器用具体类型重新检查（两阶段查找的第二阶段）
FuncDeclPtr TemplateInstantiator::cloneMethod(
    FuncDeclPtr method, const TypeSubstitution& subst,
    const std::string& newClassName) {

    // 按源节点的动态类型重建 —— 构造函数要克隆成
    // ConstructorDecl（保住 initList / isDefaultCtor），析构函数要克隆成
    // DestructorDecl（保住 isDefaultDtor）。若一律建 FunctionDecl，
    // 实例类的语义分析会把它当普通方法：合成构造/析构检测失效、
    // CodeGen 发射的构造调用（Class_Class）链接不到符号。
    // 对照 clang：TreeTransform 按 Decl 的 DeclKind 分派到对应的
    // TransformConstructorDecl / TransformDestructorDecl。
    FuncDeclPtr newMethod;
    // 按种类分派（[class.copy.ctor]/[class.dtor] 各自的克隆规则不同）：
    // 构造要带初始化列表，析构只带 isDefaultDtor 标志，普通方法走空壳。
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
// 克隆表达式（深拷贝并替换类型）
// ─────────────────────────────────────────────────────────────────────────────
// 【做什么】逐表达式节点深拷贝（对应 clang TreeTransform::TransformExpr）；
//           本项目表达式节点不存类型注解，实例的类型在语义分析中重新推出，
//           所以这里只需递归克隆结构（变量名恰与模板参数同名的边角情况留了钩子）
// 【覆盖率对账】ast.h 中 Expression 派生类共 14 种，本函数全部处理：
//   Int/Bool/String/Nullptr 字面量、VarExpr、BinaryExpr、UnaryExpr、
//   CallExpr、MemberExpr、IndexExpr、NewExpr、DynamicCastExpr、ThisExpr、
//   DeleteExpr（曾缺失，与 cloneStmt 的 DeleteStmt 一同补齐）
ExprPtr TemplateInstantiator::cloneExpr(
    ExprPtr expr, const TypeSubstitution& subst) {

    if (!expr) return nullptr;

    // 一次 switch（跳表）替代 14 级 dynamic_pointer_cast 试探。
    // 【为什么是标签分派不是访问者】clone* 是【取值型】递归 —— 每个分支都要
    //   返回新建的节点（ExprPtr / StmtPtr），而 AstVisitor::visit 返回 void。
    //   判据与 Sema 一致：handler 只要引用 → 访问者；还要所有权或返回值 → 标签分派。
    // static_cast 安全：节点 kind 由构造函数设定，恒等于自身类型。
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
        // ★ NTTP 的核心一步：值替换（value substitution）★
        //   类型替换发生在【类型位置】（substituteType），值替换发生在【表达式位置】
        //   ——模板体里出现的裸 NTTP 形参名，Parser 产出的是 VarExpr（不是类型节点），
        //   所以必须在这里把它换成整数字面量。
        //
        // 【demo】template<int N> class Buf { int size() { return N; } }
        //         Buf<4> 实例化：cloneExpr(VarExpr{"N"}, {N→Integral:4})
        //                        → IntLiteralExpr{4}
        //         ⇒ 实例方法体变成 `return 4;`，语义分析与 CodeGen 只见到常量。
        //
        // 【理论】对应 clang TreeTransform::TransformDeclRefExpr
        //   —— 它把 DeclRefExpr 指向 NonTypeTemplateParmDecl 的引用，
        //   替换为已求值的 TemplateArgument（这里退化成 IntLiteral）。
        //   真 C++ 中该表达式还需过 Sema::CheckTemplateArgument 做类型转换与
        //   常量求值（如 Buf<2+2> 要先折叠成 4）；本项目不做常量折叠，
        //   只支持写死的整数字面量（见 ROADMAP 主线 D）。
        //
        // 【已知边界】判定依据只是"名字命中 NTTP 形参名"，没有做作用域检查。
        //   若模板体内另有同名局部变量（遮蔽了 NTTP），替换会误伤。
        //   正确做法是查符号表确认该 VarExpr 绑定到 NonTypeTemplateParmDecl
        //   （clang 走 DeclRefExpr 的 ValueDecl* 而非名字字符串）。
        //   本项目蓝图期的 VarExpr 尚未绑定声明，暂以此简化；教学中足够。
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
            // ★ 但显式模板实参必须一并替换：declval<T>() 的 T 挂在 VarExpr 的
            //   explicitTemplateArgs 上（形如 VarExpr{declval, [T]}），
            //   若只克隆名字，decltype 求值阶段看到的就是裸 declval()，
            //   T 凭空丢失 → 求值必然失败。这是 decltype 探测链上的一环。
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

        // ── delete 表达式 DeleteExpr（ast.h:300）───────────────────────────────
        // 表达式位置的 `delete p` / `delete[] p`（如 `return delete p, 0;` 或被
        // 当作子表达式使用时）。pointerExpr 递归克隆，isArray 原样带上。
        // 为什么要克隆而非复用：蓝图的 ExprPtr 若被多个实例共享，析构/free 的
        // 代码生成会指向同一个节点，任一实例改写都会污染其它实例。
        // demo：蓝图 "delete data;" 配 {T := int} → 实例 "delete data;"
        //        （data 的类型 T* → int* 由语义分析阶段重新推出，不在这里）
        case NodeKind::Delete: {
            auto& e = static_cast<DeleteExpr&>(*expr);
            auto cloned = std::make_shared<DeleteExpr>(
                cloneExpr(e.pointerExpr, subst), e.isArray);
            cloned->location = e.location;
            return cloned;
        }

        // 不会到达：ast.h 中 14 种 Expression 已全覆盖。留着 default 是为了
        // 将来新增节点类型却忘了补 case 时，让编译器给出 -Wswitch 警告。
        default:
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 克隆语句（深拷贝并替换类型）
// ─────────────────────────────────────────────────────────────────────────────
// 【做什么】逐语句节点深拷贝（对应 clang TreeTransform::TransformStmt）；
//           直接碰类型的唯一位置是 VarDeclStmt 的声明类型（T x → int x），
//           其余语句经 cloneExpr 间接完成替换
//
// 【为什么必须深拷贝】蓝图 ClassDecl 全局只有一份，Box<int> 与 Box<double>
//   两个实例要各自拥有独立的 AST 子树。若浅拷贝，实例化 Box<double> 时会
//   把 Box<int> 已替换好的语句再覆盖一遍（同一个 StmtPtr 被两边共享）。
//
// 【替换只发生在类型上，不在名字上】subst 是 {模板参数名 → 具体类型} 的映射
//   （如 {"T" → int}）。语句里的变量名 item/ptr 原样保留——它们是函数体内
//   的局部名，与模板参数无关；只有"类型位置"出现的 T 才被换掉。
//
// 【覆盖率对账】ast.h 中 Statement 派生类共 8 种，本函数全部处理：
//   ✅ VarDeclStmt / ExprStmt / AssignStmt / ReturnStmt
//   ✅ IfStmt / WhileStmt / BlockStmt
//   ✅ DeleteStmt（曾缺失——模板体内 `delete ptr;` 实例化后被静默丢弃致泄漏，
//      已补分支；cloneExpr 侧对应的 DeleteExpr 表达式也已补）
//
// 【示例】蓝图 Box<T> 的方法体配 subst = {"T" → int}：
//   蓝图源码                       →  实例化结果
//   ─────────────────────────────────────────────────────────
//   T item = x;                    →  int item = x;        (VarDeclStmt：类型替换)
//   item = y;                      →  item = y;            (AssignStmt：名字不变)
//   return item;                   →  return item;         (ReturnStmt)
//   if (n > 0) { ... } else { ... }→  if (n > 0) { ... } else { ... }
//   while (i < n) { i = i + 1; }   →  while (i < n) { i = i + 1; }
//   { T a; T b; }                  →  { int a; int b; }    (BlockStmt：递归每条)
StmtPtr TemplateInstantiator::cloneStmt(
    StmtPtr stmt, const TypeSubstitution& subst) {

    if (!stmt) return nullptr;

    // ── 变量声明 ────────────────────────────────────────────────
    // 【唯一直接碰类型的分支】declaredType 过 substituteType（T → int），
    // initializer 过 cloneExpr 递归（初始化式里可能嵌着 new T(...)）。
    // demo：蓝图体中 "T item = x;" 配 {T := int} → "int item = x;"
    // demo：蓝图体中 "T* p = new T();" 配 {T := int} → "int* p = new int();"
    //        ↑ declaredType 是 Pointer(T)，substituteType 会递归进 pointeeType

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

        // ── 表达式语句 ─────────────────────────────────────────────────
        // 以分号结尾的裸表达式（如 f(x); 或 new T();）。本节点不含类型，
        // 类型替换全靠 cloneExpr 递归到内部的 new/调用实参里完成。
        case NodeKind::ExprStmt: {
            auto& s = static_cast<ExprStmt&>(*stmt);
            auto cloned = std::make_shared<ExprStmt>(cloneExpr(s.expr, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── 赋值语句 ─────────────────────────────────────────────────
        // 注：C++ 标准里赋值是表达式（[expr.ass]），本项目简化成语句（ast.h:316）。
        // target/value 两侧都过 cloneExpr；变量名不变，故通常无替换发生。
        // demo：蓝图 "item = x;" → 实例 "item = x;"（结构深拷贝，内容不变）
        case NodeKind::Assign: {
            auto& s = static_cast<AssignStmt&>(*stmt);
            auto cloned = std::make_shared<AssignStmt>(
                cloneExpr(s.target, subst),
                cloneExpr(s.value, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── return 语句 ──────────────────────────────────────────────
        // 返回值表达式过 cloneExpr。返回类型本身在 cloneMethod 里替换，不在这里。
        // demo：蓝图 "return item;" → 实例 "return item;"
        case NodeKind::Return: {
            auto& s = static_cast<ReturnStmt&>(*stmt);
            auto cloned = std::make_shared<ReturnStmt>(
                cloneExpr(s.value, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── if 语句 ──────────────────────────────────────────────────────
        // 递归点有两个：condition 走 cloneExpr，thenBranch/elseBranch 走 cloneStmt
        // （语句套语句，所以这里自递归）。elseBranch 可空，需判空后再递归。
        // demo：蓝图 "if (n > 0) { T a = n; } else { T a = 0; }"
        //    →  实例 "if (n > 0) { int a = n; } else { int a = 0; }"
        case NodeKind::If: {
            auto& s = static_cast<IfStmt&>(*stmt);
            auto cloned = std::make_shared<IfStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.thenBranch, subst),
                s.elseBranch ? cloneStmt(s.elseBranch, subst) : nullptr);
            cloned->location = s.location;
            return cloned;
        }

        // ── while 语句 ────────────────────────────────────────────────
        // condition 走 cloneExpr，body 走 cloneStmt 自递归。
        // demo：蓝图 "while (i < n) { T t = i; i = i + 1; }"
        //    →  实例 "while (i < n) { int t = i; i = i + 1; }"
        case NodeKind::While: {
            auto& s = static_cast<WhileStmt&>(*stmt);
            auto cloned = std::make_shared<WhileStmt>(
                cloneExpr(s.condition, subst),
                cloneStmt(s.body, subst));
            cloned->location = s.location;
            return cloned;
        }

        // ── 代码块 ────────────────────────────────────────────────────
        // { ... } 花括号体：逐条递归 cloneStmt。这是函数体的顶层容器——
        // cloneMethod 拿到的 body 就是一个 BlockStmt，从这里往下扇出到每条语句。
        // demo：蓝图 "{ T a; T b = a; return b; }" → 实例 "{ int a; int b = a; return b; }"
        case NodeKind::Block: {
            auto& s = static_cast<BlockStmt&>(*stmt);
            auto cloned = std::make_shared<BlockStmt>();
            cloned->location = s.location;
            for (auto& inner : s.statements) {
                cloned->statements.push_back(cloneStmt(inner, subst));
            }
            return cloned;
        }

        // ── delete 语句 ──────────────────────────────────
        // 语句位置的 `delete p;` / `delete[] p;`（析构 + free，[expr.delete]）。
        // pointerExpr 递归克隆；isArray 标志原样带上（决定 codegen 是否走数组释放）。
        // 历史缺口：此分支缺失时 `delete` 语句落到末尾 return nullptr 被静默丢弃
        // ——模板体内释放内存的语句实例化后凭空消失（不报错、内存泄漏）。
        // demo：蓝图析构方法体 "~Box() { delete data; }" 配 {T := int}
        //    →  实例 "~Box_int() { delete data; }"（data 类型 int*，codegen 发
        //       callq 析构（若有）+ callq free；类型推导在语义分析阶段完成）
        case NodeKind::DeleteStmt: {
            auto& s = static_cast<DeleteStmt&>(*stmt);
            auto cloned = std::make_shared<DeleteStmt>(
                cloneExpr(s.pointerExpr, subst), s.isArray);
            cloned->location = s.location;
            return cloned;
        }

        // 不会到达：ast.h 中 8 种 Statement 已全覆盖。留着 default 是为了
        // 将来新增节点类型却忘了补 case 时，让编译器给出 -Wswitch 警告。
        default:
            return nullptr;
    }
}

} // namespace minicc
