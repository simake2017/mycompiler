// ─────────────────────────────────────────────────────────────────────────────
// linker.cpp —— minicc 教学链接器（主线 B）实现
// ─────────────────────────────────────────────────────────────────────────────
// 五步流水线（与 docs/learn/15 一一对应）—— 每步做什么 + 一个例子：
//   ① readObject        解析 .o（ELF64 节表 / 符号表 / .rela）
//                       例：.text 节 + .rela.text 一条 R_X86_64_PLT32 指向 main
//   ② layoutSections    合并同类节，分配虚地址（非 PIE，基址 0x400000）
//                       例：0x400000 头部 → _start 槽 64B → 用户 .text → 页对齐后 RW 段
//   ③ injectRuntime     注入 _start / malloc / free 机器码（不依赖 crt/libc）
//                       例：_start 里 call main 的 rel32 链接期直接算出；malloc 溢出即返回 NULL
//   ④ resolveSymbols    符号决议：定义(全局) 对上 引用(未定义)，报错列表化
//                       例：无定义 ⇒ undefined reference to 'main'；重复定义 ⇒ 符号重复定义
//   ⑤ applyRelocations  重定位回填：把 as 留下的 e8 00 00 00 00 欠条还清
//                       例：R_X86_64_PLT32 填 call 位移；R_X86_64_32S 溢出即报错
//   最后 writeExecutable 吐出只含 2 个 PT_LOAD 的最小可执行 ELF（e_entry = _start，不是 main）
//
// 对照 clang：lld/ELF/Writer.cpp::writeResult() 做同样的事，但多动态段/线程局部存储/
//   异常帧/多架构，约 20 倍复杂度。
// ─────────────────────────────────────────────────────────────────────────────
#include "linker.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <format>
#include <map>
#include <set>

namespace minicc {

// ═══════════════════════════════════════════════════════════════════════════
// ELF64 最小子集结构体（只定义链接器需要的字段，全部静态断言尺寸）
// ═══════════════════════════════════════════════════════════════════════════
namespace {

struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
};
struct Elf64_Shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};
struct Elf64_Sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct Elf64_Rela {
    uint64_t r_offset, r_info;
    int64_t  r_addend;
};
struct Elf64_Phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
static_assert(sizeof(Elf64_Ehdr) == 64);
static_assert(sizeof(Elf64_Shdr) == 64);
static_assert(sizeof(Elf64_Sym)  == 24);
static_assert(sizeof(Elf64_Rela) == 24);
static_assert(sizeof(Elf64_Phdr) == 56);

// ── ELF 常量（只列用到的）──
constexpr uint16_t ET_REL  = 1, ET_EXEC = 2;
constexpr uint16_t EM_X86_64 = 62;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
constexpr uint32_t SHT_PROGBITS [[maybe_unused]] = 1;   // 本项目节头按序号取，暂未按类型筛
constexpr uint32_t SHT_SYMTAB = 2;
constexpr uint32_t SHT_RELA = 4, SHT_NOBITS = 8;
constexpr uint16_t SHN_UNDEF = 0;
constexpr uint8_t  STT_SECTION = 3;
constexpr uint8_t  STB_WEAK = 1;
// x86-64 重定位类型（本项目 .o 中实际出现的三种 + 常见兜底）
constexpr uint32_t R_X86_64_64 = 1;    // S + A          （8 字节，vtable/typeinfo）
constexpr uint32_t R_X86_64_PC32 = 2;  // S + A - P      （4 字节，leaq sym(%rip)）
constexpr uint32_t R_X86_64_PLT32 = 4; // S + A - P      （callq；无外部库时退化为直接调用）
constexpr uint32_t R_X86_64_32S = 11;  // S + A（符号值 32 位截断检查）

constexpr uint64_t ELF64_R_SYM(uint64_t i)  { return i >> 32; }
constexpr uint32_t ELF64_R_TYPE(uint64_t i) { return (uint32_t)(i & 0xffffffffu); }

uint64_t alignUp(uint64_t v, uint64_t a) {
    return a ? (v + a - 1) / a * a : v;
}

template <typename T>
T readStruct(const std::vector<uint8_t>& buf, size_t off) {
    T t{};
    std::memcpy(&t, buf.data() + off, sizeof(T));
    return t;
}

// 四个合并输出段的名字（顺序即编号：0=.text 1=.rodata 2=.data 3=.bss）
constexpr const char* kMergeNames[4] = {".text", ".rodata", ".data", ".bss"};

int mergeKindOf(const std::string& name) {
    for (int k = 0; k < 4; ++k)
        if (name == kMergeNames[k]) return k;
    return -1;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// ① readObject —— 解析一个 .o 文件
// ═══════════════════════════════════════════════════════════════════════════
// 把 ELF 可重定位文件拆成链接器关心的三样东西：
//   可合并节（.text/.rodata/.data/.bss 的内容）、符号表、重定位表。
// 教学点：.o 里所有符号值都是"节内偏移"，绝对地址要等链接时才产生 ——
//   这正是重定位存在的根本原因。
bool MiniLinker::readObject(const std::string& path, LinkResult& res) {
    // 整个文件读进内存（.o 很小，全量读最简单）
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        res.errorMsg = std::format("无法读取目标文件: {}", path);
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    // ── 魔数与基本字段校验 ──
    if (buf.size() < sizeof(Elf64_Ehdr) || std::memcmp(buf.data(), "\x7f" "ELF", 4) != 0) {
        res.errorMsg = std::format("{}: 不是 ELF 文件", path);
        return false;
    }
    auto eh = readStruct<Elf64_Ehdr>(buf, 0);
    if (eh.e_ident[4] != 2 || eh.e_type != ET_REL || eh.e_machine != EM_X86_64) {
        res.errorMsg = std::format("{}: 需要 ELF64 x86-64 可重定位文件(ET_REL)", path);
        return false;
    }

    ObjectFile obj;
    obj.path = path;

    // ── 节头表：一次性读出所有节头 ──
    std::vector<Elf64_Shdr> shdrs;
    for (uint16_t i = 0; i < eh.e_shnum; ++i)
        shdrs.push_back(readStruct<Elf64_Shdr>(buf, eh.e_shoff + (size_t)i * sizeof(Elf64_Shdr)));

    // 节名表（.shstrtab）：所有节的名字都在这里查
    auto& shstr = shdrs[eh.e_shstrndx];
    auto secName = [&](const Elf64_Shdr& s) -> std::string {
        return reinterpret_cast<const char*>(buf.data() + shstr.sh_offset + s.sh_name);
    };

    // ── 合并节内容：同类节首尾相接追加到对应合并缓冲区 ──
    // 关键不变量：原始节的"段内起始偏移"此刻锁定 ⇒ 之后 符号值 + 段内偏移 = 合并段中的位置。
    obj.secToMerge.assign(shdrs.size(), -1);
    obj.secOffset.assign(shdrs.size(), 0);
    std::vector<uint8_t>* mergeBuf[4] = {&mergedText_, &mergedRodata_,
                                         &mergedData_, nullptr};
    uint64_t mergedSize[4] = {mergedText_.size(), mergedRodata_.size(),
                              mergedData_.size(), mergedBssSize_};

    for (size_t i = 0; i < shdrs.size(); ++i) {
        int kind = mergeKindOf(secName(shdrs[i]));
        if (kind < 0) continue;

        obj.secToMerge[i] = kind;
        // 按节对齐要求补齐（.rodata 常见 32 对齐，代码 16 对齐）
        uint64_t aligned = alignUp(mergedSize[kind],
                                   std::max<uint64_t>(shdrs[i].sh_addralign, 1));
        obj.secOffset[i] = aligned;
        mergedSize[kind] = aligned + shdrs[i].sh_size;

        if (shdrs[i].sh_type == SHT_NOBITS) {
            // .bss：不占文件空间，只累加大小
        } else {
            auto& dst = *mergeBuf[kind];
            dst.insert(dst.end(), aligned - dst.size(), 0);   // 对齐填零
            dst.insert(dst.end(),
                       buf.begin() + shdrs[i].sh_offset,
                       buf.begin() + shdrs[i].sh_offset + shdrs[i].sh_size);
        }
    }
    // 回写累加结果（.bss 只有大小）
    mergedBssSize_ = mergedSize[3];

    // ── 符号表（.symtab，SHT_SYMTAB）与其字符串表（sh_link 指过去）──
    int symtabIdx = -1;
    for (size_t i = 0; i < shdrs.size(); ++i)
        if (shdrs[i].sh_type == SHT_SYMTAB) { symtabIdx = (int)i; break; }
    if (symtabIdx >= 0) {
        auto& st = shdrs[symtabIdx];
        auto& strtab = shdrs[st.sh_link];
        size_t n = st.sh_size / sizeof(Elf64_Sym);
        for (size_t i = 0; i < n; ++i) {
            auto sym = readStruct<Elf64_Sym>(buf, st.sh_offset + i * sizeof(Elf64_Sym));
            ObjSymbol os;
            os.name  = reinterpret_cast<const char*>(buf.data() + strtab.sh_offset + sym.st_name);
            os.bind  = sym.st_info >> 4;
            os.type  = sym.st_info & 0xf;
            os.shndx = sym.st_shndx;
            os.value = (int64_t)sym.st_value;
            obj.symbols.push_back(os);
        }
    }

    // ── 重定位表（.rela.*，SHT_RELA；sh_info = 被修正的目标节）──
    for (size_t i = 0; i < shdrs.size(); ++i) {
        if (shdrs[i].sh_type != SHT_RELA) continue;
        uint32_t target = shdrs[i].sh_info;   // 作用于哪个节
        size_t n = shdrs[i].sh_size / sizeof(Elf64_Rela);
        for (size_t j = 0; j < n; ++j) {
            auto r = readStruct<Elf64_Rela>(buf, shdrs[i].sh_offset + j * sizeof(Elf64_Rela));
            ObjReloc orl;
            orl.targetSec = target;
            orl.offset    = r.r_offset;
            orl.type      = ELF64_R_TYPE(r.r_info);
            orl.symIdx    = (size_t)ELF64_R_SYM(r.r_info);
            orl.addend    = r.r_addend;
            obj.relocs.push_back(orl);
        }
    }

    res.inputSymbols += obj.symbols.size();
    std::cout << std::format("  [link] 读入 {}: {} 个符号, {} 条重定位, 合并节:",
        path, obj.symbols.size(), obj.relocs.size());
    for (int k = 0; k < 4; ++k)
        if (std::find(obj.secToMerge.begin(), obj.secToMerge.end(), k) != obj.secToMerge.end())
            std::cout << " " << kMergeNames[k];
    std::cout << "\n";

    objects_.push_back(std::move(obj));
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ② layoutSections —— 给各输出段分配虚地址
// ═══════════════════════════════════════════════════════════════════════════
// 布局图（非 PIE，基址 0x400000）：
//
//   0x400000 ┌──────────────────────┐
//            │ ELF 头 + 2 个 Phdr   │  ← RX 段从文件偏移 0 开始
//   +0xB0    ├──────────────────────┤
//            │ 运行时 _start 等     │  （64B 固定槽）
//            ├──────────────────────┤
//            │ 用户 .text           │
//            ├──────────────────────┤
//            │ .rodata              │
//            ├──────────────────────┤  ← 页对齐边界（0x1000 的倍数）
//            │ 运行时堆控制字(16B)  │  ← RW 段
//            │ 用户 .data           │
//            ├──────────────────────┤
//            │ .bss（不占文件）     │
//            ├──────────────────────┤
//            │ malloc 的 64KB arena │
//            └──────────────────────┘
void MiniLinker::layoutSections() {
    constexpr uint64_t hdrSize = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr); // 176
    static constexpr uint64_t kRuntimeSlot = 64;   // 运行时机器码固定占位

    startAddr_   = kBaseAddr + alignUp(hdrSize, 16);   // _start 紧跟程序头
    textAddr_    = startAddr_ + kRuntimeSlot;          // 用户 .text 起点
    rodataAddr_  = textAddr_ + alignUp(mergedText_.size(), 16);
    uint64_t rxEnd = rodataAddr_ + alignUp(mergedRodata_.size(), 16);

    // 页对齐边界：文件里也在此处补齐，保证 p_offset ≡ p_vaddr (mod 0x1000)
    uint64_t pageEnd = alignUp(rxEnd - kBaseAddr, 0x1000) + kBaseAddr;

    dataAddr_    = pageEnd;                    // RW 段起点
    heapPtrAddr_ = dataAddr_;                  // 运行时控制字放 .data 最前 16B
    heapEndAddr_ = dataAddr_ + 8;
    uint64_t userDataAddr = dataAddr_ + 16;    // 用户 .data 起点
    bssAddr_     = userDataAddr + alignUp(mergedData_.size(), 16);

    uint64_t arenaStart = alignUp(bssAddr_ + mergedBssSize_, 16);
    heapInit_ = arenaStart;                    // malloc 初始指针 = arena 首
    heapEnd_  = arenaStart + kArenaSize;

    std::cout << std::format(
        "  [link] 段布局: .text @ {:#x} ({}B) | .rodata @ {:#x} ({}B) | "
        ".data @ {:#x} ({}B) | .bss @ {:#x} ({}B)\n",
        textAddr_, mergedText_.size(), rodataAddr_, mergedRodata_.size(),
        userDataAddr, mergedData_.size(), bssAddr_, mergedBssSize_);
    std::cout << std::format("  [link] 堆 arena: {:#x} ~ {:#x} ({}KB bump 分配)\n",
        heapInit_, heapEnd_, kArenaSize / 1024);
}

// ═══════════════════════════════════════════════════════════════════════════
// ③ injectRuntime —— 注入 _start / malloc / free（手写机器码）
// ═══════════════════════════════════════════════════════════════════════════
// 为什么需要：真实链接用 Scrt1.o 提供 _start、用 -lc 提供 malloc；
// 本链接器不依赖外部库，把这两样直接编成机器码注入。
void MiniLinker::injectRuntime(uint64_t mainAddr) {
    runtimeText_.clear();
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        runtimeText_.insert(runtimeText_.end(), bytes);
    };
    auto putImm64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) put({(uint8_t)(v >> (8 * i))});
    };
    auto putImm32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) put({(uint8_t)(v >> (8 * i))});
    };

    mallocAddr_ = startAddr_ + 16;    // 槽位划分：0~15 _start / 16~47 malloc / 48~ free
    freeAddr_   = startAddr_ + 48;

    // ── _start（14B）：调 main，把返回值交给 exit_group 系统调用 ──
    //   call main            e8 <rel32>      距离链接期已知，直接算
    //   mov  %eax, %edi      89 c7           退出码 = main 返回值
    //   mov  $231, %eax      b8 e7 00 00 00  syscall 号 231 = exit_group
    //   syscall              0f 05
    put({0xe8});
    int32_t rel = (int32_t)((int64_t)mainAddr - (int64_t)(startAddr_ + 5));
    putImm32((uint32_t)rel);
    put({0x89, 0xc7, 0xb8, 0xe7, 0x00, 0x00, 0x00, 0x0f, 0x05});
    while (runtimeText_.size() < 16) put({0x90});       // nop 填充到槽边界

    // ── malloc（36B）：bump 分配器，控制字在 .data 前 16 字节 ──
    //   mov  [heapPtr], %rax     48 a1 <moffs64>   读当前指针
    //   add  %rdi, %rax          48 01 f8          new = cur + size
    //   cmp  $heapEnd, %rax      48 3d <imm32>     越界检查（堆满返回 NULL）
    //   ja   .fail               77 0c
    //   mov  %rax, [heapPtr]     48 a3 <moffs64>   推进指针
    //   sub  %rdi, %rax          48 29 f8          返回旧指针（分配区起点）
    //   ret                      c3
    // .fail:
    //   xor  %eax, %eax          31 c0             分配失败返回 NULL
    // demo: `new int` ⇒ callq malloc ⇒ 返回 bump 区的【旧】指针（即新块首地址）；
    //       堆满 64KB ⇒ ja .fail ⇒ 返回 0（NULL）。free 是空操作（bump 分配器不回收）。
    //       真实日志："[link] 注入运行时: _start @ 0x4000b0 | malloc @ 0x4000c0 (bump)
    //                 | free @ 0x4000e0 (nop)"
    //   ret                      c3
    put({0x48, 0xa1}); putImm64(heapPtrAddr_);
    put({0x48, 0x01, 0xf8});
    put({0x48, 0x3d}); putImm32((uint32_t)heapEnd_);
    put({0x77, 0x0c});
    put({0x48, 0xa3}); putImm64(heapPtrAddr_);
    put({0x48, 0x29, 0xf8, 0xc3});
    put({0x31, 0xc0, 0xc3});
    while (runtimeText_.size() < 48) put({0x90});

    // ── free（1B）：bump 分配器不支持回收，空操作 ──
    put({0xc3});
    while (runtimeText_.size() < 64) put({0x90});

    std::cout << std::format(
        "  [link] 注入运行时: _start @ {:#x} | malloc @ {:#x} (bump) | free @ {:#x} (nop)\n",
        startAddr_, mallocAddr_, freeAddr_);
}

// ═══════════════════════════════════════════════════════════════════════════
// ④ resolveSymbols —— 符号决议
// ═══════════════════════════════════════════════════════════════════════════
// 核心动作：把"定义方"（各 .o 中 st_shndx != UNDEF 的符号）汇成全局表，
// 再检查每个"引用方"（未定义符号）能否对上。对不上 = 经典报错
// "undefined reference to `xxx'"。
bool MiniLinker::resolveSymbols(LinkResult& res) {
    auto mergedBase = [&](int kind) -> uint64_t {
        switch (kind) {
            case 0: return textAddr_;
            case 1: return rodataAddr_;
            case 2: return dataAddr_ + 16;   // 跳过运行时控制字
            default: return bssAddr_;
        }
    };

    std::map<std::string, uint64_t> globals;

    // ── 先登记内置运行时符号 ──
    globals["_start"] = startAddr_;
    globals["malloc"] = mallocAddr_;
    globals["free"]   = freeAddr_;

    // ── 再登记各 .o 的定义符号（用户定义可覆盖内置，如自己实现 malloc）──
    for (auto& o : objects_) {
        for (auto& s : o.symbols) {
            if (s.shndx == SHN_UNDEF) continue;      // 未定义 = 引用，跳过
            if (s.type == STT_SECTION) continue;     // 节符号：重定位时按节查
            if (s.name.empty()) continue;
            if (s.shndx >= o.secToMerge.size() || o.secToMerge[s.shndx] < 0)
                continue;                            // 定义在非合并节，不参与决议
            uint64_t addr = mergedBase(o.secToMerge[s.shndx])
                          + o.secOffset[s.shndx] + s.value;
            auto [it, inserted] = globals.try_emplace(s.name, addr);
            if (!inserted && it->second != addr && s.bind != STB_WEAK) {
                // 强符号重复定义——真实链接器的 multiple definition 错误
                res.errorMsg = std::format("符号重复定义: '{}'", s.name);
                return false;
            }
        }
    }

    // ── 检查每个未定义符号都能被满足 ──
    std::set<std::string> missing;
    for (auto& o : objects_) {
        for (auto& s : o.symbols) {
            if (s.shndx != SHN_UNDEF || s.name.empty()) continue;
            if (globals.find(s.name) == globals.end())
                missing.insert(s.name);
        }
    }
    if (!missing.empty()) {
        std::string msg = "链接失败，以下符号未定义:\n";
        for (auto& m : missing)
            msg += std::format("    undefined reference to '{}'\n", m);
        msg += "  （本链接器只内置 malloc/free；printf 等 libc 函数暂不支持）";
        res.errorMsg = msg;
        return false;
    }

    // main 必须存在——_start 的 call 目标
    if (globals.find("main") == globals.end()) {
        res.errorMsg = "undefined reference to 'main'（可执行文件需要 main 函数）";
        return false;
    }

    globalSyms_.assign(globals.begin(), globals.end());
    for (auto& [name, addr] : globalSyms_)
        res.resolvedNames.push_back(std::format("{} @ {:#x}", name, addr));

    std::cout << std::format("  [link] 符号决议: {} 个全局符号, main @ {:#x}\n",
        globals.size(), globals["main"]);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑤ applyRelocations —— 重定位回填
// ═══════════════════════════════════════════════════════════════════════════
// as 阶段给不出最终地址的地方（跨节引用），.o 里留的是占位符 +
// .rela 欠条。这里一笔笔还清：
//   R_X86_64_64   : *P = S + A           （vtable/typeinfo 里的 8 字节指针）
//   R_X86_64_PC32 : *P = S + A - P       （leaq sym(%rip) / 同段引用）
//   R_X86_64_PLT32: 同 PC32（不链外部库时 PLT 降级为直接调用）
//   其中 S = 符号最终地址，A = addend，P = 被修正处的地址
// demo: 未回填时 .o 里对应位置是 0，全靠 .rela 记账；回填后变成最终虚地址 ——
//       可观测处：vtable 槽指向 .text 里的函数体、`.quad .Lstr` 指向 .rodata 里的字符串。
//       真实日志："链接成功：9 个符号决议, 3 条重定位回填, 入口 0x4000b0"
//       （tests/ctor/test_ctor_01_basic.cpp —— 9 个符号决议 = 定义 + 引用配对，
//        3 条回填 = 该程序里所有跨节引用；数字随程序而变，不是常量）
bool MiniLinker::applyRelocations(LinkResult& res) {
    auto mergedBase = [&](int kind) -> uint64_t {
        switch (kind) {
            case 0: return textAddr_;
            case 1: return rodataAddr_;
            case 2: return dataAddr_ + 16;
            default: return bssAddr_;
        }
    };
    auto mergedBuf = [&](int kind) -> std::vector<uint8_t>* {
        switch (kind) {
            case 0: return &mergedText_;
            case 1: return &mergedRodata_;
            case 2: return &mergedData_;
            default: return nullptr;   // .bss 无文件内容，不会有重定位
        }
    };

    std::map<uint32_t, size_t> typeCount;   // 按类型统计（日志用）

    for (auto& o : objects_) {
        for (auto& r : o.relocs) {
            int kind = (r.targetSec < o.secToMerge.size())
                     ? o.secToMerge[r.targetSec] : -1;
            if (kind < 0 || kind == 3) {
                res.errorMsg = std::format("重定位目标节（索引 {}）不是可合并节",
                                           r.targetSec);
                return false;
            }
            if (r.symIdx >= o.symbols.size()) {
                res.errorMsg = "重定位符号索引越界";
                return false;
            }

            // ── 算 S：符号最终地址 ──
            uint64_t S = 0;
            auto& sym = o.symbols[r.symIdx];
            if (sym.type == STT_SECTION) {
                // 节符号（如 ".rodata + 2" 形式的字符串字面量引用）
                if (sym.shndx < o.secToMerge.size() && o.secToMerge[sym.shndx] >= 0)
                    S = mergedBase(o.secToMerge[sym.shndx])
                      + o.secOffset[sym.shndx] + sym.value;
            } else if (sym.shndx != SHN_UNDEF) {
                // 本 .o 内的具名符号（.o 里 st_value 是节内偏移）
                if (sym.shndx < o.secToMerge.size() && o.secToMerge[sym.shndx] >= 0)
                    S = mergedBase(o.secToMerge[sym.shndx])
                      + o.secOffset[sym.shndx] + sym.value;
            } else {
                // 未定义符号 → 全局表查询（含内置运行时）
                if (!findSymbol(sym.name, S)) {
                    res.errorMsg = std::format("重定位引用未定义符号 '{}'", sym.name);
                    return false;
                }
            }
            if (S == 0) {
                res.errorMsg = std::format("重定位符号 '{}' 无法解析到合并段", sym.name);
                return false;
            }

            // ── 算 P：被修正处的最终地址 ──
            uint64_t P = mergedBase(kind) + o.secOffset[r.targetSec] + r.offset;

            // ── 按类型写回 ──
            auto& buf = *mergedBuf(kind);
            size_t filePos = (size_t)o.secOffset[r.targetSec] + r.offset;
            switch (r.type) {
                case R_X86_64_64: {
                    uint64_t value = (uint64_t)((int64_t)S + r.addend);
                    std::memcpy(buf.data() + filePos, &value, 8);
                    break;
                }
                case R_X86_64_PC32:
                case R_X86_64_PLT32: {
                    int32_t v32 = (int32_t)((int64_t)S + r.addend - (int64_t)P);
                    std::memcpy(buf.data() + filePos, &v32, 4);
                    break;
                }
                case R_X86_64_32S: {
                    int64_t v = (int64_t)S + r.addend;
                    if (v > INT32_MAX || v < INT32_MIN) {
                        res.errorMsg = "R_X86_64_32S 重定位溢出（值超出 32 位有符号范围）";
                        return false;
                    }
                    int32_t v32 = (int32_t)v;
                    std::memcpy(buf.data() + filePos, &v32, 4);
                    break;
                }
                default:
                    res.errorMsg = std::format("不支持的重定位类型 R_X86_64_{}", r.type);
                    return false;
            }
            typeCount[r.type]++;
            res.resolvedRelocs++;
        }
    }

    std::string stat;
    for (auto& [t, c] : typeCount) {
        const char* n = (t == R_X86_64_64) ? "64" : (t == R_X86_64_PC32) ? "PC32"
                      : (t == R_X86_64_PLT32) ? "PLT32" : "32S";
        stat += std::format("{}={} ", n, c);
    }
    std::cout << std::format("  [link] 重定位回填: {} 条 ({})\n", res.resolvedRelocs, stat);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// writeExecutable —— 写出最小可执行 ELF（无节头，只有 2 个程序头）
// ═══════════════════════════════════════════════════════════════════════════
// 教学点：可执行文件**不需要节头表**——内核加载只看程序头(PT_LOAD)。
// 这就是为什么 strip 过的二进制没有节名仍可运行。
bool MiniLinker::writeExecutable(const std::string& outputPath, LinkResult& res) {
    constexpr uint64_t hdrSize = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
    (void)hdrSize;

    uint64_t rodataSize = mergedRodata_.size();
    // RX 段文件尺寸：内容结束处对齐到页边界（段内页对齐装载要求）
    uint64_t rxEndOff = rodataAddr_ + alignUp(rodataSize, 16) - kBaseAddr;
    uint64_t rxFileSize = alignUp(rxEndOff, 0x1000);

    uint64_t rwFileSize = 16 + mergedData_.size();  // 运行时控制字 + 用户 .data
    uint64_t rwMemSize  = alignUp(rwFileSize, 16) + mergedBssSize_
                        + (heapEnd_ - heapInit_);

    std::vector<uint8_t> out;
    out.reserve(rxFileSize + rwFileSize);

    // ── ELF 头 ──
    Elf64_Ehdr eh{};
    std::memcpy(eh.e_ident, "\x7f" "ELF", 4);
    eh.e_ident[4] = 2;              // ELFCLASS64
    eh.e_ident[5] = 1;              // 小端
    eh.e_ident[6] = 1;              // ELF 版本
    eh.e_type = ET_EXEC;            // 非 PIE 可执行文件
    eh.e_machine = EM_X86_64;
    eh.e_version = 1;
    eh.e_entry = startAddr_;        // 入口 = _start（不是 main！）
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_shoff = 0;                 // 无节头表
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 2;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    auto ehBytes = reinterpret_cast<uint8_t*>(&eh);
    out.insert(out.end(), ehBytes, ehBytes + sizeof(eh));

    // ── 程序头 1：RX（头 + 运行时代码 + .text + .rodata）──
    Elf64_Phdr ph1{};
    ph1.p_type = PT_LOAD;
    ph1.p_flags = PF_R | PF_X;
    ph1.p_offset = 0;
    ph1.p_vaddr = ph1.p_paddr = kBaseAddr;
    ph1.p_filesz = ph1.p_memsz = rxFileSize;
    ph1.p_align = 0x1000;
    auto p1 = reinterpret_cast<uint8_t*>(&ph1);
    out.insert(out.end(), p1, p1 + sizeof(ph1));

    // ── 程序头 2：RW（.data + .bss + arena）──
    Elf64_Phdr ph2{};
    ph2.p_type = PT_LOAD;
    ph2.p_flags = PF_R | PF_W;
    ph2.p_offset = rxFileSize;
    ph2.p_vaddr = ph2.p_paddr = dataAddr_;
    ph2.p_filesz = rwFileSize;
    ph2.p_memsz = rwMemSize;
    ph2.p_align = 0x1000;
    auto p2 = reinterpret_cast<uint8_t*>(&ph2);
    out.insert(out.end(), p2, p2 + sizeof(ph2));

    // ── RX 段内容：头对齐填充 → 运行时 → 用户 .text → .rodata ──
    while (out.size() < startAddr_ - kBaseAddr) out.push_back(0);
    out.insert(out.end(), runtimeText_.begin(), runtimeText_.end());
    out.insert(out.end(), mergedText_.begin(), mergedText_.end());
    out.insert(out.end(), mergedRodata_.begin(), mergedRodata_.end());
    while (out.size() < rxFileSize) out.push_back(0);   // 页对齐填充

    // ── RW 段内容：堆控制字（初始值）→ 用户 .data ──
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(heapInit_ >> (8 * i)));
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(heapEnd_  >> (8 * i)));
    out.insert(out.end(), mergedData_.begin(), mergedData_.end());

    std::ofstream f(outputPath, std::ios::binary);
    if (!f) {
        res.errorMsg = std::format("无法写入输出文件: {}", outputPath);
        return false;
    }
    f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
    f.close();

    // +x 可执行权限
    std::filesystem::permissions(outputPath,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
        std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);

    res.entryAddr = startAddr_;
    std::cout << std::format(
        "  [link] 写出 ELF: {} ({}B, entry={:#x}, RX段 {}B / RW段 {}B)\n",
        outputPath, out.size(), startAddr_, rxFileSize, rwMemSize);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// link —— 驱动五步流水线
// ═══════════════════════════════════════════════════════════════════════════
LinkResult MiniLinker::link(const std::vector<std::string>& objPaths,
                            const std::string& outputPath) {
    LinkResult res;

    // ① 读入所有 .o
    for (auto& p : objPaths)
        if (!readObject(p, res)) return res;

    // ② 布局（此时才能算出各段地址）
    layoutSections();

    // ③ 注入运行时：需要 main 的地址（先按 ①② 的结果算出来）
    uint64_t mainAddr = 0;
    for (auto& o : objects_) {
        for (auto& s : o.symbols) {
            if (s.name == "main" && s.shndx != SHN_UNDEF
                && s.type != STT_SECTION
                && s.shndx < o.secToMerge.size() && o.secToMerge[s.shndx] >= 0) {
                uint64_t base = textAddr_;                 // main 必在 .text
                if (o.secToMerge[s.shndx] == 2) base = dataAddr_ + 16;
                mainAddr = base + o.secOffset[s.shndx] + s.value;
            }
        }
    }
    if (mainAddr == 0) {
        res.errorMsg = "undefined reference to 'main'（可执行文件需要 main 函数）";
        return res;
    }
    injectRuntime(mainAddr);

    // ④ 符号决议
    if (!resolveSymbols(res)) return res;

    // ⑤ 重定位回填
    if (!applyRelocations(res)) return res;

    // ⑥ 写可执行文件
    res.ok = writeExecutable(outputPath, res);
    return res;
}

bool MiniLinker::findSymbol(const std::string& name, uint64_t& addrOut) {
    for (auto& [n, a] : globalSyms_)
        if (n == name) { addrOut = a; return true; }
    return false;
}

} // namespace minicc
