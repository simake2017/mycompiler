#pragma once
// =============================================================================
// 阶段 5：代码生成器 (Code Generator)
// =============================================================================
// 核心职责：将树状的 AST "拍平"为线性的 x86-64 汇编指令。
//
// 这是"运行期看偏移量"的最终体现：
//   - obj.field   → [rbp + offset]（直接用数字偏移量访问内存）
//   - ptr->vfunc() → 三部曲：读 vptr → 加偏移 → 跳转
//   - 类型和字段名在此阶段彻底消失，只剩下地址和数字
//
// 目标架构：x86-64 (System V AMD64 ABI)
// 输出格式：AT&T 语法的汇编文件（.s）
// =============================================================================
//
// ─── 在编译管线中的位置 ──────────────────────────────────────────────────
//   Preprocessor → Lexer → Parser → SemanticAnalyzer → TemplateDeduction /
//   TemplateInstantiation → ★CodeGen（本文件）★ → .s 文本 → 外部 as/ld
//
//   输入：语义分析与模板实例化均已完成的 AST —— 类型全部解析、auto 已
//         替换为具体类型、ClassLayout（字段偏移/大小/vtable）已算好。
//   输出：一份可直接交给 GNU as 汇编的 AT&T 语法 .s（.text/.data/.rodata）。
//
// ─── 理论背景 ────────────────────────────────────────────────────────────
// 1) 调用约定 System V AMD64 ABI（Linux 下 C/C++ 的事实标准）：
//      整数/指针参数依次用 rdi, rsi, rdx, rcx, r8, r9；返回值用 rax；
//      栈 16 字节对齐；成员函数的 this 指针占用第一个参数槽 rdi
//      （Itanium C++ ABI 规定），显式实参从 rsi 起顺延。
// 2) 栈帧布局（本项目：一切以 %rbp 为基址，详见 codegen.cpp emitFunction）：
//
//        高地址
//      ┌──────────────────────┐
//      │ 返回地址 (call 压入)   │
//      ├──────────────────────┤ ← %rbp（pushq %rbp 保存旧帧基址）
//      │ 旧 %rbp              │
//      ├──────────────────────┤ -8(%rbp)
//      │ this（仅成员函数）     │
//      ├──────────────────────┤ -16(%rbp)
//      │ 形参 1（寄存器 spill） │
//      ├──────────────────────┤ -24(%rbp)
//      │ 形参 2 …             │
//      ├──────────────────────┤
//      │ 局部变量（每个 8B）    │ ← m_currentStackOffset 向低地址增长
//      └──────────────────────┘ ← %rsp（subq $64, %rsp 预留）
//        低地址
//
// 3) vtable 与动态分发（Itanium C++ ABI 简化版）：
//      含虚函数的类 → 一张 vtable（.data 段）；对象偏移 0 处藏 _vptr
//      指向它。虚调用 = 两次访存 + 一次间接跳转（详见 emitVirtualCall）：
//          movq (%rdi), %rax   ; movq N(%rax), %rax   ; callq *%rax
// 4) 名字修饰（GCC/Itanium mangling，本项目实现的子集）：
//      · 普通成员方法：简化方案 "类名_方法名"（如 Dog_speak）
//      · vtable / typeinfo：标准前缀 _ZTV7MyClass / _ZTI7MyClass
//      · 模板实例：_Z5MyPtrIiE 风格，由阶段 4 的 NameMangler 生成
//
// ─── 对应 LLVM 模块 ──────────────────────────────────────────────────────
//   本文件 ≈ 把 LLVM 好几个阶段压缩进 ~900 行手写代码：
//   · lib/CodeGen/SelectionDAG（指令选择）→ emitExpr/emitStmt 手写模式匹配
//   · lib/Target/X86/X86ISelLowering.cpp（调用约定降级）→ emitCall/emitVirtualCall
//   · RegAllocGreedy（寄存器分配）→ 极简"单累加器 rax + 栈周转"策略
//   · PrologEpilogInserter（序言/尾声插入）→ emitFunction 手写 push/leave/ret
//   · AsmPrinter（汇编打印）→ generate() 拼装三段输出
// =============================================================================

#include "ast.h"
#include "type.h"
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace minicc {

class CodeGen {
public:
    // 默认构造即可用：所有状态（输出缓冲、计数器、偏移量表）均有类内默认值
    CodeGen();

    // 生成整个编译单元的汇编代码
    //   unit       —— 整个 AST 的根（函数/类列表经由下面两个参数单独传入）
    //   classTypes —— 全局类类型表（Sema/模板实例化产出，含 ClassLayout）
    //   functions  —— 待发射的函数列表（含模板实例化后的实例）
    //   返回值     —— 完整 .s 文本（.text + .data + .rodata 三段拼装）
    std::string generate(const TranslationUnit& unit,
                         const std::unordered_map<std::string, TypePtr>& classTypes,
                         const std::vector<FuncDeclPtr>& functions);

private:
    // 三个输出缓冲对应 ELF 的三个段，最后在 generate() 里按序拼装：
    // .text 放指令；.data 放可写全局数据（vtable/RTTI）；.rodata 放只读数据（字符串）
    std::ostringstream m_code;      // .text 段（代码）
    std::ostringstream m_data;      // .data 段（全局数据）
    std::ostringstream m_rodata;    // .rodata 段（只读数据）
    // 标签计数器：保证汇编标签全局唯一（else_0、while_begin_1 …）
    int                m_labelCounter = 0;

    // 当前函数的局部变量偏移量追踪
    // 名字 → 相对 %rbp 的负偏移（如 "x" → -32）；隐式参数 "this" 也登记在此表
    std::unordered_map<std::string, int> m_localVars;
    // 栈分配指针：每新增一个局部变量先 -= 8 再登记（栈向低地址增长）
    int m_currentStackOffset = 0;

    // 当前类上下文
    // （正在发射成员函数时非空）：支撑裸字段名访问
    // （方法体内写 age 等价于 this->age）与 this 解析
    std::string m_currentClassName;
    TypePtr     m_currentClassType;
    // 全局类类型表指针（generate 传入）：虚调用查 vtable、new 查对象大小时用
    const std::unordered_map<std::string, TypePtr>* m_classTypes = nullptr;

    // ── 顶层生成 ──
    // 发射一个完整函数：序言 → 形参 spill → 函数体 → 尾声
    void emitFunction(FuncDeclPtr func);
    // 为含虚函数的类发射 vtable（.data 段，符号名 _ZTV 前缀）
    void emitVTable(const std::string& className, TypePtr classType);
    // 为含虚函数的类发射 RTTI type_info（.data 段，符号名 _ZTI 前缀）
    void emitRTTI(const std::string& className, TypePtr classType);
    // 把生成期间收集的字符串字面量统一发射到 .rodata（常量池思想）
    void emitStringLiterals();

    // ── 语句生成 ──
    // 语句分发器：按节点动态类型派发到具体 emit（"lowering 降级"的入口）
    void emitStmt(StmtPtr stmt);
    // 复合语句：顺序发射子语句
    void emitBlockStmt(std::shared_ptr<BlockStmt> block);
    // 局部变量声明：分配 8B 栈槽 + 发射初始化式（无初始化式则零初始化）
    void emitVarDecl(std::shared_ptr<VarDeclStmt> decl);
    // 赋值：普通变量 / obj.field（字段名在此降级为数字偏移量）
    void emitAssign(std::shared_ptr<AssignStmt> stmt);
    // return：结果算进 rax 后直接 leave/ret 撤销栈帧
    void emitReturn(std::shared_ptr<ReturnStmt> stmt);
    // delete 语句
    void emitDelete(std::shared_ptr<DeleteStmt> stmt);
    // if/else：testq + je 条件跳转的结构化降级
    void emitIf(std::shared_ptr<IfStmt> stmt);
    // while：条件跳出 + 回边 jmp 的循环降级
    void emitWhile(std::shared_ptr<WhileStmt> stmt);
    // 表达式语句：只求值（价值在副作用），结果 rax 丢弃
    void emitExprStmt(std::shared_ptr<ExprStmt> stmt);

    // ── 表达式生成 ──
    // 每个 emit 函数将表达式的值计算到 rax 寄存器中
    // （单累加器约定）emitExpr 是表达式分发器（与 emitStmt 同构，按节点动态类型派发）
    void emitExpr(ExprPtr expr);
    // 整数字面量 → movq $v, %rax
    void emitIntLiteral(std::shared_ptr<IntLiteralExpr> expr);
    // 布尔字面量 → movq $0/1, %rax
    void emitBoolLiteral(std::shared_ptr<BoolLiteralExpr> expr);
    // 字符串字面量 → leaq str_N(%rip), %rax（RIP 相对寻址）
    void emitStringLiteral(std::shared_ptr<StringLiteralExpr> expr);
    // 局部变量/类字段加载（两级查找）
    void emitVar(std::shared_ptr<VarExpr> expr);
    // 左值压栈 → 右值 → 运算
    void emitBinary(std::shared_ptr<BinaryExpr> expr);
    // negq / 逻辑非
    void emitUnary(std::shared_ptr<UnaryExpr> expr);
    // 普通/方法/虚调用三路分发
    void emitCall(std::shared_ptr<CallExpr> expr);
    // obj.field → [addr+偏移]（字段名降级为偏移量）
    void emitMember(std::shared_ptr<MemberExpr> expr);
    // malloc + 安装 _vptr
    void emitNew(std::shared_ptr<NewExpr> expr);
    // 从栈槽加载 this
    void emitThis(std::shared_ptr<ThisExpr> expr);

    // ── 虚函数调用（核心！） ──
    // ptr->vfunc(args) 的汇编三部曲：
    //   (a) 从 ptr 读出 _vptr 机器地址
    //   (b) 加上虚函数在表中的 Index 偏移量
    //   (c) 跳转至该地址执行
    // 参数说明：className/methodName 仅用于生成可读注释；args 是实参表达式
    // 列表（不含 this）；vtableIndex 是语义阶段在 ClassLayout.vtableEntries
    // 中查得的编译期常数 —— "下标编译期定死，表项内容运行期才定"。
    void emitVirtualCall(const std::string& className,
                         const std::string& methodName,
                         const std::vector<ExprPtr>& args,
                         uint32_t vtableIndex);

    // ── 辅助 ──
    // 生成全局唯一标签：前缀 + "_" + 计数器，如 else_0、while_begin_1
    std::string newLabel(const std::string& prefix = "L");
    // 写一条指令进 .text（自动带 4 空格缩进，GAS 排版惯例）
    void emit(const std::string& line);
    // 写一行进 .data（vtable/RTTI）
    void emitData(const std::string& line);
    // 写一行进 .rodata（字符串常量）
    void emitRodata(const std::string& line);
    // 写一条 GAS 行注释 "# ..."：把编译期决策写进 .s，教学可观测性手段
    void emitComment(const std::string& comment);

    // 字符串字面量收集
    // （生成期间登记，结束时由 emitStringLiterals 统一发射到 .rodata）
    std::vector<std::pair<std::string, std::string>> m_stringLiterals; // label → value
    // 登记一个字符串字面量并返回引用标签（稍后 leaq 取址）；本实现不做去重
    std::string addStringLiteral(const std::string& value);
};

} // namespace minicc
