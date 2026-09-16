// =============================================================================
// 阶段 5：代码生成器实现 —— x86-64 汇编输出
// =============================================================================
// 这是编译器的"最后一公里"：将 AST 翻译为真实的机器指令。
//
// 核心转换示例：
//   源码: u.age = 20;
//   汇编: movl $20, -8(%rbp)    # 直接硬编码偏移量，字段名已消失
//
//   源码: ptr->foo();  (虚函数)
//   汇编: movq (%rax), %rax     # (a) 从对象读出 _vptr
//         movq 0(%rax), %rax    # (b) 加上 vtable[0] 偏移
//         callq *%rax           # (c) 跳转到该地址执行
//
// 目标架构：x86-64, System V AMD64 ABI
//   - 参数传递：rdi, rsi, rdx, rcx, r8, r9
//   - 返回值：rax
//   - 栈对齐：16 字节
// =============================================================================
//
// 在管线中的位置：
//   Preprocessor → Lexer → Parser → Sema → TemplateDeduction/Instantiation
//   → ★CodeGen（本文件）★ → .s 汇编 → 外部 as/ld 产出可执行文件
//   走到这里时：类型已定、auto 已替换、字段偏移（ClassLayout）已算好、
//   模板已实例化并 mangle —— 代码生成只需做"树 → 线性指令"的机械翻译。
//
// 名字修饰（GCC Itanium C++ ABI，本项目实现的子集）：
//   · 成员方法：简化为 "类名_方法名"（Dog::speak → Dog_speak）
//   · vtable  ：_ZTV + <长度><类名>     （Dog → _ZTV3Dog）
//   · typeinfo：_ZTI + <长度><类名>     （Dog → _ZTI3Dog）
//   · 模板实例：_Z + 模板名 + I<类型码>E 风格（见阶段 4 NameMangler）
//
// 对应 LLVM 模块（详见 codegen.h 头注）：
//   指令选择 ≈ lib/CodeGen/SelectionDAG；调用约定降级 ≈ X86ISelLowering；
//   寄存器分配 ≈ RegAlloc（本项目退化为"单累加器 rax + 栈周转"）；
//   序言/尾声 ≈ PrologEpilogInserter；汇编打印 ≈ AsmPrinter。
//
// 本文件的组织结构：
//   generate()      主入口，拼装 .text/.data/.rodata 三段
//   emitVTable/RTTI 数据段：vtable 与 type_info
//   emitFunction    函数框架（序言/形参 spill/尾声）
//   emitStmt*       语句降级（声明/赋值/return/if/while）
//   emitExpr*       表达式降级（结果一律落 rax）
//   emitVirtualCall 虚函数调用三部曲（本编译器的高光时刻）
// =============================================================================

#include "codegen.h"
// 复用阶段 4 的 NameMangler：生成 _ZTV/_ZTI 标准符号名
#include "template_instantiation.h"
#include <format>
#include <algorithm>
#include <cassert>
#include <set>

namespace minicc {

// 默认构造即可用：输出缓冲为空、计数器归零、偏移量表为空，全部由类内默认值完成
CodeGen::CodeGen() = default;

// ─────────────────────────────────────────────────────────────────────────────
// 辅助函数
// ─────────────────────────────────────────────────────────────────────────────
// 生成全局唯一的汇编标签。
// 理论：控制流语句（if/while）的跳转目标是"位置"，汇编里用标签表示；
//       计数器单调递增，保证多个 if/while 的标签互不冲突。
// demo: 依次调用 newLabel("else") → "else_0"，newLabel("endif") → "endif_1"
std::string CodeGen::newLabel(const std::string& prefix) {
    return std::format("{}_{}", prefix, m_labelCounter++);
}

// 写一条指令进 .text 段。统一加 4 空格缩进是 GAS 的排版惯例：
// 标签顶格、指令缩进，肉眼即可区分"跳转目标"与"要执行的指令"。
void CodeGen::emit(const std::string& line) {
    m_code << "    " << line << "\n";
}

// 写一行进 .data 段（可写全局数据：vtable、RTTI 结构）
void CodeGen::emitData(const std::string& line) {
    m_data << line << "\n";
}

// 写一行进 .rodata 段（只读数据：字符串字面量、类型名字符串）
void CodeGen::emitRodata(const std::string& line) {
    m_rodata << line << "\n";
}

// 写一条 GAS 行注释（# 开头），只影响 .s 可读性、不产生机器码。
// 这是教学编译器的重要可观测性手段：把编译期决策（字段偏移、vtable 下标）
// 直接写进汇编注释，让"符号 → 数字"的降级过程肉眼可查。
void CodeGen::emitComment(const std::string& comment) {
    m_code << "    # " << comment << "\n";
}

// 登记一个字符串字面量，返回引用它的标签。
// 理论：字符串属于只读数据，不能内联在指令流中，要放 .rodata 再用
//       RIP 相对寻址取地址。此处只"登记"，真正发射推迟到 generate()
//       末尾的 emitStringLiterals()，保证常量池集中在一处。
// demo: addStringLiteral("hello") → 登记 ("str_0", "hello")，返回 "str_0"
//       最终 .rodata 中出现： str_0:  .string "hello"
std::string CodeGen::addStringLiteral(const std::string& value) {
    std::string label = newLabel("str");
    m_stringLiterals.emplace_back(label, value);
    return label;
}

 // 汇编符号净化——gas 标签只允许 [A-Za-z0-9_.$]，源码层符号
// 里的 "::"（命名空间限定）、"<>&,*"（模板实参）都会让汇编器报
// "junk at end of line"。统一替换为下划线得到合法标签，如
// Math::scale → Math__scale。这是教学版 name mangling 的一小步；
// 对照 clang/GCC 的真 mangling：_ZN4Math5scaleEi（Itanium ABI，
// 可编码任意类型签名且双向可逆）。
// 注意成对一致：标签发射端（函数/全局变量）与使用端（callq / %rip 读写）
// 必须走同一个净化函数，否则汇编期报 undefined reference。
static std::string asmSymbol(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '$') {
            out += c;
        } else {
            out += '_';  // ':' '<' '>' '&' ',' '*' ' ' 等一律下划线
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// 主入口：生成整个编译单元的汇编
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 AST（函数列表 + 类类型表）翻译为一份完整 .s 文本。
// 流程：① 数据段先行 —— 为每个含虚函数的类发射 vtable 与 RTTI
//       ② 代码段   —— 逐个发射函数（序言/形参 spill/函数体/尾声）
//       ③ 只读数据 —— 统一发射生成期间收集的字符串字面量
//       ④ 拼装     —— .text → .data → .rodata 三段按序串接返回
// 理论：段内顺序不影响正确性——汇编器允许前向引用（call 尚未定义的
//       标签、引用后面的数据标签），真正的地址在汇编/链接期重定位填回。
// demo（输入）: class Animal { virtual int speak(); }; + main 调用之
// demo（输出骨架）:
//       .text
//           .globl Animal_speak
//       Animal_speak:  pushq %rbp ...
//           .globl main
//       main: ...
//       .data
//       _ZTV6Animal:   .quad 0 / .quad _ZTI6Animal / .quad Animal_speak
//       .section .rodata
//       str_0:         .string "..."
std::string CodeGen::generate(
    const TranslationUnit& unit,
    const std::unordered_map<std::string, TypePtr>& classTypes,
    const std::vector<FuncDeclPtr>& functions) {

    // 保存全局类类型表指针：发射成员函数、虚调用、new 时都要回头查它
    m_classTypes = &classTypes;

    // ── 建立"类名 → 基类名"表：typeinfo 第三槽（基类 typeinfo 指针）要用 ──
    // 递归遍历顶层声明（含命名空间内），ClassLayout 里不存基类名，
    // 继承关系的唯一事实来源是 AST 的 ClassDecl.baseClassNames。
    std::unordered_map<std::string, std::string> baseClassOf;
    std::function<void(const std::vector<DeclPtr>&)> collectBases =
        [&](const std::vector<DeclPtr>& decls) {
            for (auto& decl : decls) {
                // 按节点种类分派（需要 shared_ptr 交给递归 ⇒ 标签分派，非访问者）
                switch (decl->kind) {
                    case NodeKind::Class: {
                        auto cls = std::static_pointer_cast<ClassDecl>(decl);
                        baseClassOf[cls->name] = cls->firstBase();
                        break;
                    }
                    case NodeKind::Namespace:
                        collectBases(std::static_pointer_cast<NamespaceDecl>(decl)->declarations);
                        break;
                    case NodeKind::Template:
                        // 模板蓝图本身不产码；实例化后的 ClassDecl 已在 functions/
                        // 类型表层面处理，这里只登记非模板类即可。模板实例走
                        // NameMangler 的 _ZTI 名字，其基类信息与普通类同样经由
                        // ClassDecl 路径登记（若被实例化）。
                        break;
                    default:
                        break;
                }
            }
        };
    collectBases(unit.declarations);

    // ── 生成数据段：vtable、RTTI、字符串字面量 ──

    // 为每个有虚函数的类生成 vtable 和 RTTI
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.hasVTable) {
            emitVTable(name, type);
            emitRTTI(name, type, baseClassOf[name]); // 无基类时取到空串
        }
    }

    // 补发【非多态基类】的 RTTI：多态派生类的 typeinfo 会逐个引用其所有基类
    // 的 _ZTI（emitRTTI 里的 base[i] typeinfo 指针），若某基类非多态（无 vtable）
    // 上面循环就漏发了它，链接期报 undefined reference to '_ZTINonPolyBase'。
    // 只在它确实被某个多态类当基类引用时才补发，避免给孤立非多态类平白产 RTTI。
    std::set<std::string> nonPolyBaseNeeded;
    for (auto& [name, type] : classTypes) {
        if (!type->classLayout.hasVTable) continue;
        for (auto& base : type->classLayout.bases) {
            auto bIt = classTypes.find(base.baseClassName);
            if (bIt != classTypes.end() && !bIt->second->classLayout.hasVTable)
                nonPolyBaseNeeded.insert(base.baseClassName);
        }
    }
    for (auto& baseName : nonPolyBaseNeeded) {
        emitRTTI(baseName, classTypes.at(baseName), baseClassOf[baseName]);
    }

    // 为全局变量生成数据段
    std::function<void(const std::vector<DeclPtr>&)> emitGlobalVars = [&](const std::vector<DeclPtr>& decls) {
        for (auto& decl : decls) {
            if (decl->kind == NodeKind::Namespace) {
                emitGlobalVars(std::static_pointer_cast<NamespaceDecl>(decl)->declarations);
                continue;
            }
            if (decl->kind != NodeKind::GlobalVar) continue;
            {
                auto gvar = std::static_pointer_cast<GlobalVarDecl>(decl);
                // asmSymbol：命名空间内全局变量名字带 "::"（Math::g_factor），
                // 净化成合法标签（标签端与 %rip 引用端必须一致）
                std::string sym = asmSymbol(gvar->name);
                emitData(std::format("    .globl {}             # 导出全局变量符号", sym));
                emitData("    .align 8                # 8 字节对齐");
                emitData(std::format("{}:                    # 全局变量标签", sym));
                // 初始化式只支持整型/布尔字面量，其余零初始化
                if (gvar->initializer &&
                    gvar->initializer->kind == NodeKind::IntLiteral) {
                    auto lit = std::static_pointer_cast<IntLiteralExpr>(gvar->initializer);
                    emitData(std::format("    .quad {}              # 初始化值：{}",
                                         lit->value, lit->value));
                } else if (gvar->initializer &&
                           gvar->initializer->kind == NodeKind::BoolLiteral) {
                    auto b = std::static_pointer_cast<BoolLiteralExpr>(gvar->initializer);
                    emitData(std::format("    .quad {}              # 初始化值：{}",
                                         b->value ? 1 : 0, b->value ? "true" : "false"));
                } else if (gvar->initializer) {
                    emitData("    .quad 0                # 默认零初始化");
                } else {
                    emitData("    .quad 0                # 零初始化");
                }
                emitData("");
            }
        }
    };
    emitGlobalVars(unit.declarations);

    // ── 生成代码段：所有函数 ──
    for (auto& func : functions) {
        if (func->body) {
            emitFunction(func);
        }
    }

    // 若出现过 dynamic_cast，补发运行时助手 __minicc_dynamic_cast
    if (m_needsDynamicCastHelper) {
        emitDynamicCastHelper();
    }

    // ── 生成只读数据段：字符串字面量 ──
    emitStringLiterals();

    // ── 拼装最终输出 ──
    std::ostringstream output;

    output << "# ═══════════════════════════════════════════════════════════\n";
    output << "# Mini C++ Compiler - x86-64 Assembly Output\n";
    output << "# Generated by minicc\n";
    output << "# ═══════════════════════════════════════════════════════════\n\n";

    output << "    .text\n";
    output << m_code.str();

    if (!m_data.str().empty()) {
        output << "\n    .data\n";
        output << m_data.str();
    }

    if (!m_rodata.str().empty()) {
        output << "\n    .section .rodata\n";
        output << m_rodata.str();
    }

    return output.str();
}

// ═════════════════════════════════════════════════════════════════════════════
// vtable 生成
// ═════════════════════════════════════════════════════════════════════════════
// vtable 在内存中的布局（GCC ABI 风格简化版）：
//
//   _ZTV7MyClass:
//       .quad 0                  # 偏移量到顶层（通常为0）
//       .quad _ZTI7MyClass       # vtable[-1]: RTTI type_info 指针
//       .quad _ZN7MyClass3fooEv  # vtable[0]:  第一个虚函数
//       .quad _ZN7MyClass3barEv  # vtable[1]:  第二个虚函数
//
// 对象的 _vptr 指向 vtable[0]（即跳过前两个槽位）。
// 当需要 RTTI 时，通过 _vptr[-1] 获取 type_info 指针。
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：把一个类的 vtable 实体写入 .data 段。
// 理论：vtable 是"每类一张、每对象一个指针"的间接分派数据结构；
//       继承/override 的差异在语义阶段就已固化进 vtableEntries 列表，
//       这里只是把那张表逐字写成 .quad 指针数组。
// demo（输入）: className="Dog"，vtableEntries=[{Dog_speak, index=0}]
// demo（输出）:
//       .globl _ZTV3Dog
//       .align 8
//   _ZTV3Dog:
//       .quad 0                # offset to top
//       .quad _ZTI3Dog         # vtable[-1]: RTTI 指针
//       .quad Dog_speak        # vtable[0]
void CodeGen::emitVTable(const std::string& className, TypePtr classType) {
    // Itanium ABI 标准符号：_ZTV + <长度><类名>，如 Dog → _ZTV3Dog
    std::string vtableLabel = NameMangler::mangleVTable(className);
    // 配套 typeinfo 符号：_ZTI + <长度><类名>，如 Dog → _ZTI3Dog
    std::string rttiLabel = NameMangler::mangleRTTI(className);

    // .globl 使符号对链接器可见（动态分发要跨目标文件引用这张表）
    emitData(std::format("    .globl {}             # 导出 vtable 符号", vtableLabel));
    // 8 字节对齐：表项是指针，对齐保证单次 8B 读取不跨界
    emitData(std::format("    .align 8                # 8 字节对齐", vtableLabel));
    emitData(std::format("{}:                        # vtable 标签", vtableLabel));

    // ── 主表段（primary vtable segment）──
    // 槽 0：offset-to-top —— 主表恒为 0（主基类子对象就在对象起始处）
    emitData("    .quad 0                    # offset-to-top = 0（主基类子对象与对象起始重合）");
    // 槽 1：type_info 指针，即 vtable[-1]（_vptr 向前退一格即可取到，
    //       typeid/异常类型匹配用它）
    emitData(std::format("    .quad {}       # RTTI type_info 指针（vtable[-1]）",
        rttiLabel));

    // 主表虚函数条目
    for (auto& entry : classType->classLayout.vtableEntries) {
        emitData(std::format("    .quad {}   # vtable[{}]: 虚函数 {}",
            asmSymbol(entry.mangledName), entry.index, entry.mangledName));
    }

    // ── 次表段（secondary vtable segments）──
    // 每个次基类各占一段，表头 ott=-子对象偏移，槽位覆写项用 thunk
    for (auto& base : classType->classLayout.bases) {
        if (base.isPrimary || !base.hasVTable) continue;

        emitData(std::format("    # ── secondary vtable for '{}' @ offset {} ──",
            base.baseClassName, base.offset));
        // offset-to-top = -(子对象偏移)
        emitData(std::format("    .quad {}                   # offset-to-top = {}（次基类 '{}' 子对象偏移取反）",
            -(int)base.offset, -(int)base.offset, base.baseClassName));
        emitData(std::format("    .quad {}       # RTTI type_info 指针（次表段的 typeinfo 与主表相同）", rttiLabel));

        // 次表虚函数条目（覆写项填 thunk 地址，非覆写项填基类函数地址）
        for (size_t i = 0; i < base.entries.size(); ++i) {
            auto& entry = base.entries[i];
            if (entry.isOverridden && entry.thunkAdjust != 0) {
                // 发射 thunk 跳板（简化 mangling：类名_函数名_thunk偏移）
                std::string thunkLabel = asmSymbol(std::format("{}_{}_thunk{}",
                    className, entry.baseFunctionName, -entry.thunkAdjust));
                emitThunk(thunkLabel, asmSymbol(entry.mangledName), entry.thunkAdjust);
                emitData(std::format("    .quad {}   # secondary[{}]: {}（经 thunk 跳板，this 调整量={}）",
                    thunkLabel, i, entry.mangledName, entry.thunkAdjust));
            } else {
                // 未覆写：直接填基类函数地址（调用方传基类 this，无需调整）
                emitData(std::format("    .quad {}   # secondary[{}]: {}（直接引用基类函数）",
                    asmSymbol(entry.mangledName), i, entry.mangledName));
            }
        }
    }

    emitData("");
}

// ─────────────────────────────────────────────────────────────────────────────
// Thunk 跳板发射（多继承覆写调整）
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：为每个次表覆写项发射一个跳板函数。
// 理论（Itanium ABI §2.4）：次基类虚函数被派生类覆写时，调用方传的是
//   次基类指针（this = obj + 子对象偏移），而覆写函数期待最派生类 this。
//   Thunk 在运行期读 vptr[-2]（= offset-to-top = -(子对象偏移)），加到 this
//   上归顶，再跳真实函数。
// 简化声明：本实现直接读 vptr[-2] 而非硬编码偏移（与真实 ABI 一致，
//   但跳板函数名用简化 mangling 而非 Itanium 标准）。
// demo: thunkLabel="D_g_thunk16", funcLabel="D_g", thunkAdjust=-16
//   D_g_thunk16:
//       movq  (%rdi), %rax     # vptr
//       movq  -16(%rax), %rcx  # vptr[-2] = offset-to-top（负偏移）
//       addq  %rcx, %rdi       # this 归顶
//       jmp   D_g              # 跳真实函数
void CodeGen::emitThunk(const std::string& thunkLabel,
                        const std::string& funcLabel, int thunkAdjust) {
    emitComment(std::format("thunk {} → {} (adjust={})", thunkLabel, funcLabel, thunkAdjust));
    emit(std::format(".globl {}             # 导出 thunk 符号供链接器可见", thunkLabel));
    emit(std::format("{}:                   # thunk 跳板入口标签", thunkLabel));
    emit("movq  (%rdi), %rax         # 从 this 首 8 字节读出 _vptr");
    emit("movq  -16(%rax), %rcx      # vptr[-2] = offset-to-top（次基类→最派生偏移）");
    emit("addq  %rcx, %rdi           # this += offset-to-top，归顶到最派生对象");
    emit(std::format("jmp   {}              # 无条件跳转到真实函数（已归顶的 this 直入）", funcLabel));
    emit("");
}

// ═════════════════════════════════════════════════════════════════════════════
// RTTI type_info 生成
// ═════════════════════════════════════════════════════════════════════════════
// type_info 结构（简化版，对齐 Itanium ABI 的 __si_class_type_info 思想）：
//   _ZTI7MyClass:
//       .quad 0                                       # vtable for type_info（简化为 0）
//       .quad .Ltype_name_MyClass                     # 类型名称字符串
//       .quad _ZTI<Base>（或 0）                       # 基类 typeinfo 指针（继承链）
//
// 第三槽是 dynamic_cast 的关键：运行时助手沿它逐级向上走继承链，
// 与目标 typeinfo 地址比较，命中即转型成功。
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：发射类的 type_info 对象（GCC ABI 简化版）。
// 理论：真实 Itanium ABI 中 type_info 本身是有虚表的 C++ 对象，且按继承
//       形态分三种：无基类 __class_type_info、单继承 __si_class_type_info
//       （含 __base_type 字段）、多继承 __vmi_class_type_info（含基类数组）。
//       本项目只支持单继承：统一三槽布局，无基类时第三槽填 0——
//       运行时助手把"空基类指针"当作继承链终点，语义等价。
// demo（输入）: className="Dog", baseClassName="Animal"
// demo（输出）: _ZTI3Dog: .quad 0 / .quad .Ltype_name_Dog / .quad _ZTI6Animal
//               .rodata 中 .Ltype_name_Dog: .string "Dog"
void CodeGen::emitRTTI(const std::string& className, TypePtr classType,
                       const std::string& baseClassName) {
    std::string rttiLabel = NameMangler::mangleRTTI(className);
    std::string nameLabel = std::format(".Ltype_name_{}", asmSymbol(className));

    // RTTI 结构（计数式布局，统一处理 0/1/N 个基类）
    // 对照真实 __vmi_class_type_info：
    //   slot 0: vptr (simplified = 0)
    //   slot 1: name pointer
    //   slot 2: base count N
    //   slots 3..3+2N: [base_ti_0, offset_0, base_ti_1, offset_1, ...]
    emitData(std::format("    .globl {}             # 导出 RTTI 符号", rttiLabel));
    emitData(std::format("    .align 8                # 8 字节对齐"));
    emitData(std::format("{}:                        # typeinfo 标签", rttiLabel));
    emitData(std::format("    .quad 0                    # type_info vtable = 0（简化版，未链接真实 RTTI）"));
    emitData(std::format("    .quad {}                 # 指向类型名称字符串", nameLabel));

    // 基类计数 + 逐基类 typeinfo 指针 + 子对象偏移
    if (!classType->classLayout.bases.empty()) {
        emitData(std::format("    .quad {}                 # 基类计数（MI 计数风格）",
            classType->classLayout.bases.size()));
        for (auto& base : classType->classLayout.bases) {
            emitData(std::format("    .quad {}                 # base[{}] typeinfo 指针",
                NameMangler::mangleRTTI(base.baseClassName), base.baseClassName));
            emitData(std::format("    .quad {}                 # base[{}] 子对象偏移（字节）",
                base.offset, base.baseClassName));
        }
    } else if (!baseClassName.empty()) {
        // 单继承兼容路径（bases 为空但 baseClassName 非空 = 旧格式）
        emitData(std::format("    .quad 1                 # 基类计数 = 1"));
        emitData(std::format("    .quad {}                 # 基类 typeinfo 指针",
            NameMangler::mangleRTTI(baseClassName)));
        emitData(std::format("    .quad 0                 # 基类子对象偏移 = 0（单继承主基类）"));
    } else {
        emitData(std::format("    .quad 0                 # 无基类（基类计数 = 0）"));
    }

    // 类型名称字符串
    emitRodata(std::format("{}:                     # 类型名称标签", nameLabel));
    emitRodata(std::format("    .string \"{}\"           # 类型名称字符串", className));

    emitData("");
}

// ─────────────────────────────────────────────────────────────────────────────
// 字符串字面量
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把生成期间 addStringLiteral 收集的所有字符串统一发射到 .rodata。
// 理论：字符串常量池（constant pool）——同类只读数据集中存放，段的
//       只读/可共享属性由 ELF section 标志统一管理，指令流只引用标签。
// demo: 收集了 ("str_0", "hello") → 输出
//       str_0:
//           .string "hello"        # GAS 自动追加 '\0' 结尾
void CodeGen::emitStringLiterals() {
    for (auto& [label, value] : m_stringLiterals) {
        emitRodata(std::format("{}:                     # 字符串字面量标签", label));
        emitRodata(std::format("    .string \"{}\"        # 字符串内容（自动追加 '\\0'）", value));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 函数生成
// ═════════════════════════════════════════════════════════════════════════════
// 函数结构（x86-64 标准栈帧）：
//
//   func:
//       pushq %rbp              # 保存旧的基指针
//       movq  %rsp, %rbp        # 设置新的基指针
//       subq  $N, %rsp          # 分配局部变量空间
//
//       ... 函数体 ...
//
//       leave                   # 恢复栈帧
//       ret                     # 返回
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：发射一个完整函数 = 序言 + 形参 spill + 函数体 + 尾声。
// 理论要点：
//   · 序言/尾声（prologue/epilogue）：pushq %rbp / movq %rsp,%rbp 建立
//     栈帧，leave/ret 撤销。对应 LLVM 的 PrologEpilogInserter 阶段。
//   · 形参 spill：System V ABI 用寄存器传参，本实现统一把形参存回栈槽
//     （教学简化：访问路径单一，无需寄存器存活分析）。
//   · 成员函数：this 指针占用第一个参数寄存器 rdi（Itanium C++ ABI），
//     显式形参从 rsi 开始依次后移（regIdx = i + 1）。
// demo（输入）: int add(int a, int b) { return a + b; }
// demo（输出）:
//       .globl add
//   add:
//       pushq %rbp
//       movq %rsp, %rbp
//       subq $64, %rsp
//       movq %rdi, -8(%rbp)    # param: a
//       movq %rsi, -16(%rbp)   # param: b
//       ...（函数体：加载 a、b，addq，结果在 rax）
//       leave
//       ret
void CodeGen::emitFunction(FuncDeclPtr func) {
    // 复位每函数状态：局部变量表、栈分配指针、所属类上下文
    m_localVars.clear();
    m_classLocals.clear();
    m_blockDtorStack.clear();
    m_currentStackOffset = 0;
    m_currentClassName = func->ownerClassName;

    // 查找当前类类型
    m_currentClassType = nullptr;
    if (!m_currentClassName.empty() && m_classTypes) {
        auto it = m_classTypes->find(m_currentClassName);
        if (it != m_classTypes->end()) {
            m_currentClassType = it->second;
        }
    }

    // 确定函数名
    // 模板实例等带 mangledName 的优先用修饰名（阶段 4 NameMangler 产出），
    // 普通函数/方法直接用名字（成员方法的"类名_方法名"由 Sema 阶段填好）；
    // 最后统一过 asmSymbol 净化——命名空间函数的名字带 "::"（Sema 阶段
    // processNamespaceDecl 改名而来），不是合法汇编标签。
    std::string funcName = asmSymbol(func->mangledName.empty()
        ? func->name : func->mangledName);

    // .globl 导出符号：main 入口与跨函数/跨文件调用都靠它被链接器找到
    emit(std::format("    .globl {}             # 导出函数符号，使链接器可见", funcName));
    emit(std::format("{}:                       # 函数入口标签", funcName));

    emitComment(std::format("Function: {} (params: {})",
        func->name, func->parameters.size()));

    // 函数序言（Prologue）
    // 理论：pushq %rbp 保存调用者的帧基址（rbp 是 callee-saved 寄存器）；
    //       movq %rsp,%rbp 让新帧获得稳定锚点——此后所有局部访问都是
    //       %rbp+常数偏移，与 %rsp 的瞬时位置（push/pop 变化）无关。
    emit("pushq %rbp                    # 保存调用者的帧基址到栈上");
    emit("movq %rsp, %rbp               # 建立新栈帧：rbp = rsp（此后用 rbp+偏移访问局部）");

    // 预留局部变量空间
    // ★ P1：不再固定 64 字节——栈上类对象可能远超 8 字节，固定预留会
    //   在"类对象稍多几个"时写穿帧底（踩坏调用者栈）。先扫一遍函数体
    //   数出所有局部声明的总尺寸（类对象按布局 totalSize 对齐 8，其余
    //   按 8 字节槽），再一次性减出来。
    //   对照真实编译器：即 LLVM PrologEpilogInserter 的帧大小计算，
    //   此处为"先数后减"的朴素一遍扫描；嵌套块里的声明也数进去
    //   （宁可多留，不做精确复用——与槽位不回收的语义保持一致）。
    //   保底 64 字节：兼顾临时 pushq 周转（表达式求值的栈暂存）。
    uint32_t frameSize = estimateFrameSize(func);
    if (frameSize < 64) frameSize = 64;
    frameSize = (frameSize + 15) / 16 * 16;   // 16 字节对齐（System V）
    emit(std::format("subq ${}, %rsp              # 预留局部变量栈空间（16B 对齐）", frameSize));

    // 保存参数到栈上
    // System V 整数参数寄存器序列；第 7 个及以后的参数应经栈传递
    // （本项目不支持 >6 参数）
    static const char* paramRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    int paramOffset = -8;

    // 如果是成员函数，第一个参数是 this 指针
    // Itanium C++ ABI：this 视为隐式第 0 参数，独占 rdi；
    // 登记为 "this" 槽位后，emitThis/裸字段访问都从这里加载
    if (!func->ownerClassName.empty()) {
        m_localVars["this"] = paramOffset;
        emit(std::format("movq %{}, {}(%rbp)         # 保存 this 指针到栈槽", paramRegs[0], paramOffset));
        paramOffset -= 8;
    }

    // 形参 spill：非成员函数形参 i 用第 i 个寄存器；
    // 成员函数整体右移一位（regIdx = i+1），因为 rdi 已被 this 占用
    for (size_t i = 0; i < func->parameters.size() && i < 6; i++) {
        size_t regIdx = func->ownerClassName.empty() ? i : i + 1;
        if (regIdx < 6) {
            m_localVars[func->parameters[i].name] = paramOffset;
            emit(std::format("movq %{}, {}(%rbp)         # 形参 {} 从寄存器 spill 到栈",
                paramRegs[regIdx], paramOffset, func->parameters[i].name));
            paramOffset -= 8;
        }
    }

    // 栈分配指针接到形参区末尾，之后的局部变量从这里继续向低地址分配
    m_currentStackOffset = paramOffset;

    // 构造函数：如果是构造函数，安装所有 vptr（多继承时可能有多个），并执行初始化列表
    if (func->kind == NodeKind::Constructor) {
        auto ctor = std::static_pointer_cast<ConstructorDecl>(func);
        std::string vtableLabel;
        if (m_currentClassType && m_currentClassType->classLayout.hasVTable) {
            vtableLabel = NameMangler::mangleVTable(m_currentClassName);
            emit("movq -8(%rbp), %rax         # 加载 this 指针");

            // 主 vptr：偏移 0，指向主表段（vtableLabel + 16）
            emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
            emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
            emit("movq %rcx, (%rax)           # 安装主 _vptr 到对象首 8 字节");

            // 次 vptr：每个次基类各一个，指向对应次表段
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (base.isPrimary || !base.hasVTable) continue;
                uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                    vtableLabel, base.baseClassName));
                emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                emit(std::format("movq %rcx, {}(%rax)     # 安装次 _vptr（偏移 {} 处）",
                    base.offset, base.baseClassName));
            }
        }
        // 收集是否有基类构造调用
        bool hasBaseCtors = false;
        for (auto& init : ctor->initList) {
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (init.memberName == base.baseClassName) { hasBaseCtors = true; break; }
            }
            if (hasBaseCtors) break;
        }

        for (auto& init : ctor->initList) {
            if (!m_currentClassType) continue;

            // 检查是否为基类构造调用（memberName 匹配 baseClassName）
            bool isBaseCtor = false;
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (init.memberName == base.baseClassName) {
                    isBaseCtor = true;
                    // 调用基类构造函数（base_className_N 格式）
                    std::string baseCtorName = base.baseClassName + "_" + base.baseClassName;
                    if (!init.arguments.empty()) {
                        baseCtorName += "_" + std::to_string(init.arguments.size());
                    }
                    // 计算实参
                    for (size_t ai = 0; ai < init.arguments.size() && ai < 5; ai++) {
                        emitExpr(init.arguments[ai]);
                        emit("pushq %rax               # 暂存实参值到栈上");
                    }
                    for (int ai = static_cast<int>(init.arguments.size()) - 1; ai >= 0 && ai < 5; ai--) {
                        static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                        emit(std::format("popq %{}              # 逆序弹出实参到寄存器", regs[ai]));
                    }
                    // this 指针：主基类用原始 this，次基类用 this + offset
                    emit("movq -8(%rbp), %rdi       # 加载 this 指针作为基类构造第 0 参数");
                    if (!base.isPrimary) {
                        emit(std::format("addq ${}, %rdi          # 次基类偏移调整（'{}'）",
                            base.offset, base.baseClassName));
                    }
                    emit(std::format("callq {}                # 调用基类构造函数 '{}'",
                        baseCtorName, base.baseClassName));
                    break;
                }
            }

            if (!isBaseCtor) {
                // 字段初始化
                const FieldInfo* field = m_currentClassType->classLayout.findField(init.memberName);
                if (field && !init.arguments.empty()) {
                    if (field->type && field->type->isClass()) {
                        // ── 嵌套类字段：f(7,9) 是【在子对象上调构造函数】──
                        // 子对象内联在 this+field->offset 处，构造调用与基类
                        // 构造同构：实参进寄存器，this = 原始 this + 字段偏移。
                        // 历史 bug：曾按标量走 movq 把实参裸写进子对象头部，
                        // f(7) 变成"把 7 当指针存进 f 的前 8 字节"。
                        // 对照 clang：初始化列表 f(7,9) 生成对
                        // Five::Five(int,int) 的直接调用（this 已调整）。
                        const std::string& cn = field->type->name;
                        std::string ctorName = cn + "_" + cn + "_" +
                            std::to_string(init.arguments.size());
                        for (size_t ai = 0; ai < init.arguments.size() && ai < 5; ai++) {
                            emitExpr(init.arguments[ai]);
                            emit("pushq %rax               # 暂存嵌套构造实参");
                        }
                        for (int ai = static_cast<int>(init.arguments.size()) - 1; ai >= 0 && ai < 5; ai--) {
                            static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                            emit(std::format("popq %{}              # 逆序弹出实参到寄存器", regs[ai]));
                        }
                        emit("movq -8(%rbp), %rdi       # 加载 this 指针");
                        emit(std::format("addq ${}, %rdi          # 调整到嵌套子对象 '{}'（偏移 {}）",
                            field->offset, field->name, field->offset));
                        emit(std::format("callq {}                # 调用嵌套类构造函数", ctorName));
                    } else {
                        // ── 标量字段：按宽度选存储指令 ──
                        // 历史 bug：一律 movq 写 8 字节，4 字节 int 字段会踩坏
                        // 相邻字段（tag@0 写 8B → 踩掉 f 的 b1..b4）。
                        // 对照 clang：按 TI.Width 选 movl/movb/movq。
                        emitExpr(init.arguments[0]);
                        emit("movq -8(%rbp), %rcx       # 加载 this 指针");
                        if (field->size == 1) {
                            emit(std::format("movb %al, {}(%rcx)    # 初始化字段 {}（偏移 {}，1B）",
                                field->offset, field->name, field->offset));
                        } else if (field->size <= 4) {
                            emit(std::format("movl %eax, {}(%rcx)    # 初始化字段 {}（偏移 {}，4B）",
                                field->offset, field->name, field->offset));
                        } else {
                            emit(std::format("movq %rax, {}(%rcx)    # 初始化字段 {}（偏移 {}，8B）",
                                field->offset, field->name, field->offset));
                        }
                    }
                }
            }
        }

        // 基类构造后重新安装 D 的 vptr（基类构造会覆盖为基类 vptr）
        if (hasBaseCtors && m_currentClassType->classLayout.hasVTable) {
            emitComment("re-install derived vptrs after base constructors");
            emit("movq -8(%rbp), %rax         # 加载 this 指针");
            emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
            emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
            emit("movq %rcx, (%rax)           # 重新安装主 _vptr（覆盖基类写入的）");
            for (auto& base : m_currentClassType->classLayout.bases) {
                if (base.isPrimary || !base.hasVTable) continue;
                uint32_t vptrOff = base.vtableSegmentOffset + 16;
                emit("movq -8(%rbp), %rax         # 加载 this 指针");
                emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                    vtableLabel, base.baseClassName));
                emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOff));
                emit(std::format("movq %rcx, {}(%rax)     # 重新安装次 _vptr（偏移 {} 处）",
                    base.offset, base.baseClassName));
            }
        }
    }

    // 生成函数体
    // ★ 函数级析构层：直接声明在函数体顶层（不在任何 {} 块内）的类对象
    //   也要有人管——压一层"函数层"，正常落尾（fallthrough）路径在尾声
    //   前逆序析构该层。（中途 return 跳过析构：教学简化，见文档 12）
    m_blockDtorStack.push_back({});
    if (func->body) {
        for (auto& stmt : func->body->statements) {
            emitStmt(stmt);
        }
    }

    // 落尾路径：析构函数层登记的栈对象（逆序）
    {
        std::vector<std::string> toDestroy = std::move(m_blockDtorStack.back());
        m_blockDtorStack.pop_back();
        for (auto it = toDestroy.rbegin(); it != toDestroy.rend(); ++it) {
            auto ci = m_classLocals.find(*it);
            if (ci == m_classLocals.end()) continue;
            emitComment(std::format("~{}() auto at function end (RAII)",
                ci->second.className));
            emitClassDtorCall(ci->second.className, ci->second.offset);
        }
    }

    // 检查最后一条语句是否是 return（避免重复 leave/ret）
    bool endsWithReturn = false;
    if (func->body && !func->body->statements.empty()) {
        endsWithReturn =
            func->body->statements.back()->kind == NodeKind::Return;
    }

    // 如果函数没有显式 return，添加默认返回
    if (!endsWithReturn) {
        if (func->kind == NodeKind::Constructor) {
            emit("movq -8(%rbp), %rax         # 构造函数返回 this 指针");
        } else if (func->returnType && func->returnType->isVoid()) {
            emit("movq $0, %rax               # void 函数返回 0");
        }

        // 函数尾声（Epilogue）
        // leave = movq %rbp,%rsp; popq %rbp（一步回收栈帧+恢复旧帧基址）
        // ret   = 弹出返回地址跳回调用方；返回值已按约定留在 rax
        emit("leave                         # 恢复栈帧（movq %rbp,%rsp; popq %rbp）");
        emit("ret                           # 返回调用者（从栈上弹出返回地址）");
    }
    emit("");
}

// ═════════════════════════════════════════════════════════════════════════════
// 语句生成
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：语句分发器——按 AST 节点动态类型派发到对应 emit 函数。
// 理论：这就是"lowering（降级）"的入口：高级结构（声明/赋值/if/while）
//       逐层展开为线性指令序列。对应 LLVM SelectionDAG 的类型匹配与
//       Legalize 阶段，区别是这里用 dynamic_pointer_cast 手写派发。
void CodeGen::emitStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    // 一次虚表跳转就落到对应的 visit（改造前是逐级 dynamic_pointer_cast 试探）。
    stmt->accept(*this);
}

// 复合语句：顺序发射各子语句 + ★块尾逆序析构本块声明的类对象（RAII）★。
// 理论（[basic.stc.dcl] + [class.dtor]）：块作用域存储期的对象，其析构
//       在控制流离开块时自动发生，且顺序与构造相反（LIFO）——先构造的
//       后析构。这正是 RAII 的机器实现：不需要任何显式调用，编译器在块尾
//       "代劳"。
// 实现：进入块时在 m_blockDtorStack 压一层空列表；块内 emitVarDecl 每声明
//       一个类对象就往当前层追加名字；块结束时逆序发射析构调用后弹层。
//       嵌套块天然正确：内层弹层只析构自己那层的对象，外层列表原封不动
//       （与符号表 enterScope/exitScope 同构）。
// 简化：中途 return（emitReturn 直接 leave/ret）会跳过析构——真实编译器
//       会在每个退栈点补析构调用，教学版接受此差距并在文档中注明。
void CodeGen::visit(BlockStmt& block) {
    m_blockDtorStack.push_back({});   // enter scope：本块专属析构层

    for (auto& stmt : block.statements) {
        emitStmt(stmt);
    }

    // exit scope：取出本块登记的类对象，逆序析构（构造的镜像顺序）
    std::vector<std::string> toDestroy = std::move(m_blockDtorStack.back());
    m_blockDtorStack.pop_back();

    for (auto it = toDestroy.rbegin(); it != toDestroy.rend(); ++it) {
        auto ci = m_classLocals.find(*it);
        if (ci == m_classLocals.end()) continue;
        emitComment(std::format("~{}() auto at block end (RAII, reverse order)",
            ci->second.className));
        emitClassDtorCall(ci->second.className, ci->second.offset);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 栈对象析构调用（块尾析构与"析构但不 free"的统一发射点）
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：发射"对栈上对象调用析构函数"的完整序列：
//   ① leaq off(%rbp), %rdi —— this = 对象地址（栈对象没有指针变量，
//      地址是编译期常数，直接取址）；
//   ② 定虚实：查该类 vtable 条目有无 "dtor"（与 emitDelete 同一判据）——
//      虚析构走 emitVirtualCall 三部曲（运行期定派，[class.dtor]/4），
//      否则静态 callq {Class}_dtor（符号约定见 registerFunction）。
// 与 emitDelete 的区别：没有后续 `callq free`——栈帧空间随函数返回自动
//      回收（[basic.stc.dcl]），这正是栈对象相对堆对象的便宜之处。
void CodeGen::emitClassDtorCall(const std::string& className, int rbpOffset) {
    emit(std::format("leaq {}(%rbp), %rdi       # this = 栈上对象地址（直接取址，无指针变量）", rbpOffset));

    // 虚析构？查 vtable 条目（同 emitDelete 的子串匹配约定）
    if (m_classTypes) {
        auto it = m_classTypes->find(className);
        if (it != m_classTypes->end()) {
            for (auto& entry : it->second->classLayout.vtableEntries) {
                if (entry.mangledName.find("dtor") != std::string::npos) {
                    emitVirtualCall(className, "dtor", {}, entry.index);
                    return;
                }
            }
        }
    }
    emit(std::format("callq {}            # 静态调用析构函数（非虚析构路径）",
        asmSymbol(className + "_dtor")));
}

// ─────────────────────────────────────────────────────────────────────────────
// 帧大小预估（序言的 subq $N 用）
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：扫一遍函数体，累加每条变量声明的栈占用：
//   · 类类型局部 → ClassLayout.totalSize 对齐 8（与 emitVarDecl 同规则）
//   · 其余（int/指针/形参外局部）→ 8 字节槽
// 递归进 if/while/嵌套块——块尾析构后槽位并不复用（emitBlockStmt 的
// 简化语义），所以最保守的算法是"全部加起来"：宁可帧大，不可写穿。
// 注意：只数声明，不数表达式求值用的临时 push 空间（emitFunction 保底
// 64 字节已覆盖常见深度；嵌套极深的表达式属可接受的教学差距）。
uint32_t CodeGen::estimateBlockSize(std::shared_ptr<BlockStmt> block) {
    if (!block) return 0;
    uint32_t total = 0;
    for (auto& stmt : block->statements) {
        // 按节点种类分派。这是【取值型】递归（累加 total），且各分支都要
        // shared_ptr 下钻 ⇒ NodeKind 标签分派 —— 判据同 Sema / Instantiator。
        switch (stmt->kind) {
            case NodeKind::VarDecl: {
                auto v = std::static_pointer_cast<VarDeclStmt>(stmt);
                uint32_t size = 8;
                if (v->declaredType && !v->declaredType->isPointer() && m_classTypes) {
                    auto cit = m_classTypes->find(v->declaredType->name);
                    if (cit != m_classTypes->end() && cit->second->isClass()) {
                        size = cit->second->classLayout.totalSize;
                        if (size < 8) size = 8;
                        size = (size + 7) / 8 * 8;
                    }
                }
                total += size;
                break;
            }
            case NodeKind::Block:
                total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(stmt));
                break;
            case NodeKind::If: {
                auto i = std::static_pointer_cast<IfStmt>(stmt);
                if (i->thenBranch && i->thenBranch->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(i->thenBranch));
                if (i->elseBranch && i->elseBranch->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(i->elseBranch));
                break;
            }
            case NodeKind::While: {
                auto w = std::static_pointer_cast<WhileStmt>(stmt);
                if (w->body && w->body->kind == NodeKind::Block)
                    total += estimateBlockSize(std::static_pointer_cast<BlockStmt>(w->body));
                break;
            }
            default:
                break;   // 其余语句（表达式/return/delete…）不占栈槽
        }
    }
    return total;
}

// 帧大小 = 局部变量总尺寸 + 序言区（帧基 + 形参 spill 槽）。
//
// ★ 为什么必须补上序言区（曾经的 bug）：
//   局部偏移的分配从 -8 起步（emitFunction 里 paramOffset = -8），每个
//   spill 的形参再各占 8 字节，局部变量接着往下排；而 estimateBlockSize
//   只数了局部变量的尺寸。于是帧比实际用到的最深偏移【浅了 8~56 字节】，
//   最深那几个槽位落在 rsp 之下，被两样东西踩掉：
//     · 表达式求值的 `pushq %rax`（左操作数暂存）—— 正好落在 rsp-8；
//     · `callq` 压入的返回地址 —— 同样在 rsp-8。
//   症状极具迷惑性：变量单独读出来是对的，一旦参与"需要压栈暂存"的
//   二元表达式、或此前发生过一次函数调用，读到的就是邻居的值
//   （`int a..h; return a + h;` 返回 2 而不是 9）。
//   对照真实编译器：帧大小是序言与局部布局【同一份】分配器的产物
//   （LLVM PrologEpilogInserter / X86FrameLowering::determineFrameLayout），
//   不存在"两处各算一份、彼此对不上"的可能。
uint32_t CodeGen::estimateFrameSize(FuncDeclPtr func) {
    uint32_t total = estimateBlockSize(func->body);

    uint32_t slots = 1;                            // 帧基：paramOffset 从 -8 起
    if (!func->ownerClassName.empty()) slots++;    // this 先占 rdi，spill 一个槽
    for (size_t i = 0; i < func->parameters.size() && i < 6; i++) {
        size_t regIdx = func->ownerClassName.empty() ? i : i + 1;
        if (regIdx < 6) slots++;
    }
    total += slots * 8;
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// 多级继承 upcast 偏移计算（3+ 层）
// ─────────────────────────────────────────────────────────────────────────────
// BFS 遍历继承图，累积从 derivedClass 到 targetBase 的子对象偏移。
// 返回值：targetBase 子对象在 derivedClass 完整对象中的起始偏移（字节）。
// 返回 0 表示：① 同一类；② 主基类（偏移 0）；③ 无继承关系。
// demo：class A; class B : A; class C : B;
//   getBaseOffset("C", "A") = offset(B in C) + offset(A in B)
// 对照 clang：CastExpr::getSubExpr()->getType()->getAsCXXRecordDecl() 的
//   getASTContext().getASTRecordLayout(BaseDecl).getBaseClassOffset()。
uint32_t CodeGen::getBaseOffset(const std::string& derivedClassName,
                                const std::string& targetBase) const {
    if (derivedClassName == targetBase) return 0;
    if (!m_classTypes) return 0;

    // BFS 队列：(当前类名, 累积偏移)
    std::vector<std::pair<std::string, uint32_t>> queue;
    queue.push_back({derivedClassName, 0});
    size_t head = 0;

    while (head < queue.size()) {
        auto [currentName, accOffset] = queue[head++];

        auto typeIt = m_classTypes->find(currentName);
        if (typeIt == m_classTypes->end()) continue;

        for (const auto& base : typeIt->second->classLayout.bases) {
            uint32_t newOffset = accOffset + base.offset;

            if (base.baseClassName == targetBase) {
                return newOffset;
            }

            queue.push_back({base.baseClassName, newOffset});
        }
    }

    return 0;  // 无继承关系或主基类
}

// ─────────────────────────────────────────────────────────────────────────────
// 变量声明
// ─────────────────────────────────────────────────────────────────────────────
// auto x = 10; 到了这里，auto 已经被替换为 int，所以直接生成：
//   movq $10, -offset(%rbp)
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：为新变量分配栈槽，并生成初始化。两条路径：
//   ① 标量路径（int/bool/指针）：8 字节槽 + 初始化式求值写入。
//   ② ★类对象路径（P1 新增，[basic.stc.dcl] 栈对象）：
//      分配 totalSize 字节（对齐 8）→ 零初始化 → 安装 _vptr（如有）→
//      调用构造函数（this = leaq 取栈上地址）。对象地址本身登记进
//      m_classLocals，块尾由 emitBlockStmt 逆序发射析构。
//      对照真实编译器：即"在栈上 placement 构造"——new 在堆上做的事，
//      这里直接在帧内完成（无 malloc，故析构后也无需 free）。
// demo: int x = 10;  →  movq $10, %rax
//                       movq %rax, -32(%rbp)    # store to x
//       int y;       →  movq $0, -40(%rbp)      # 无初始化式则零初始化
//       Dog d;       →  subq 帧内留 sizeof(Dog) 字节
//                       movq $0, off(%rbp)…      # 全部清零
//                       leaq _ZTV3Dog(%rip)+16, %rcx; movq %rcx, off(%rbp)
//                       leaq off(%rbp), %rdi; callq Dog_Dog
void CodeGen::visit(VarDeclStmt& decl) {
    // ── 路径②：声明类型是类 → 栈对象（RAII 的地基）──
    // 判据：declaredType 的类名命中全局类表（auto 已在 Sema 阶段替换完，
    // 此处看到的必然是最终类型；指针/引用不属于此路径）。
    if (decl.declaredType && !decl.declaredType->isPointer()
        && m_classTypes) {
        auto cit = m_classTypes->find(decl.declaredType->name);
        if (cit != m_classTypes->end() && cit->second->isClass()) {
            auto& layout = cit->second->classLayout;

            // 尺寸：布局算好的 totalSize，向上对齐 8（至少 8，保证
            // leaq 出来的地址 8 字节对齐，满足 System V 约定）
            uint32_t size = layout.totalSize;
            if (size < 8) size = 8;
            size = (size + 7) / 8 * 8;

            // 分配：栈向低地址增长，按对象实际尺寸推进分配指针
            // （不再固定 8 字节/变量——这是类对象支持的关键一步）
            m_currentStackOffset -= static_cast<int>(size);
            int offset = m_currentStackOffset;

            // 双表登记：m_localVars 供"名字 → 偏移"通用查询；
            // m_classLocals 额外记住"这是类对象"，供 emitVar 取址、
            // 块尾析构、清零三处使用
            m_localVars[decl.name] = offset;
            m_classLocals[decl.name] = ClassLocalInfo{
                cit->second->name, offset, size};
            if (!m_blockDtorStack.empty()) {
                m_blockDtorStack.back().push_back(decl.name);
            }

            emitComment(std::format("stack object {} : {} ({} bytes, RAII)",
                decl.name, cit->second->name, size));

            // ① 零初始化整个对象（8 字节一拍）——构造函数写入字段前，
            //    其余字节必须是确定的 0（对应真实编译器的"默认成员初始化
            //    + 填充清零"；int 字段若构造未写就是 0，行为可观测）
            for (uint32_t i = 0; i < size; i += 8) {
                emit(std::format("movq $0, {}(%rbp)    # 零初始化 {} 偏移 +{}",
                    offset + static_cast<int>(i), decl.name, i));
            }

            // ② 安装 _vptr（仅多态类）：与 emitNew/构造函数内部安装同一公式
            //    ——对象偏移 0 处 = &_ZTV + 16（跳过 offset-to-top 与 RTTI）
            if (layout.hasVTable) {
                std::string vtableLabel = NameMangler::mangleVTable(cit->second->name);
                emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
                emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
                emit(std::format("movq %rcx, {}(%rbp)    # 安装主 _vptr 到栈对象",
                    offset));

                // 次 vptr：每个次基类各一个
                for (auto& base : layout.bases) {
                    if (base.isPrimary || !base.hasVTable) continue;
                    uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                    emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                        vtableLabel, base.baseClassName));
                    emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                    emit(std::format("movq %rcx, {}(%rbp)    # 安装次 _vptr（偏移 {} 处）",
                        offset + static_cast<int>(base.offset), base.baseClassName));
                }
            }

            // ③ 调用构造函数：this = 栈上对象地址（leaq 取址）
            //    零参：约定符号名 类名_类名（与 emitNew、registerFunction 一致）；
            //    带参：用 Sema 选定并回填的 ctorSymbol —— mangling 是有状态的
            //    （同名多参会追加参数个数后缀），CodeGen 自己拼不出来。
            //    实参传递与 emitNew 同一套：先逐个求值压栈暂存，再逆序弹出
            //    到 rsi/rdx/rcx/r8/r9（rdi 被 this 占用）。
            if (!decl.ctorArgs.empty() && !decl.ctorSymbol.empty()) {
                for (size_t i = 0; i < decl.ctorArgs.size() && i < 5; i++) {
                    emitExpr(decl.ctorArgs[i]);
                    emit("pushq %rax                  # 构造实参压栈暂存");
                }
                for (int i = static_cast<int>(decl.ctorArgs.size()) - 1; i >= 0 && i < 5; i--) {
                    static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                    emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
                }
                emit(std::format("leaq {}(%rbp), %rdi       # this = 栈对象地址 &{}",
                    offset, decl.name));
                emit(std::format("callq {}               # 调用构造函数（带参重载）",
                    asmSymbol(decl.ctorSymbol)));
                return;
            }

            emit(std::format("leaq {}(%rbp), %rdi       # this = 栈对象地址 &{}",
                offset, decl.name));
            emit(std::format("callq {}               # 调用构造函数",
                asmSymbol(cit->second->name + "_" + cit->second->name)));
            return;
        }
    }

    // ── 路径①：标量 —— 8 字节槽 ──
    m_currentStackOffset -= 8;
    m_localVars[decl.name] = m_currentStackOffset;

    if (decl.initializer) {
        emitComment(std::format("var {} = ...", decl.name));
        emitExpr(decl.initializer);

        // Upcast 指针调整：D* → B* 时需加偏移（多继承次基类）
        if (decl.declaredType && decl.declaredType->isPointer() &&
            decl.initializer->resolvedType && decl.initializer->resolvedType->isPointer()) {

            std::string baseName = decl.declaredType->pointeeType->name;
            std::string derivedName = decl.initializer->resolvedType->pointeeType->name;

            uint32_t adjust = getBaseOffset(derivedName, baseName);

            if (adjust > 0) {
                emitComment(std::format("upcast adjust: {} -> {} (+{} bytes)",
                    derivedName, baseName, adjust));
                emit(std::format("addq ${}, %rax          # 指针 upcast 偏移调整（+{} 字节）", adjust, adjust));
            }
        }

        emit(std::format("movq %rax, {}(%rbp)       # 存储到局部变量 {}",
            m_currentStackOffset, decl.name));
    } else {
        // 零初始化
        emit(std::format("movq $0, {}(%rbp)         # {} 零初始化",
            m_currentStackOffset, decl.name));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 赋值语句
// ─────────────────────────────────────────────────────────────────────────────
// 普通字段赋值的消除：
//   源码: u.age = 20;
//   编译期: age 的偏移量是 4（假设）
//   汇编: movl $20, 4(%rax)    # 直接用偏移量，字段名已消失
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：生成赋值指令，两条路径：
//   ① 普通变量 x = e    → e 求值进 rax，写入 x 的栈槽
//   ② 字段赋值 o.f = e  → 对象地址进 rcx，按 ClassLayout 查到的字段偏移
//      生成 movl %eax, off(%rcx) —— 字段名在此"降级"为数字偏移量
// demo: u.age = 20;（age 偏移 0）→
//       movq $20, %rax             # 先算右值
//       <对象地址> → %rcx
//       movq $20, %rax             # 重算右值（见下方说明）
//       movl %eax, 0(%rcx)         # .age (offset 0)
// 注：字段路径会先算一遍右值再算对象地址、然后重新计算右值（右值被求值
//     两次）——教学简化，假定初始化式无副作用；真实编译器用寄存器分配
//     避免重复求值。
void CodeGen::visit(AssignStmt& stmt) {
    // 计算右值到 rax
    emitExpr(stmt.value);

    // 赋值目标有三态：① 下标 v[i]（糖化成 v.set(i,value) 调用）
    // ② 裸名（局部变量 / 裸字段 / 全局变量，按作用域优先级）
    // ③ 成员访问 o.f（按 ClassLayout 查到偏移量后直接写内存）。
    // 三者互斥 —— 按节点种类一次 switch，跳表分派替代 RTTI 试探链。
    switch (stmt.target->kind) {
        case NodeKind::Index: {
            auto idx = std::static_pointer_cast<IndexExpr>(stmt.target);
            // ─── 下标赋值（写形态）→ 糖化为 v.set(i, value) ───
            // 读走 at()、写走 set()，是 at()/set() 约定的另一半
            // （[expr.ass] 左值语义的降级：赋值目标必须是函数调用形态）。
            // 此刻 rax 已是右值（函数开头统一求值）：压栈保序，
            // 依次取 this 与下标，再按 System V 装参 rdi/rsi/rdx。
            emitComment("subscript assign v[i] = ... → desugar to v.set(i, value)");
            TypePtr objType = idx->object->resolvedType;
            if (objType && objType->isPointer()) {
                objType = objType->pointeeType;
            }
            std::string className = (objType && objType->isClass()) ? objType->name : "";

            emit("pushq %rax                  # 暂存右值（待写入的值）到栈上");
            emitExpr(idx->index);
            emit("pushq %rax                  # 暂存下标值到栈上");
            emitExpr(idx->object);
            emit("movq %rax, %rdi             # this = 容器对象地址（第 0 参数）");
            emit("popq %rsi                   # arg1: 弹出下标 i 到 rsi");
            emit("popq %rdx                   # arg2: 弹出右值 value 到 rdx");
            emit(std::format("callq {}_set              # 调用 v.set(i, value) 完成写入", className));
        } break;
        case NodeKind::Var: {
            auto var = std::static_pointer_cast<VarExpr>(stmt.target);
            // ★ 裸字段名赋值：方法体内写 age = a; 等价于 this->age = a;
            //   （与 emitVar 的"裸字段读"路径对称）。必须在全局兜底之前检查，
            //   否则降级成 movq %rax, age(%rip) 全局写 → 链接期 undefined reference。
            //   注意顺序：局部表优先（this 指针也登记在 m_localVars），
            //   之后才是字段，最后才轮到全局。
            bool fieldHandled = false;
            if (m_localVars.find(var->name) == m_localVars.end()
                && !m_currentClassName.empty() && m_currentClassType) {
                auto field = m_currentClassType->classLayout.findField(var->name);
                auto thisIt = m_localVars.find("this");
                if (field && thisIt != m_localVars.end()) {
                    // rax 已是右值（emitAssign 开头统一求值）：压栈暂存 →
                    // 取 this → 弹出右值 → 写入 [this + offset]
                    emit("pushq %rax                  # 暂存右值到栈上");
                    emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
                    emit("movq %rax, %rcx                # this 地址存入 rcx");
                    emit("popq %rax                    # 弹出右值回 rax");
                    emit(std::format("movl %eax, {}(%rcx)    # .{} = ...（偏移 {}，4 字节写入）",
                        field->offset, var->name, field->offset));
                    fieldHandled = true;
                }
            }
            if (!fieldHandled) {
                auto it = m_localVars.find(var->name);
                if (it != m_localVars.end()) {
                    emit(std::format("movq %rax, {}(%rbp)       # 赋值局部变量 {}",
                        it->second, var->name));
                } else {
                    // 全局变量赋值（asmSymbol 净化 :: 等非法标签字符，
                    // 与 .data 段标签发射端一致）
                    emit(std::format("movq %rax, {}(%rip)       # 全局变量 {} = ...",
                        asmSymbol(var->name), var->name));
                }
            }
        } break;
        case NodeKind::Member: {
            auto mem = std::static_pointer_cast<MemberExpr>(stmt.target);
            // ─── 字段访问的消除 ───
            // 将 obj.field = value 转化为 [objAddr + fieldOffset] = value
            emitComment(std::format("member assign: .{} = ...", mem->memberName));

            // 先计算对象地址到 rcx
            emitExpr(mem->object);
            emit("movq %rax, %rcx                # 对象地址存入 rcx");

            // 计算右值到 rax
            emitExpr(stmt.value);

            // 查找字段偏移量
            if (mem->object->resolvedType) {
                TypePtr objType = mem->object->resolvedType;
                if (mem->isArrow && objType->isPointer()) {
                    objType = objType->pointeeType;
                }
                if (objType->isClass()) {
                    auto field = objType->classLayout.findField(mem->memberName);
                    if (field) {
                        emit(std::format("movl %eax, {}(%rcx)    # 写入字段 .{}（偏移 +{}）",
                            field->offset, mem->memberName, field->offset));
                        return;
                    }
                }
            }

            // 如果找不到偏移量，生成通用代码
            emit("movq %rax, (%rcx)              # 成员赋值（偏移未知，默认偏移 0）");
        } break;
        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// return 语句
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：生成 return —— 把返回值算进 rax，然后就地撤销栈帧返回。
// 理论：System V ABI 规定整数返回值在 rax；leave 等价于
//       movq %rbp,%rsp; popq %rbp（回收栈帧），ret 弹返回地址跳回调用方。
//       这里的 leave/ret 与 emitFunction 尾声相同——任意位置 return 都
//       能正确退栈，因为所有局部都锚定在 %rbp 上。
// demo: return a + 1;（a 在 -8(%rbp)）→
//       movq -8(%rbp), %rax    # load a
//       pushq %rax / movq $1,%rax / movq %rax,%rcx / popq %rax
//       addq %rcx, %rax        # 结果已在 rax
//       leave
//       ret
void CodeGen::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        emitComment("return expr");
        emitExpr(stmt.value);
        // 返回值已经在 rax 中
    } else {
        emit("movq $0, %rax               # void 返回，结果置 0");
    }
    emit("leave                         # 恢复栈帧（movq %rbp,%rsp; popq %rbp）");
    emit("ret                           # 返回调用者（从栈上弹出返回地址）");
}

// ─────────────────────────────────────────────────────────────────────────────
// delete 语句
// ─────────────────────────────────────────────────────────────────────────────
void CodeGen::visit(DeleteStmt& stmt) {
    emitComment("delete pointer");
    emitExpr(stmt.pointerExpr);
    emit("movq %rax, %rdi             # 待删除指针传入第 0 参数寄存器");
    emit("pushq %rdi                  # 暂存指针（析构后还要 free）");

    // 检查是否需要调用析构函数
    TypePtr ptrType = stmt.pointerExpr->resolvedType;
    if (ptrType && ptrType->isPointer() && ptrType->pointeeType && ptrType->pointeeType->isClass()) {
        std::string className = ptrType->pointeeType->name;
        if (m_classTypes) {
            auto it = m_classTypes->find(className);
            if (it != m_classTypes->end()) {
                auto& layout = it->second->classLayout;
                bool hasVirtualDtor = false;
                uint32_t dtorIndex = 0;
                for (auto& entry : layout.vtableEntries) {
                    if (entry.mangledName.find("dtor") != std::string::npos) {
                        hasVirtualDtor = true;
                        dtorIndex = entry.index;
                        break;
                    }
                }
                if (hasVirtualDtor) {
                    emitVirtualCall(className, "dtor", {}, dtorIndex);
                } else {
                    emit(std::format("callq {}           # 静态调用析构函数",
                        asmSymbol(className + "_dtor")));
                }
            }
        }
    }

    emit("popq %rdi                   # 恢复指针准备 free");
    emit("callq free                  # 释放堆内存");
}

// ─────────────────────────────────────────────────────────────────────────────
// if 语句
// ─────────────────────────────────────────────────────────────────────────────
// 生成模式：
//   <condition>
//   testq %rax, %rax
//   je .L_else_N
//   <then branch>
//   jmp .L_end_N
//   .L_else_N:
//   <else branch>
//   .L_end_N:
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 if/else 降级为"条件跳转 + 标签"。
// 理论：结构化控制流在机器层面只剩两条原语——测试与跳转。
//       testq %rax,%rax 用 rax 与自身按位与来置标志位（结果非 0 则
//       ZF=0）；je（jump if zero）在 ZF=1 即"条件为假"时跳走。
//       无 else 时，条件为假直接跳到 endif 标签。
// demo: if (x > 0) { y = 1; } else { y = 2; }  →
//       <x 与 0 的比较>           # rax = 条件值（0 或 1）
//       testq %rax, %rax
//       je else_0                 # 为假 → 跳 else
//       <then 分支>
//       jmp endif_1               # 为真 → 跳过 else
//   else_0:
//       <else 分支>
//   endif_1:
void CodeGen::visit(IfStmt& stmt) {
    std::string elseLabel = newLabel("else");
    std::string endLabel = newLabel("endif");

    emitComment("if condition");
    emitExpr(stmt.condition);
    emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
    emit(std::format("je {}                     # 条件为假（ZF=1）跳转到 else/endif", stmt.elseBranch ? elseLabel : endLabel));

    emitComment("then branch");
    emitStmt(stmt.thenBranch);

    if (stmt.elseBranch) {
        emit(std::format("jmp {}                    # then 分支结束，无条件跳转到 endif", endLabel));
        emit(std::format("{}:                        # else 分支标签", elseLabel));
        emitComment("else branch");
        emitStmt(stmt.elseBranch);
    }

    emit(std::format("{}:                        # endif 标签", endLabel));
}

// ─────────────────────────────────────────────────────────────────────────────
// while 语句
// ─────────────────────────────────────────────────────────────────────────────
// 生成模式：
//   .L_begin_N:
//   <condition>
//   testq %rax, %rax
//   je .L_end_N
//   <body>
//   jmp .L_begin_N
//   .L_end_N:
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 while 降级为"标签 + 条件跳出 + 回边跳转"。
// 理论：循环 = 条件测试在前的基本块 + 一条回边（back edge）。
//       这也是流图 reducibility 的经典形状，自然循环的识别就靠回边。
// demo: while (i > 0) { i = i - 1; }  →
//   while_begin_2:
//       <i 与 0 的比较>
//       testq %rax, %rax
//       je while_end_3            # 条件为假 → 退出循环
//       <循环体>
//       jmp while_begin_2         # 回边：回到条件测试
//   while_end_3:
void CodeGen::visit(WhileStmt& stmt) {
    std::string beginLabel = newLabel("while_begin");
    std::string endLabel = newLabel("while_end");

    emit(std::format("{}:                        # while 循环开始标签", beginLabel));
    emitComment("while condition");
    emitExpr(stmt.condition);
    emit("testq %rax, %rax             # 条件值与自身按位与，设置 ZF 标志位");
    emit(std::format("je {}                     # 条件为假（ZF=1）跳出循环", endLabel));

    emitComment("while body");
    emitStmt(stmt.body);
    emit(std::format("jmp {}                    # 无条件跳回循环开始（回边）", beginLabel));

    emit(std::format("{}:                        # while 循环结束标签", endLabel));
}

// 表达式语句：只求值、结果（rax）丢弃；价值在求值过程产生的副作用
// （典型如 ptr->speak() 这类调用语句）。
void CodeGen::visit(ExprStmt& stmt) {
    emitExpr(stmt.expr);
}

// ═════════════════════════════════════════════════════════════════════════════
// 表达式生成
// ═════════════════════════════════════════════════════════════════════════════
// 核心约定：每个表达式的结果放在 rax 寄存器中
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：表达式分发器——与 emitStmt 同构，按节点动态类型派发。
// 约定（再强调）：无论表达式多复杂，结果一律落在 rax（单累加器模型）；
//       子表达式的中间值靠 push/pop 经栈周转（见 emitBinary）。
// 注意：nullptr 没有单独的 emit 函数，直接内联在此——xorq 自异或清零
//       是 x86 惯用的"置 0"写法（比 movq $0 更短且不依赖立即数）。
void CodeGen::emitExpr(const ExprPtr& expr) {
    if (!expr) return;
    // 同上：accept 虚表分派。
    // 注意 NullptrLiteralExpr 在此就地发射（无独立 helper），故在下面以
    // visit(NullptrLiteralExpr&) 的形式保留 —— 改造前它也在这条链的末尾。
    expr->accept(*this);
}

// nullptr 字面量：x86 惯用自异或清零（比 movq $0 少一个字节且更快）
void CodeGen::visit(NullptrLiteralExpr&) {
    emit("xorq %rax, %rax             # nullptr = 0（x86 惯用自异或清零）");
}

// 整数字面量：立即数直接进 rax。
// demo: 42 → movq $42, %rax
void CodeGen::visit(IntLiteralExpr& expr) {
    emit(std::format("movq ${}, %rax           # 整数字面量载入 rax", expr.value));
}

// 布尔字面量：本项目 bool 按整数 0/1 表示，
// 与比较运算 setcc/movzbq 的产出形式天然一致。
// demo: true → movq $1, %rax      false → movq $0, %rax
void CodeGen::visit(BoolLiteralExpr& expr) {
    emit(std::format("movq ${}, %rax           # 布尔字面量",
        expr.value ? 1 : 0));
}

// 字符串字面量：登记进常量池取标签，再用 RIP 相对寻址取地址。
// 理论：leaq str_N(%rip), %rax 是位置无关的地址计算方式——
//       目标地址 = 执行本条指令时的 RIP + 汇编器算好的偏移，
//       由重定位在汇编/链接期填回，代码段无需知道绝对地址。
// demo: "hello" → .rodata 中 str_0: .string "hello"
//                 此处发射 leaq str_0(%rip), %rax
void CodeGen::visit(StringLiteralExpr& expr) {
    std::string label = addStringLiteral(expr.value);
    emit(std::format("leaq {}(%rip), %rax      # 加载字符串字面量地址（PIC）", label));
}

// 做什么：把变量的值加载进 rax。两级查找：
//   ① 局部变量表（含 this 与 spill 下来的形参）→ movq off(%rbp), %rax
//   ② 类方法体内的裸字段名 → 隐含 this->field：先加载 this，
//      再按字段偏移 movl off(%rax), %eax（名字 → 偏移的降级）
// demo: x（局部，槽位 -32）→ movq -32(%rbp), %rax
//       age（Animal 字段，偏移 0）→ movq -8(%rbp), %rax   # load this
//                                   movl 0(%rax), %eax    # load .age
// 兜底：两级都查不到时输出 WARNING 注释并清零（语义阶段本应已拦截，
//       这里是双保险，保证 .s 仍然合法可汇编）。
void CodeGen::visit(VarExpr& expr) {
    // ★ 栈上类对象：值语义 = 对象地址（与指针表达式的值一致）——
    //   后续 emitMember/emitCall 对 "." 的约定就是"地址已在 rax"，
    //   用 leaq 取址即可复用整套成员访问代码，无需解引用。
    //   （必须放在普通局部加载之前：类对象槽里存的是对象内容首 8 字节，
    //     直接加载会得到 _vptr 而非对象地址——虚调用碰巧也能跑，
    //     但字段访问与析构取址会错，统一按地址语义发射才自洽。）
    auto ci = m_classLocals.find(expr.name);
    if (ci != m_classLocals.end()) {
        emit(std::format("leaq {}(%rbp), %rax    # 取栈对象地址 &{}",
            ci->second.offset, expr.name));
        return;
    }

    // 先查局部变量
    auto it = m_localVars.find(expr.name);
    if (it != m_localVars.end()) {
        emit(std::format("movq {}(%rbp), %rax    # 加载局部变量 {} 到 rax",
            it->second, expr.name));
        return;
    }

    // 如果在类方法中，查类字段（通过 this 指针访问）
    if (!m_currentClassName.empty() && m_currentClassType) {
        auto field = m_currentClassType->classLayout.findField(expr.name);
        if (field) {
            // 通过 this 指针访问字段
            auto thisIt = m_localVars.find("this");
            if (thisIt != m_localVars.end()) {
                emit(std::format("movq {}(%rbp), %rax    # 加载 this 指针", thisIt->second));
                emit(std::format("movl {}(%rax), %eax    # 读取字段 .{}（偏移 +{} 字节）",
                    field->offset, expr.name, field->offset));
                return;
            }
        }
    }

    // 查全局变量或常量符号（RIP 寻址；asmSymbol 净化 :: 等非法标签字符）
    emit(std::format("movq {}(%rip), %rax    # 加载全局变量 {} 到 rax（RIP 相对寻址）",
        asmSymbol(expr.name), expr.name));
}

// ─────────────────────────────────────────────────────────────────────────────
// 二元表达式
// ─────────────────────────────────────────────────────────────────────────────
// 生成模式：
//   <left>  → rax
//   pushq %rax           # 保存左操作数
//   <right> → rax
//   movq %rax, %rcx      # 右操作数移到 rcx
//   popq %rax            # 恢复左操作数
//   <operation>          # 执行运算
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把二元运算降级为指令序列。
// 理论（单累加器求值）：本实现只有 rax 一个万能结果寄存器，求右操作数
//       会冲掉左操作数，因此左值先压栈暂存、算完右值再弹回——这是
//       "表达式树 → 线性指令序列"的经典调度问题（LLVM 由寄存器分配器
//       全局优化；教学实现用栈周转，正确但多访存）。
// demo: a + 1（a 在 -8(%rbp)）→
//       movq -8(%rbp), %rax    # load a
//       pushq %rax
//       movq $1, %rax          # int literal
//       movq %rax, %rcx
//       popq %rax
//       addq %rcx, %rax        # +
void CodeGen::visit(BinaryExpr& expr) {
    emitComment("binary expr");

    // 左操作数 → rax → 压栈
    emitExpr(expr.left);
    emit("pushq %rax                  # 左操作数压栈暂存（求右值会覆盖 rax）");

    // 右操作数 → rax → rcx
    emitExpr(expr.right);
    emit("movq %rax, %rcx             # 右操作数从 rax 转移到 rcx");

    // 恢复左操作数到 rax
    emit("popq %rax                   # 弹出左操作数回到 rax");

    switch (expr.op) {
        case BinaryOp::Add:
            emit("addq %rcx, %rax             # 加法：rax = rax + rcx");
            break;
        case BinaryOp::Sub:
            emit("subq %rcx, %rax             # 减法：rax = rax - rcx");
            break;
        case BinaryOp::Mul:
            emit("imulq %rcx, %rax            # 乘法：rax = rax * rcx（有符号）");
            break;
        // ── 除法族 ──
        // x86 的 idivq %rcx 计算的是 128 位被除数 rdx:rax ÷ rcx：
        // 商 → rax，余数 → rdx。cqto 先把 rax 符号扩展到 rdx:rax，
        // 否则 rdx 的垃圾值会毁掉被除数（经典陷阱）。
        case BinaryOp::Div:
            emit("cqto                        # 符号扩展 rax → rdx:rax（128 位被除数）");
            emit("idivq %rcx                  # 有符号除法：rax=商，rdx=余数");
            break;
        case BinaryOp::Mod:
            emit("cqto                        # 符号扩展 rax → rdx:rax（128 位被除数）");
            emit("idivq %rcx                  # 有符号除法：rax=商，rdx=余数");
            emit("movq %rdx, %rax             # 取模：结果在 rdx，移回 rax");
            break;
        // ── 比较族 ──
        // 三步缺一不可：cmpq 做减法只置标志位 → setcc 把标志写成 8 位
        // 0/1（setcc 只能写 %al 等 8 位寄存器）→ movzbq 零扩展回 64 位，
        // 与"结果统一在 rax"的约定对齐。
        case BinaryOp::Eq:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("sete %al                    # 相等时 al = 1（ZF=1）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Neq:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setne %al                   # 不等时 al = 1（ZF=0）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Lt:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setl %al                    # 小于时 al = 1（SF≠OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Gt:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setg %al                    # 大于时 al = 1（ZF=0 且 SF=OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Le:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setle %al                   # 小于等于时 al = 1（ZF=1 或 SF≠OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case BinaryOp::Ge:
            emit("cmpq %rcx, %rax             # 比较：rax - rcx 设置标志位");
            emit("setge %al                   # 大于等于时 al = 1（SF=OF）");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        // ── 逻辑与/或 ──
        // 本实现直接按位与/或（要求操作数已规范化为 0/1，比较指令恰好
        // 保证这一点）。⚠ 理论点：这放弃了短路求值（short-circuit）——
        // 真正的 &&/|| 必须用条件跳转实现"左值为假则不算右值"，
        // 其降级形状等价于两个嵌套 if（可作课后练习）。
        case BinaryOp::And:
            emit("andq %rcx, %rax             # 逻辑与：rax = rax & rcx");
            break;
        case BinaryOp::Or:
            emit("orq %rcx, %rax              # 逻辑或：rax = rax | rcx");
            break;
    }
}

// 做什么：一元运算。Neg 直接 negq 取负（先求值再原地取负）；
//       Not 用 testq + sete 实现"等于 0 则为 1"的逻辑非。
// demo: -x → <加载 x 进 rax>; negq %rax
//       !x → <加载 x 进 rax>; testq %rax, %rax; sete %al; movzbq %al, %rax
void CodeGen::visit(UnaryExpr& expr) {
    // ── 取地址 &x：要的是【地址】而不是值，故不能先 emitExpr(operand) ──
    // 对栈上的局部变量，地址就是 leaq off(%rbp)；先加载值再取址会得到
    // "值所在的内存地址"这种毫无意义的指针。
    // 对照 clang：CodeGenFunction::EmitUnaryOp 中 UO_AddrOf 走
    //   EmitLValue(E) + EmitLValueAsAddr（求左值地址，而非求值）。
    if (expr.op == UnaryOp::Addr) {
        if (expr.operand->kind == NodeKind::Var) {
            auto var = std::static_pointer_cast<VarExpr>(expr.operand);
            auto ci = m_classLocals.find(var->name);
            if (ci != m_classLocals.end()) {
                emit(std::format("leaq {}(%rbp), %rax    # &{} = 栈对象地址",
                    ci->second.offset, var->name));
                return;
            }
            auto it = m_localVars.find(var->name);
            if (it != m_localVars.end()) {
                emit(std::format("leaq {}(%rbp), %rax    # &{} = 局部变量栈地址",
                    it->second, var->name));
                return;
            }
        }
        // 其它形态（字段地址、数组元素地址…）尚未实现 —— 明确报错而非静默给错值
        throw std::runtime_error(std::format(
            "[CodeGen Error] address-of is only supported on local variables "
            "for now (line {})", expr.location.line));
    }

    emitExpr(expr.operand);

    switch (expr.op) {
        case UnaryOp::Neg:
            emit("negq %rax                   # 取负：rax = -rax（二进制补码）");
            break;
        case UnaryOp::Not:
            emit("testq %rax, %rax            # 测试 rax 是否为 0（设置 ZF）");
            emit("sete %al                    # 逻辑非：ZF=1 时 al=1");
            emit("movzbq %al, %rax            # 零扩展 al 到 64 位 rax");
            break;
        case UnaryOp::Addr:
            // 上面已提前 return（取地址不走"先求值"路径），此处不可达
            throw std::runtime_error(
                "[CodeGen Error] UnaryOp::Addr should be handled before emitExpr");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数调用
// ─────────────────────────────────────────────────────────────────────────────
// System V AMD64 ABI 参数传递约定：
//   第1个参数: rdi
//   第2个参数: rsi
//   第3个参数: rdx
//   第4个参数: rcx
//   第5个参数: r8
//   第6个参数: r9
//   返回值: rax
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：生成函数/方法调用，三路分发：
//   ① callee 是成员表达式且命中该类 vtable 条目 → 虚调用 emitVirtualCall
//   ② callee 是成员表达式但非虚方法 → 直接 call "类名_方法名"，this 进 rdi
//   ③ 普通自由函数 → 实参进 rdi/rsi/... 后 call 函数名
// 理论：System V 调用约定 = "调用方把参数放进约定寄存器 → call →
//       返回后从 rax 取值"。实参为何先逐个压栈、再逆序弹出到寄存器？
//       因为求第 i+1 个实参会破坏 rax 中第 i 个的值，栈是天然暂存区；
//       逆序弹出恰好让"第 1 个实参"最后进入第 1 个参数寄存器。
// demo: add(1, 2) →
//       movq $1, %rax; pushq %rax     # 实参 1 暂存
//       movq $2, %rax; pushq %rax     # 实参 2 暂存
//       popq %rsi                     # 逆序：实参 2 → rsi
//       popq %rdi                     # 实参 1 → rdi
//       callq add
// demo: d.get(5)（非虚方法）→ this(d 的地址) → rdi，5 → rsi，
//       callq Dog_get（"类名_方法名"简化 mangling）
void CodeGen::visit(CallExpr& expr) {
    emitComment("function call");

    // 检查是否是方法调用
    // ── 路径①/②：callee 形如 obj.method —— 先解析对象类型再定虚实 ──
    if (expr.callee->kind == NodeKind::Member) {
        auto mem = std::static_pointer_cast<MemberExpr>(expr.callee);
        if (mem->isMethodCall || (mem->object && mem->object->resolvedType)) {
            TypePtr objType = mem->object->resolvedType;
            if (mem->isArrow && objType && objType->isPointer()) {
                objType = objType->pointeeType;
            }

            // 检查是否是虚函数调用
            if (objType && objType->isClass() && m_classTypes) {
                auto it = m_classTypes->find(objType->name);
                if (it != m_classTypes->end()) {
                    auto& layout = it->second->classLayout;
                    // 在该类的 vtable 条目里找同名方法（子串匹配，教学简化；
                    // 完备实现应精确比较 mangled 签名）；命中 → 虚调用，
                    // entry.index 就是语义阶段定好的 vtable 下标
                    for (auto& entry : layout.vtableEntries) {
                        if (entry.mangledName.find(mem->memberName) != std::string::npos) {
                            // 这是虚函数调用！
                            // ★ 喂入 this：emitVirtualCall 约定"入口时对象
                            //   地址在 rdi"，此处先求值对象表达式（"." 取
                            //   栈对象地址 / "->" 取指针值）再送入 rdi。
                            //   （P1 修正：原先直接进三部曲，rdi 里是上次
                            //     调用遗留的垃圾——栈对象支持暴露了它）
                            emitExpr(mem->object);
                            emit("movq %rax, %rdi            # this = 对象地址（第 0 参数）");
                            emitVirtualCall(objType->name, mem->memberName,
                                          expr.arguments, entry.index);
                            return;
                        }
                    }
                }
            }

            // 普通方法调用（非虚函数）
            // 先计算对象地址作为第一个参数（this 指针）
            // Itanium C++ ABI：this 是隐式第 0 参数走 rdi，
            // 显式实参从 rsi 起 —— 故下面实参最多取 5 个（this + 5 = 6 寄存器）
            emitExpr(mem->object);
            emit("pushq %rax                   # 暂存 this 指针到栈上");

            // 计算参数
            // （逐个压栈暂存，随后逆序弹入 rsi/rdx/rcx/r8/r9）
            for (size_t i = 0; i < expr.arguments.size() && i < 5; i++) {
                emitExpr(expr.arguments[i]);
                emit("pushq %rax                  # 实参值压栈暂存");
            }

            // 恢复参数到寄存器
            for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0 && i < 5; i--) {
                static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
            }

            // this 指针到 rdi
            emit("popq %rdi                   # 弹出 this 指针到 rdi（第 0 参数）");

            // 调用
            // 符号名构造：简化 mangling "类名_方法名"（如 Dog_speak），
            // 与 emitFunction 发射的标签、vtable 表项保持一致；
            // 标准 GCC ABI 下应为 _ZN3Dog5speakEv 形式
            std::string funcName;
            if (objType && objType->isClass()) {
                funcName = objType->name + "_" + mem->memberName;
            } else {
                funcName = mem->memberName;
            }
            emit(std::format("callq {}                  # 调用方法 {}", funcName, funcName));
            return;
        }
    }

    // 普通函数调用
    // 计算参数并放入对应寄存器
    for (size_t i = 0; i < expr.arguments.size() && i < 6; i++) {
        emitExpr(expr.arguments[i]);
        emit("pushq %rax                  # 实参值压栈暂存");
    }

    // 恢复参数到寄存器（逆序）
    static const char* regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (int i = static_cast<int>(expr.arguments.size()) - 1; i >= 0; i--) {
        if (i < 6) {
            emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
        }
    }

    // 确定函数名
    std::string funcName;
    if (expr.callee->kind == NodeKind::Var) {
        auto var = std::static_pointer_cast<VarExpr>(expr.callee);
        // asmSymbol：限定名调用（Math::scale）在源码层带 "::"，
        // 净化后与标签发射端一致（见 emitFunction）
        funcName = asmSymbol(var->name);
    }

    emit(std::format("callq {}                  # 调用函数 {}", funcName, funcName));
}

// ═════════════════════════════════════════════════════════════════════════════
// 虚函数调用 —— 编译器最精彩的"三部曲"
// ═════════════════════════════════════════════════════════════════════════════
//
// ptr->vfunc(args) 在机器码层面绝对不能直接 call vfunc！
// 因为编译期不知道 ptr 指向哪个具体类型（多态），
// 必须通过虚函数表在运行期动态查找到真正的函数地址。
//
// 汇编三部曲：
//
//   (a) 从对象读出 _vptr 机器地址
//       movq (objAddr), %rax          # rax = obj._vptr
//
//   (b) 加上虚函数在表中的 Index 偏移量
//       movq index*8(%rax), %rax      # rax = vtable[index]
//
//   (c) 跳转到该地址执行
//       callq *%rax                   # 间接调用
//
// 这就是"编译期看符号，运行期看偏移量"的终极体现：
//   编译期：ptr->vfunc()  → 查符号表得到 vtable index
//   运行期：[ptr+0] → vtable → [vtable+index*8] → 真实函数地址 → 执行
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：生成经 vtable 的间接调用。className/methodName 仅用于生成
//       可读注释；args 不含 this；vtableIndex 是编译期常数。
// demo: pet->speak()（Animal，speak 在 vtable[0]，无实参）→
//       movq %rdi, %rbx              # this 暂存到 rbx
//       movq %rbx, %rdi              # this 就位（第 0 参数）
//       movq (%rdi), %rax            # (a) rax = obj._vptr
//       movq 0(%rax), %rax           # (b) rax = 表项（index 0 → 偏移 0）
//       callq *%rax                  # (c) 间接调用
// ⚠ 实现细节（约定必须统一，否则就是段错误）：
//   1. this 来源：进入本函数时对象地址必须已在 rdi——
//      emitCall 走 "emitExpr(object) → pushq → popq %rdi" 喂入；
//      块尾/emitDelete 走 "leaq off(%rbp), %rdi"（栈对象）喂入。
//      rbx 是 callee-saved 寄存器，本函数用它暂存对象地址后自行恢复，
//      保证离开时调用者的 rbx 原值不变（P1 修正：原先直接挪用不恢复，
//      栈对象的析构经虚表分派时依赖此约定）。
//   2. 偏移公式：index*8。emitNew 与构造函数把 _vptr 写为
//      表首+16（已指向 vtable[0]），因此从 _vptr 起按 index*8
//      取项即可；若误用 (index+2)*8 会越过表尾读到垃圾地址。
void CodeGen::emitVirtualCall(
    const std::string& className,
    const std::string& methodName,
    const std::vector<ExprPtr>& args,
    uint32_t vtableIndex) {

    emitComment(std::format("VIRTUAL CALL: {}::{} (vtable[{}])",
        className, methodName, vtableIndex));

    // ─── 保存对象地址 ───
    // this 进入时在 rdi；先压栈保护（下面求/弹实参会反复使用 rax 与
    // 参数寄存器），最后弹回 rdi 兼作第 0 实参（rbx 只做中途暂存）
    emit("pushq %rdi                   # 暂存 this（对象地址）到栈上保护");

    // ─── 准备参数 ───
    // 第一个参数是 this 指针（对象地址）

    // 先计算参数（跳过 this）
    // 显式实参最多 5 个：rdi 留给 this，rsi/rdx/rcx/r8/r9 装 5 个实参
    for (size_t i = 0; i < args.size() && i < 5; i++) {
        emitExpr(args[i]);
        emit("pushq %rax                  # 实参值压栈暂存");
    }

    // 恢复参数到寄存器
    for (int i = static_cast<int>(args.size()) - 1; i >= 0 && i < 5; i--) {
        static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
        emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
    }

    // this 指针到 rdi：从栈中弹回函数入口时保存的对象地址
    // （弹栈同时把调用方的栈指针复原——push/pop 严格配对，
    //   虚析构经块尾反复调用也不会漂移）
    emit("popq %rdi                   # 弹出 this 指针（恢复对象地址）");

    // ─── 虚函数调用三部曲 ───

    // (a) 从对象偏移量 +0 处读出 _vptr 指针
    emit("# ═══ 虚函数调用 (a)：从对象读出 _vptr ═══");
    emit("movq (%rdi), %rax            # 从对象首 8 字节读出 _vptr");

    // (b) 从 vtable 中加上 index 偏移量，读出真正的函数地址
    //     _vptr 已指向 vtable[0]（emitNew/构造函数写入 表首+16），
    //     所以从 _vptr 起第 index 项 = 偏移 index*8
    uint32_t vtableOffset = vtableIndex * 8;
    emit("# ═══ 虚函数调用 (b)：从 vtable 加载函数地址 ═══");
    emit(std::format("movq {}(%rax), %rax       # 从 vtable[{}] 读出函数地址（偏移 {}）",
        vtableOffset, vtableIndex, vtableOffset));

    // (c) 间接跳转到该地址执行
    emit("# ═══ 虚函数调用 (c)：间接跳转到真实函数 ═══");
    emit("callq *%rax                  # 经 vtable 间接调用（跳转到 rax 所指地址）");

    emitComment(std::format("END VIRTUAL CALL {}::{}", className, methodName));
}

// ─────────────────────────────────────────────────────────────────────────────
// 成员访问
// ─────────────────────────────────────────────────────────────────────────────
// obj.field 的消除过程：
//   编译期：查 ClassLayout 得到 field 的偏移量
//   汇编：  [objAddr + fieldOffset] → rax
//   字段名在此刻彻底消失，只剩下数字偏移量。
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：生成 obj.field 的读取，三个步骤：
//   ① 对 object 求值进 rax（-> 情形下对象表达式的值本身就是地址）
//   ② 解析出类的 ClassLayout，findField 查到字段偏移（语义阶段已算好）
//   ③ movq off(%rax), %rax 取出字段值
// demo: p->age（age 偏移 0，p 在 -8(%rbp)）→
//       movq -8(%rbp), %rax      # load p（对象地址）
//       movq 0(%rax), %rax       # .age (offset 0)
// 兜底：查不到偏移时按偏移 0 读取并留注释（语义阶段本应已拦截）。
void CodeGen::visit(MemberExpr& expr) {
    emitComment(std::format("member access: .{}", expr.memberName));

    // 计算对象地址
    emitExpr(expr.object);

    TypePtr objType = expr.object->resolvedType;
    if (expr.isArrow && objType && objType->isPointer()) {
        // 对于 -> 操作，对象本身就是一个指针
        // 不需要额外解引用
    } else if (objType && !objType->isPointer()) {
        // 对于 . 操作，如果对象不是指针，需要获取其地址
        // 这里简化处理
    }

    if (objType) {
        TypePtr actualType = objType;
        if (expr.isArrow && objType->isPointer()) {
            actualType = objType->pointeeType;
        }

        if (actualType && actualType->isClass()) {
            auto field = actualType->classLayout.findField(expr.memberName);
            if (field) {
                // ── 嵌套类字段：取地址而非取值（子对象内联在父对象里）──
                // o->f.a 的 f 步：类类型字段不是"值"，是父对象内的子对象，
                // 应产出地址 obj+fieldOffset（leaq），供下一层 .a 再加偏移。
                // 历史 bug：曾按 8B 走 movq 把 f 的头 8 字节（成员 a、b 的内容）
                // 当指针解引用 → 段错误（o->f.a 崩，o->tag 正常）。
                // 对照 clang：链式访问折叠为常量总偏移（GEP 求和），
                // minicc 教学版保留逐层 leaq，便于观察每一跳。
                if (field->type && field->type->isClass()) {
                    emit(std::format("leaq {}(%rax), %rax    # 嵌套类字段 .{} 取地址（子对象偏移 +{}）",
                        field->offset, expr.memberName, field->offset));
                    return;
                }
                // 直接用偏移量访问字段
                // ★ 按字段宽度选指令：int(4B) 用 movl（32 位加载自动零
                //   扩展进 rax，避免 movq 连相邻字段/对象外字节一起读进
                //   高 32 位）；指针/8B 用 movq；bool(1B) 用 movzbq。
                if (field->size <= 4) {
                    emit(std::format("movl {}(%rax), %eax    # 读取字段 .{}（偏移 +{}，4B int）",
                        field->offset, expr.memberName, field->offset));
                } else if (field->size == 1) {
                    emit(std::format("movzbq {}(%rax), %rax  # 读取字段 .{}（偏移 +{}，1B bool）",
                        field->offset, expr.memberName, field->offset));
                } else {
                    emit(std::format("movq {}(%rax), %rax    # 读取字段 .{}（偏移 +{}，8B 指针/long）",
                        field->offset, expr.memberName, field->offset));
                }
                return;
            }
        }
    }

    // 默认：偏移量 0
    emit("movq (%rax), %rax            # 成员访问（偏移未知，默认 0）");
}

// ─────────────────────────────────────────────────────────────────────────────
// 下标表达式 v[i]（读值形态）→ 糖化为 v.at(i) 成员调用
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 IndexExpr 降级为"this + 实参 + callq <类>_at"。
// 理论（语法糖的降级时机）：真 C++ 里 v[i] 是 operator[] 重载调用
//       （[over.sub]），重载决议在语义阶段完成。minicc 无运算符重载，
//       于是把重载决议"固化"成约定：类提供 at() 即获得下标读能力
//       （写能力在 emitAssign 里走 set()）。语义阶段的 inferIndex 已
//       校验过 at() 存在且形参匹配——代码生成只管按约定发射。
// demo: v[i]（v 是栈上容器对象，i 在 -16(%rbp)）→
//       leaq -24(%rbp), %rax    # &v（emitVar 的类对象取址路径）
//       pushq %rax              # save this
//       movq -16(%rbp), %rax    # load i
//       pushq %rax
//       popq %rsi               # arg1 = i
//       popq %rdi               # this = &v
//       callq Vector_at         # v.at(i)
void CodeGen::visit(IndexExpr& expr) {
    emitComment("subscript v[i] → desugar to v.at(i)");

    // 容器类名：resolvedType 由语义阶段填充（指针容器先解引用一层）
    TypePtr objType = expr.object->resolvedType;
    if (objType && objType->isPointer()) {
        objType = objType->pointeeType;
    }
    std::string className = (objType && objType->isClass()) ? objType->name : "";

    // this = 对象地址（栈对象经 emitVar 走 leaq 取址；指针直接取值）
    emitExpr(expr.object);
    emit("pushq %rax                  # 保存 this 指针（容器对象地址）");

    // 下标表达式 → rax，随后按 System V 调用约定装参
    emitExpr(expr.index);
    emit("pushq %rax                  # 下标值压栈暂存");

    emit("popq %rsi                   # arg1: 弹出下标到 rsi");
    emit("popq %rdi                   # this: 弹出容器地址到 rdi");
    emit(std::format("callq {}_at                # 调用 v.at(i) 读取元素", className));
}

// ─────────────────────────────────────────────────────────────────────────────
// new 表达式（简化版：调用 malloc）
// ─────────────────────────────────────────────────────────────────────────────
// new MyClass() 被翻译为：
//   1. 调用 malloc(classSize) 分配内存
//   2. 设置 _vptr 指向该类的 vtable
//   3. 返回对象指针
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 new MyClass() 降级为"分配内存 + 安装 _vptr"两步。
// 理论对照：标准 C++ 中 new 表达式 = 调用 operator new(sizeof T)
//       （全局分配函数，GCC mangling 为 _Znwm，即 operator new(unsigned
//       long)）+ 构造函数调用。本项目简化为直接 callq malloc：不抛
//       bad_alloc、不调用构造函数（构造/析构支持在 ROADMAP 主线 A），
//       但 _vptr 的安装方式与真实 ABI 完全一致。
// demo: new Dog()（Dog 大小 8，含 vtable）→
//       movq $8, %rdi                    # malloc size
//       callq malloc                     # 返回地址在 rax
//       leaq _ZTV3Dog(%rip), %rcx        # vtable 地址
//       addq $16, %rcx                   # 跳过 offset-to-top 与 RTTI，指向 vtable[0]
//       movq %rcx, (%rax)                # obj._vptr = vtable
void CodeGen::visit(NewExpr& expr) {
    emitComment(std::format("new {}()", expr.className));

    // 查找类的大小
    uint32_t size = 8; // 默认大小
    std::string vtableLabel;
    bool hasVTable = false;

    // 在全局类表中查布局：totalSize 决定分配字节数（含 _vptr 槽，
    // 由语义阶段的 ClassLayout 计算），hasVTable 决定是否要安装 _vptr
    if (m_classTypes) {
        auto it = m_classTypes->find(expr.className);
        if (it != m_classTypes->end()) {
            size = it->second->classLayout.totalSize;
            if (size == 0) size = 8;
            hasVTable = it->second->classLayout.hasVTable;
            if (hasVTable) {
                vtableLabel = NameMangler::mangleVTable(expr.className);
            }
        }
    }

    // 调用 malloc
    emit(std::format("movq ${}, %rdi             # malloc 分配大小：{} 字节", size, size));
    emit("callq malloc                  # 调用 malloc 分配堆内存");
    emit("pushq %rax                    # 暂存返回的对象指针到栈上");

    if (hasVTable) {
        // 设置主 _vptr：对象偏移量 0 处 = vtable 地址 + 16
        emit(std::format("leaq {}(%rip), %rcx    # 取 vtable 首地址", vtableLabel));
        emit("addq $16, %rcx              # 跳过 offset-to-top 与 RTTI，指向 vtable[0]");
        emit("movq (%rsp), %rax           # 从栈上取回对象指针");
        emit("movq %rcx, (%rax)           # 安装主 _vptr 到对象首 8 字节");

        // 次 vptr：每个次基类各一个
        if (m_classTypes) {
            auto it = m_classTypes->find(expr.className);
            if (it != m_classTypes->end()) {
                for (auto& base : it->second->classLayout.bases) {
                    if (base.isPrimary || !base.hasVTable) continue;
                    uint32_t vptrOffset = base.vtableSegmentOffset + 16;
                    emit("movq (%rsp), %rax           # 从栈上取回对象指针");
                    emit(std::format("leaq {}(%rip), %rcx    # 取次表首地址（基类 '{}'）",
                        vtableLabel, base.baseClassName));
                    emit(std::format("addq ${}, %rcx          # 跳过次表头，指向 secondary[0]", vptrOffset));
                    emit(std::format("movq %rcx, {}(%rax)     # 安装次 _vptr（偏移 {} 处）",
                        base.offset, base.baseClassName));
                }
            }
        }
    }

    // 调用构造函数
    // 先计算参数（实参最多 5 个，this 占用 rdi）
    for (size_t i = 0; i < expr.constructorArgs.size() && i < 5; i++) {
        emitExpr(expr.constructorArgs[i]);
        emit("pushq %rax                  # 构造实参压栈暂存");
    }

    // 恢复参数到 rsi, rdx, rcx, r8, r9
    for (int i = static_cast<int>(expr.constructorArgs.size()) - 1; i >= 0 && i < 5; i--) {
        static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
        emit(std::format("popq %{}                   # 逆序弹出实参到寄存器", regs[i]));
    }

    // 从栈顶取回 objPtr 传给 rdi
    emit("movq (%rsp), %rdi             # this = 已分配对象指针（栈顶取出）");
    // 构造函数名：与 registerFunction 的 mangling 对齐——
    // 0 参 → ClassName_ClassName，N 参 → ClassName_ClassName_N
    std::string ctorName = expr.className + "_" + expr.className;
    if (!expr.constructorArgs.empty()) {
        ctorName += "_" + std::to_string(expr.constructorArgs.size());
    }
    // 命名空间内的类（N::S）名字里带 "::" —— 符号标签必须净化，
    // 且要与函数定义端（emitFunction 里对 mangledName 走的同一个 asmSymbol）一致
    ctorName = asmSymbol(ctorName);
    emit(std::format("callq {}                 # 调用构造函数 {}", ctorName, ctorName));

    emit("popq %rax                     # 弹出对象指针作为 new 表达式返回值");
    emitComment(std::format("end new {}()", expr.className));
}

// ─────────────────────────────────────────────────────────────────────────────
// this 表达式
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 this 指针加载进 rax。this 在函数序言中已作为隐式形参
//       spill 进栈槽（见 emitFunction 的成员函数分支），此处按普通
//       局部变量查表加载即可 —— "this 只是一个普通参数"是 Itanium ABI
//       的本质。
// demo: this（成员函数内）→ movq -8(%rbp), %rax
void CodeGen::visit(ThisExpr&) {
    auto it = m_localVars.find("this");
    if (it != m_localVars.end()) {
        emit(std::format("movq {}(%rbp), %rax       # 加载 this 指针", it->second));
    } else {
        emit("# WARNING: 'this' 在类方法外部使用");
        emit("xorq %rax, %rax             # 错误路径：清零返回");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// dynamic_cast 降级
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 dynamic_cast<T*>(e) 翻译为一次运行时助手调用。
// 理论（Itanium ABI，[expr.dynamic.cast]）：
//   真 C++ 的 dynamic_cast 不做编译期静态变换，而是把问题推迟到运行时——
//   取对象 vptr → vtable[-1] 的 typeinfo → 沿继承链找目标类型。
//   本编译器的落地方式：
//     ① 先求值操作数（结果 = 对象指针，落 %rax）
//     ② 装入 System V 调用约定的前两个参数寄存器：
//          %rdi = 对象指针，%rsi = 目标类的 typeinfo 地址
//     ③ callq __minicc_dynamic_cast（助手在 generate() 末尾按需发射）
//     ④ 助手返回值即表达式结果：成功 = 原指针，失败 = 0，统一落 %rax
// demo: dynamic_cast<Dog*>(p)
//       → emitExpr(p)                     # %rax = 指针
//         leaq _ZTI3Dog(%rip), %rsi       # 目标 typeinfo
//         movq %rax, %rdi                 # 第一参：对象指针
//         callq __minicc_dynamic_cast
void CodeGen::visit(DynamicCastExpr& expr) {
    m_needsDynamicCastHelper = true;

    emitComment(std::format("dynamic_cast<{}*>(operand) —— 运行时 RTTI 检查",
        expr.targetClassName));
    emitExpr(expr.operand);                     // %rax = 源对象指针
    emit(std::format("movq %rax, %rdi              # 参数 1：源对象指针"));
    emit(std::format("leaq {}(%rip), %rsi    # 参数 2：目标 typeinfo",
        NameMangler::mangleRTTI(expr.targetClassName)));
    emit("callq __minicc_dynamic_cast  # 调用运行时 RTTI 遍历检查类型兼容性");
    emitComment(std::format("%rax = 成功:原指针 / 失败:0 ({}) ",
        expr.targetClassName));
}

// ─────────────────────────────────────────────────────────────────────────────
// 运行时助手 __minicc_dynamic_cast（整个单元只发射一次）
// ─────────────────────────────────────────────────────────────────────────────
// 功能：判断对象的实际类型是否在目标类型的继承链上。
// 参数（System V AMD64 ABI）：%rdi = 对象指针，%rsi = 目标 typeinfo 地址
// 返回：%rax = 转型成功→原指针；失败（或空指针）→ 0
//
// 核心依据是发射好的数据结构（与真实 ABI 同构，数字可对照）：
//   对象首 8 字节  = _vptr，指向 vtable[0]
//   vtable[-1]     = 本对象的 typeinfo 地址（%vptr - 8）
//   typeinfo[+16]  = 基类 typeinfo 指针（__si_class_type_info 的 __base_type；
//                    无基类时为 0 = 继承链终点）
//
// 算法（线性上溯，对应真 ABI 的单继承路径）：
//   cur = 对象的 typeinfo
//   while (cur != 0):
//       if cur == target:  return obj   # 命中：目标在自己或祖先链上
//       cur = *(cur + 16)               # 上溯一级
//   return 0                            # 走完整条链都没命中
//
// 注意：真 ABI 还要处理多重继承（__vmi）与菱形虚继承的指针偏移调整，
// 单继承场景指针恒不变，因此"成功即原指针"是正确的。
void CodeGen::emitDynamicCastHelper() {
    // ── 运行时助手 __minicc_dynamic_cast（多继承版）──
    // 参数：%rdi = 对象指针，%rsi = 目标 typeinfo 地址
    // 返回：%rax = 转型成功→调整后指针；失败→ 0
    //
    // 算法（DFS 遍历 typeinfo 树）：
    //   1. vptr = *obj
    //   2. top = obj + vptr[-2]          (offset-to-top 归顶)
    //   3. ti  = vptr[-1]                (最派生 typeinfo)
    //   4. DFS(ti, target, top, acc=0):
    //        ti == target → return top + acc
    //        N = ti[+16] (base count)
    //        for i in 0..N: r = DFS(bases[i].ti, target, top, acc + bases[i].offset)
    //        return null
    //
    // RTTI 计数式布局（emitRTTI 输出）：
    //   [+0]  vptr (0)
    //   [+8]  name pointer
    //   [+16] base count N
    //   [+24] bases[0].typeinfo
    //   [+32] bases[0].offset
    //   [+40] bases[1].typeinfo ...

    emitComment("__minicc_dynamic_cast(obj, target_typeinfo) —— DFS RTTI 树匹配（多继承版）");
    emit("__minicc_dynamic_cast:        # 运行时助手入口：rdi=对象指针，rsi=目标 typeinfo");
    emit("testq %rdi, %rdi             # 检测对象指针是否为空");
    emit("je .Ldc_fail                 # 空指针直接走失败路径");
    emit("movq (%rdi), %rcx            # 从对象首 8 字节读出 vptr");
    emit("movq -16(%rcx), %rdx         # vptr[-2] = offset-to-top（次表偏移量）");
    emit("leaq (%rdi,%rdx), %rdi       # top = obj + ott（归顶到最派生对象）");
    emit("movq -8(%rcx), %rdx          # vptr[-1] = 最派生 typeinfo 地址");
    emit("xorq %rcx, %rcx              # acc_offset = 0（累积偏移清零）");
    emit("callq .Ldc_dfs               # 进入 DFS：DFS(ti, target, top, acc)");
    emit("ret                          # 助手返回（rax = 转型结果或 0）");

    // ── DFS 递归搜索 ──
    // 参数约定：rdx = current ti, rsi = target, rdi = top, rcx = acc_offset
    // 返回：rax = 结果指针或 0
    emit(".Ldc_dfs:                    # DFS 递归入口：rdx=当前 ti，rsi=目标");
    emit("cmpq %rsi, %rdx              # 当前 typeinfo 是否 == 目标？");
    emit("je .Ldc_dfs_hit              # 命中：返回 top + acc");
    emit("movq 16(%rdx), %rax          # 读取基类计数 N = ti[+16]");
    emit("testq %rax, %rax             # N == 0 表示无基类（叶子节点）");
    emit("je .Ldc_dfs_ret0             # 叶子节点直接返回 0");
    // 建立栈帧，保存 caller-saved 寄存器
    emit("pushq %rbp                   # 保存旧栈帧基址");
    emit("movq %rsp, %rbp              # 建立新栈帧");
    emit("pushq %rdi                   # 保存 top（最派生对象地址）");
    emit("pushq %rsi                   # 保存 target（目标 typeinfo）");
    emit("pushq %rdx                   # 保存 current ti（当前 typeinfo）");
    emit("pushq %rcx                   # 保存 acc_offset（累积偏移）");
    emit("xorq %r12, %r12              # i = 0（基类遍历下标）");
    emit("movq 16(%rdx), %r13          # N = base count（callee-saved 保存计数）");
    emit(".Ldc_dfs_loop:               # 基类遍历循环开始");
    emit("cmpq %r13, %r12              # i >= N？检查是否遍历完所有基类");
    emit("jge .Ldc_dfs_end             # 遍历完成，跳到收尾");
    // 加载 bases[i]：ti 在 +24+16*i，offset 在 +24+16*i+8
    emit("movq -24(%rbp), %rdx         # 从栈帧恢复 current ti");
    emit("leaq 24(%rdx), %rax          # 计算 &bases[0]（ti + 24）");
    emit("imulq $16, %r12, %r14        # i * 16（每个 base 条目 16 字节）");
    emit("addq %r14, %rax              # &bases[i] = &bases[0] + i*16");
    emit("movq (%rax), %rdx            # 加载 bases[i].typeinfo");
    emit("movq 8(%rax), %rcx           # 加载 bases[i].offset");
    emit("addq -32(%rbp), %rcx         # acc = parent_acc + bases[i].offset（累积偏移）");
    emit("callq .Ldc_dfs               # 递归搜索子树");
    emit("testq %rax, %rax             # 检测递归结果是否为 0");
    emit("jne .Ldc_dfs_found           # 子树命中！直接透传");
    emit("incq %r12                    # i++（继续下一个基类）");
    emit("jmp .Ldc_dfs_loop            # 循环继续");
    // 所有基类都没命中
    emit(".Ldc_dfs_end:                # 所有基类都没命中，返回 0");
    emit("xorq %rax, %rax              # 结果清零");
    emit("addq $32, %rsp               # 释放 4 个保存槽（acc, ti, target, top）");
    emit("popq %rbp                    # 恢复栈帧基址");
    emit("ret                          # 返回失败");
    // 命中：返回 top + acc_offset
    emit(".Ldc_dfs_hit:                # 当前 ti == target，命中路径");
    emit("movq %rdi, %rax              # rax = top（最派生对象地址）");
    emit("addq %rcx, %rax              # rax = top + acc_offset（调整后指针）");
    emit("ret                          # 返回转型成功");
    // 递归返回命中：直接透传 rax
    emit(".Ldc_dfs_found:              # 子树递归命中，透传结果");
    emit("addq $32, %rsp               # 释放 4 个保存槽");
    emit("popq %rbp                    # 恢复栈帧基址");
    emit("ret                          # 返回命中结果");
    // 叶子节点失败
    emit(".Ldc_dfs_ret0:               # 叶子节点（无基类）失败路径");
    emit("xorq %rax, %rax              # 结果清零");
    emit("ret                          # 返回 0");
    emit(".Ldc_fail:                   # 入口空指针失败路径");
    emit("xorq %rax, %rax              # 失败：返回 0");
    emit("ret                          # 返回");
    emit("");
}

} // namespace minicc
