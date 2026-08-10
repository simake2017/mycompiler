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

    // ── 生成数据段：vtable、RTTI、字符串字面量 ──

    // 为每个有虚函数的类生成 vtable 和 RTTI
    for (auto& [name, type] : classTypes) {
        if (type->classLayout.hasVTable) {
            emitVTable(name, type);
            emitRTTI(name, type);
        }
    }

    // ── 生成代码段：所有函数 ──
    for (auto& func : functions) {
        if (func->body) {
            emitFunction(func);
        }
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
    emitData(std::format("    .globl {}", vtableLabel));
    // 8 字节对齐：表项是指针，对齐保证单次 8B 读取不跨界
    emitData(std::format("    .align 8", vtableLabel));
    emitData(std::format("{}:", vtableLabel));
    // 槽 0：offset-to-top —— 多继承时当前子对象到最派生对象的距离
    //       （本项目不支持多继承，恒为 0）
    emitData("    .quad 0                    # offset to top");
    // 槽 1：type_info 指针，即 vtable[-1]（_vptr 向前退一格即可取到，
    //       typeid/异常类型匹配用它）
    emitData(std::format("    .quad {}       # RTTI type_info pointer (vtable[-1])",
        rttiLabel));

    // 虚函数条目
    // 每个 .quad 是一个函数指针，运行期 callq *%rax 的目标；
    // 顺序与语义阶段构建的 vtableEntries 一致 —— 下标就是编译期约定
    for (auto& entry : classType->classLayout.vtableEntries) {
        emitData(std::format("    .quad {}   # vtable[{}]: {}",
            entry.mangledName, entry.index, entry.mangledName));
    }

    emitData("");
}

// ═════════════════════════════════════════════════════════════════════════════
// RTTI type_info 生成
// ═════════════════════════════════════════════════════════════════════════════
// type_info 结构（简化版）：
//   _ZTI7MyClass:
//       .quad _ZTVN10__cxxabiv117__class_type_infoE  # vtable for type_info
//       .quad .Ltype_name_MyClass                     # 类型名称字符串
//
// 这使得运行时可以通过 typeid() 获取类型信息。
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：发射类的 type_info 对象（GCC ABI 简化版）。
// 理论：真实 Itanium ABI 中 type_info 本身是有虚表的 C++ 对象
//       （__class_type_info / __si_class_type_info 等），本项目把
//       其虚表指针简化为 0，只保留"类型名字符串"这一个语义。
// demo（输入）: className="Dog"
// demo（输出）: _ZTI3Dog:  .quad 0  /  .quad .Ltype_name_Dog
//               .rodata 中 .Ltype_name_Dog: .string "Dog"
void CodeGen::emitRTTI(const std::string& className, TypePtr) {
    std::string rttiLabel = NameMangler::mangleRTTI(className);
    std::string nameLabel = std::format(".Ltype_name_{}", className);

    // RTTI 结构
    emitData(std::format("    .globl {}", rttiLabel));
    emitData(std::format("    .align 8"));
    emitData(std::format("{}:", rttiLabel));
    emitData(std::format("    .quad 0                    # type_info vtable (simplified)"));
    emitData(std::format("    .quad {}                 # type name string", nameLabel));

    // 类型名称字符串
    emitRodata(std::format("{}:", nameLabel));
    emitRodata(std::format("    .string \"{}\"           # type name", className));

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
        emitRodata(std::format("{}:", label));
        emitRodata(std::format("    .string \"{}\"", value));
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
    // 普通函数/方法直接用名字（成员方法的"类名_方法名"由 Sema 阶段填好）
    std::string funcName = func->mangledName.empty()
        ? func->name : func->mangledName;

    // .globl 导出符号：main 入口与跨函数/跨文件调用都靠它被链接器找到
    emit(std::format("    .globl {}", funcName));
    emit(std::format("{}:", funcName));

    emitComment(std::format("Function: {} (params: {})",
        func->name, func->parameters.size()));

    // 函数序言（Prologue）
    // 理论：pushq %rbp 保存调用者的帧基址（rbp 是 callee-saved 寄存器）；
    //       movq %rsp,%rbp 让新帧获得稳定锚点——此后所有局部访问都是
    //       %rbp+常数偏移，与 %rsp 的瞬时位置（push/pop 变化）无关。
    emit("pushq %rbp");
    emit("movq %rsp, %rbp");

    // 预留局部变量空间（先扫描一遍计算大小）
    // 简化：先预留 64 字节
    // 教学简化：不做精确帧大小计算（LLVM 由帧布局 pass 完成）；
    // 局部变量从形参区下方向低地址逐个分配，不超出预留范围即可
    emit("subq $64, %rsp");

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
        emit(std::format("movq %{}, {}(%rbp)", paramRegs[0], paramOffset));
        paramOffset -= 8;
    }

    // 形参 spill：非成员函数形参 i 用第 i 个寄存器；
    // 成员函数整体右移一位（regIdx = i+1），因为 rdi 已被 this 占用
    for (size_t i = 0; i < func->parameters.size() && i < 6; i++) {
        size_t regIdx = func->ownerClassName.empty() ? i : i + 1;
        if (regIdx < 6) {
            m_localVars[func->parameters[i].name] = paramOffset;
            emit(std::format("movq %{}, {}(%rbp)    # param: {}",
                paramRegs[regIdx], paramOffset, func->parameters[i].name));
            paramOffset -= 8;
        }
    }

    // 栈分配指针接到形参区末尾，之后的局部变量从这里继续向低地址分配
    m_currentStackOffset = paramOffset;

    // 生成函数体
    if (func->body) {
        for (auto& stmt : func->body->statements) {
            emitStmt(stmt);
        }
    }

    // 如果函数没有显式 return，添加默认返回
    // void 函数补一个确定的 rax 值，避免调用方读到不确定的旧值。
    // 注意：若函数体已经用 return 结束（自带 leave/ret），这里再补的
    // leave/ret 就是不可达的死代码——教学实现不做控制流分析来消除它，
    // 汇编器与 CPU 都不介意（.s 里能看到连续的 leave/ret leave/ret）。
    if (func->returnType->isVoid()) {
        emit("movq $0, %rax");
    }

    // 函数尾声（Epilogue）
    // leave = movq %rbp,%rsp; popq %rbp（一步回收栈帧+恢复旧帧基址）
    // ret   = 弹出返回地址跳回调用方；返回值已按约定留在 rax
    emit("leave");
    emit("ret");
    emit("");
}

// ═════════════════════════════════════════════════════════════════════════════
// 语句生成
// ═════════════════════════════════════════════════════════════════════════════
// 做什么：语句分发器——按 AST 节点动态类型派发到对应 emit 函数。
// 理论：这就是"lowering（降级）"的入口：高级结构（声明/赋值/if/while）
//       逐层展开为线性指令序列。对应 LLVM SelectionDAG 的类型匹配与
//       Legalize 阶段，区别是这里用 dynamic_pointer_cast 手写派发。
void CodeGen::emitStmt(StmtPtr stmt) {
    if (auto s = std::dynamic_pointer_cast<BlockStmt>(stmt))
        emitBlockStmt(s);
    else if (auto s = std::dynamic_pointer_cast<VarDeclStmt>(stmt))
        emitVarDecl(s);
    else if (auto s = std::dynamic_pointer_cast<AssignStmt>(stmt))
        emitAssign(s);
    else if (auto s = std::dynamic_pointer_cast<ReturnStmt>(stmt))
        emitReturn(s);
    else if (auto s = std::dynamic_pointer_cast<IfStmt>(stmt))
        emitIf(s);
    else if (auto s = std::dynamic_pointer_cast<WhileStmt>(stmt))
        emitWhile(s);
    else if (auto s = std::dynamic_pointer_cast<ExprStmt>(stmt))
        emitExprStmt(s);
}

// 复合语句：顺序发射各子语句即可（结构化编程的顺序语义）。
// 教学简化：不处理块级作用域回收——块内声明的变量槽在块结束后不释放，
// 变量名在函数剩余部分仍可解析（真实编译器会在出作用域时回收槽位）。
void CodeGen::emitBlockStmt(std::shared_ptr<BlockStmt> block) {
    for (auto& stmt : block->statements) {
        emitStmt(stmt);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 变量声明
// ─────────────────────────────────────────────────────────────────────────────
// auto x = 10; 到了这里，auto 已经被替换为 int，所以直接生成：
//   movq $10, -offset(%rbp)
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：为新变量分配一个 8 字节栈槽，并生成初始化。
// 理论（栈分配）：局部变量的生存期 = 所在函数帧的生存期，用 %rbp 相对
//       偏移寻址。本项目统一按 8 字节槽分配（int/bool/指针同宽，教学简化，
//       不做对齐/宽度优化）。
// demo: int x = 10;  →  movq $10, %rax
//                       movq %rax, -32(%rbp)    # store to x
//       int y;       →  movq $0, -40(%rbp)      # 无初始化式则零初始化
void CodeGen::emitVarDecl(std::shared_ptr<VarDeclStmt> decl) {
    // 栈向低地址增长：先把分配指针下移 8 字节，再把新槽位登记进变量表
    m_currentStackOffset -= 8;
    m_localVars[decl->name] = m_currentStackOffset;

    if (decl->initializer) {
        emitComment(std::format("var {} = ...", decl->name));
        emitExpr(decl->initializer);
        emit(std::format("movq %rax, {}(%rbp)    # store to {}",
            m_currentStackOffset, decl->name));
    } else {
        // 零初始化
        emit(std::format("movq $0, {}(%rbp)    # zero init {}",
            m_currentStackOffset, decl->name));
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
void CodeGen::emitAssign(std::shared_ptr<AssignStmt> stmt) {
    // 计算右值到 rax
    emitExpr(stmt->value);

    // 赋值目标
    if (auto var = std::dynamic_pointer_cast<VarExpr>(stmt->target)) {
        auto it = m_localVars.find(var->name);
        if (it != m_localVars.end()) {
            emit(std::format("movq %rax, {}(%rbp)    # {} = ...",
                it->second, var->name));
        }
    }
    else if (auto mem = std::dynamic_pointer_cast<MemberExpr>(stmt->target)) {
        // ─── 字段访问的消除 ───
        // 将 obj.field = value 转化为 [objAddr + fieldOffset] = value
        emitComment(std::format("member assign: .{} = ...", mem->memberName));

        // 先计算对象地址到 rcx
        emitExpr(mem->object);
        emit("movq %rax, %rcx                # object address");

        // 计算右值到 rax
        emitExpr(stmt->value);

        // 查找字段偏移量
        if (mem->object->resolvedType) {
            TypePtr objType = mem->object->resolvedType;
            if (mem->isArrow && objType->isPointer()) {
                objType = objType->pointeeType;
            }
            if (objType->isClass()) {
                auto field = objType->classLayout.findField(mem->memberName);
                if (field) {
                    emit(std::format("movl %eax, {}(%rcx)    # .{} (offset {})",
                        field->offset, mem->memberName, field->offset));
                    return;
                }
            }
        }

        // 如果找不到偏移量，生成通用代码
        emit("movq %rax, (%rcx)              # store to member");
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
void CodeGen::emitReturn(std::shared_ptr<ReturnStmt> stmt) {
    if (stmt->value) {
        emitComment("return expr");
        emitExpr(stmt->value);
        // 返回值已经在 rax 中
    } else {
        emit("movq $0, %rax                  # return void");
    }
    emit("leave");
    emit("ret");
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
void CodeGen::emitIf(std::shared_ptr<IfStmt> stmt) {
    std::string elseLabel = newLabel("else");
    std::string endLabel = newLabel("endif");

    emitComment("if condition");
    emitExpr(stmt->condition);
    emit("testq %rax, %rax");
    emit(std::format("je {}", stmt->elseBranch ? elseLabel : endLabel));

    emitComment("then branch");
    emitStmt(stmt->thenBranch);

    if (stmt->elseBranch) {
        emit(std::format("jmp {}", endLabel));
        emit(std::format("{}:", elseLabel));
        emitComment("else branch");
        emitStmt(stmt->elseBranch);
    }

    emit(std::format("{}:", endLabel));
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
void CodeGen::emitWhile(std::shared_ptr<WhileStmt> stmt) {
    std::string beginLabel = newLabel("while_begin");
    std::string endLabel = newLabel("while_end");

    emit(std::format("{}:", beginLabel));
    emitComment("while condition");
    emitExpr(stmt->condition);
    emit("testq %rax, %rax");
    emit(std::format("je {}", endLabel));

    emitComment("while body");
    emitStmt(stmt->body);
    emit(std::format("jmp {}", beginLabel));

    emit(std::format("{}:", endLabel));
}

// 表达式语句：只求值、结果（rax）丢弃；价值在求值过程产生的副作用
// （典型如 ptr->speak() 这类调用语句）。
void CodeGen::emitExprStmt(std::shared_ptr<ExprStmt> stmt) {
    emitExpr(stmt->expr);
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
void CodeGen::emitExpr(ExprPtr expr) {
    if (auto e = std::dynamic_pointer_cast<IntLiteralExpr>(expr))
        emitIntLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<BoolLiteralExpr>(expr))
        emitBoolLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<StringLiteralExpr>(expr))
        emitStringLiteral(e);
    else if (auto e = std::dynamic_pointer_cast<VarExpr>(expr))
        emitVar(e);
    else if (auto e = std::dynamic_pointer_cast<BinaryExpr>(expr))
        emitBinary(e);
    else if (auto e = std::dynamic_pointer_cast<UnaryExpr>(expr))
        emitUnary(e);
    else if (auto e = std::dynamic_pointer_cast<CallExpr>(expr))
        emitCall(e);
    else if (auto e = std::dynamic_pointer_cast<MemberExpr>(expr))
        emitMember(e);
    else if (auto e = std::dynamic_pointer_cast<NewExpr>(expr))
        emitNew(e);
    else if (auto e = std::dynamic_pointer_cast<ThisExpr>(expr))
        emitThis(e);
    else if (std::dynamic_pointer_cast<NullptrLiteralExpr>(expr)) {
        emit("xorq %rax, %rax                # nullptr = 0");
    }
}

// 整数字面量：立即数直接进 rax。
// demo: 42 → movq $42, %rax
void CodeGen::emitIntLiteral(std::shared_ptr<IntLiteralExpr> expr) {
    emit(std::format("movq ${}, %rax           # int literal", expr->value));
}

// 布尔字面量：本项目 bool 按整数 0/1 表示，
// 与比较运算 setcc/movzbq 的产出形式天然一致。
// demo: true → movq $1, %rax      false → movq $0, %rax
void CodeGen::emitBoolLiteral(std::shared_ptr<BoolLiteralExpr> expr) {
    emit(std::format("movq ${}, %rax           # bool literal",
        expr->value ? 1 : 0));
}

// 字符串字面量：登记进常量池取标签，再用 RIP 相对寻址取地址。
// 理论：leaq str_N(%rip), %rax 是位置无关的地址计算方式——
//       目标地址 = 执行本条指令时的 RIP + 汇编器算好的偏移，
//       由重定位在汇编/链接期填回，代码段无需知道绝对地址。
// demo: "hello" → .rodata 中 str_0: .string "hello"
//                 此处发射 leaq str_0(%rip), %rax
void CodeGen::emitStringLiteral(std::shared_ptr<StringLiteralExpr> expr) {
    std::string label = addStringLiteral(expr->value);
    emit(std::format("leaq {}(%rip), %rax      # string literal", label));
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
void CodeGen::emitVar(std::shared_ptr<VarExpr> expr) {
    // 先查局部变量
    auto it = m_localVars.find(expr->name);
    if (it != m_localVars.end()) {
        emit(std::format("movq {}(%rbp), %rax    # load {}",
            it->second, expr->name));
        return;
    }

    // 如果在类方法中，查类字段（通过 this 指针访问）
    if (!m_currentClassName.empty() && m_currentClassType) {
        auto field = m_currentClassType->classLayout.findField(expr->name);
        if (field) {
            // 通过 this 指针访问字段
            auto thisIt = m_localVars.find("this");
            if (thisIt != m_localVars.end()) {
                emit(std::format("movq {}(%rbp), %rax    # load this", thisIt->second));
                emit(std::format("movl {}(%rax), %eax    # load .{} (offset {})",
                    field->offset, expr->name, field->offset));
                return;
            }
        }
    }

    emit(std::format("# WARNING: undefined variable '{}'", expr->name));
    emit("xorq %rax, %rax");
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
void CodeGen::emitBinary(std::shared_ptr<BinaryExpr> expr) {
    emitComment("binary expr");

    // 左操作数 → rax → 压栈
    emitExpr(expr->left);
    emit("pushq %rax");

    // 右操作数 → rax → rcx
    emitExpr(expr->right);
    emit("movq %rax, %rcx");

    // 恢复左操作数到 rax
    emit("popq %rax");

    switch (expr->op) {
        case BinaryOp::Add:
            emit("addq %rcx, %rax              # +");
            break;
        case BinaryOp::Sub:
            emit("subq %rcx, %rax              # -");
            break;
        case BinaryOp::Mul:
            emit("imulq %rcx, %rax             # *");
            break;
        // ── 除法族 ──
        // x86 的 idivq %rcx 计算的是 128 位被除数 rdx:rax ÷ rcx：
        // 商 → rax，余数 → rdx。cqto 先把 rax 符号扩展到 rdx:rax，
        // 否则 rdx 的垃圾值会毁掉被除数（经典陷阱）。
        case BinaryOp::Div:
            emit("cqto                         # sign extend rax → rdx:rax");
            emit("idivq %rcx                   # / (rax = quotient)");
            break;
        case BinaryOp::Mod:
            emit("cqto");
            emit("idivq %rcx                   # % (rdx = remainder)");
            emit("movq %rdx, %rax");
            break;
        // ── 比较族 ──
        // 三步缺一不可：cmpq 做减法只置标志位 → setcc 把标志写成 8 位
        // 0/1（setcc 只能写 %al 等 8 位寄存器）→ movzbq 零扩展回 64 位，
        // 与"结果统一在 rax"的约定对齐。
        case BinaryOp::Eq:
            emit("cmpq %rcx, %rax");
            emit("sete %al                     # ==");
            emit("movzbq %al, %rax");
            break;
        case BinaryOp::Neq:
            emit("cmpq %rcx, %rax");
            emit("setne %al                    # !=");
            emit("movzbq %al, %rax");
            break;
        case BinaryOp::Lt:
            emit("cmpq %rcx, %rax");
            emit("setl %al                     # <");
            emit("movzbq %al, %rax");
            break;
        case BinaryOp::Gt:
            emit("cmpq %rcx, %rax");
            emit("setg %al                     # >");
            emit("movzbq %al, %rax");
            break;
        case BinaryOp::Le:
            emit("cmpq %rcx, %rax");
            emit("setle %al                    # <=");
            emit("movzbq %al, %rax");
            break;
        case BinaryOp::Ge:
            emit("cmpq %rcx, %rax");
            emit("setge %al                    # >=");
            emit("movzbq %al, %rax");
            break;
        // ── 逻辑与/或 ──
        // 本实现直接按位与/或（要求操作数已规范化为 0/1，比较指令恰好
        // 保证这一点）。⚠ 理论点：这放弃了短路求值（short-circuit）——
        // 真正的 &&/|| 必须用条件跳转实现"左值为假则不算右值"，
        // 其降级形状等价于两个嵌套 if（可作课后练习）。
        case BinaryOp::And:
            emit("andq %rcx, %rax              # &&");
            break;
        case BinaryOp::Or:
            emit("orq %rcx, %rax               # ||");
            break;
    }
}

// 做什么：一元运算。Neg 直接 negq 取负（先求值再原地取负）；
//       Not 用 testq + sete 实现"等于 0 则为 1"的逻辑非。
// demo: -x → <加载 x 进 rax>; negq %rax
//       !x → <加载 x 进 rax>; testq %rax, %rax; sete %al; movzbq %al, %rax
void CodeGen::emitUnary(std::shared_ptr<UnaryExpr> expr) {
    emitExpr(expr->operand);

    switch (expr->op) {
        case UnaryOp::Neg:
            emit("negq %rax                    # negate");
            break;
        case UnaryOp::Not:
            emit("testq %rax, %rax");
            emit("sete %al                     # logical not");
            emit("movzbq %al, %rax");
            break;
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
void CodeGen::emitCall(std::shared_ptr<CallExpr> expr) {
    emitComment("function call");

    // 检查是否是方法调用
    // ── 路径①/②：callee 形如 obj.method —— 先解析对象类型再定虚实 ──
    if (auto mem = std::dynamic_pointer_cast<MemberExpr>(expr->callee)) {
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
                            emitVirtualCall(objType->name, mem->memberName,
                                          expr->arguments, entry.index);
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
            emit("pushq %rax                   # save this pointer");

            // 计算参数
            // （逐个压栈暂存，随后逆序弹入 rsi/rdx/rcx/r8/r9）
            for (size_t i = 0; i < expr->arguments.size() && i < 5; i++) {
                emitExpr(expr->arguments[i]);
                emit("pushq %rax");
            }

            // 恢复参数到寄存器
            for (int i = static_cast<int>(expr->arguments.size()) - 1; i >= 0 && i < 5; i--) {
                static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
                emit(std::format("popq %{}", regs[i]));
            }

            // this 指针到 rdi
            emit("popq %rdi                    # this pointer");

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
            emit(std::format("callq {}               # method call", funcName));
            return;
        }
    }

    // 普通函数调用
    // 计算参数并放入对应寄存器
    for (size_t i = 0; i < expr->arguments.size() && i < 6; i++) {
        emitExpr(expr->arguments[i]);
        emit("pushq %rax");
    }

    // 恢复参数到寄存器（逆序）
    static const char* regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (int i = static_cast<int>(expr->arguments.size()) - 1; i >= 0; i--) {
        if (i < 6) {
            emit(std::format("popq %{}", regs[i]));
        }
    }

    // 确定函数名
    std::string funcName;
    if (auto var = std::dynamic_pointer_cast<VarExpr>(expr->callee)) {
        funcName = var->name;
    }

    emit(std::format("callq {}               # function call", funcName));
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
//       movq 16(%rax), %rax          # (b) rax = 表项（index 0 → 偏移 16）
//       callq *%rax                  # (c) 间接调用
// ⚠ 实现细节（教学点，值得对照真实 ABI 思考）：
//   1. this 来源：当前实现假定进入本函数时对象地址已在 rdi
//      （完备做法是先对 mem->object 求值并送入 rdi）。
//   2. 偏移公式：(index+2)*8 把 %rax 视为"表首地址"；而 emitNew 把
//      _vptr 写为 表首+16（已指向 vtable[0]）。两处约定尚未统一——
//      标准 GCC 做法是 _vptr=表首+16 且按 index*8 取项。
//   3. rbx 按 System V ABI 是 callee-saved 寄存器，这里直接借用未保存，
//      依赖"本项目所有函数都不修改 rbx"的内部约定（真实编译器会在
//      序言/尾声保存恢复）。
void CodeGen::emitVirtualCall(
    const std::string& className,
    const std::string& methodName,
    const std::vector<ExprPtr>& args,
    uint32_t vtableIndex) {

    emitComment(std::format("VIRTUAL CALL: {}::{} (vtable[{}])",
        className, methodName, vtableIndex));

    // ─── 准备参数 ───
    // 第一个参数是 this 指针（对象地址）

    // 先计算参数（跳过 this）
    // 显式实参最多 5 个：rdi 留给 this，rsi/rdx/rcx/r8/r9 装 5 个实参
    for (size_t i = 0; i < args.size() && i < 5; i++) {
        emitExpr(args[i]);
        emit("pushq %rax");
    }

    // 计算对象地址（this 指针）
    // （假定 rdi 中已是对象地址；借用 callee-saved 的 rbx 暂存，
    //   避免下面弹参寄存器时互相覆盖，见函数头注 3）
    emit("movq %rdi, %rbx              # save object pointer");

    // 恢复参数到寄存器
    for (int i = static_cast<int>(args.size()) - 1; i >= 0 && i < 5; i--) {
        static const char* regs[] = {"rsi", "rdx", "rcx", "r8", "r9"};
        emit(std::format("popq %{}", regs[i]));
    }

    // this 指针到 rdi
    emit("movq %rbx, %rdi              # this pointer");

    // ─── 虚函数调用三部曲 ───

    // (a) 从对象偏移量 +0 处读出 _vptr 指针
    emit("# ═══ Virtual Call Step (a): Read _vptr from object ═══");
    emit("movq (%rdi), %rax            # rax = obj._vptr (at offset 0)");

    // (b) 从 vtable 中加上 index 偏移量，读出真正的函数地址
    //     vtable 布局：[0] = offset-to-top, [1] = RTTI, [2+] = 虚函数
    //     所以实际偏移量 = (index + 2) * 8
    uint32_t vtableOffset = (vtableIndex + 2) * 8;
    emit("# ═══ Virtual Call Step (b): Load function address from vtable ═══");
    emit(std::format("movq {}(%rax), %rax       # rax = vtable[{}] (offset {})",
        vtableOffset, vtableIndex, vtableOffset));

    // (c) 间接跳转到该地址执行
    emit("# ═══ Virtual Call Step (c): Jump to the real function ═══");
    emit("callq *%rax                  # indirect call via vtable");

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
void CodeGen::emitMember(std::shared_ptr<MemberExpr> expr) {
    emitComment(std::format("member access: .{}", expr->memberName));

    // 计算对象地址
    emitExpr(expr->object);

    TypePtr objType = expr->object->resolvedType;
    if (expr->isArrow && objType && objType->isPointer()) {
        // 对于 -> 操作，对象本身就是一个指针
        // 不需要额外解引用
    } else if (objType && !objType->isPointer()) {
        // 对于 . 操作，如果对象不是指针，需要获取其地址
        // 这里简化处理
    }

    if (objType) {
        TypePtr actualType = objType;
        if (expr->isArrow && objType->isPointer()) {
            actualType = objType->pointeeType;
        }

        if (actualType && actualType->isClass()) {
            auto field = actualType->classLayout.findField(expr->memberName);
            if (field) {
                // 直接用偏移量访问字段
                emit(std::format("movq {}(%rax), %rax    # .{} (offset {})",
                    field->offset, expr->memberName, field->offset));
                return;
            }
        }
    }

    // 默认：偏移量 0
    emit("movq (%rax), %rax            # member access (unknown offset)");
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
void CodeGen::emitNew(std::shared_ptr<NewExpr> expr) {
    emitComment(std::format("new {}()", expr->className));

    // 查找类的大小
    uint32_t size = 8; // 默认大小
    std::string vtableLabel;
    bool hasVTable = false;

    // 在全局类表中查布局：totalSize 决定分配字节数（含 _vptr 槽，
    // 由语义阶段的 ClassLayout 计算），hasVTable 决定是否要安装 _vptr
    if (m_classTypes) {
        auto it = m_classTypes->find(expr->className);
        if (it != m_classTypes->end()) {
            size = it->second->classLayout.totalSize;
            if (size == 0) size = 8;
            hasVTable = it->second->classLayout.hasVTable;
            if (hasVTable) {
                vtableLabel = NameMangler::mangleVTable(expr->className);
            }
        }
    }

    // 调用 malloc
    // System V 下 malloc(size) 的参数走 rdi，返回的指针在 rax ——
    // 调用约定在"编译器生成的代码"与"库函数"之间同样生效
    emit(std::format("movq ${}, %rdi             # malloc size", size));
    emit("callq malloc                  # allocate memory");

    if (hasVTable) {
        // 设置 _vptr：对象偏移量 0 处 = vtable 地址 + 16
        // （跳过 offset-to-top 和 RTTI 指针）
        emit(std::format("leaq {}(%rip), %rcx    # vtable address", vtableLabel));
        emit("addq $16, %rcx              # skip to vtable[0]");
        emit("movq %rcx, (%rax)           # obj._vptr = vtable");
    }

    emitComment(std::format("end new {}()", expr->className));
}

// ─────────────────────────────────────────────────────────────────────────────
// this 表达式
// ─────────────────────────────────────────────────────────────────────────────
// 做什么：把 this 指针加载进 rax。this 在函数序言中已作为隐式形参
//       spill 进栈槽（见 emitFunction 的成员函数分支），此处按普通
//       局部变量查表加载即可 —— "this 只是一个普通参数"是 Itanium ABI
//       的本质。
// demo: this（成员函数内）→ movq -8(%rbp), %rax
void CodeGen::emitThis(std::shared_ptr<ThisExpr>) {
    auto it = m_localVars.find("this");
    if (it != m_localVars.end()) {
        emit(std::format("movq {}(%rbp), %rax    # this pointer", it->second));
    } else {
        emit("# WARNING: 'this' used outside of class method");
        emit("xorq %rax, %rax");
    }
}

} // namespace minicc
