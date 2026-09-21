// =============================================================================
// src/sfinae.cpp —— SFINAE 模块的实现（协议与设计说明见 include/sfinae.h）
// =============================================================================
// 本文件是整个项目里唯一打印 "[sfinae]" 前缀日志的地方：
// 输出里每一行 "[sfinae]" 都能在这里找到出处与上下文。
// =============================================================================

#include "sfinae.h"

#include <format>
#include <iostream>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// SfinaeContext：直接上下文深度计数
// ─────────────────────────────────────────────────────────────────────────────
// 深度为 0 ⇔ 不在任何直接上下文内：此时抛出 SubstitutionFailure 无人接住。
int SfinaeContext::s_depth = 0;

SfinaeContext::SfinaeContext()  { ++s_depth; }
SfinaeContext::~SfinaeContext() { --s_depth; }

bool SfinaeContext::inImmediateContext() { return s_depth > 0; }
int  SfinaeContext::depth()              { return s_depth; }

// ─────────────────────────────────────────────────────────────────────────────
// 产生信号
// ─────────────────────────────────────────────────────────────────────────────
// ★ 直接上下文信息只是"记录"不是"判断"：不在直接上下文里抛软失败信号在有些场景下
//   合法（如 resolveType 对非依赖 decltype 的立即求值试探 —— 失败后要保留节点延迟
//   处理，那里本就没有候选集），故这里只如实标注。
void Sfinae::fail(const std::string& reason) {
    std::cout << std::format(
        "  [sfinae] ⚡ 发出替换失败信号（{}）：{}\n",
        SfinaeContext::inImmediateContext() ? "直接上下文内 → 可被吸收为软失败"
                                            : "直接上下文【外】→ 无人吸收则升级为硬错误",
        reason);
    throw SubstitutionFailure(reason);
}

// ─────────────────────────────────────────────────────────────────────────────
// 候选被移除
// ─────────────────────────────────────────────────────────────────────────────
void Sfinae::rejected(const std::string& candidateDesc, const std::string& reason,
                      const std::string& indent) {
    std::cout << std::format(
        "{}  [sfinae] ⤵ 候选【{}】被移出候选集（SFINAE 软失败，非错误）\n"
        "{}  [sfinae]    原因：{}\n",
        indent, candidateDesc, indent, reason);
}

// ─────────────────────────────────────────────────────────────────────────────
// void_t 探测链路
// ─────────────────────────────────────────────────────────────────────────────
// 探测是 SFINAE 里最容易讲错的一环，故日志刻意写细：逐条报告"第几个条件通过了"，
// 让"全有或全无"看得见 —— 后续某条失败时，前面"已通过"的记录不会被回滚。
void Sfinae::probeBegin(size_t argCount) {
    std::cout << std::format(
        "  [sfinae] void_t<{} 个实参> —— 逐个替换 + 求值探测\n", argCount);
}

void Sfinae::probeStep(size_t index1based, const std::string& gotType) {
    std::cout << std::format("  [sfinae]   ├─ 实参 {} 探测通过 → {}\n",
                             index1based, gotType);
}

void Sfinae::probeAllOk() {
    std::cout << "  [sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓\n";
}

void Sfinae::probeNoArgs() {
    std::cout << "  [sfinae] void_t<> 无实参 ⇒ 归约为 void ✓\n";
}

void Sfinae::probeNoEvaluator() {
    std::cout << "  [sfinae] void_t: no evaluator attached — 跳过求值\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// decltype 求值失败被降级
// ─────────────────────────────────────────────────────────────────────────────
// 内层抛的多半是普通 runtime_error（如"类没有这个成员"）：在别处是硬错误，在
// decltype 探测里却是"这个类型不满足要求"的信号，必须降级成软失败。
// ★ 这是全项目"硬错误 → 软失败"的唯一转换点，出问题时第一个该看的地方。
void Sfinae::demote(const std::string& innerError) {
    std::cout << std::format(
        "  [sfinae] ⟲ decltype 操作数在内层报错，降级为【软失败】:\n"
        "  [sfinae]    {}\n", innerError);
}

} // namespace minicc
