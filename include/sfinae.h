#pragma once

// =============================================================================
// include/sfinae.h —— SFINAE：替换失败的处理协议（理论见 docs/learn/20）
// =============================================================================
// [temp.deduct]/8「替换失败不是错误」背后是一套【三方协议】—— 位置 ⇒ 用例：
//
//   | 角色 | 位置（谁） | 动作 |
//   |---|---|---|
//   | ① 产生方 | substituteType / evaluateDecltype | Sfinae::fail(原因) |
//   | ② 传播方 | 各层递归、DecltypeEvaluator 回调链 | 不捕获，任其冒泡 |
//   | ③ 吸收方 | Sfinae::attempt ← 本模块入口 | 捕获 → 移除候选 → 试下一个 |
//
//   ① 的例（错误文案与 clang 逐字相同）：查不到 `typename T::type`
//        ⇒ fail("no type named 'type' in 'WithoutType'")
//   ② 的例：中间层若捕获 ⇒ 本该报错的程序静默通过
//   ③ 的例（日志原文）：
//        [sfinae] ⤵ 候选【偏特化模式第 N 位 '<模式>'】
//                被移出候选集（SFINAE 软失败，非错误）
//
// ★ ② 必须透明：中间层一旦多管闲事地捕获，软失败会在半路被误判成硬错误。
//
// ── 直接上下文边界（[temp.deduct]/8）───────────────────────────────────────
//   错误在【被替换类型/表达式自身的构成过程】里 ⇒ 直接上下文内 ⇒ 软失败，包起来
//     例：substituteType Case 5.5 查成员表查不到 `T::type`
//   错误在【被调用函数的函数体】里               ⇒ 上下文外     ⇒ 硬错误，别包
//     例：抛的不是 SubstitutionFailure（如 std::runtime_error）⇒ 原样穿出 attempt
//
// ── 收口点（全项目仅此三处，改动时请同步）──────────────────────────────────
//   偏特化 matchPattern │ 重载决议 inferCall │ 偏序 classSpecAtLeastAsSpecialized
//
// ── clang 对照 ─────────────────────────────────────────────────────────────
//   SFINAETrap ≈ SfinaeContext │ TDK_SubstitutionFailure ≈ SubstitutionFailure │
//   捕获点 ≈ Sfinae::attempt（clang 用返回码传播，本项目借异常做栈回退 —— 语义等价、代码更短）
// =============================================================================

#include <cstddef>     // size_t —— 本文件是唯一在【函数声明】里用 size_t 的头文件，
                       // 必须显式包含，不能依赖 <string> 的传递包含：换标准库实现
                       // 或 IDE 单独解析本头文件时，声明签名会"残缺"、跨文件跳转失效。
#include <stdexcept>
#include <string>
#include <utility>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// ① 信号：可恢复的替换失败
// ─────────────────────────────────────────────────────────────────────────────
// 必须与"真正的语义错误"区分开 —— 二者后果相反：本信号 ⇒ 移除候选、继续试下一个；
// 其它异常 ⇒ 立即报错终止。故用独立类型，不能复用 std::runtime_error。
// 它被推导/语义/实例化三方共用，故不挂靠任何一方名下（这就是它搬进本文件的原因）。
class SubstitutionFailure : public std::runtime_error {
public:
    explicit SubstitutionFailure(const std::string& what)
        : std::runtime_error(what) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ② 直接上下文标记（RAII）
// ─────────────────────────────────────────────────────────────────────────────
// 吸收点进入时构造、离开时析构。配合产生点做健全性检查：在没有标记的地方抛软失败
// 信号 = 实现 bug（信号无人吸收），日志里显式告警。
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
// 所有 [sfinae] 前缀的日志都从本类出去 —— 想知道"SFINAE 都打了什么"，看
// src/sfinae.cpp 一个文件即可。
class Sfinae {
public:
    // ── 产生信号 ──
    // 统一在一处打印"信号已发出 + 当时是否处于直接上下文"（来自 SfinaeContext），
    // 让 [temp.deduct]/8 的边界可观测。
    // ★ 它【不】判断该不该抛 —— 判断是产生方的事，本函数只负责发信号与记录。
    [[noreturn]] static void fail(const std::string& reason);

    // ── 吸收一个候选：在直接上下文内执行 fn ──
    //   fn 抛 SubstitutionFailure ⇒ 软失败，返回 false（候选被移除）
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
        // 不捕获 std::runtime_error 等其它异常 —— 那是硬错误，必须让它穿过去。
        // 这正是"软/硬"分界的代码形态。
    }

    // ── 候选被移除（日志出口）──
    // 两种失败表达方式共用同一个出口：
    //   异常式   —— Sfinae::attempt 内部失败时调用（自动带 SfinaeContext）
    //   返回码式 —— 函数模板重载决议自己调用（deduce 返回 success=false）
    // ★ 返回码式【不】包 SfinaeContext：它压根没有异常在栈上回退，也就没有
    //   "直接上下文"这回事，硬塞标记只会让状态失真。
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
