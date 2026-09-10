# 15 - 链接器原理与 mini-ld 实现（主线 B）

> 主线 B：编译器默认直出**可运行的可执行文件**，不再停在 .s 汇编。
> 核心思想：借用系统 `as` 产出 .o，自己实现一个教学链接器（`src/linker.cpp`），
> 不依赖系统 `ld`、crt 启动文件与 libc——用约 700 行走完
> **读 .o → 合并节 → 地址布局 → 符号决议 → 重定位回填 → 写可执行 ELF** 全流程。

---

## ① 理论背景

### 为什么需要链接器？

编译产物 .o（可重定位文件）里函数调用只是**占位符**：

```asm
callq _Z5twiceIiE      # 此刻 _Z5twiceIiE 的地址未知
```

`as` 汇编时不知道该函数最终会被放在内存哪个位置（多文件链接时它甚至可能在另一个 .o 里），
于是留下两样东西：

1. **符号表（.symtab）**：记录"我定义了谁"（`main`、`_Z5twiceIiE`）与"我引用了谁"（`malloc`）。
2. **重定位表（.rela.text）**：记录"哪个偏移处的几字节，等地址定下来后要按什么公式回填"。

链接器的工作就是：**把所有 .o 的节拼成一个整体 → 给每节分配虚地址 → 按公式把占位符改成真实地址**。

### ELF 的两种视角

| | 可重定位 .o | 可执行文件 |
|---|---|---|
| ELF 头 Type | `ET_REL` | `ET_EXEC`（本项目）/ `ET_DYN`（PIE） |
| 内核/加载器看什么 | 节头表（section headers） | **程序头（program headers / segments）** |
| 虚地址 | 各节均从 0 起算（未定位） | 每段有固定 VirtAddr |
| 符号表 | 完整，供链接 | 可完全不带 |

关键认知：**节（section）是链接视角，段（segment）是运行视角**。
链接器把若干节塞进少数几个段；内核只按段加载，不关心节。
所以 mini-ld 输出的可执行文件可以**零节头**——这是教学简化的合法空间。

### 重定位类型（x86-64，本项目支持的 4 种）

| 类型 | 公式 | 典型用途 |
|---|---|---|
| `R_X86_64_64` | `S + A` | .data 里的 8 字节指针（vtable 表项、typeinfo 指针） |
| `R_X86_64_PC32` | `S + A − P` | RIP 相对寻址（`leaq str(%rip)`） |
| `R_X86_64_PLT32` | `S + A − P`（退化直调） | `call` 指令。真正的链接器会生成 PLT 跳板支持动态库；本项目无外部库，直接当相对调用处理 |
| `R_X86_64_32S` | `S + A`（符号扩展 4 字节） | 32 位绝对地址 |

其中 `S`=符号最终地址，`A`=addend（.o 里写死的常量修正项，如 `call` 指令的 −4），`P`=回填点自身地址。

### 一个隐蔽考点：节符号（STT_SECTION）

编译器引用字符串常量时不总是给字符串起名字，而是用
"**.rodata 这个节 + 偏移 0**"的方式表达（`.LC0` 这类局部标签不进符号表）。
符号表里因此有 `Type=SECTION`、`Ndx=3` 的匿名条目：

```text
Num: Value  Type    Bind   Ndx Name
  2: 0      SECTION LOCAL  3          ← 代表 .rodata 节本身
```

链接器必须支持"按节索引查基址"来解析这类重定位——这是实现中最容易漏的一点。

### crt 启动链与 libc 依赖（真实世界）

`clang++ a.cpp -o a` 背后真实的链接行（`-###` 可观察）：

```text
/usr/bin/ld -pie -dynamic-linker /lib64/ld-linux-x86-64.so.2
    /lib/x86_64-linux-gnu/Scrt1.o   ← _start：建立栈、调用 __libc_start_main
    /lib/x86_64-linux-gnu/crti.o    ← .init/.fini 段函数序言
    crtbeginS.o <用户.o> -lstdc++ -lm -lgcc_s -lgcc -lc crtendS.o crtn.o
```

minicc 的教学策略：**绕开整条链**。

| 真实组件 | 作用 | mini-ld 替代 |
|---|---|---|
| Scrt1.o（`_start`） | 启动后调 `__libc_start_main` 再进 main | 内置 14 字节 `_start` 机器码：`call main` → `exit_group` syscall |
| crti.o / crtn.o | C++ 全局构造/析构注册（.init_array） | 不支持（语言尚无全局对象构造需求） |
| -lc（libc） | printf/malloc/free… | 只内置 64KB bump 版 `malloc` + 空 `free`；其余报"未定义" |
| 动态链接器 | 运行时加载共享库 | 静态非 PIE，完全不需要 |

---

## ② 设计决策

### 决策 1：自己写链接器，而不是调用系统 `ld`

ROADMAP 原方案是"fork/exec 系统 as+ld"。实测摸底后改为自研，理由：

1. **项目定位是可讲解、可观测**——调系统 `ld` 等于把最核心的教学点黑盒化；
2. 我们生成的汇编只用 4 种重定位类型、4 类节，自研工作量可控（~700 行）；
3. 自研后可在日志里逐步打印符号决议与回填过程，符合"全阶段中文日志 + trace"规范。

### 决策 2：非 PIE + 固定基址 0x400000

PIE（地址无关可执行）要求全部代码用 `lea(%rip)` 相对寻址 + 增加重定位段让内核随机化基址，
教学上收益低、复杂度高。选择经典 `ET_EXEC` 固定基址：**地址即常数，一切可在链接期算死**。

### 决策 3：只要 2 个段，输出无节头

```text
RX 段（文件偏移 0）      ：ELF头+程序头 + _start运行时 + .text + .rodata
RW 段（文件偏移 0x1000） ：malloc 控制字 + .data +（memsz 扩到 .bss + 64KB 堆 arena）
```

内核加载只看程序头，`readelf -h` 可见 `Number of section headers: 0` 仍正常运行——
这本身就是最好的教学证据：**节头对运行完全可选**。

### 决策 4：内置运行时用机器码注入，而非汇编

`_start`/`malloc`/`free` 不是生成 .s 再交给 as，而是在链接器里直接**拼字节**
（`src/linker.cpp:293 injectRuntime`）。理由：

- 运行时基址由链接器自己决定，先布局再注入，`call main` 的相对偏移当场可算，**零重定位**；
- 演示"字节即程序"：文档可直接用 `xxd` 验证每条指令的机器码。

**注入的三段机器码**（地址为示例布局）：

```text
0x4000b0  _start (14B):
    e8 <rel32>        call main          # rel32 = main地址 - (此处+5)
    89 c7             mov %eax,%edi      # 退出码 = main 返回值
    b8 e7 00 00 00    mov $231,%eax      # syscall 号 231 = exit_group
    0f 05             syscall

0x4000c0  malloc (36B): 64KB arena bump 分配器
    48 a1 <moffs64>   mov [heapPtr],%rax # 读当前堆指针（绝对寻址）
    48 01 f8          add %rdi,%rax      # cur + size
    48 3d <imm32>     cmp $heapEnd,%rax  # 越界?
    77 0c             ja .fail           # 堆满 → 返回 NULL
    48 a3 <moffs64>   mov %rax,[heapPtr] # 推进指针
    48 29 f8          sub %rdi,%rax      # 返回旧指针
    c3                ret
.fail: 31 c0 c3       xor %eax,%eax; ret

0x4000e0  free (1B):
    c3                ret                # bump 分配器不回收，空操作
```

### 决策 5：内存布局总图

```text
虚地址                 内容                        段
0x400000 ┌──────────────────────────┐
         │ ELF头(64B) + 2个程序头   │
0x4000b0 ├──────────────────────────┤
         │ _start │ malloc │ free   │  ← 链接器注入的运行时（64B 槽）
0x4000f0 ├──────────────────────────┤
         │ .text（用户代码）        │ ┐
         ├──────────────────────────┤ │ RX（只读可执行）
         │ .rodata（字符串等）      │ ┘ 文件前 0x1000 字节
0x401000 ┌──────────────────────────┐
         │ heapPtr/heapEnd 控制字   │  ← 16B，链接器自留
         ├──────────────────────────┤
         │ .data（vtable/typeinfo） │ ┐
         ├──────────────────────────┤ │ RW（读写）
         │ .bss                     │ ┘ 文件仅 0x10 字节，
         ├──────────────────────────┤   memsz 扩到 0x10010
         │ 64KB malloc arena        │
         └──────────────────────────┘
```

注意 .data 里的 vtable 表项是指向 .text 的指针，由 `R_X86_64_64` 重定位回填——
**虚函数表就是链接期回填的一张地址表**，这是 14 号汇编文档中 `callq *%rax` 间接调用的另一半。

---

## ③ clang 源码对照表

clang 生态中链接由独立的 **lld** 完成（本实现不逐行移植，只对齐流水线结构）：

| lld 源码位置 | 职责 | minicc 对应 | 简化了什么 |
|---|---|---|---|
| `lld/ELF/InputFiles.cpp` :: `ObjFile::parse` | 解析 .o：节、符号、重定位 | `readObject()` (linker.cpp:118) | 只认 4 类节名；不支持 COMDAT/归档(.a)/动态库 |
| `lld/ELF/Writer.cpp` :: `createSyntheticSections` | 合成输出段 | `layoutSections()` (:257) | 固定 4 段布局，不做段合并优化 |
| `lld/ELF/Symbols.cpp` :: `resolve` | 符号决议、weak/强符号冲突 | `resolveSymbols()` (:354) | 只做"重名强符号报错"；不处理 common 符号、版本脚本 |
| `lld/ELF/Writer.cpp` :: `finalizeSections` | 分配虚地址、定入口 | `layoutSections()` + `injectRuntime()` | 非 PIE 固定基址，无动态段 |
| `lld/ELF/Arch/X86_64.cpp` :: `relocateOne` | 按类型回填重定位 | `applyRelocations()` (:432) | 只支持 4 种类型；无 PLT/GOT 生成、无松弛优化 |
| `lld/ELF/Writer.cpp` :: `writeResult` | 写最终文件（头+段） | `writeExecutable()` (:543) | 无节头、无动态段、无 eh_frame |
| crt 启动文件（Scrt1.o 等） | `_start` → `__libc_start_main` → main | `injectRuntime()` 内置 14 字节 `_start` | 无 argv/envp、无 atexit/全局构造 |

**对应关系的记忆锚点**：lld 的 `Writer.cpp::writeResult()` ≈ 本实现的 `link()` 总编排
（`linker.cpp:637`），六步顺序完全同构。

---

## ④ 可复现实验

### 实验 1：一条命令直出可执行文件并运行

```bash
./minicc tests/tmpl/test_tmpl_14_deduce_basic.cpp -o /tmp/t14
/tmp/t14; echo "exit=$?"        # exit=0（twice(21)-42 == 0）
```

链接阶段日志（节选）：

```text
[link] 注入运行时: _start @ 0x4000b0 | malloc @ 0x4000c0 (bump) | free @ 0x4000e0 (nop)
[link] 重定位回填: 1 条 (PLT32=1 )
[link] 写出 ELF: /tmp/t14 (4112B, entry=0x4000b0, RX段 4096B / RW段 65552B)
链接成功：6 个符号决议, 1 条重定位回填, 入口 0x4000b0
```

### 实验 2：对比 .o 与成品 —— 看链接到底改了什么

```bash
./minicc tests/tmpl/test_tmpl_14_deduce_basic.cpp -S -o /tmp/t14.s
as -o /tmp/t14.o /tmp/t14.s

readelf -h /tmp/t14.o | grep Type          # Type: REL（可重定位）
readelf -sW /tmp/t14.o                     # 6 个符号：3 个 SECTION + main + _Z5twiceIiE
readelf -rW /tmp/t14.o                     # 1 条重定位：
#  Offset 0x12  Type R_X86_64_PLT32  Sym _Z5twiceIiE  Addend -4

readelf -lW /tmp/t14                       # 成品：2 个 LOAD 段，entry 0x4000b0，0 个节头
xxd -s 0x101 -l 5 /tmp/t14                 # 回填点处已是真实指令：e8 11 00 00 00
```

验证公式（用实测数据）：运行时槽占 0x4000b0~0x4000f0，用户 .text 自 0x4000f0 起。

- `S` = .text 基址 + `_Z5twiceIiE` 节内偏移 = 0x4000f0 + 0x27 = **0x400117**
- `P` = 回填点 = 0x4000f0 + 0x12 = **0x400102**
- `S + A − P` = 0x400117 + (−4) − 0x400102 = **0x11** ✓

`xxd` 看到的 `e8 11 00 00 00` 正是这条公式的产物——链接器把占位符改成了真实跳转。

### 实验 3：符号决议失败 —— 重现经典报错

```bash
printf 'int bar(int x);\nint main() { return bar(3); }\n' > /tmp/u.cpp
./minicc /tmp/u.cpp -o /tmp/u
```

```text
[LINK ERROR] 链接失败，以下符号未定义:
    undefined reference to 'bar'
  （本链接器只内置 malloc/free；printf 等 libc 函数暂不支持）
```

与 `g++` 的报错文案逐字同源——这正是 `resolveSymbols()` 里"未定义符号收集清单"的产出。

### 实验 4：机器码肉眼验证（可观测性）

```bash
xxd -s 0xb0 -l 64 /tmp/t14
# 000000b0: e83b 0000 0089 c7b8 e700 0000 0f05 9090  ← _start：e8=call
# 000000c0: 48a1 0010 4000 ...                       ← malloc：48 a1=moffs 读
# 000000e0: c3                                       ← free：单字节 ret
```

`e8 3b 00 00 00`：`call` 相对偏移 0x3b，从 `_start+5`=0x4000b5 起跳 →
落点 0x4000f0 = 用户 .text 起点 = `main`。三条指令的机器码全部手算可验证。

### 实验 5：回归红线

```bash
cmake --build build-linux -j$(nproc) && ctest --test-dir build-linux
# 98% tests passed（122 个中 2 个为历史遗留失败，与本主线无关）
```

执行类测试批量对照（退出码与 `clang++-18` oracle 一致）：
test_tmpl_14/17/19、test_pp_02/03、test_ctor_01(=30)/02、test_decl_01(=20) 全绿。

---

## ⑤ 关键结论（可复述版）

1. **链接器只做三件事**：拼图（合并节）、定位（分配虚地址）、填空（重定位回填）。
2. **callq 的地址是链接期算出来的**：.o 里只有"符号 + 修正量"占位，`S+A−P` 一条公式完成回填。
3. **节是链接视角，段是运行视角**；可执行文件可以没有节头，内核只认程序头。
4. **vtable 是链接器的作业**：`R_X86_64_64` 把函数地址写进 .data，多态调用在运行时读表。
5. **_start 不在你的代码里**，它是启动文件注入的第一段代码；本项目用 14 字节机器码
   替代了整个 crt + libc 依赖，换来"零外部依赖、字节级可讲解"的可执行文件。
