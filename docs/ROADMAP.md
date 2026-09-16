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

## ✅ 类模板特化（已完成 2026-09-14，见 docs/learn/19）

**落地内容**：`template<class T, class U = void>` 默认模板实参（使用点补全 + 空洞检查）、
`template<class T> class Box<T*, T>` 偏特化（复用 `TemplateDeducer::deducePair` 跑合一）、
`template<> class Box<int*, int>` 全特化（逐位类型相等）、`struct` 模板体；
择优顺序 ①全特化 → ②偏特化 → ③主模板补默认实参。
安装点：`SemanticAnalyzer::selectClassTemplate` + `m_partialSpecs`/`m_explicitSpecs` 两张注册表，
实例名与 mangling 一律取**使用点实参**。测试 test_tmpl_27..32 + ctest `PartialSpec.*` 4 例。

**未做（各自独立可接手）**
- **`[temp.class.order]` 偏序裁决**：多个偏特化同时匹配时应"更特化者胜"，
  现在取先注册者并打印结论。算法与 S6 函数模板偏序同族，复用现成引擎即可，
  缺的是候选集构造与歧义诊断 → 可作为 S6 的收尾任务。
- **NTTP 特化模式** `template<int N> class Buf<N>` / `Box<int, N>`：Parser 直接报错。
- **类外定义特化成员**、**函数模板特化**、**特化体的延迟匹配**（现在要求先声明后使用）。

## ✅ 第一/第二梯队（已完成 2026-09-14，见 docs/learn/24..28）

来源：对 `/root/cppproject/my-test/cpp/src/test/template` 那批模板用例做能力盘点后
排出的两项待补清单。

**第一梯队**
- ⑨ cv 限定位置修正 —— `const int*` = Pointer(Const(Int))，见 docs/learn/23
- 类内类型别名 `using X = T;` / `typedef T X;`（三个使用点）—— docs/learn/24
- 依赖类型名 `typename T::type`（+ `void_t<typename T::type>` 探测）—— docs/learn/25

**第二梯队**
- 别名模板 `template<class T> using Vec = MyPtr<T>;`（解糖而非实例化；三处解糖点；
  顺带补齐类模板 id 结构合一 + 实例"出身"记录）—— docs/learn/27
- ADL + 限定名查找 —— docs/learn/26
- CTAD + 推导指引（`MyPtr m(7);` / `Two(int) -> Two<int,int>;`）—— docs/learn/28

**未做（各自独立可接手）**
- `auto m = MyPtr(7);`（纯右值 CTAD）、拷贝推导 `MyPtr m = other;`（需先有拷贝语义）、
  聚合推导、指引参与重载决议、CTAD 用于 `new` / 函数返回类型 —— 见 docs/learn/28 §5 边界表。
- `std::enable_if_t`：需要**类型级条件选择**（别名底层类型里表达 `cond ? X : Y`）。
- 别名模板偏特化、别名模板作模板模板实参。
- ⑧ 一元 `*` 解引用（`return *p;` 不解析）→ ④ [stmt.ambig] 完整裁决 → ⑥ 后置 const。

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

> **与 NTTP 的接口**：NTTP 的实参解析目前只认字面量与一元负号
> （`Buf<4>` / `Buf<-3>`，见 `Parser::parseTemplateArgumentList`）。
> 主线 D 落地后应把该处换成「解析完整常量表达式 → 折叠求值」，
> 于是 `Buf<2+2>`、`Buf<N*2>`（依赖前面的 NTTP）随之打通。
> 这是主线 D 最自然的第一个消费者。

## 主线 E：数组 / enum / namespace（P2，`[]` 与 `::` token 已留坑)

**理论点**：数组类型与指针退化（array-to-pointer decay）；enum 的底层类型；namespace 的限定名查找与 using 指令
**任务**：Type 增加 Array 种类 + 下标表达式；EnumDecl；NamespaceDecl + `::` 限定查找
**验收/产物**：tests/test_misc_01..NN + docs/learn/12

> **与 NTTP 的接口**：NTTP 最经典的用法就是数组长度
> `template<int N> class Buf { int data[N]; }`。该写法目前已能在 Parser
> 正确登记 NTTP 形参（测试 test_tmpl_21 的日志可见），但**卡在数组语法**——
> `int data[N]` 报 `Expected ';' after field declaration`。
> 主线 E 补上数组后，NTTP 才对使用者产生实际价值。

## 主线 F（选做，深水区）

| 特性 | 理论点 | 参照 |
|---|---|---|
| 运算符重载 | 重写集（rewrite）vs 真正重载；成员/非成员形式 | SemaOverload.cpp |
| lambda | 闭包转换（closure conversion）：匿名类 + operator() + 捕获 | SemaLambda.cpp |
| 异常 | Itanium EH ABI：personality 函数、LSDA、zero-cost 展开 | libunwind + CGException.cpp |
| 虚继承 | 虚基类指针 vbptr、菱形继承布局 | CGRecordLayoutBuilder |

---

## 主线 H：decltype / SFINAE（编译期类型查询）—— ✅ 已完成（2026-09-14）

> **落地结果**：文档 `docs/learn/20-decltype-and-sfinae.md`（decltype + SFINAE）
> 与 `docs/learn/21-partial-ordering.md`（偏序裁决）；
> 测试 `tests/tmpl/test_tmpl_33..43` + `tests/unit/test_decltype_sfinae.cpp`
> 的 `Decltype.*` / `Sfinae.*` / `PartialOrder.*` 三套件（ctest 145/145）。
> 下方原始规划保留，末尾附「实际完成 / 未做」对照。



**为什么单独立线**：这不是"再补一个语法糖"，而是给编译器加一个**「表达式 → 类型」的求值子系统**。
它同时是主线 D（常量折叠/constexpr）的接口——两者都是"编译期求值"，共享同一套 AST 遍历。

**现状**：`decltype` 在 `src/` `include/` 里**零出现**，词法阶段就没有该关键字。
重载决议里的**软失败机制已具备**（`semantic_analyzer.cpp:2144`：候选推导失败仅 `candidate rejected`，循环继续；
全部失败才 `error`）——这正是 SFINAE 的引擎。缺的是**在源码里表达条件**的能力。

**落地内容（已做）**
- `decltype(expr)`：新增表达式求值入口 `inferDecltype`，复用既有 `inferType`，返回 TypePtr
- SFINAE 触发条件：推导失败即移除候选（复用既有软失败路径），不新增报错
- `template<typename T, typename = void>`：无名形参 + 默认值
- 尾置返回类型 `auto f() -> T`：返回类型位置接受 `->` 后的类型
- 最小 `std` 垫片：`void_t` / `false_type` / `true_type` / `declval` / `enable_if_t` / `decay_t`

**理论点**
- `decltype` 的两套规则：`decltype(e)` 与 `decltype((e))` 的差异（[dcl.type.decltype]）
  —— 未加括号的 id-expression 取**声明类型**，加括号取**值类别推导类型**
- SFINAE（[temp.deduct]/8）：**推导失败不是错误**（immediate context 内），
  这是"合一算法失败态"的正面表达 —— 与本项目主线「实参推导≈合一算法」直接呼应
- `void_t` 探测惯例（[temp.deduct]/8 的 CWG 1558 修正）：别名模板实参替换失败**也**是软失败
- 尾置返回类型与 SFINAE 的配合：返回类型参与替换 → 失败即移除候选

**参照**
- `clang/lib/Sema/SemaExprCXX.cpp` → `BuildDecltypeType` / `Sema::ActOnDecltypeExpression`
- `clang/lib/Sema/SemaTemplateDeduction.cpp` → SFINAE 的 `TDK_ImmediateContext` 判定
- `clang/lib/Sema/SemaType.cpp` → 尾置返回类型的 `ActOnFunctionDeclarator` 分支

**任务分解**
1. 词法 + Parser：`decltype` 关键字、`decltype(expr)` 类型节点
2. Sema：`inferDecltype` —— 求表达式类型，区分加括号/不加括号两套规则
3. Parser：尾置返回类型（`->` 分支接入函数声明解析）
4. Parser：无名形参 + 默认值 `typename = void`
5. std 垫片头文件 + `::value` 静态成员访问
6. 测试 `tests/decl/test_decltype_NN.cpp` + `tests/tmpl/test_tmpl_33_sfinae_*.cpp`
7. 文档 `docs/learn/20-decltype-and-sfinae.md`

**验收**：`is_range<decltype(numbers)>::value == 1` 且 `is_range<decltype(1)>::value == 0`；
SFINAE 探测不命中时**静默回退**而非报错；与 clang oracle 对拍一致。

**依赖**：模板管线（✅ 已完成）→ 本线 → 主线 D（常量折叠）可复用求值框架。

### 实际完成 / 未做对照

| 规划项 | 状态 | 说明 |
|---|---|---|
| `decltype(expr)` 求值 | ✅ | `SemanticAnalyzer::evaluateDecltype`；两套规则齐备 |
| `decltype` 依赖上下文延迟求值 | ✅ | `TypeKind::Decltype` 节点 + `substituteType` Case 1.5 两段式 |
| SFINAE 软失败 | ✅ | 新增 `SubstitutionFailure` 异常类型，与真错误显式区分 |
| `void_t` 探测惯例 | ✅ | `resolveType` + `TemplateDeducer::reducePattern` 按名识别归约 |
| `declval` | ✅ | `inferCall` 内建分支，按名识别给出 `T&&` |
| `false_type` / `true_type` | ✅ | `registerBuiltins` 内建注入（含 `value` 静态常量） |
| 无名形参 `typename = void` | ✅ | 合成名 `$unnamedN` |
| **偏序裁决**（原属 learn/19 遗留） | ✅ | **超额完成**：本线顺带把 `[temp.class.order]` 补全，见 learn/21 |
| 引用结构匹配修正 | ✅ | 顺带修 bug：此前 `Probe<int>` 会错误匹配 `Probe<T&>` |
| 尾置返回类型 `auto f() -> T` | ⚠️ 部分 | 解析可用，但未系统验证；见 `test_tmpl_12_func_forms` |
| `enable_if_t` / `decay_t` | ❌ 未做 | 别名模板缺口（Parser 不支持 `using X = Y;`），故 `operator|` 那半边仍不通 |
| 函数默认实参 | ❌ 未做 | Parser 不支持 `void f(int x = 0)`，enable_if 惯用法暂时写不出 |
| 函数形参里的 decltype 依赖表达式 | ❌ 未做 | `f(T t, decltype(t.begin())* g)` 推导期不会把 `t` 绑定到实参类型 |

**验收对拍**：`is_range<decltype(numbers)>::value == 1` 且 `is_range<decltype(1)>::value == 0` ✅
（`tests/tmpl/test_tmpl_39_is_range_demo.cpp`，退出码与 clang 一致）。

---

## 约定（所有主线通用）

1. 遵循 CLAUDE.md 的交付三件套与改动顺序（README → Ast/Parser → Sema → 新模块 → CodeGen → tests → docs/learn）
2. 每条主线配 docs/learn/NN（编号接续 08 起）与同系列测试，回归红线不破
3. 语义拿 `clang++-18 -emit-llvm -S` / `-Xclang -ast-dump` 当 oracle
4. 动手前按 docs/prompt-template-deduction.md 的格式产出：文件清单 + EBNF + 算法伪代码，确认后实现
