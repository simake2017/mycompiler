#pragma once

// =============================================================================
// include/sfinae.h —— SFINAE：替换失败的处理协议
// =============================================================================
//
// 【这个模块解决什么问题】
//   "替换失败不是错误" 这句话背后其实是一套【三方协议】。协议不集中，
//   就会散落成一堆彼此不认识的 try/catch —— 读代码的人无法回答
//   "SFINAE 到底在哪儿发生"。本模块把协议的三方收进一个文件。
//
// ── 协议的三方 ────────────────────────────────────────────────────────────
//
//   ① 产生方（producer）—— 谁发现"这一步替换不成立"
//        · TemplateInstantiator::substituteType  替换类型时发现实参形态不对
//        · SemanticAnalyzer::evaluateDecltype     求值 decltype 操作数时发现不合法
//      动作：throw SubstitutionFailure{原因}
//
//   ② 传播方（propagator）—— 谁只是把信号往上送，不做判断
//        · 深层递归（Pointer/Reference/Class 各层 substituteType）
//        · DecltypeEvaluator 回调链
//      动作：不捕获，任其冒泡。★ 只有产生方和吸收方知道自己在干什么，
//            中间层必须是透明的，否则"软失败"会在半路被误判成"硬错误"。
//
//   ③ 吸收方（absorber）—— 谁把"候选不成立"消化成"换下一个候选"
//        · Sfinae::attempt   ← 本模块提供的统一入口
//      动作：捕获 → 把该候选移出候选集 → 继续试下一个。
//            若候选集空了 ⇒ 才升级为真正的编译错误（与 SFINAE 无关，
//            那是"没有可用实体"引发的常规语义错误）。
//
// ── 收口点一览（全项目仅此三处，改动时请同步本表）────────────────────────
//
//   | 场景                        | 吸收点                                 |
//   |-----------------------------|----------------------------------------|
//   | 类模板偏特化模式匹配        | TemplateDeducer::matchPattern          |
//   | 函数模板重载决议            | SemanticAnalyzer::inferCall 候选循环   |
//   | 偏序"至少同样特化"比较      | SemanticAnalyzer::classSpecAtLeastAsSpecialized |
//
// ── 直接上下文（immediate context）────────────────────────────────────────
//
//   [temp.deduct]/8 只对"直接上下文"内的失败网开一面。边界就是：
//
//     在直接上下文内  ⇒ 软失败：候选移除，继续        → 用 Sfinae::attempt 包起来
//     在直接上下文外  ⇒ 硬错误：立刻报错，SFINAE 管不着 → 别包，让它抛出去
//
//   典型判据：错误发生在【被替换的那个类型/表达式自身的构成过程】里 ⇒ 直接上下文；
//   错误发生在【被调用函数的函数体】里 ⇒ 不是直接上下文。
//
//   本模块用 SfinaeContext（RAII）把"当前是否在直接上下文内"变成可观测状态：
//   吸收点进入时打标，SubstitutionFailure 产生时若发现没有标 ⇒ 说明有人
//   在非直接上下文里抛了软失败信号（实现 bug），会在日志里显式告警。
//
// ── 与 clang 的对照 ───────────────────────────────────────────────────────
//
//   clang 不用异常，而是用 Sema 里的 SFINAETrap（include/clang/Sema/Sema.h）
//   + DeduceTemplateArguments 的返回码（TDK_*）表达同一件事：
//     · SFINAETrap        ≈ 本模块的 SfinaeContext（标记"现在失败是可恢复的"）
//     · TDK_SubstitutionFailure ≈ 本模块的 SubstitutionFailure
//     · Sema::SubstitutionFailure 的捕获点   ≈ 本模块的 Sfinae::attempt
//   本实现借 C++ 异常做栈回退，语义等价、代码更短。
//
// =============================================================================

#include <cstddef>     // size_t —— 下面 probeBegin/probeStep 的形参用到。
                       // 【为什么要显式写】不写也能过（<string> 会传递地带上
                       //   <cstddef>），但那是依赖传递包含 —— 换个标准库实现、
                       //   或 IDE 的索引器单独解析本头文件时，size_t 就可能解不出来，
                       //   于是这两个声明的签名"残缺"，与 src/sfinae.cpp 里的
                       //   定义对不上，跨文件跳转失效。本文件是全项目唯一在
                       //   【函数声明】里用 size_t 的头文件（其他头文件只在数据
                       //   成员里用），所以只有它会踩到这个。
#include <stdexcept>
#include <string>
#include <utility>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// ① 信号：可恢复的替换失败
// ─────────────────────────────────────────────────────────────────────────────
// 【为什么要一个独立异常类型】替换失败必须能与"真正的语义错误"区分开，
//   因为二者后果完全相反：
//     SubstitutionFailure → 软失败：候选移除，继续试下一个
//     其它异常            → 硬失败：直接报错终止
//   早先的实现一律 throw std::runtime_error，捕获方无从分辨，SFINAE 无从谈起。
//
// 【归属】它被推导（TemplateDeducer）、语义（SemanticAnalyzer）、
//   实例化（TemplateInstantiator）三方共用，故不再挂靠在任何一方名下 ——
//   这就是它从 template_instantiation.h 搬到本文件的原因。
class SubstitutionFailure : public std::runtime_error {
public:
    explicit SubstitutionFailure(const std::string& what)
        : std::runtime_error(what) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ② 直接上下文标记（RAII）
// ─────────────────────────────────────────────────────────────────────────────
// 吸收点进入时构造，离开时析构。配合 SubstitutionFailure 的产生点做健全性检查：
//   在没有标记的地方抛软失败信号 = 实现 bug（信号无人吸收），日志里告警。
class SfinaeContext {
public:
    SfinaeContext();
    ~SfinaeContext();
    SfinaeContext(const SfinaeContext&) = delete;
    SfinaeContext& operator=(const SfinaeContext&) = delete;

    static bool inImmediateContext();
    static int  depth();

private:
    static int s_depth;
};

// ─────────────────────────────────────────────────────────────────────────────
// ③ 吸收器 + 统一日志出口
// ─────────────────────────────────────────────────────────────────────────────
// 所有 [sfinae] 前缀的日志都从本类出去 —— 想知道"SFINAE 都打了什么"，
// 只需要看 src/sfinae.cpp 一个文件。
class Sfinae {
public:
    // ── 产生信号 ──
    // 所有 `throw SubstitutionFailure(...)` 都应该改走这里：
    //   · 统一在一处打印"信号已发出 + 当时是否处于直接上下文"
    //   · 直接上下文信息来自 SfinaeContext，让 [temp.deduct]/8 的边界可观测
    // 注意：它【不】判断该不该抛 —— 判断是产生方的事，本函数只负责发信号与记录。
    [[noreturn]] static void fail(const std::string& reason);

    // ── 吸收一个候选：在直接上下文内执行 fn ──
    //   fn 抛 SubstitutionFailure ⇒ 记为软失败，返回 false（候选被移除）
    //   fn 抛其它异常             ⇒ 不属于 SFINAE，原样上抛（硬错误）
    //   fn 正常返回               ⇒ 返回 true（候选成立）
    // outReason 非空时写入失败原因，供调用方拼装诊断文案。
    template <typename Fn>
    static bool attempt(const std::string& candidateDesc, Fn&& fn,
                        std::string* outReason = nullptr) {
        SfinaeContext ctx;                 // ← 进入直接上下文
        try {
            fn();
            return true;
        } catch (const SubstitutionFailure& e) {
            if (outReason) *outReason = e.what();
            rejected(candidateDesc, e.what());
            return false;
        }
        // 注意：不捕获 std::runtime_error 等其它异常 —— 那是硬错误，
        //       必须让它穿过去。这正是"软/硬"分界的代码形态。
    }

    // ── 候选被移除（日志出口）──
    // 两种失败表达方式共用同一个出口：
    //   异常式  —— Sfinae::attempt 内部失败时调用（自动带 SfinaeContext）
    //   返回码式 —— 函数模板重载决议自己调用（deduce 返回 success=false）
    // 【为什么"返回码式"不包 SfinaeContext】它压根没有异常在栈上回退，
    //   也就没有"直接上下文"这回事 —— 硬塞一个标记只会让状态失真。
    static void rejected(const std::string& candidateDesc, const std::string& reason,
                         const std::string& indent = "");

    // ── void_t 探测链路 ──
    static void probeBegin(size_t argCount);
    static void probeStep(size_t index1based, const std::string& gotType);
    static void probeAllOk();
    static void probeNoArgs();
    static void probeNoEvaluator();

    // ── decltype 求值失败被降级为软失败 ──
    static void demote(const std::string& innerError);
};

} // namespace minicc
