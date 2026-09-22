// =============================================================================
// 阶段 5：代码生成器实现 —— x86-64 汇编输出（理论见 docs/learn/14）
// =============================================================================
// 编译器的"最后一公里"：AST → 机器指令。走到这里时类型已定、auto 已替换、
// 字段偏移（ClassLayout）已算好、模板已实例化并 mangle —— 只剩"树 → 线性
// 指令"的机械翻译。
//
//   u.age = 20;       ⇒  movl $20, -8(%rbp)   # 字段名已消失，只剩硬编码偏移
//   ptr->foo();（虚）  ⇒  movq (%rax), %rax    # (a) 从对象读出 _vptr
//                         movq 0(%rax), %rax   # (b) 加 vtable[0] 偏移
//                         callq *%rax          # (c) 跳转执行
//
// 目标：x86-64 System V AMD64 ABI —— 参数 rdi/rsi/rdx/rcx/r8/r9，返回值 rax，
// 栈 16 字节对齐。
//
// 在管线中的位置：
//   Preprocessor → Lexer → Parser → Sema → TemplateDeduction/Instantiation
//   → ★CodeGen（本文件）★ → .s 汇编 → 自研链接器产出可执行文件
//
// 名字修饰（Itanium C++ ABI 子集）：
//   成员方法 "类名_方法名"（Dog::speak → Dog_speak）│ 模板实例 _Z + 名 + I<码>E
//   vtable _ZTV<长度><名>（_ZTV3Dog）│ typeinfo _ZTI<长度><名>（_ZTI3Dog）
//
// 本文件组织（节点 ⇒ 指令 速查表；每行展开见对应 visit 重载）：
//   generate()       主入口，拼装 .text/.data/.rodata 三段
//   emitVTable/RTTI  数据段：vtable 与 type_info
//   emitFunction     函数框架（序言 / 形参 spill / 尾声）
//   emitStmt*        语句降级  │  emitExpr*  表达式降级（结果一律落 rax）
//   emitVirtualCall  虚函数调用三部曲
//   IntLiteral 42     ⇒ movq $42,%rax          │ VarExpr(x@-32) ⇒ movq -32(%rbp),%rax
//   BinaryExpr(a + 1) ⇒ pushq 左 / movq %rax,%rcx(右) / popq 左 / addq %rcx,%rax
//   CallExpr(f(a,b))  ⇒ a,b 逆序 push / popq rdi,rsi / callq f
//   NewExpr(new Dog)  ⇒ movq $size,%rdi / callq malloc / 装 _vptr / callq Dog_Dog
//   MemberExpr(u.age) ⇒ 对象地址 → %rax / movl off(%rax),%eax（按字段宽度分派）
//   ptr->vfunc()      ⇒ movq (%rdi),%rax / movq idx*8(%rax),%rax / callq *%rax
//   IfStmt/WhileStmt  ⇒ testq %rax,%rax / je <跳转标签>（循环另有 jmp 回边）
//   字段宽度分派       ⇒ 1B movb/movzbq │ 2~4B movl │ 8B movq
//
// 对应 LLVM 模块（详见 codegen.h 头注）：指令选择 ≈ SelectionDAG │ 调用约定 ≈
//   X86ISelLowering │ 寄存器分配 ≈ RegAlloc（本项目退化为"单累加器 rax + 栈
//   周转"）│ 序言/尾声 ≈ PrologEpilogInserter │ 汇编打印 ≈ AsmPrinter
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
// 字符串属只读数据不能内联进指令流，要放 .rodata 再 RIP 相对取址；此处只"登记"，
// 真正发射推迟到 generate() 末尾的 emitStringLiterals()（常量池集中在一处）。
// demo: addStringLiteral("hello") → 登记 ("str_0", "hello")，返回 "str_0"
//       最终 .rodata 中出现： str_0:  .string "hello"
std::string CodeGen::addStringLiteral(const std::string& value) {
    std::string label = newLabel("str");
    m_stringLiterals.emplace_back(label, value);
    return label;
}

// 汇编符号净化 —— 非法字符替换倒查表（gas 标签只允许 [A-Za-z0-9_.$]，其余字符
// 会让汇编器报 "junk at end of line"）：
//   Math::scale   ⇒ Math__scale     # ':' → '_'（命名空间限定）
//   Box<int*>     ⇒ Box_int__       # '<' '>' '*' → '_'（模板实参）
//   Box<int&>     ⇒ Box_int__       # '&' → '_'（★ 与上一行撞名，见 docs/learn/22）
//   f(int,int)    ⇒ f_int_int_      # '(' ')' ',' → '_'
// ★ 必须成对一致：标签发射端（函数/全局变量）与使用端（callq / %rip 读写）走同一个
//   净化函数，否则汇编期报 undefined reference。
// 对照 clang/GCC 真 mangling：_ZN4Math5scaleEi（Itanium ABI，可编码任意类型签名且可逆）。
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
// 四步 ⇒ 各自的输入与产物：
//   ① 类表里有虚函数的类  ⇒ .data 里 _ZTV<类>（.quad 数组）+ _ZTI<类>（typeinfo）
//   ② 顶层 GlobalVarDecl  ⇒ .data 里 <sym>: .quad <初值>（无初值则 0）
//   ③ functions 里的函数  ⇒ .text 里 emitFunction（序言/spill/函数体/尾声）
//   ④ m_stringLiterals    ⇒ .rodata 里 str_N: .string "..."（emitStringLiterals）
//   最后按 .text → .data → .rodata 串接返回
// 段内顺序不影响正确性：汇编器允许前向引用（call 尚未定义的标签、引用后面的数据
//   标签），真正的地址在汇编/链接期重定位填回。
// demo: class Animal { virtual int speak(); }; + main 调用之 ⇒
//       .text    Animal_speak: pushq %rbp ...   │  main: ...
//       .data    _ZTV6Animal: .quad 0 / _ZTI6Animal / Animal_speak
//       .rodata  str_0: .string "..."
std::string CodeGen::generate(
    const TranslationUnit& unit,
    const std::unordered_map<std::string, TypePtr>& classTypes,
    const std::vector<FuncDeclPtr>& functions) {

    // 保存全局类类型表指针：发射成员函数、虚调用、new 时都要回头查它
    m_classTypes = &classTypes;

    // ── 建立"类名 → 基类名"表：emitRTTI 的基类表要用（旧格式兼容路径）──
    // 递归遍历顶层声明（含命名空间内）：ClassLayout 里不存基类名，继承关系的唯一
    // 事实来源是 AST 的 ClassDecl.baseClassNames。
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
// 布局（GCC ABI 风格简化版）—— 对象的 _vptr 指向 vtable[0]（跳过前两个槽位）：
//
//   _ZTV7MyClass:
//       .quad 0                 # offset-to-top（通常为 0）
//       .quad _ZTI7MyClass      # vtable[-1]: RTTI type_info 指针
//       .quad MyClass_foo       # vtable[0]:  第一个虚函数
//       .quad MyClass_bar       # vtable[1]:  第二个虚函数
//
// vtable = "每类一张、每对象一个指针"的间接分派结构；继承/override 的差异在语义
//   阶段已固化进 vtableEntries，这里只是把那张表写成 .quad 数组。
// demo: className="Dog", vtableEntries=[{Dog_speak, index=0}] ⇒
//       .globl _ZTV3Dog / .align 8 / _ZTV3Dog:
//       .quad 0 / .quad _ZTI3Dog / .quad Dog_speak
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
// 场景（Itanium ABI §2.4）：次基类虚函数被派生类覆写 ⇒ 调用方传的是次基类 this
//   （obj + 子对象偏移），覆写函数却要最派生类 this。三条指令：
//   ① movq (%rdi), %rax       读 _vptr
//   ② movq -16(%rax), %rcx    vptr[-2] = offset-to-top = -(子对象偏移)
//   ③ addq %rcx, %rdi         this 归顶，再 jmp 真函数
// demo: 次基类子对象偏移 16 的 D::g 覆写 ⇒
//   D_g_thunk16: movq (%rdi),%rax / movq -16(%rax),%rcx / addq %rcx,%rdi / jmp D_g
// 简化：读 vptr[-2] 而非硬编码偏移（与真实 ABI 一致），但跳板名用简化 mangling。
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
// 计数式布局（简化版 __vmi_class_type_info，0/1/N 个基类统一处理）：
//   [+0] vptr（恒 0，本实现未接真实 type_info 虚表）
//   [+8] 类型名字符串指针
//   [+16] 基类计数 N │ [+24+16i] bases[i].typeinfo │ [+32+16i] bases[i].offset
// ★ 基类表是 dynamic_cast 的关键：运行时助手沿它 DFS 继承链，typeinfo 地址相等
//   即命中；N = 0（无基类）即继承链终点。
// 真 ABI 按继承形态分三种（__class_type_info / __si_class_type_info / __vmi_...），
//   本项目统一用计数式一种，故 0/1/N 个基类走同一发射路径。
// demo: class Dog : Animal ⇒
//   _ZTI3Dog: .quad 0 / .quad .Ltype_name_Dog / .quad 1 / .quad _ZTI6Animal / .quad 0
//   .rodata   .Ltype_name_Dog: .string "Dog"
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
// 做什么：把生成期间 addStringLiteral 收集的全部字符串统一发射到 .rodata（字符串
//   常量池思想：只读数据集中在 .rodata，指令流只引用标签）。
// demo: m_stringLiterals = [("str_0","hello")] ⇒
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
// 栈帧（x86-64 标准）：
//   func: pushq %rbp │ movq %rsp,%rbp │ subq $N,%rsp    # 序言：建帧 + 局部空间
//         ... 函数体 ...
//         leave │ ret                                   # 尾声：撤帧 + 返回
//
// 四段 ⇒ 各发什么：
//   序言   pushq %rbp / movq %rsp,%rbp / subq $N,%rsp   # N 由 estimateFrameSize 先数后减
//   spill  成员函数 this → -8(%rbp)；形参 i → paramRegs[regIdx]，槽位自 -8 起递减
//   体     逐条 emitStmt（结果一律落 rax）
//   尾声   leave / ret（leave = movq %rbp,%rsp; popq %rbp）
// ★ 形参一律 spill 回栈槽：System V 用寄存器传参，本实现让局部/形参/this 全走
//   m_localVars 查表 → -N(%rbp)，访问路径单一，不做寄存器存活分析。
// ★ 成员函数：this 占 rdi（Itanium C++ ABI，隐式第 0 参数），显式形参从 rsi 起
//   右移一位（regIdx = i + 1）。
// 对照 clang：CodeGenFunction::GenerateCode + PrologEpilogInserter（序言/尾声）。
// demo: int add(int a, int b) { return a + b; } ⇒
//   .globl add │ add: pushq %rbp / movq %rsp,%rbp / subq $64,%rsp
//                    movq %rdi,-8(%rbp)  # a │ movq %rsi,-16(%rbp)  # b
//                    ... 函数体（结果在 rax）... │ leave │ ret
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

    // 函数序言（Prologue）：pushq %rbp 保存调用者的帧基址（%rbp 是 callee-saved，
    //   离开前必须还原）；movq %rsp,%rbp 让新帧获得稳定锚点 —— 此后所有局部访问都是
    //   %rbp+常数偏移，与 %rsp 的瞬时位置（push/pop 变化）无关。
    emit("pushq %rbp                    # 保存调用者的帧基址到栈上");
    emit("movq %rsp, %rbp               # 建立新栈帧：rbp = rsp（此后用 rbp+偏移访问局部）");

    // 预留局部变量空间：subq $N,%rsp，N 由 estimateFrameSize 先数后减
    //   int x;                  ⇒ 计 8B（8 字节槽）
    //   Dog d;                  ⇒ 计 sizeof(Dog) 对齐 8（可能远超 8B）
    //   嵌套块里的声明          ⇒ 也数（槽位不回收，宁可帧大）
    // 保底 64B：覆盖表达式求值的临时 pushq 周转。
    // ★ P1 教训：不可固定 64 字节 —— 栈上类对象稍多几个就写穿帧底（踩坏调用者栈）。
    // 对照 clang：PrologEpilogInserter / X86FrameLowering::determineFrameLayout。
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
    // ★ static 成员函数【没有 this】（[class.static]/2）：它 ownerClassName 仍非空
    //   （符号名要带类前缀），但参数寄存器从 rdi 起算，不占隐式第 0 个。
    //   漏掉这个判据的症状：调用端按自由函数装参（rdi=arg0…），被调端却把 rdi 当
    //   this 丢进栈槽 ⇒ 形参表整体错位一个寄存器，静默算错
    //   （`C::add(3,4)` 返回 4 而非 7：a 取到 rsi=4，b 取到 rdx=垃圾）。
    // ★ 这个判据只算一次，下方形参 spill 复用同一个变量。
    //   教训：同一条语义散成两处 `ownerClassName.empty()` 时，加 static 只改一处
    //   必然错位 —— this 不再占 rdi，形参却仍右移一格，全体静默算错。
    const bool hasThis = !func->ownerClassName.empty() && !func->isStatic;

    if (hasThis) {
        m_localVars["this"] = paramOffset;
        emit(std::format("movq %{}, {}(%rbp)         # 保存 this 指针到栈槽", paramRegs[0], paramOffset));
        paramOffset -= 8;
    }

    // 形参 spill：无 this 的（自由函数 / static 成员）形参 i 用第 i 个寄存器；
    // 有 this 的整体右移一位（regIdx = i+1），因为 rdi 已被隐式第 0 参数占用
    for (size_t i = 0; i < func->parameters.size() && i < 6; i++) {
        size_t regIdx = hasThis ? i + 1 : i;
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
                        // 子对象内联在 this+field->offset 处，构造调用与基类构造同构：
                        // 实参进寄存器，this = 原始 this + 字段偏移。
                        // ★ 类类型字段不可按标量处理（踩坑史 C2）。
                        // 对照 clang：初始化列表直接调 Five::Five(int,int)（this 已调整）
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
                        // ── 标量字段：【必须】按宽度选存储指令，与读路径对称 ──
                        //   1B ⇒ movb │ ≤4B ⇒ movl │ 8B ⇒ movq
                        // ★ 一律 movq 会踩坏相邻字段（踩坑史 C3）。
                        // 对照 clang：CodeGenFunction::EmitStoreOfScalar（按 TI.Width 选指令）。
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
    //   前逆序析构该层。
    // ★ 中途 return 走 visit(ReturnStmt) → emitDtorsOnReturn() 就近析构，
    //   不再依赖"函数末尾"这个位置（BUGS.md B4 修复前那段是死代码）。
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
// 做什么：语句分发器——stmt->accept(*this) 一次虚表跳转落到对应 visit 重载（分派
//   判据见 ast_visitor.h / semantic_analyzer.h），是"lowering（降级）"的入口。
// 节点 ⇒ 目标 visit（高级结构就此逐层展开为线性指令序列）：
//   BlockStmt/IfStmt/WhileStmt/VarDeclStmt/AssignStmt/ReturnStmt/DeleteStmt/ExprStmt
//   ⇒ 同名 visit(…) 重载；对应 LLVM SelectionDAG 的类型匹配与 Legalize 阶段。
void CodeGen::emitStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    // 一次虚表跳转就落到对应的 visit（改造前是逐级 dynamic_pointer_cast 试探）。
    stmt->accept(*this);
}

// 复合语句：顺序发射子语句 + ★块尾逆序析构本块声明的类对象（RAII）★。
// 机制（[basic.stc.dcl] + [class.dtor]）：进块在 m_blockDtorStack 压一层空表
//   → 块内每个类对象声明 push_back 名字 → 出块逆序发射 ~T() 后弹层。
// demo: { Dog a; Dog b; } ⇒ Dog_Dog / Dog_Dog / 块尾 ~Dog(b) / ~Dog(a)（LIFO）
//   （int x; 之类标量不进此表，不析构）
// 嵌套块天然正确：内层弹层只析构自己那层，外层列表原封不动（与符号表
//   enterScope/exitScope 同构）。
// 提前出口：块内 return 的析构由 visit(ReturnStmt) → emitDtorsOnReturn() 补齐
//   （同一套 LIFO 规则，只是提前到 return 处；BUGS.md B4）。
//   return 之后本块末尾那段析构仍会发射，但已不可达 —— 无害的冗余。
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
// 两条路径（判据：该类 vtableEntries 里有没有 "dtor"，同 visit(DeleteStmt)）：
//   虚析构 ⇒ leaq off(%rbp),%rdi → emitVirtualCall 三部曲（运行期定派，[class.dtor]/4）
//   非虚   ⇒ leaq off(%rbp),%rdi → callq {Class}_dtor（符号约定见 registerFunction）
// demo: { Dog d; } 块尾 ⇒ leaq -16(%rbp),%rdi / movq (%rdi),%rax /
//                        movq 0(%rax),%rax / callq *%rax
// ★ this = 对象地址，栈对象没有指针变量 ⇒ 编译期常数偏移，直接 leaq 取址。
// 与 visit(DeleteStmt) 的区别：没有后续 callq free——栈帧随函数返回自动回收
//   （[basic.stc.dcl]），这正是栈对象相对堆对象的便宜之处。
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
// return 点就近析构（RAII，[class.dtor]/2、[basic.stc]）
// ─────────────────────────────────────────────────────────────────────────────
// 【为什么需要】块尾析构是挂在"块结束"这个【位置】上的，而 return 是提前出口 ——
//   函数末尾那条"统一析构"发在 leave; ret 之后，永远执行不到（BUGS.md B4：
//   带 return 的函数里局部对象从不析构，静默泄漏）。
// 做法：return 处就地补一遍析构，层序与块尾规则完全一致 ——
//   从最内层到最外层，每层按【逆声明序】；返回值先 pushq 保起来再逐层 callq。
// demo: int f() { Dog d; return 0; }
//   movq $0, %rax                           # 返回值
//   pushq %rax                              # ← 保护（析构 callq 会踩 rax）
//   leaq -16(%rbp), %rdi / callq Dog_dtor   # ~Dog()
//   popq %rax                               # 恢复返回值
//   leave / ret
// 简化点：return 之后的块尾/函数尾析构代码仍会照常发射（已不可达，无害）；
//   对照 clang：ReturnStmt → EmitBranchThroughCleanup → 统一 cleanup block，
//   布局更省代码但要跨作用域收集，教学版用"就近发射"更好讲。
bool CodeGen::emitDtorsOnReturn() {
    // 先探一遍有没有对象要析构 —— 没有就【一个字节都不发】（零漂移的关键）
    bool any = false;
    for (const auto& layer : m_blockDtorStack) {
        for (const auto& name : layer) {
            if (m_classLocals.count(name)) { any = true; break; }
        }
        if (any) break;
    }
    if (!any) return false;

    emit("pushq %rax                  # 保护返回值（析构调用会踩掉 rax）");
    for (auto layer = m_blockDtorStack.rbegin(); layer != m_blockDtorStack.rend(); ++layer) {
        for (auto it = layer->rbegin(); it != layer->rend(); ++it) {
            auto ci = m_classLocals.find(*it);
            if (ci == m_classLocals.end()) continue;
            emitComment(std::format("~{}() before return (RAII, scope exit)",
                ci->second.className));
            emitClassDtorCall(ci->second.className, ci->second.offset);
        }
    }
    emit("popq %rax                   # 恢复返回值");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 帧大小预估（序言的 subq $N 用）
// ─────────────────────────────────────────────────────────────────────────────
// 节点 ⇒ 计入字节数（switch 按 NodeKind 标签分派）：
//   VarDecl + 类类型 ⇒ totalSize 对齐 8（至少 8）│ VarDecl 其余 ⇒ 8 字节槽
//   Block / If / While ⇒ 递归进子块（嵌套块里的声明也数进去）
//   其余（表达式/return/delete…）⇒ 0
// ★ 只数声明，不数表达式求值的临时 push 空间（保底 64B 覆盖常见深度）。
// ★ 最保守算法："全部加起来"——块尾析构后槽位并不复用，宁可帧大，不可写穿。
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
// demo: int f(int a, int b) { int x; } ⇒ 8B(帧基) + 16B(两形参槽) + 8B(x) = 32B
// ★ 序言区【必须】计入：漏算会让最深的局部落到 rsp 之下，被表达式求值的
//   `pushq %rax` 与 `callq` 压入的返回地址踩掉（症状极具迷惑性，见踩坑史 C1）。
// 对照 clang：PrologEpilogInserter / X86FrameLowering::determineFrameLayout ——
//   帧大小是序言与局部布局【同一份】分配器的产物，不存在两处各算一份却对不上。
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
// 路径分派（declaredType 命中全局类表 ⇒ ②，否则 ①；auto 此时已被替换为具体类型）：
//   ① 标量 int/bool/指针 ⇒ 槽 8B；有初值 emitExpr → movq %rax,off(%rbp)，无则 movq $0
//   ② 类对象 Dog d;      ⇒ 槽 totalSize 对齐 8 → 整对象按 8B 一拍清零
//                         → 装 _vptr（多态类）→ leaq off(%rbp),%rdi / callq Dog_Dog
// ★ ②是"栈上 placement 构造"（[basic.stc.dcl]）：地址登记进 m_classLocals，块尾
//   由 visit(BlockStmt) 逆序析构 —— new 在堆上做的事这里直接在帧内完成，无 malloc
//   故析构后也无需 free。
// demo: int x = 10; ⇒ movq $10,%rax / movq %rax,-32(%rbp)
//       int y;      ⇒ movq $0,-40(%rbp)                     # 无初始化式则清零
//       Dog d;      ⇒ movq $0,off(%rbp)（按 8B 拍清零）/
//                     leaq _ZTV3Dog(%rip),%rcx / addq $16,%rcx / movq %rcx,off(%rbp) /
//                     leaq off(%rbp),%rdi / callq Dog_Dog
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
// 赋值目标三态 ⇒ 各自落点（按 NodeKind 一次 switch，三者互斥）：
//   Index  v[i] = e ⇒ 糖化 v.set(i, e)：右值 pushq 暂存 / 下标 → %rsi / this → %rdi / callq V_set
//   Var    x = e    ⇒ ① 裸字段名 age = e ≡ this->age = e（查类字段 + 偏移写入）
//                     ② m_localVars 命中 ⇒ movq %rax, off(%rbp)
//                     ③ 兜底全局        ⇒ movq %rax, <sym>(%rip)
//   Member o.f = e  ⇒ 对象地址 → %rcx，按字段宽度写：≤4B movl %eax,off(%rcx) │ 8B movq
// demo: u.age = 20;（age 偏移 0）⇒ <右值 → %rax> / <对象地址 → %rcx> / movl %eax,0(%rcx)
// ★ 裸字段名必须在全局兜底【之前】判：否则降级成 movq %rax, age(%rip) ⇒ 链接期
//   undefined reference。
// 注：字段路径把右值【求值两次】（先算一遍、算完对象地址再重算）—— 教学简化，
//   假定右值无副作用；真实编译器用寄存器分配避免重复求值。
void CodeGen::visit(AssignStmt& stmt) {
    // 计算右值到 rax
    emitExpr(stmt.value);

    // 赋值目标有三态：① 下标 v[i]（糖化成 v.set(i,value) 调用）
    // ② 裸名（局部变量 / 裸字段 / 全局变量，按作用域优先级）
    // ③ 成员访问 o.f（按 ClassLayout 查到偏移量后直接写内存）。
    // 三者互斥 —— 按节点种类一次 switch，跳表分派替代 RTTI 试探链。
    switch (stmt.target->kind) {
        case NodeKind::Unary: {
            // ── *p = v（[expr.ass]/3：解引用产生【左值】，可作赋值目标）──
            // 与 B3 的裸字段写完全同构的两步：先算出目标地址，再按宽度存进去。
            // rax 此刻已是右值（本函数开头统一求值）⇒ 压栈保序，
            // 求值指针拿到地址后弹出右值，写入 (地址)。
            auto un = std::static_pointer_cast<UnaryExpr>(stmt.target);
            if (un->op != UnaryOp::Deref) {
                throw std::runtime_error(std::format(
                    "[CodeGen Error] unsupported unary assignment target (line {})",
                    stmt.target->location.line));
            }
            emitComment("*p = ...  (解引用左值，[expr.ass]/3)");
            emit("pushq %rax                  # 暂存右值（待写入的值）到栈上");
            emitExpr(un->operand);
            emit("movq %rax, %rcx                # 目标地址存入 rcx");
            emit("popq %rax                    # 弹出右值回 rax");
            // 宽度按被指类型选（与写字段、读解引用同一条判据，避免踩坏邻居）
            TypePtr pt = un->operand->resolvedType;
            TypePtr pointee = (pt && pt->isPointer()) ? pt->pointeeType : nullptr;
            if (pointee && pointee->isBool()) {
                emit("movb %al, (%rcx)             # 按 1 字节写入");
            } else if (pointee && pointee->isInt()) {
                emit("movl %eax, (%rcx)            # 按 4 字节写入");
            } else {
                emit("movq %rax, (%rcx)            # 按 8 字节写入");
            }
        } break;
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
                    // ── 按字段宽度选存储指令，与读路径、初始化列表路径对称 ──
                    //   1B ⇒ movb │ ≤4B ⇒ movl │ 8B ⇒ movq
                    // ★ 硬编码 movl 会让 8B 字段（指针/long）丢掉高 32 位（BUGS.md B3）。
                    // 对照 clang：CodeGenFunction::EmitStoreOfScalar（按 TI.Width 选指令）。
                    // 用例：struct MyPtr { int* ptr; void set(int* x) { ptr = x; } };
                    //       ptr 宽 8 ⇒ 此处分派到 movq（旧版写死 movl，指针高 32 位丢失）
                    if (field->size == 1) {
                        emit(std::format("movb %al, {}(%rcx)    # .{} = ...（偏移 {}，1 字节写入）",
                            field->offset, var->name, field->offset));
                    } else if (field->size <= 4) {
                        emit(std::format("movl %eax, {}(%rcx)    # .{} = ...（偏移 {}，4 字节写入）",
                            field->offset, var->name, field->offset));
                    } else {
                        emit(std::format("movq %rax, {}(%rcx)    # .{} = ...（偏移 {}，8 字节写入）",
                            field->offset, var->name, field->offset));
                    }
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
                        // ★ 【必须】按字段宽度选指令，与读路径对称（踩坑史 C4）——
                        //   硬编码 movl 会截掉 8B 字段的高 32 位，症状是"写得进、
                        //   读出来错"。
                        // size <= 4 保持原指令与原注释文案不动 —— 避免无谓的
                        //   汇编/日志漂移（本项目日志即契约，见 logdiff.sh）。
                        if (field->size <= 4) {
                            emit(std::format("movl %eax, {}(%rcx)    # 写入字段 .{}（偏移 +{}）",
                                field->offset, mem->memberName, field->offset));
                        } else {
                            emit(std::format("movq %rax, {}(%rcx)    # 写入字段 .{}（偏移 +{}，8B）",
                                field->offset, mem->memberName, field->offset));
                        }
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
// 返回值算进 rax 后就地 leave / ret 撤帧（System V 规定整数返回值在 rax）。
// ★ 任意位置 return 都能正确退栈：所有局部都锚定在 %rbp 上，leave 一步还原
//   （leave = movq %rbp,%rsp; popq %rbp，ret 弹返回地址跳回调用方）。
// ★ 撤帧【之前】先就近析构本作用域链上的栈对象（RAII）——有对象才发，
//   没有则一个字节都不多发（见 emitDtorsOnReturn）。
// demo: return a + 1;（a 在 -8(%rbp)，无局部对象）⇒ movq -8(%rbp),%rax /
//       pushq %rax / movq $1,%rax / movq %rax,%rcx / popq %rax /
//       addq %rcx,%rax / leave / ret   ← 无析构，与旧版逐字节相同
// demo: int f() { Dog d; return 0; } ⇒ movq $0,%rax / pushq %rax /
//       leaq -16(%rbp),%rdi / callq Dog_dtor / popq %rax / leave / ret
void CodeGen::visit(ReturnStmt& stmt) {
    if (stmt.value) {
        emitComment("return expr");
        emitExpr(stmt.value);
        // 返回值已经在 rax 中
    } else {
        emit("movq $0, %rax               # void 返回，结果置 0");
    }
    emitDtorsOnReturn();   // ★ return 是提前出口，析构必须在这里补（BUGS.md B4）
    emit("leave                         # 恢复栈帧（movq %rbp,%rsp; popq %rbp）");
    emit("ret                           # 返回调用者（从栈上弹出返回地址）");
}

// ─────────────────────────────────────────────────────────────────────────────
// delete 语句：析构（vtableEntries 有 "dtor" ⇒ 虚析构走三部曲，否则静态 callq）
//   + callq free。对应真 C++ 的 delete = 析构 + operator delete。
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
// 降级模板（条件跳转 + 标签）：
//   <condition>                   # rax = 条件值（0 或 1）
//   testq %rax, %rax              # rax 与自身按位与置 ZF（非 0 ⇒ ZF=0）
//   je else_0                     # ZF=1 即"条件为假"时跳走
//   <then 分支>
//   jmp endif_1                   # 为真 → 跳过 else
//   else_0:
//   <else 分支>
//   endif_1:
// 无 else 时 je 直接跳 endif（不生成 else_0）。testq + je 是机器层仅剩的两条
// 控制流原语。
// demo: if (x > 0) { y = 1; } else { y = 2; } ⇒ 上面这张图（标签即实际所见）
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
// 降级模板（标签 + 条件跳出 + 回边跳转）：
//   while_begin_2: <condition>              # i > 0 的比较，rax = 0/1
//                  testq %rax, %rax / je while_end_3   # 条件为假跳出
//                  <body>                  # i = i - 1
//                  jmp while_begin_2       # 回边（back edge）
//   while_end_3:
// 回边 = 自然循环的识别依据（流图 reducibility 的经典形状：条件测试在前的基本块
//   + 一条回边）。
// demo: while (i > 0) { i = i - 1; } ⇒ 上面这张图
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
// 做什么：表达式分发器——与 emitStmt 同构，expr->accept(*this) 一次虚表跳转落到
//   对应 visit 重载。
// ★ 单累加器模型：无论表达式多复杂，结果一律落在 rax；子表达式的中间值靠
//   push/pop 经栈周转（见 visit(BinaryExpr)）。
// 节点 ⇒ 目标 visit：IntLiteral/BoolLiteral/StringLiteral/Var/Binary/Unary/Call/
//   Member/Index/New/This/NullptrLiteral/DynamicCast ⇒ 同名 visit(…) 重载。
// 注意：nullptr 没有独立的 emit 函数，就地以 visit(NullptrLiteralExpr&) 发射
//   （改造前它就在这条链的末尾）。
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
// leaq str_N(%rip),%rax 是位置无关的地址计算：目标 = 本条指令的 RIP + 汇编器算好的
// 偏移，由重定位在汇编/链接期填回，代码段无需知道绝对地址。
// demo: "hello" → .rodata 中 str_0: .string "hello"
//                 此处发射 leaq str_0(%rip), %rax
void CodeGen::visit(StringLiteralExpr& expr) {
    std::string label = addStringLiteral(expr.value);
    emit(std::format("leaq {}(%rip), %rax      # 加载字符串字面量地址（PIC）", label));
}

// 做什么：把变量的值加载进 rax。四级查找（顺序即优先级）：
//   ① m_classLocals 栈类对象   ⇒ leaq off(%rbp),%rax（★ 值语义 = 对象地址）
//   ② m_localVars 局部/形参/this ⇒ movq off(%rbp),%rax
//   ③ 裸字段名（方法体内 age ≡ this->age）⇒ movq this(%rbp),%rax / movl off(%rax),%eax
//   ④ 兜底：全局变量            ⇒ movq <sym>(%rip),%rax（asmSymbol 净化 "::"）
// demo: x（槽位 -32）⇒ movq -32(%rbp),%rax │ g（全局）⇒ movq g(%rip),%rax
//       age（Animal 字段，偏移 0）⇒ movq -8(%rbp),%rax # load this
//                                  movl 0(%rax),%eax  # load .age
// ★ ①必须排在②前：类对象槽里存的是对象内容首 8 字节（_vptr），直接加载会得到
//   _vptr 而非对象地址（虚调用碰巧能跑，字段访问与析构取址全错）。
// 兜底④保证 .s 仍合法可汇编（符号表查不到的路径 Sema 本应拦截，见 docs/learn/22 ⑦）。
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
// 指令模式：<left> → rax │ pushq %rax │ <right> → rax │ movq %rax,%rcx │
//           popq %rax │ <operation>
// ★ 左值为什么要压栈：只有 rax 一个结果寄存器，求右值会冲掉左值 —— 表达式树 →
//   线性指令的经典调度问题（LLVM 交给寄存器分配器全局优化，本实现用栈周转）。
// 按 op 分派 ⇒ 运算指令：
//   + ⇒ addq %rcx,%rax │ - ⇒ subq │ * ⇒ imulq（有符号）
//   / ⇒ cqto + idivq（rax=商）│ % ⇒ 同上 + movq %rdx,%rax（余数在 rdx）
//   == ⇒ cmpq + sete %al + movzbq（!= < > <= >= 同理，只换 setcc 后缀）
//   && ⇒ andq │ || ⇒ orq（⚠ 短路求值未实现：真 &&/|| 需条件跳转，形状等价两个嵌套 if）
// ⚠ 除法族必须先 cqto：idivq 算的是 128 位被除数 rdx:rax ÷ rcx，rdx 里的垃圾值会毁掉被除数。
// demo: a + 1（a 在 -8(%rbp)）⇒ movq -8(%rbp),%rax / pushq %rax /
//       movq $1,%rax / movq %rax,%rcx / popq %rax / addq %rcx,%rax
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
    // ── &x 要的是【地址】而不是值，故不能先 emitExpr(operand) ──
    // 操作数形态 ⇒ 指令：栈对象/局部变量 ⇒ leaq off(%rbp),%rax
    //   其它形态（字段地址、数组元素地址…）尚未实现 ⇒ 明确报错，不静默给错值。
    // 对照 clang：CodeGenFunction::EmitUnaryOp 的 UO_AddrOf 走 EmitLValue + EmitLValueAsAddr。
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

    // ── *p 解引用（[expr.unary.op]/1）：先求值拿到【地址】，再间接加载 ──
    // 与 &x 相反的一步：& 是"算出地址"，* 是"把地址当值读回来"。
    // ★ 宽度按本项目的既有约定统一 movq（8 字节）—— 与 visit(VarExpr) 加载局部变量
    //   同一条约定：本项目局部变量一律按 8 字节槽处理（教学简化，见 B3 之外的类型宽度讨论）。
    //   对照 clang：EmitLoadOfLValue 会按 lvalue 的 AST 类型选 movb/movl/movq。
    // demo: int a = 5; int* p = &a; *p
    //       ⇒ movq -16(%rbp), %rax   # rax = p（地址）
    //          movq (%rax), %rax      # rax = *(rax)
    if (expr.op == UnaryOp::Deref) {
        emitExpr(expr.operand);   // rax = 指针的值（即目标地址）
        // 宽度按【被指类型】选，与 B3 字段写的判据同源。
        // ★ 这里不能无脑 movq：`*p` 的目标可以是任意左值 —— 局部变量（8B 槽）、
        //   结构体字段（可能只有 4B）、全局 —— 读 8 字节会越界读到邻居。
        //   对照 clang：EmitLoadOfLValue 按 lvalue 的 AST 类型选 movb/movl/movq。
        TypePtr pt = expr.operand->resolvedType;
        TypePtr pointee = (pt && pt->isPointer()) ? pt->pointeeType : nullptr;
        if (pointee && pointee->isBool()) {
            emit("movzbq (%rax), %rax        # 解引用：按 1 字节读 + 零扩展");
        } else if (pointee && pointee->isInt()) {
            emit("movl (%rax), %eax          # 解引用：按 4 字节读（movl 自动清高 32 位）");
        } else {
            emit("movq (%rax), %rax          # 解引用：按 8 字节读");
        }
        return;
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
        case UnaryOp::Deref:
            // 同上：解引用也已提前 return（它要的是地址而非值），此处不可达
            throw std::runtime_error(
                "[CodeGen Error] UnaryOp::Deref should be handled before emitExpr");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 函数调用
// ─────────────────────────────────────────────────────────────────────────────
// System V AMD64 参数传递：rdi / rsi / rdx / rcx / r8 / r9，返回值 rax。
// callee 形态 ⇒ 走哪条路：
//   obj.vf() / p->vf() 且命中 vtable 条目 ⇒ 虚调用 emitVirtualCall（三部曲）
//   obj.m()  / p->m()  非虚方法          ⇒ emitExpr(obj) → pushq → popq %rdi，
//                                          实参逆序 popq 到 rsi/rdx/…，callq 类名_方法名
//   f(a,b)             自由函数          ⇒ 实参逆序 push → popq rdi/rsi/… → callq f
// ★ 实参为什么先逐个压栈再逆序弹出：求第 i+1 个实参会破坏 rax 中第 i 个的值，
//   栈是天然暂存区；逆序弹出恰好让"第 1 个实参"最后进入第 1 个参数寄存器。
// demo: add(1,2) ⇒ movq $1,%rax / pushq %rax / movq $2,%rax / pushq %rax /
//                   popq %rsi / popq %rdi / callq add
//       d.get(5)（非虚）⇒ this(d 的地址) → rdi，5 → rsi，callq Dog_get
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
            if (!mem->resolvedCalleeSymbol.empty()) {
                // 成员模板实例：符号由 Sema 推导+实例化后回填
                // （`S_id_int` 这种——同一方法名的多个实例必须各有符号）
                funcName = mem->resolvedCalleeSymbol;
            } else if (objType && objType->isClass()) {
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
// ptr->vfunc(args) 在机器码层面【不能】直接 call —— 编译期不知道 ptr 的具体类型，
// 必须经 vtable 在运行期查出真地址："编译期看符号，运行期看偏移量"：
//
//   编译期  ptr->vfunc()  ⇒ 查符号表得 vtable 下标（= 本函数的 vtableIndex 参数）
//   运行期  [obj+0] → vtable → [vtable+index*8] → 真地址
//
// 实际发射（demo: pet->speak()，Animal，speak 在 vtable[0]，无实参）：
//   pushq %rdi             # 暂存 this（求实参要用 rax 与参数寄存器）
//   popq %rdi              # 弹回 this —— 排在逆序弹实参之后，兼作第 0 实参
//   movq (%rdi), %rax      # (a) 对象首 8 字节 = _vptr
//   movq 0(%rax), %rax     # (b) vtable[index]（下标编译期定死）
//   callq *%rax            # (c) 间接调用（表项内容运行期才定）
//
// className/methodName 仅用于生成可读注释；args 不含 this；vtableIndex 是编译期常数。
//
// ⚠ 两条约定必须统一，否则就是段错误：
//   1. this 来源：进入本函数时对象地址必须已在 rdi —— visit(CallExpr) 走
//      "emitExpr(object) → movq %rax,%rdi" 喂入，块尾析构/visit(DeleteStmt) 走
//      "leaq off(%rbp),%rdi"（栈对象）喂入。
//   2. 偏移公式 index*8：emitNew 与构造函数把 _vptr 写成表首+16（已指向 vtable[0]），
//      从 _vptr 起按 index*8 取项即可；误用 (index+2)*8 会越过表尾读到垃圾地址。
void CodeGen::emitVirtualCall(
    const std::string& className,
    const std::string& methodName,
    const std::vector<ExprPtr>& args,
    uint32_t vtableIndex) {

    emitComment(std::format("VIRTUAL CALL: {}::{} (vtable[{}])",
        className, methodName, vtableIndex));

    // ─── 保存对象地址 ───
    // this 进入时在 rdi；先压栈保护（下面求/弹实参会反复使用 rax 与
    // 参数寄存器），最后弹回 rdi 兼作第 0 实参
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
// 成员访问 obj.field（字段名在此彻底消失，只剩 ClassLayout 算好的数字偏移量）
// ─────────────────────────────────────────────────────────────────────────────
// 三步：① 对象地址 → rax（"." 走 leaq 栈对象地址 / "->" 对象表达式本身就是指针值）
//       ② findField 查 ClassLayout 得偏移（语义阶段已算好）
//       ③ 按字段宽度从 [rax+off] 取值 → rax
//   字段宽度分派 ⇒ 1B movzbq（bool）│ ≤4B movl（int，32 位加载自动零扩展）│ 8B movq（指针/long）
// ★ 类类型字段 ⇒ leaq off(%rax),%rax 取【地址】而非取值：子对象内联在父对象里，
//   走 movq 取值会把子对象头 8 字节当指针解引用 ⇒ 段错误（踩坑史 C5）；下一层 .a 再叠偏移。
// demo: p->age（age 偏移 0，p 在 -8(%rbp)）⇒
//       movq -8(%rbp), %rax      # load p（对象地址）
//       movl 0(%rax), %eax       # .age（偏移 0，4B int）
// 兜底：查不到偏移时按偏移 0 读取并留注释（语义阶段本应已拦截）。
// 对照 clang：CodeGenFunction::EmitMemberExpr（链式访问折叠为常量总偏移，本实现逐层 leaq）。
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
                // ★ 走 movq 取值会把子对象头 8 字节当指针解引用 ⇒ 段错误（踩坑史 C5）。
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
// 真 C++ 里 v[i] 是 operator[] 重载调用（[over.sub]），决议在语义阶段完成；minicc
//   无运算符重载，把决议"固化"成约定：类提供 at() 获得下标【读】能力，【写】走
//   set()（在 visit(AssignStmt) 里）。inferIndex 已校验 at() 存在且形参匹配。
// demo: v[i]（v 是栈上容器对象，i 在 -16(%rbp)）⇒
//       leaq -24(%rbp),%rax / pushq %rax / movq -16(%rbp),%rax / pushq %rax /
//       popq %rsi / popq %rdi / callq Vector_at
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
// 步骤 ⇒ 指令：
//   ① 分配   movq $size,%rdi / callq malloc / pushq %rax（暂存对象指针）
//   ② 装 vptr leaq _ZTV<类>(%rip),%rcx / addq $16,%rcx（跳过 ott 与 RTTI，指向 vtable[0]）
//             / movq (%rsp),%rax / movq %rcx,(%rax)（次基类再来一遍，偏移不同）
//   ③ 构造   实参逆序 push → popq rsi..r9 → movq (%rsp),%rdi / callq 类名_类名[_N]
//   ④ 返回值 popq %rax
// 真 C++ 的 new = operator new(sizeof T)（GCC mangling _Znwm）+ 构造函数调用；本项目
//   简化为直接 callq malloc（不抛 bad_alloc），但 _vptr 安装方式与真实 ABI 完全一致。
// demo: new Dog()（Dog 大小 8，含 vtable）⇒
//       movq $8,%rdi / callq malloc / leaq _ZTV3Dog(%rip),%rcx /
//       addq $16,%rcx      # 跳过 offset-to-top 与 RTTI，指向 vtable[0]
//       movq %rcx,(%rax)   # obj._vptr = vtable
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
// 做什么：把 this 指针加载进 rax —— this 已在函数序言里作为隐式形参 spill 进栈槽
//   （见 emitFunction 的成员函数分支），此处按普通局部变量查表加载即可：
//   "this 只是一个普通参数"正是 Itanium ABI 的本质。
// demo: this（成员函数内，槽位 -8）⇒ movq -8(%rbp), %rax
// 兜底：不在成员函数里 ⇒ 发 WARNING 注释 + xorq %rax,%rax 清零（保证 .s 可汇编）。
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
// 真 C++ 的 dynamic_cast 不做编译期静态变换（[expr.dynamic.cast]），而是把问题推迟
//   到运行时：取对象 vptr → vtable[-1] 的 typeinfo → 沿继承链找目标类型（Itanium ABI）。
// 步骤 ⇒ 指令：
//   ① 求值操作数  emitExpr(operand)                        # %rax = 源对象指针
//   ② 装参       movq %rax,%rdi（对象）/ leaq _ZTI<目标>(%rip),%rsi（目标 typeinfo）
//   ③ 调用       callq __minicc_dynamic_cast（助手由 generate() 末尾按需发射）
//   ④ 结果       %rax = 成功→原指针 / 失败→0（即表达式结果）
// demo: dynamic_cast<Dog*>(p) ⇒ emitExpr(p) / leaq _ZTI3Dog(%rip),%rsi /
//                               movq %rax,%rdi / callq __minicc_dynamic_cast
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
// 参数：%rdi = 对象指针，%rsi = 目标 typeinfo │ 返回：%rax = 成功→调整后指针，失败→0
// 数据依据（emitRTTI 的计数式布局，数字可对照）：
//   对象首 8B = _vptr → vtable[0] │ vtable[-2] = offset-to-top │ vtable[-1] = 本对象 typeinfo
//   typeinfo： [+16] 基类计数 N │ [+24+16i] bases[i].typeinfo │ [+32+16i] bases[i].offset
// ★ 必须归顶：this 可能是次基类子对象指针，结果 = 最派生对象地址 + 累积偏移
//   （真 ABI 还要处理菱形虚继承，本实现按计数式布局统一 DFS）。
// demo: dynamic_cast<Dog*>(p)：命中 ⇒ %rax = 调整后指针；未命中 ⇒ %rax = 0
// 对照 clang：CodeGenFunction::EmitDynamicCast + __dynamic_cast（libc++abi）。
void CodeGen::emitDynamicCastHelper() {
    // ── 运行时助手（多继承版）：DFS 遍历 typeinfo 树 ──
    // demo: class Dog : Animal，p 指向 Dog；dynamic_cast<Animal*>(p) ⇒
    //   ti = _ZTI3Dog ≠ _ZTI6Animal ⇒ N=1 ⇒ DFS(bases[0].ti=_ZTI6Animal, acc+0) 命中
    //   ⇒ 返回 top + 0（单继承偏移恒 0；次基类才靠 acc 累加调整）
    //
    // 算法（DFS 遍历 typeinfo 树）：
    //   1. vptr = *obj;  top = obj + vptr[-2]（offset-to-top 归顶）;  ti = vptr[-1]
    //   2. DFS(ti, target, top, acc=0):
    //        ti == target → return top + acc
    //        N = ti[+16]（base count）
    //        for i in 0..N: r = DFS(bases[i].ti, target, top, acc + bases[i].offset)
    //        return null
    //
    // RTTI 计数式布局（emitRTTI 输出）：
    //   [+0] vptr(0) │ [+8] name │ [+16] base count N │
    //   [+24] bases[0].typeinfo │ [+32] bases[0].offset │ [+40] bases[1].typeinfo ...

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
