# 后续计划（ROADMAP）

> 状态快照（2026-09-09）：P0 主线（函数模板推导 S1~S6、预处理器）+ 主线 A（构造/析构）
> + 主线 B（自研链接器）+ 主线 G（多继承布局，见 learn/17）已完成。
> 回归红线 test_tmpl_01..10 全绿；mi_03/mi_04 为登记的既有失败（见主线 G 待办表）。
> 构建：`cmake -S . -B build-linux -DCMAKE_CXX_COMPILER=clang++-18 && cmake --build build-linux -j`

---

## 主线 A：构造/析构（下一项，理论密度高）

**理论点**
- Itanium ABI 构造变体：C1（complete，含虚基类）/ C2（base，子对象构造）/ C3（allocating，配 new）
- 成员初始化顺序 = **声明顺序**（与初始化列表书写顺序无关——经典陷阱）
- 构造三步：分配 → vptr 安装 → 成员按序初始化；析构逆序 + 虚析构的 deleting destructor（D0/D1）
- 隐式默认构造/复制构造的生成条件（Sema 层）

**参照**
- `clang/lib/CodeGen/CGClass.cpp`：`EmitConstructorCall`、`InitializeVTablePtr`
- `clang/lib/Sema/SemaDeclCXX.cpp`：隐式特殊成员函数生成
- 本项目现有基础：`NewExpr.constructorArgs` 已存在（只缺真正的 ctor 声明与调用）

**任务分解**
1. AST/Parser：识别"与类同名的方法"为 ctor、`~名字` 为 dtor；初始化列表 `: a(x), b(y)`
2. Sema：无 ctor 时生成隐式默认 ctor；成员初始化列表排序校验（警告乱序）
3. CodeGen：ctor 发射 C2 变体（vptr 安装 + 成员初始化）；`new T(args)` → malloc + C3/C1 调用；`delete` → D1 + free
4. 虚析构：基类指针 delete 派生对象走 vtable[0]

**验收/产物**：tests/test_ctor_01..NN（含初始化顺序陷阱用例、多态 delete 用例）+ docs/learn/08

## ✅ 主线 B：自动链接出可执行文件（已完成 2026-09-03）

**实际落地方案**（摸底后调整，见 docs/learn/15）：不调用系统 `ld`，
而是自研教学链接器 `src/linker.cpp`（~700 行），借助系统 `as` 产出 .o 后
自行完成 读 .o → 合并节 → 布局 → 符号决议 → 重定位回填 → 写非 PIE 可执行 ELF。
内置 `_start`（call main + exit_group syscall）与 64KB bump 版 malloc/free，
不依赖系统 ld / crt / libc。`-S` 保留只吐汇编。
产物：执行类用例一条命令直出二进制运行（退出码与 clang oracle 一致）+
docs/learn/15-linker-elf-and-mini-ld.md（ROADMAP 原定编号 09，
因 09..14 已被其他主题占用，顺延为 15）。

**理论点**
- 编译流水线全貌：.s → as → .o → ld → ELF；`_start`(crt1.o) → __libc_start_main → main
- crt 文件链：Scrt1.o / crti.o / crtn.o 的 init/fini 段作用；-lc 与动态链接器 /lib64/ld-linux
- 静态 vs 动态链接；`gcc -v` 观察真实链接行

**参照**
- `clang/lib/Driver/ToolChains/Linux.cpp`：链接行构造算法（crt 选择、搜索路径）
- 本项目 Phase 6 目前只打印提示命令

**任务分解**
1. minicc 默认产出可执行文件（fork/exec 调用系统 as+ld 或直接 cc），`-S` 保留只吐汇编
2. 链接行最小实现：`crt1.o crti.o <obj> -lc crtn.o`（从 `gcc -print-file-name=` 动态取路径，勿写死）
3. 错误透传：链接失败时原样展示 ld 报错

**验收/产物**：现有执行类用例（tmpl 14/15/17/19、pp 01..03）一条命令直出二进制并运行 + docs/learn/09

## ✅ 主线 G：多继承布局（已完成 2026-09-09，见 docs/learn/17）

**落地内容**：Itanium ABI primary base（第一个**多态**基类恒占 offset 0，
全非多态时首个基类视作 primary）、次表 thunk 覆写（thunkAdjust = -base.offset）、
size/align 分离（新增 `alignOf()`，类类型 align = 成员 align 递归 max）、
嵌套类字段内联子对象（Sema 字段 `resolveType` + CodeGen `leaq` 折叠 +
ctor 初始化列表嵌套构造调用）。
产物：tests/mi/test_mi_01..07 + docs/learn/17-multiple-inheritance-layout.md。

**登记待办（既有失败，非本轮引入）**

| 项 | 现状 | 修复方向 |
|---|---|---|
| mi_03 | lexer 不支持 `?:` 三元运算符，COMPILE_FAIL | 主线 C 顺带做（Lexer+Parser+Sema+CodeGen 四层） |
| mi_04 | 菱形继承 `Q_f` LINK ERROR：Sema 未拒绝重复基类，Q 继承 X 后符号未生成 | Sema 层加菱形/重复基类检测，期望报错文案见测试头注释；真正支持留给主线 F 虚继承 |

**P2 架构观察（只记录，暂不动手）**

1. `decl->fields` 与 `classLayout.fields` 双清单 + 末尾整体回填
   （semantic_analyzer.cpp:891）；clang 是一次成型不可变 ASTRecordLayout
2. 成员查找不穿透继承链：`obj->get()` 在 `D*` 上查不到 `P::get`
3. vtable 覆写靠字符串剥/拼类名前缀匹配符号名，脆弱；宜存结构化符号引用
4. **内置 bump malloc 不做对齐（待修）**：`new T` 的堆地址对齐保证在真实
   世界里由 operator new 契约提供（[new.delete.single]，x86-64 恒 16B 对齐；
   过对齐类型走 `operator new(size, align_val_t)`，clang 参照
   CGExprCXX.cpp::EmitCXXNewExpr 的 getNewAlignment()）。minicc 的
   linker.cpp:319 注入 malloc 是 `heapPtr + size` 裸推进——连续 new 多个
   小对象后，后续对象首地址可能不满足类型 align，alignOf 的布局计算在堆上
   不再成立。现有 mi 测试每例只 new 一个对象故未踩雷。修复方向：推进处
   `size = alignUp(size,16)`（add $15 / and $-16，3 条指令），并补 mi_08
   连续 new 两个小对象验证地址对齐

## 主线 C：控制流补全（P2，KwFor token 已留坑）

**理论点**：结构化控制的 CFG 降级（for → 条件块 + 回边）；break/continue 的标签栈语义；switch 的跳转表 vs if 链决策
**参照**：`clang/lib/Sema/SemaJump.cpp`；LLVM Kaleidoscope 第 5 章
**任务**：ForStmt/SwitchStmt/Break/Continue AST 节点 → sema 标签栈校验（break 在循环外报错）→ codegen 标签
**验收/产物**：tests/test_flow_01..NN + docs/learn/10

## 主线 D：常量折叠 / constexpr（P2）

**理论点**：编译期求值 = 解释器（部分求值/抽象解释入门）；常量传播与折叠代数规则；constexpr 的"必须可折叠"约束
**参照**：`clang/lib/AST/ExprConstant.cpp`（递归求值器）；LLVM InstCombine 折叠表
**任务**：sema 层折叠常量二元/一元表达式（日志展示折叠前后）；`constexpr` 关键字最小语义（初始化式必须可折叠）
**验收/产物**：tests/test_const_01..NN + docs/learn/11

## 主线 E：数组 / enum / namespace（P2，`[]` 与 `::` token 已留坑)

**理论点**：数组类型与指针退化（array-to-pointer decay）；enum 的底层类型；namespace 的限定名查找与 using 指令
**任务**：Type 增加 Array 种类 + 下标表达式；EnumDecl；NamespaceDecl + `::` 限定查找
**验收/产物**：tests/test_misc_01..NN + docs/learn/12

## 主线 F（选做，深水区）

| 特性 | 理论点 | 参照 |
|---|---|---|
| 运算符重载 | 重写集（rewrite）vs 真正重载；成员/非成员形式 | SemaOverload.cpp |
| lambda | 闭包转换（closure conversion）：匿名类 + operator() + 捕获 | SemaLambda.cpp |
| 异常 | Itanium EH ABI：personality 函数、LSDA、zero-cost 展开 | libunwind + CGException.cpp |
| 虚继承 | 虚基类指针 vbptr、菱形继承布局 | CGRecordLayoutBuilder |

---

## 约定（所有主线通用）

1. 遵循 CLAUDE.md 的交付三件套与改动顺序（README → Ast/Parser → Sema → 新模块 → CodeGen → tests → docs/learn）
2. 每条主线配 docs/learn/NN（编号接续 08 起）与同系列测试，回归红线不破
3. 语义拿 `clang++-18 -emit-llvm -S` / `-Xclang -ast-dump` 当 oracle
4. 动手前按 docs/prompt-template-deduction.md 的格式产出：文件清单 + EBNF + 算法伪代码，确认后实现
