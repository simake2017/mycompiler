// ─────────────────────────────────────────────────────────────────────────────
// linker.h —— minicc 教学链接器（主线 B）
// ─────────────────────────────────────────────────────────────────────────────
// 定位：把系统 as 产出的 .o（ELF64 可重定位文件）链接成一个可直接运行的非 PIE
//       可执行文件。★ 不依赖系统 ld、不链接 libc/crt。
//
// 【阶段 ⇒ 做什么 ⇒ 例子】（均为教学设计，详见 docs/learn/09）
//   启动   注入 _start（14B 机器码，nop 补齐 16B 槽），替代 Scrt1.o/crti.o/crtn.o
//          例子：call main; mov %eax,%edi; mov $231,%eax; syscall（231 = exit_group）
//   new    内置 mini 运行时：malloc = 64KB arena bump 分配器，free = 空操作
//          例子：new Dog ⇒ movq $8,%rdi; callq malloc; …（用户定义 malloc 可覆盖内置）
//   段布局 非 PIE：固定基址 0x400000，一个 RX 段 + 一个 RW 段
//   输入   单输入 .o（接口已按多 .o 设计）
//
// 完整流程：读 .o → 合并节 → 符号决议 → 重定位回填 → 写可执行 ELF
// 对照 clang：ld.lld（lld/ELF/Writer.cpp::writeResult）
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace minicc {

// 链接结果 + 可观测统计（驱动层打印用）
struct LinkResult {
    bool ok = false;
    std::string errorMsg;          // 失败原因（ld 风格：列出未定义符号等）
    // ── 观测数据 ──
    size_t inputSymbols = 0;       // 输入符号总数
    size_t resolvedRelocs = 0;     // 回填的重定位条数
    uint64_t entryAddr = 0;        // 入口点（_start）地址
    uint64_t textAddr = 0, textSize = 0;   // .text 布局
    uint64_t dataAddr = 0, dataSize = 0;   // .data 布局
    std::vector<std::string> resolvedNames;  // 被决议的符号名（日志用）
};

class MiniLinker {
public:
    // 把若干 .o 链接为一个可执行文件。失败时返回带 errorMsg 的 LinkResult。
    LinkResult link(const std::vector<std::string>& objPaths,
                    const std::string& outputPath);

private:
    static constexpr uint64_t kBaseAddr   = 0x400000;  // 非 PIE 固定基址
    static constexpr uint64_t kArenaSize  = 64 * 1024; // mini malloc 的堆大小

    // ── 内部结构：解析一个 .o 后的中间表示 ──
    struct ObjReloc {
        size_t targetSec;          // 作用于哪个节（.o 内的原始节索引）
        uint64_t offset;           // 节内偏移
        uint32_t type;             // R_X86_64_*
        int64_t addend;
        size_t symIdx;             // 符号表索引
    };
    struct ObjSymbol {
        std::string name;
        uint8_t bind = 0, type = 0;
        uint16_t shndx = 0;        // SHN_UNDEF=0 / 节索引 / SHN_ABS
        int64_t value = 0;         // 节内偏移（链接前无全局意义）
    };
    struct ObjectFile {
        std::string path;
        // 原始节索引 → 所属合并段（0=.text 1=.rodata 2=.data 3=.bss，-1=不参与）
        std::vector<int> secToMerge;
        // 原始节索引 → 该节在合并段内的偏移
        std::vector<uint64_t> secOffset;
        std::vector<ObjReloc> relocs;
        std::vector<ObjSymbol> symbols;
    };

    // ── 五步流水线 ──
    bool readObject(const std::string& path, LinkResult& res);
    void layoutSections();         // 分配各输出段的虚地址
    void injectRuntime(uint64_t mainAddr); // 注入 _start / malloc / free 机器码
    bool resolveSymbols(LinkResult& res);  // 符号决议 + 未定义检查
    bool applyRelocations(LinkResult& res);// 重定位回填
    bool writeExecutable(const std::string& outputPath, LinkResult& res);

    // ── 状态 ──
    std::vector<ObjectFile> objects_;

    // 四个合并输出段（按此顺序布局）
    std::vector<uint8_t> mergedText_, mergedRodata_, mergedData_;
    uint64_t mergedBssSize_ = 0;
    uint64_t textAddr_ = 0, rodataAddr_ = 0, dataAddr_ = 0, bssAddr_ = 0;

    // 链接器内置运行时
    uint64_t startAddr_ = 0, mallocAddr_ = 0, freeAddr_ = 0;
    uint64_t heapPtrAddr_ = 0, heapEndAddr_ = 0;  // .data 内的两个控制字地址
    uint64_t heapInit_ = 0, heapEnd_ = 0;         // arena 首/尾（控制字的值）
    std::vector<uint8_t> runtimeText_;             // _start/malloc/free 机器码

    // 全局符号表：名字 → 最终虚地址
    std::vector<std::pair<std::string, uint64_t>> globalSyms_;
    bool findSymbol(const std::string& name, uint64_t& addrOut);
};

} // namespace minicc
