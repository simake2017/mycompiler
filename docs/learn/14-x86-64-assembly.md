# 14 - x86-64 汇编语法指南（AT&T 格式）

## 14.1 为什么需要了解汇编？

minicc 的最终输出是 x86-64 汇编文件（`.s`），理解汇编是理解代码生成的前提。
本文档介绍 minicc 使用的汇编语法子集。

## 14.2 AT&T vs Intel 语法

x86-64 有两种汇编写法，minicc（以及 GCC/Clang）使用 **AT&T 语法**：

| 特性 | AT&T（minicc 使用） | Intel |
|------|---------------------|-------|
| 操作数顺序 | `指令 源, 目标` | `指令 目标, 源` |
| 寄存器前缀 | `%rax` | `rax` |
| 立即数前缀 | `$42` | `42` |
| 尺寸后缀 | `movl`（l=32位） | `mov dword` |
| 内存引用 | `8(%rax)` | `[rax+8]` |

**示例对比**：
```asm
# AT&T：把 rax 的值存到 rbp-8 的内存位置
movq %rax, -8(%rbp)

# Intel：同样的操作
mov [rbp-8], rax
```

## 14.3 寄存器

### 14.3.1 通用寄存器

| 64位 | 32位 | 16位 | 8位 | 用途 |
|------|------|------|-----|------|
| `%rax` | `%eax` | `%ax` | `%al` | 累加器/返回值 |
| `%rbx` | `%ebx` | `%bx` | `%bl` | 通用（callee-saved） |
| `%rcx` | `%ecx` | `%cx` | `%cl` | 通用/第4参数 |
| `%rdx` | `%edx` | `%dx` | `%dl` | 通用/第3参数 |
| `%rsi` | `%esi` | `%si` | `%sil` | 通用/第2参数 |
| `%rdi` | `%edi` | `%di` | `%dil` | 通用/第1参数 |
| `%r8` | `%r8d` | `%r8w` | `%r8b` | 第5参数 |
| `%r9` | `%r9d` | `%r9w` | `%r9b` | 第6参数 |
| `%rsp` | `%esp` | `%sp` | `%spl` | 栈指针 |
| `%rbp` | `%ebp` | `%bp` | `%bpl` | 帧基址指针 |

### 14.3.2 System V AMD64 ABI（调用约定）

函数调用时参数放在哪些寄存器：

```
第1参数 → %rdi    （成员函数的 this 指针）
第2参数 → %rsi
第3参数 → %rdx
第4参数 → %rcx
第5参数 → %r8
第6参数 → %r9
第7个及以后 → 栈上

返回值 → %rax
```

### 14.3.3 Caller-saved vs Callee-saved

| 类别 | 寄存器 | 含义 |
|------|--------|------|
| Callee-saved | `%rbx`, `%rbp`, `%r12`-`%r15` | 被调用者负责保存/恢复 |
| Caller-saved | 其余所有 | 调用者自己保存（如果需要） |

## 14.4 指令尺寸后缀

每条指令的后缀决定操作数宽度：

| 后缀 | 宽度 | C 类型 | 示例 |
|------|------|--------|------|
| `b` | 8 位 | `char` | `movb` |
| `w` | 16 位 | `short` | `movw` |
| `l` | 32 位 | `int` | `movl` |
| `q` | 64 位 | `long`/指针 | `movq` |

```asm
movl %eax, -8(%rbp)    # 存 32 位 int
movq %rax, -8(%rbp)    # 存 64 位指针/long
```

## 14.5 常用指令

### 14.5.1 数据传送

```asm
# mov：寄存器到寄存器
movq %rax, %rbx            # rbx = rax

# mov：立即数到寄存器
movq $42, %rax             # rax = 42

# mov：寄存器到内存
movq %rax, -8(%rbp)        # [rbp-8] = rax

# mov：内存到寄存器
movq -8(%rbp), %rax        # rax = [rbp-8]

# movl（32位）
movl $10, -16(%rbp)        # [rbp-16] = 10（只写4字节）
movl -16(%rbp), %eax       # eax = [rbp-16]（只读4字节）

# lea：加载有效地址（不读内存，只算地址）
leaq 8(%rax), %rbx         # rbx = rax + 8
leaq _ZTV3Dog(%rip), %rcx  # rcx = _ZTV3Dog 的地址（PIC）
```

### 14.5.2 算术运算

```asm
# 加法
addq %rbx, %rax            # rax = rax + rbx
addq $8, %rsp              # rsp = rsp + 8（栈指针加8）

# 减法
subq $64, %rsp             # rsp = rsp - 64（分配64字节栈空间）
subq %rbx, %rax            # rax = rax - rbx

# 乘法
imulq %rbx, %rax           # rax = rax * rbx（有符号）

# 除法
cqto                        # rdx:rax = rax 的符号扩展
idivq %rbx                 # rax = rax/rbx, rdx = rax%rbx

# 取负
negq %rax                  # rax = -rax
```

### 14.5.3 比较与跳转

```asm
# 比较（做减法但不保存结果，只设标志位）
cmpq %rbx, %rax            # 比较 rax 和 rbx（实际做 rax-rbx）

# 条件跳转
je  label                  # 相等时跳转（ZF=1）
jne label                  # 不相等时跳转（ZF=0）
jg  label                  # 大于时跳转（有符号）
jl  label                  # 小于时跳转（有符号）
jge label                  # 大于等于时跳转
jle label                  # 小于等于时跳转
jmp label                  # 无条件跳转

# 示例：if (a > b) then ... else ...
    cmpq %rbx, %rax        # 比较 a(rax) 和 b(rbx)
    jle  else_label         # a <= b 则跳到 else
    # then 分支 ...
    jmp  end_label
else_label:
    # else 分支 ...
end_label:
```

### 14.5.4 栈操作

```asm
# push：压栈（先减 rsp 再存值）
pushq %rax                 # rsp -= 8; [rsp] = rax

# pop：弹栈（先取值再加 rsp）
popq %rax                  # rax = [rsp]; rsp += 8
```

**栈的增长方向**：向下增长（push 减小 rsp，pop 增大 rsp）

### 14.5.5 函数调用

```asm
# 直接调用
callq function_name        # 跳转到 function_name，返回地址压栈

# 间接调用（虚函数用这个）
callq *%rax                # 跳转到 rax 指向的地址

# 返回
ret                        # 弹出返回地址并跳转

# leave（函数尾声的快捷方式）
leave                      # 等价于：movq %rbp, %rsp; popq %rbp
```

### 14.5.6 位运算

```asm
andq %rbx, %rax            # rax = rax & rbx
orq  %rbx, %rax            # rax = rax | rbx
xorq %rax, %rax            # rax = rax ^ rax = 0（清零技巧）
notq %rax                  # rax = ~rax
shlq $2, %rax              # rax = rax << 2
shrq $2, %rax              # rax = rax >> 2（逻辑右移）
```

## 14.6 内存寻址模式

### 14.6.1 基本形式

```
偏移(基址寄存器)

示例：
8(%rax)        → 地址 = rax + 8
-16(%rbp)      → 地址 = rbp - 16
(%rdi)         → 地址 = rdi（偏移为0）
```

### 14.6.2 完整形式

```
偏移(基址, 索引, 比例)

地址 = 基址 + 索引 × 比例 + 偏移
比例可选：1, 2, 4, 8

示例：
0(%rax, %rcx, 8)    → 地址 = rax + rcx*8（数组寻址：rcx 是下标，8 是元素大小）
```

### 14.6.3 RIP 相对寻址（PIC）

```asm
_ZTV3Dog(%rip)     # 地址 = _ZTV3Dog 符号 + 当前 RIP

# 用于位置无关代码（Position Independent Code）
# 常用于加载全局数据/标签的地址
leaq _ZTV3Dog(%rip), %rcx   # rcx = vtable 地址
```

## 14.7 汇编文件结构

### 14.7.1 段（Section）

```asm
    .text              # 代码段：可执行指令
    .data              # 数据段：可读写全局变量
    .section .rodata   # 只读数据段：字符串常量、typeinfo
```

### 14.7.2 数据定义

```asm
    .globl main        # 导出符号（链接器可见）
    .align 8           # 对齐到 8 字节边界

_ZTV3Dog:              # 标签（数据起始地址）
    .quad 0            # 8 字节整数（vtable 的 offset-to-top）
    .quad _ZTI3Dog     # 8 字节指针（指向 typeinfo）
    .quad Dog_speak    # 8 字节函数指针

.Ltype_name:
    .string "Dog"      # 以 \0 结尾的字符串
```

| 伪指令 | 大小 | 用途 |
|--------|------|------|
| `.byte` | 1 字节 | 字符/小整数 |
| `.short` | 2 字节 | short |
| `.long` | 4 字节 | int |
| `.quad` | 8 字节 | long/指针 |
| `.string` | 变长 | 字符串（自动加 \0） |

### 14.7.3 标签

```asm
main:                  # 全局标签（.globl 后可被外部引用）
.Lstr_0:              # 局部标签（.L 开头，链接器不可见）
loop_3:               # 跳转目标标签
```

## 14.8 函数框架（Prologue/Epilogue）

minicc 生成的每个函数都有标准框架：

```asm
func_name:
    # ── 序言（Prologue）──
    pushq %rbp                 # 保存调用者的帧基址
    movq %rsp, %rbp            # 建立新栈帧（rbp = 当前 rsp）
    subq $64, %rsp             # 分配局部变量空间

    # ── 函数体 ──
    # ...

    # ── 尾声（Epilogue）──
    leave                      # movq %rbp, %rsp; popq %rbp
    ret                        # 返回调用者
```

**栈帧布局**：

```
高地址
┌─────────────────┐
│ 调用者的栈帧     │
├─────────────────┤ ← rbp（帧基址）
│ 返回地址         │ ← pushq %rbp 前的 rsp
├─────────────────┤
│ 保存的 rbp       │ ← pushq %rbp 压入
├─────────────────┤ ← 新的 rbp
│ 局部变量 -8      │
├─────────────────┤
│ 局部变量 -16     │
├─────────────────┤
│ ...              │
├─────────────────┤ ← rsp（栈顶，随时变化）
低地址
```

## 14.9 minicc 特殊模式

### 14.9.1 new 表达式

```asm
    # new Dog()
    movq $24, %rdi              # 参数1 = sizeof(Dog) = 24字节
    callq malloc                # 调用 malloc 分配内存
    # rax = 返回的对象指针

    leaq _ZTV3Dog(%rip), %rcx   # 加载 vtable 地址
    addq $16, %rcx              # 跳过 [-2] 和 [-1]，指向 vtable[0]
    movq (%rsp), %rax           # 取出对象指针
    movq %rcx, (%rax)           # obj._vptr = &vtable[0]

    movq (%rsp), %rdi           # this = 对象指针
    callq Dog_Dog               # 调用构造函数
```

### 14.9.2 虚函数调用（三部曲）

```asm
    # obj->speak()  → speak 在 vtable[0]

    # (a) 读 _vptr
    movq (%rdi), %rax           # rax = obj._vptr（对象偏移0处）

    # (b) 从 vtable 读函数地址
    movq 0(%rax), %rax          # rax = vtable[0]（第0个虚函数）

    # (c) 间接调用
    callq *%rax                 # 调用 rax 指向的函数
```

### 14.9.3 成员字段访问

```asm
    # obj->age（age 在偏移 +8）
    movq -16(%rbp), %rax        # rax = obj 指针
    movl 8(%rax), %eax          # eax = obj->age（读4字节 int）

    # obj->age = 20
    movq -16(%rbp), %rcx        # rcx = obj 指针
    movl $20, 8(%rcx)           # obj->age = 20（写4字节 int）
```

### 14.9.4 if/while 控制流

```asm
    # if (a > b) { ... } else { ... }
    movq -8(%rbp), %rax         # rax = a
    movq -16(%rbp), %rcx        # rcx = b
    cmpq %rcx, %rax             # 比较 a 和 b
    jle else_0                  # a <= b 跳到 else
    # then 分支 ...
    jmp endif_1
else_0:
    # else 分支 ...
endif_1:

    # while (i < n) { ... }
while_2:
    movq -8(%rbp), %rax         # rax = i
    cmpq -16(%rbp), %rax        # 比较 i 和 n
    jge endwhile_3              # i >= n 退出循环
    # 循环体 ...
    jmp while_2
endwhile_3:
```

## 14.10 完整示例

以下 minicc 源码及对应的汇编输出：

```cpp
class A {
public:
    int a;
    virtual int getA() { return a; }
};

int main() {
    A* obj = new A();
    obj->a = 42;
    return obj->getA();
}
```

```asm
# ── 构造函数 ──
A_A:
    pushq %rbp                 # 保存帧基址
    movq %rsp, %rbp            # 建立栈帧
    subq $64, %rsp             # 分配局部空间
    movq %rdi, -8(%rbp)        # 保存 this 指针

    movq -8(%rbp), %rax        # 取 this
    leaq _ZTV1A(%rip), %rcx    # vtable 地址
    addq $16, %rcx             # 跳过 offset-to-top 和 typeinfo
    movq %rcx, (%rax)          # this->_vptr = &vtable[0]

    movq -8(%rbp), %rax        # 返回 this
    leave
    ret

# ── 虚函数 ──
A_getA:
    pushq %rbp
    movq %rsp, %rbp
    subq $64, %rsp
    movq %rdi, -8(%rbp)        # 保存 this

    movq -8(%rbp), %rax        # this 指针
    movl 8(%rax), %eax         # 读 this->a（偏移8，32位 int）
    leave
    ret

# ── main ──
main:
    pushq %rbp
    movq %rsp, %rbp
    subq $64, %rsp

    # new A()
    movq $16, %rdi             # sizeof(A) = 16
    callq malloc               # 分配内存
    pushq %rax                 # 保存指针

    leaq _ZTV1A(%rip), %rcx    # vtable 地址
    addq $16, %rcx             # 指向 vtable[0]
    movq (%rsp), %rax          # 取对象指针
    movq %rcx, (%rax)          # 安装 _vptr

    movq (%rsp), %rdi          # this = obj
    callq A_A                  # 构造
    popq %rax                  # 取回指针

    movq %rax, -16(%rbp)       # obj = 指针

    # obj->a = 42
    movq -16(%rbp), %rcx       # rcx = obj
    movl $42, 8(%rcx)          # obj->a = 42

    # return obj->getA()
    movq -16(%rbp), %rax       # rax = obj
    movq %rax, %rdi            # this = obj
    movq (%rdi), %rax          # rax = obj->_vptr
    movq 0(%rax), %rax         # rax = vtable[0] = A_getA
    callq *%rax                # 调用 A_getA
    leave
    ret

# ── 数据段 ──
    .data
    .globl _ZTV1A
    .align 8
_ZTV1A:
    .quad 0                    # offset-to-top
    .quad _ZTI1A               # typeinfo 指针
    .quad A_getA               # vtable[0]

    .globl _ZTI1A
_ZTI1A:
    .quad 0                    # type_info vptr（简化）
    .quad .Ltype_name_A        # 类型名
    .quad 0                    # 无基类

    .section .rodata
.Ltype_name_A:
    .string "A"
```

## 14.11 快速参考

| 你想做什么 | 汇编指令 |
|-----------|---------|
| 赋值 `x = 42` | `movl $42, -8(%rbp)` |
| 读取 `x` | `movl -8(%rbp), %eax` |
| 加法 `a + b` | `addq %rbx, %rax` |
| 比较 `a > b` | `cmpq %rbx, %rax` + `jg label` |
| 函数调用 `f(x)` | `movq x, %rdi` + `callq f` |
| 虚函数调用 | `movq (%rdi), %rax` → `movq N(%rax), %rax` → `callq *%rax` |
| 读字段 `obj->f` | `movq off(%rax), %rax` |
| 写字段 `obj->f = v` | `movl v, off(%rcx)` |
| 分配内存 `new T` | `movq $sizeof, %rdi` + `callq malloc` |
