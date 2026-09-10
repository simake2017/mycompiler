# 设计：完整 typeinfo 结构演示 + minicc 非虚多继承全链路

日期：2026-09-01
状态：已与用户确认（独立 demo / 非虚多继承全链路 / 新文档 13 号 / Itanium 主基类优化模型）

## 背景

- 现状：minicc 仅支持单继承（`ClassDecl::baseClassName` 为单个字符串，Sema/CodeGen 均按单继承写死）；
  `emitRTTI` 发射统一三槽简化结构（`[vptr=0][名字][基类ti或0]`），`__minicc_dynamic_cast`
  助手沿单链表上溯。
- 已完成的观测基础：`docs/learn/11-dynamic_cast-rtti.md` 的 4.5~4.8 已用真实编译器
  实测了多继承布局（双子表、offset-to-top=-16、thunk）与 `__vmi_class_type_info`
  逐字节解码（`offset_flags`：低 2 位 = virtual|public，高位 = 偏移<<8）。
- 本次目标：把"观测"变成"实现 + 可讲解的代码结构"。

## 非目标

- 虚继承 / 菱形共享（vbptr / vbtable / VTT）——单开主线。
- `dynamic_cast<T&>`、跨 .so 的 typeinfo 弱符号合并。
- protected / private 继承（直接报错拒绝）。

---

## Part 1：独立可运行 typeinfo 完整结构演示

### 交付物

`demos/13-typeinfo/typeinfo_full.cpp`——自包含、一条命令可编译运行的教学件，
**不依赖** minicc 管线，纯手写三种形态类 + 手写搜索助手 + 编号测试用例。

### 数据结构（手写三种形态，对齐 Itanium ABI 字段语义）

```cpp
// 基类：所有 typeinfo 的公共头部
struct type_info {
    const void* vptr;     // 形态判别（真实世界里指向 __class/__si/__vmi 的虚表）
    const char* name;     // 类型名（本演示用源码名，真实是 mangled 名）
};

// 形态一：无基类（2 槽，16 字节）—— 链终点
struct __class_type_info : type_info {};

// 形态二：单一公有基类（3 槽，24 字节）—— 单链表
struct __si_class_type_info : type_info {
    const type_info* base;
};

// 形态三：多继承（变长）—— 基类数组 + 偏移编码
struct __vmi_class_type_info : type_info {
    unsigned flags;        // 演示中置 0
    unsigned base_count;
    struct base_info {
        const type_info* ti;
        long offset_flags; // 低 2 位 = virtual|public 标志；>>8 = 子对象偏移
    } bases[];             // C 柔性数组；演示用定长手写
};
```

### 手写助手 `mi_dynamic_cast(obj, target_ti)`

```text
1. vptr = *obj
2. top  = obj + vptr[-2]          # offset-to-top 归顶（主表为 0，次表为 -子对象偏移）
3. ti   = vptr[-1]                # 最派生 typeinfo
4. DFS(ti, target, acc_offset=0):
     ti == target            → 返回 top + acc_offset（命中）
     __class 形态            → 链终点，失败
     __si   形态             → DFS(base, target, acc+0)   # 单继承偏移恒 0
     __vmi  形态             → 逐基类 DFS(bases[i].ti, target, acc + (offset_flags>>8))
5. 未命中返回 nullptr
```

### 测试用例（同文件内编号函数 + 断言 + 逐槽内存图形打印，main 汇总退出码）

| # | 用例 | 验证点 |
|---|---|---|
| T1 | 三种形态 `sizeof` / 槽布局 | `__class`=16B、`__si`=24B、`__vmi` 按基数 |
| T2 | 手工搭 `__si` 链（D→A）| 上溯命中、偏移累加为 0 |
| T3 | 手工搭 `__vmi` 链（D:[A@0,B@16]）| 命中 B 时返回 top+16 |
| T4 | 下溯（top→最派生）与自身命中 | 返回 top |
| T5 | 兄弟类拒绝 | 无公共路径 → nullptr |
| T6 | 跨转型（A\* → B\*，经 D）| `__vmi` 基类数组的两个分支都可达 |
| T7 | 链中偏移编码解码 | `offset_flags` 低 2 位标志与 >>8 偏移的位运算断言 |
| T8 | 与真实 `typeid` 对照 | 同一继承形状下，真实 `_ZTI` 的槽数/形态选择与手工结构一致（打印对照，不做指针等值比较） |

### 运行方式（写入文档）

```bash
clang++-18 -std=c++20 -g demos/13-typeinfo/typeinfo_full.cpp -o build-linux/typeinfo_demo -ldl
./build-linux/typeinfo_demo
```

---

## Part 2：minicc 非虚多继承全链路

### 虚表模型：Itanium 主基类优化（与 clang 逐字节对拍）

- **主表合并**：第一个多态基类为"主基类"，其虚表与派生类合并为一张主表
  （覆写原地替换、新虚函数追加），主表 vptr 与主基类子对象共享。
- **次表独立**：其余每个多态基类各有一张次表，表头 `offset-to-top = -(子对象偏移)`，
  覆写槽填 **thunk**（不直接填函数地址）。
- 两张（多张）子表连续发射在同一 `_ZTV` 符号内（与真实布局一致）。

### 分层改动

#### Parser

- `ClassDecl::baseClassName`(string) → `baseClassNames`(vector<string>)；
  语法 `class D : public A, public B { ... }`（逗号分隔，各带访问说明符）。
- 非 `public` 说明符 → 报错"仅支持 public 继承"。
- 同步更新所有 `baseClassName` 使用点（Sema 森林打印、类注册、初始化列表检查、
  dynamic_cast 静态检查等约 8 处）。

#### Sema（布局算法）

```text
layout(D, bases[]):
  offset = 0; primaryVTable = null
  for B in bases:                        # 声明顺序放置
      B 子对象置于 offset（对齐取 B 的最大对齐，多态类 ≥8）
      if B.hasVTable:
          if primaryVTable == null:      # 第一个多态基类 = 主基类
              主表 = B 的表 + D 的覆写/新增（同现有单继承逻辑推广）
              vptr 安装点 += (子对象offset, _ZTV_D 主表段)
          else:
              次表 = B 的表 + D 的覆写（覆写项标记为 thunk）
              vptr 安装点 += (子对象offset, _ZTV_D 次表段)
      offset += sizeof(B 子对象，含对齐填充)
  D 自身字段从 offset 继续累加
  记录每个基类的子对象偏移表 baseOffsets[B]（指针调整与 RTTI 用）
```

- **布局顺序简化声明**：按声明顺序放置（真实 Itanium 会把主基类提到最前）；
  当首个基类即多态时二者一致（覆盖全部测试场景），差异写入文档 ⑤ 简化表。
- **菱形/重复基类检测**：建继承图，DFS 发现同一基类出现两次 →
  报错 `diamond/duplicate base 'X' detected in 'D' (virtual inheritance not supported)`。
- `dynamic_cast` 静态检查推广到多基类上溯/下溯/跨转型可达性。

#### CodeGen

1. **emitVTable 升级**：主表段（ott=0）+ 每个次表段（ott=-子对象偏移）顺序发射；
   每段各有表头 `[ott][ti]`。
2. **thunk 发射**：每个（次表, 覆写函数）对发射一个跳板：

   ```asm
   D_g_thunk16:               # 简化 mangling：类名_函数名_thunk偏移
       movq  (%rdi), %rax     # vptr
       movq  -16(%rax), %rcx  # vptr[-2] = offset-to-top（负偏移）
       addq  %rcx, %rdi       # this 归顶
       jmp   D_g              # 跳真实函数
   ```

   虚析构的删除变体在次表中同样走 thunk——保证 `delete (B*)obj` 正确
  （归顶后析构整个对象并释放）。
3. **构造函数**：vptr 安装从"仅偏移 0"改为遍历全部安装点
   （栈对象路径与 `new` 路径两处同步改）。
4. **上转型指针调整**：派生类指针 → 非主基类指针的转换点
   （初始化、赋值、函数实参、显式转换）发射 `add $子对象偏移`；
   主基类偏移 0，退化为空操作。下溯 `static_cast` 发射 `sub $偏移`。
5. **emitRTTI 升级为计数式布局**（替代三槽简化，统一处理 0/1/N 个基类）：

   ```asm
   _ZTI1D:
       .quad 0                # vptr 占位（简化，真实指向形态类虚表）
       .quad .Ltype_name_D    # 类型名
       .quad 2                # 基类数 N
       .quad _ZTI1A           # 基类 i 的 typeinfo
       .quad 0                # 基类 i 的子对象偏移（真实编码进 offset_flags 高位）
       .quad _ZTI1B
       .quad 16
   ```

   与真实 `__vmi` 的差异（单偏移字段替代 `offset_flags` 位编码、无形态类区分）
   写入文档对照表。
6. **__minicc_dynamic_cast 助手升级**（伪码）：

   ```text
   vptr = *obj;  top = obj + vptr[-2];  ti = vptr[-1]
   DFS(ti, target, acc=0):
       ti == target        → return top + acc
       N = ti[+16 槽]（基类数）
       for i in 0..N:  r = DFS(ti 的第 i 个基类ti, target, acc + 第 i 个偏移); if r: return r
       return null
   ```

   发射条件不变：仅当程序中出现过 `dynamic_cast` 时。

### 错误处理清单

| 场景 | 报错文案（期望，测试断言用） |
|---|---|
| 私有/保护继承 | `only public inheritance is supported` |
| 菱形/重复基类 | `diamond/duplicate base 'X' detected ... not supported` |
| 基类未定义 | 沿用现有 `Base class 'X' not found`（推广到逐个基类名） |
| `dynamic_cast` 跨无公共派生的类 | 沿用现有静态拒绝 |

---

## Part 3：测试（三件套之三）

| 文件 | 内容 |
|---|---|
| `tests/test_mi_01_layout.cpp` | 双多态基类对象布局：两个 vptr 位置、各基类/自身字段偏移、sizeof |
| `tests/test_mi_02_dispatch.cpp` | 主表派发 + 次表经 thunk 派发（覆写函数读 D 自身字段验证 this 已归顶） |
| `tests/test_mi_03_dynamic_cast.cpp` | 上溯（含非主基类 +16 调整）、下溯、跨转型（A\*→B\*）、无关类返回空 |
| `tests/test_mi_04_error.cpp` | 菱形检测、私有继承拒绝（文件头注明期望报错文案） |
| `tests/unit/test_rtti_layout.cpp` 追加 | 白盒：多基类时 RTTI 计数式槽数、次表 ott 值、thunk 出现在 .s 中、助手按需发射 |

文件头注释规范沿用现状：预期行为 + 考察理论点 + （错误用例）期望报错文案。
回归红线：`test_tmpl_01..19`、`test_ctor_01..02`、`test_decl_01`、`test_rtti_01..03`、
`test_pp_01..04`、`unit_tests` 全绿。

## Part 4：文档（三件套之二）

- 新建 `docs/learn/13-multiple-inheritance.md`：
  ① 理论背景（[class.mi]、Itanium ABI §2.4 布局 / §2.9.5 RTTI、`offset_flags` 编码）
  ② Part 1 demo 的三种形态结构逐槽讲解 + 运行命令
  ③ minicc 实现：分层改动说明、布局算法、thunk、计数式 RTTI、助手算法、关键过程 ASCII 图
  ④ clang 对照表（`ItaniumRTTIBuilder.cpp:BuildVMIClassTypeInfo`、
     `CGClass.cpp:EmitVTablePtr/ComputeVirtualBaseObjectOffsets`、
     `VTableBuilder` thunk 发射 → 本实现位置 → 简化了什么）
  ⑤ 已知简化与真实世界差距 ⑥ 复现实验命令 ⑦ 测试矩阵
- 更新 `docs/learn/11-dynamic_cast-rtti.md` ⑤ 表：
  "仅单继承上溯 → `__vmi` 处理多继承"一行改为"已支持（见 13 号文档）"，加交叉链接。
- 更新 `README.md` 语法表：多继承语法与约束。

## 实施顺序

1. Part 1 demo（独立可跑，先把算法讲透，作为 Part 2 的语义参照）
2. README 语法表（三件套固定顺序首位）
3. Parser：基类列表
4. Sema：布局 + 菱形检测 + 静态检查推广
5. CodeGen：双表 + thunk + vptr 多安装点 + 指针调整 + 计数式 RTTI + 助手升级
6. 集成测试 `test_mi_01..04` + 单测追加
7. 文档 13 + 11 号文档修订
8. 全量回归（含 `build-linux` 构建、全部现存测试）

## 风险与对策

| 风险 | 对策 |
|---|---|
| 次表虚析构删除路径（`delete (B*)`）与现有 `test_ctor_02` 交互 | 次表析构槽同样走 thunk；`test_mi_02` 增加经次基类 delete 的用例 |
| 上转型调整点分散在 emitExpr 多处 | 收敛到统一辅助函数 `emitUpcastAdjust(from,to)`，只在 4 类转换点调用 |
| 对齐/填充与 clang 不一致导致无法对拍 | 测试用 `-fdump-vtable-layouts` 与 sizeof 双对拍；首个基类为多态的场景与 clang 一致 |
| `baseClassName`→vector 的重构波及面 | 编译器静态保证：改字段类型后逐个修复编译错误，全绿才算完成 |

## 验收标准

1. `demos/13-typeinfo/typeinfo_full.cpp` 一条命令编译运行，T1~T8 全部 PASS。
2. `test_mi_01..04` 全绿；布局与偏移和 `clang++-18 -fdump-vtable-layouts` 输出一致
   （首个基类为多态的场景）。
3. 全部现存测试回归通过（红线清单见上）。
4. `docs/learn/13-multiple-inheritance.md` 产出且含三件套要求的全部要素；
   11 号文档与 README 同步更新。
