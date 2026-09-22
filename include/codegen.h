#pragma once
// =============================================================================
// 阶段 5：代码生成器 (Code Generator)（理论见 docs/learn/14）
// =============================================================================
// 职责：把树状 AST "拍平"成线性的 x86-64 指令（AT&T 语法 .s）。类型与字段名在此
//   彻底消失，只剩地址和数字。
// 目标：x86-64 System V AMD64 ABI │ 输出：GNU as 可汇编的 .s（.text/.data/.rodata）
// 管线：Preprocessor → Lexer → Parser → Sema → 模板推导/实例化 → ★CodeGen★
//   → .s → 自研链接器产出可执行文件。输入：类型全解析、auto 已替换、ClassLayout 已算好。
//
// ── 节点 ⇒ 指令 速查表（逐条展开见 codegen.cpp 各 visit 重载）────────────
//   IntLiteral 42        ⇒ movq $42, %rax
//   VarExpr(局部 x@-32)   ⇒ movq -32(%rbp), %rax
//   VarExpr(全局 g)       ⇒ movq g(%rip), %rax
//   VarExpr(栈类对象 d)   ⇒ leaq -N(%rbp), %rax        # 值语义 = 对象地址
//   MemberExpr(u.age)    ⇒ 对象地址 %rax → movl off(%rax), %eax（宽度分派）
//   AssignStmt(u.age=20) ⇒ movl %eax, off(%rcx)
//   BinaryExpr(a + 1)    ⇒ 左 pushq → 右 %rcx → popq 左 → addq %rcx, %rax
//   CallExpr(f(a,b))     ⇒ 实参逆序 push → popq rdi/rsi → callq f
//   NewExpr(new Dog)     ⇒ movq $size,%rdi → callq malloc → 装 _vptr → callq ctor
//   IfStmt               ⇒ testq %rax,%rax / je else_N ... / jmp endif_N
//   WhileStmt            ⇒ while_begin_N: 条件 / je while_end_N / body / jmp 回边
//   ptr->vfunc()         ⇒ movq (%rdi),%rax / movq N(%rax),%rax / callq *%rax
//   字段宽度分派          ⇒ 1B movb（写）/ movzbq（读）│ 2~4B movl │ 8B movq
//
// ── 调用约定（System V AMD64 ABI）──────────────────────────────────────
//   整数/指针参数依次用 rdi, rsi, rdx, rcx, r8, r9，返回值 rax，栈 16 字节对齐；
//   成员函数的 this 占第一个参数槽 rdi（Itanium C++ ABI），显式实参从 rsi 起顺延。
//   callee-saved = %rbp/%rbx/%r12~%r15：谁用谁恢复（emitVirtualCall 用 rbx，
//   __minicc_dynamic_cast 的 DFS 用 r12~r14，均自行保存/恢复）。
//   栈帧以 %rbp 为基址（详见 codegen.cpp emitFunction）：-8 起依次是 this、
//   形参 spill（各 8B），再往下是局部变量，向低地址增长，止于 %rsp（序言 subq 预留）。
//
// ── vtable 与名字修饰（Itanium C++ ABI 简化版）─────────────────────────
//   含虚函数的类 → 一张 vtable（.data），对象偏移 0 处藏 _vptr
//   成员方法 "类名_方法名"（Dog_speak）│ 模板实例 _Z5MyPtrIiE（阶段 4 NameMangler）
//   vtable/typeinfo：_ZTV7MyClass / _ZTI7MyClass
//
// ── 对应 LLVM 模块（把 LLVM 好几个阶段压缩进 ~900 行手写代码）──────────
//   SelectionDAG 指令选择 → emitExpr/emitStmt 手写模式匹配 │
//   X86ISelLowering 调用约定降级 → emitCall/emitVirtualCall │
//   RegAllocGreedy → 极简"单累加器 rax + 栈周转"策略 │
//   PrologEpilogInserter（序言/尾声）→ emitFunction 手写 push/leave/ret │
//   AsmPrinter（汇编打印）→ generate() 拼装三段输出
// =============================================================================

#include "ast.h"
#include "ast_visitor.h"
#include "type.h"
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace minicc {

// CodeGen 是一个 AST 访问者：分发交给 accept 的虚表分派（见 ast_visitor.h），
// 本类只重写关心的 visit 重载，不再有 if-else + dynamic_pointer_cast 链。
class CodeGen : public AstVisitor {
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

    // ── 块作用域析构（RAII 的地基）──────────────────────────────────────
    // 栈上类对象登记：名字 → { 类名, 栈偏移, 尺寸 }
    // [class.dtor] 栈对象在作用域结束时必须自动析构——这张表记住
    // "这个函数里哪些局部是类对象、各自在哪、多大"，块尾据此逆序发射析构
    struct ClassLocalInfo {
        std::string className;
        int offset = 0;     // 相对 %rbp 的负偏移（对象起始地址）
        uint32_t size = 0;  // 对象尺寸（alloc 与清零都用它）
    };
    std::unordered_map<std::string, ClassLocalInfo> m_classLocals;

    // 块作用域析构栈：每进一个 BlockStmt 压一层，块内声明的类局部追加进
    // 当前层；块结束时逆序发射本层析构后弹层（LIFO → 逆声明序析构，
    // [class.dtor]/2）。emitBlockStmt 用空列表压栈、返回时恢复外层列表，
    // 天然支持嵌套块——与符号表 enterScope/exitScope 同构。
    std::vector<std::vector<std::string>> m_blockDtorStack;

    // 发射"调用 C 的析构函数"：rdi = leaq off(%rbp)（对象地址），按 vtable 有无定虚实。
    // 与 emitDelete 共享；块尾析构是它的"无 free"版（栈对象不经过 malloc）。
    void emitClassDtorCall(const std::string& className, int rbpOffset);

    // 帧空间预估：扫描函数体内所有局部变量声明并累加占用字节（类类型按布局
    // totalSize 对齐到 8，其余按 8 字节槽）—— 序言的 subq $N 用它，保证类对象
    // （可能 >8B）不越出预留空间。
    // 对照真实编译器：即 LLVM 的 PrologEpilogInserter 帧布局计算，此处为最朴素的
    // "先数后减"一遍扫描。
    uint32_t estimateFrameSize(FuncDeclPtr func);
    uint32_t estimateBlockSize(std::shared_ptr<BlockStmt> block);

    // 多继承 upcast：从 derivedClassName 转为 baseClassName 时的偏移量。
    // 返回 0 表示主基类或无继承关系（无需调整）。
    uint32_t getBaseOffset(const std::string& derivedClassName,
                           const std::string& baseClassName) const;

    // 当前类上下文
    // （正在发射成员函数时非空）：支撑裸字段名访问
    // （方法体内写 age 等价于 this->age）与 this 解析
    std::string m_currentClassName;
    TypePtr     m_currentClassType;
    // 全局类类型表指针（generate 传入）：虚调用查 vtable、new 查对象大小时用
    const std::unordered_map<std::string, TypePtr>* m_classTypes = nullptr;

    // 是否出现动态转型：出现则 generate() 末尾补发运行时助手 __minicc_dynamic_cast
    bool m_needsDynamicCastHelper = false;

    // ── 顶层生成 ──
    // 发射一个完整函数：序言 → 形参 spill → 函数体 → 尾声
    void emitFunction(FuncDeclPtr func);
    // 为含虚函数的类发射 vtable（.data 段，符号名 _ZTV 前缀）
    void emitVTable(const std::string& className, TypePtr classType);
    // 为含虚函数的类发射 RTTI type_info（.data 段，符号名 _ZTI 前缀）
    // baseClassName 非空时第三槽写入基类 typeinfo 地址（__si_class_type_info 风格）
    void emitRTTI(const std::string& className, TypePtr classType,
                  const std::string& baseClassName);
    // 多继承 thunk 跳板：次表覆写项的 this 归顶跳板
    void emitThunk(const std::string& thunkLabel, const std::string& funcLabel,
                   int thunkAdjust);
    // 把生成期间收集的字符串字面量统一发射到 .rodata（常量池思想）
    void emitStringLiterals();

    // ── 语句生成 ──
    // 语句分发器：accept 走虚表分派到下方对应的 visit 重载（"lowering 降级"的入口）。
    // 分派手法的判据见 include/ast_visitor.h 与 semantic_analyzer.h。
    void emitStmt(const StmtPtr& stmt);
    // 复合语句：顺序发射子语句
    void visit(BlockStmt& block) override;
    // 局部变量声明：分配 8B 栈槽 + 发射初始化式（无初始化式则零初始化）
    void visit(VarDeclStmt& decl) override;
    // 赋值：普通变量 / obj.field（字段名在此降级为数字偏移量）
    void visit(AssignStmt& stmt) override;
    // return：结果算进 rax 后直接 leave/ret 撤销栈帧
    void visit(ReturnStmt& stmt) override;
    // delete 语句
    void visit(DeleteStmt& stmt) override;
    // if/else：testq + je 条件跳转的结构化降级
    void visit(IfStmt& stmt) override;
    // while：条件跳出 + 回边 jmp 的循环降级
    void visit(WhileStmt& stmt) override;
    // 表达式语句：只求值（价值在副作用），结果 rax 丢弃
    void visit(ExprStmt& stmt) override;

    // ── 表达式生成 ──
    // 每个 visit 把表达式的值算进 rax（单累加器约定）。
    // emitExpr 是表达式分发器（与 emitStmt 同构，走 accept 虚表分派）。
    void emitExpr(const ExprPtr& expr);
    // 整数字面量 → movq $v, %rax
    void visit(IntLiteralExpr& expr) override;
    // 布尔字面量 → movq $0/1, %rax
    void visit(BoolLiteralExpr& expr) override;
    // 字符串字面量 → leaq str_N(%rip), %rax（RIP 相对寻址）
    void visit(StringLiteralExpr& expr) override;
    // 局部变量/类字段加载（两级查找）
    void visit(VarExpr& expr) override;
    // 左值压栈 → 右值 → 运算
    void visit(BinaryExpr& expr) override;
    // negq / 逻辑非
    void visit(UnaryExpr& expr) override;
    // 普通/方法/虚调用三路分发
    void visit(CallExpr& expr) override;
    // obj.field → [addr+偏移]（字段名降级为偏移量）
    void visit(MemberExpr& expr) override;
    // v[i]（读值）→ 降级为 v.at(i) 成员调用
    // （[expr.sub] 糖化：约定方法 at()，见 ast.h IndexExpr 注释）
    void visit(IndexExpr& expr) override;
    // malloc + 安装 _vptr
    void visit(NewExpr& expr) override;
    // 从栈槽加载 this
    void visit(ThisExpr&) override;
    // nullptr 字面量：无独立 helper，就地发射 xorq
    void visit(NullptrLiteralExpr&) override;
    // dynamic_cast<T*>(e)：操作数进 %rax → 装参 → 调运行时助手，结果回 %rax
    void visit(DynamicCastExpr& expr) override;
    // 发射 RTTI 运行时助手 __minicc_dynamic_cast（沿 typeinfo 基类链匹配）
    void emitDynamicCastHelper();

    // ── 虚函数调用（核心！） ──
    // pet->speak()（Animal，vtable[0]）⇒
    //   (a) movq (%rdi), %rax      # 读出 obj._vptr
    //   (b) movq 0(%rax), %rax     # vtable[idx*8]
    //   (c) callq *%rax            # 间接调用
    // 参数：className/methodName 只用于生成可读注释；args 不含 this；
    //   vtableIndex 是 Sema 在 ClassLayout.vtableEntries 里查好的编译期常数
    //   —— "下标编译期定死，表项内容运行期才定"。
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
