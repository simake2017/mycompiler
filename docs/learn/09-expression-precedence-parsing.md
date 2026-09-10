# 09 表达式解析与运算符优先级（优先级分层递归下降）

> 被测对象：`src/parser.cpp` 表达式优先级链
> `parseExpression → parseOrExpr → parseAndExpr → parseEqualityExpr → parseComparisonExpr
>  → parseAdditiveExpr → parseMultiplicativeExpr → parseUnaryExpr → parsePostfixExpr → parsePrimaryExpr`
> 单元测试：`tests/unit/test_expr_parser.cpp`（13 用例，白盒断言树形）

## ① 理论背景

**标准依据**：[expr.prop]（运算符优先级与结合性表）、[expr.prim]（基本表达式）、
[expr.ass]（左结合与赋值）、[temp.names]（template-id 与 `<` 的歧义）。

C++ 标准用一个"大表"定义优先级（§[expr.prop]，17 个等级），但表本身**不是算法**。
把表变成解析器有两条经典路线：

1. **把优先级编译进文法**——每个优先级等级一个非终结符（产生式），
   优先级越低嵌套越外层 → 这就是**优先级分层递归下降**（Precedence-Layered
   Recursive Descent），本项目采用的路线。
2. **运行时查优先级表**——解析时动态比较"结合力"（Pratt / 调度场），
   优先级不进文法，进数据。

### 1.1 文法形态

```text
or-expr    := and-expr   ('||' and-expr)*          ← 最低优先级，最外层
and-expr   := equality   ('&&' equality)*
equality   := comparison (('==' | '!=') comparison)*
comparison := additive   (('<'|'>'|'<='|'>=') additive)*
additive   := mult       (('+' | '-') mult)*
mult       := unary      (('*' | '/' | '%') unary)*
unary      := ('-' | '!') unary | postfix          ← 右结合（递归在右侧）
postfix    := primary ( call-args | ('.'|'->') IDENT )*
primary    := INT | BOOL | STRING | 'nullptr' | 'this' | IDENT | '(' or-expr ')'
```

两个关键形状：

- **每层的 `(...)* `（while 循环）编码左结合**：
  `left = Bin(op, left, right)` 反复把新节点套在**左边**。
- **二元层全部左递归消除成循环**；唯一的递归在**右侧**的只有
  一元层（`('-'|'!') unary`），所以 `- - x` 解析成 `Neg(Neg(x))`。

### 1.2 优先级正确性的来源：不比较，只嵌套

"为什么 `*` 先于 `+` 绑定"——不是因为运行时比较了优先级数值，而是因为
**加法层在取右操作数时递归进了乘法层**：乘法层还没返回，加法层就没机会消费
下一个运算符。优先级高的运算符位于更深的栈帧里，物理上先被看见、先被折叠。
整个解析过程**没有一次优先级比较**。

## ② 设计决策

| 决策 | 取舍 |
|---|---|
| 用 9 个函数分层，不用优先级表 | 每层可单独讲解/单独打日志/单独测试；代价是加新等级要加一层函数（C++ 有 17 级所以 clang 不用这招） |
| 二元层左结合用 `while` 循环 | 与文法 `(op operand)*` 一一对应；循环次数 = 连续同层运算符个数 |
| 一元层用真递归 | 一元右结合（`--x = -(-x)`），递归方向即结合方向 |
| 括号不产生节点 | `'(' expr ')'` 直接返回内层表达式——括号只是"改变递归入口"，AST 上不留痕（与 clang `ParenExpr` 不同，教学取舍：树更干净） |
| 错误处理用 panic 模式 | 语法错误直接抛 `std::runtime_error("[Parse Error] ...")`，不做 token 级同步恢复（见 ⑤ 实验） |

## ③ 例子：从 Token 流到 AST

### 例 1：`1 + 2 * 3`（两层协作）

```text
parseExpression
 └─ parseOrExpr → ... → parseAdditiveExpr
      left = parseMultiplicativeExpr()
        ├─ parseUnary → parsePrimary → Int(1)
        └─ 见 '+' ? 不是本层运算符 → 返回 Int(1)
      见 '+' ✓ 消费
      right = parseMultiplicativeExpr()        ← 关键：取右操作数时下潜一层
        ├─ Int(2)
        └─ 见 '*' ✓ 是本层！消费，再取 Int(3)
        └─ 折叠 → Mul(2, 3)                    ← * 在更深的栈帧里先绑定
      折叠 → Add(1, Mul(2, 3)) ✓
```

```text
        Add
       ╱   ╲
     Int(1)  Mul
            ╱   ╲
         Int(2) Int(3)
```

### 例 2：全链贯穿 `a + b == c && d || e`

优先级：`+` > `==` > `&&` > `||`，每层只消费自己等级的运算符：

```text
                    Or
                  ╱    ╲
                And    Var(e)
               ╱   ╲
             Eq    Var(d)
            ╱  ╲
          Add   Var(c)
         ╱  ╲
     Var(a) Var(b)
```

（对应单测 `ExprParserPrecedence.FullChainShape`；
clang 对照见 ⑤ 实验，ast-dump 树根同样是 `'||'`。）

### 例 3：左结合 `10 - 5 - 2`（`while` 向左折叠）

```text
第1圈循环：left=Sub(10, 5)          第2圈循环：left=Sub(Sub(10,5), 2)

      Sub                                Sub
     ╱   ╲                              ╱   ╲
  Sub    Int(2)          ←最终形状   Sub   Int(2)
  ╱ ╲                                ╱  ╲
Int(10) Int(5)                   Int(10) Int(5)
```

语义上左结合保证了 `(10-5)-2 = 3` 而不是 `10-(5-2) = 7`。

### 例 4：一元与二元的边界 `-a * b`

一元层在乘法层的**下一层**，所以 `-` 先把 `a` 抢走：

```text
parseMultiplicativeExpr
  left = parseUnaryExpr()          ← 一元层先跑
        '-' ✓ → Neg( Var(a) )      ← - 只绑住紧随其后的一个操作数
  见 '*' ✓, right = parseUnaryExpr() → Var(b)
  → Mul( Neg(a), b )
```

同理 `!a == b → Eq(Not(a), b)`（一元层比相等层深两级）。

### 例 5：后缀链 `p->next.getValue()`

后缀层用 `while` 循环**从左到右逐层外包**，每步把已有结果包进新节点：

```text
Var(p) →[->] Member(p, next) →[.] Member(..., getValue) →[()] Call(..., [])

              CallExpr
                │ callee
            MemberExpr(getValue, isArrow=false)
                │ object
            MemberExpr(next, isArrow=true)
                │ object
              Var(p)
```

（对应单测 `ExprParserPostfix.CallAndMemberChains`。）

## ④ 三种等价实现对照

同一个优先级问题，三种编码方式可以机械互译：

### 4.1 隐式栈：调用栈（本项目）

递归本身就是栈。解析 `1 + 2 * 3` 时的调用栈：

```text
时刻1  parseAdditiveExpr          ← 挂起等 left
         parseMultiplicativeExpr  ← 返回 Int(1)

时刻2  parseAdditiveExpr          ← 持有 Int(1)，见 '+'，挂起等 right
         parseMultiplicativeExpr  ← 挂起
           parseUnary → Int(2)
           见 '*'，取 Int(3)，折叠 Mul(2,3)   ← * 在栈深处先绑定
         返回 Mul(2,3)
       折叠 Add(1, Mul(2,3))
```

**递归深度 ≈ 优先级层数；"晚绑定"的物理机制 = 外层栈帧一直挂着等内层返回。**

### 4.2 显式栈：调度场算法（Shunting-Yard，Dijkstra）

把运算符压进显式栈，用"弹栈时机"表达优先级：

```text
输入：1 + 2 * 3

步骤    动作                      运算符栈    输出队列
───────────────────────────────────────────────────
读 1    数字直接输出               []          [1]
读 +    栈空，压栈                 [+]         [1]
读 2    数字直接输出               [+]         [1,2]
读 *    * 优先级 > 栈顶+，压栈     [+, *]      [1,2]
读 3    数字直接输出               [+, *]      [1,2,3]
结束    依次弹栈                   []          [1,2,3,*,+]

输出 = 后缀式 1 2 3 * + → 先算 2*3，再算 1+(2*3) ✓
```

左结合的编码：遇到与栈顶**同优先级**的运算符时，先弹栈顶再压新符。

### 4.3 数值化：Pratt 解析（现代编译器主流）

一个函数 + 优先级数值，递归参数携带"最低结合力"：

```cpp
// Pratt 伪代码（本项目未采用，仅作对照）
ExprPtr parseExpr(int minPrec) {
    ExprPtr left = parsePrimary();
    while (prec(peek()) >= minPrec) {
        int p = prec(peek());
        auto op = consume();
        left = Binary(op, left, parseExpr(p + 1)); // +1 → 左结合；传 p → 右结合
    }
    return left;
}
```

三者关系：

```text
优先级分层递归下降 ──把 9 个函数合并成一个 + 优先级表──▶ Pratt
        │（隐式调用栈）                                  │（隐式调用栈）
        └────────────── 同构翻译 ──────────────▶ 调度场（显式运算符栈）
```

| 维度 | 分层递归下降（本项目） | 调度场 | Pratt |
|---|---|---|---|
| 优先级载体 | 文法嵌套深度（静态） | 运行时优先级表 | 运行时优先级表 |
| 栈 | 隐式：调用栈 | 显式：运算符栈 | 隐式：调用栈 |
| 左结合 | `while` 向左折叠 | 同级先弹后压 | 递归传 `p+1` |
| 加新等级 | 加一个函数 | 表里加一行 | 表里加一行 |
| 适合场景 | 等级少、要教学可观测 | 计算器/表达式求值 | 等级多（C++ 17 级） |

## ⑤ 可复现实验

```bash
# 1. 跑本主题单测（13 用例：优先级/结合性/括号/后缀/歧义/报错）
cd build-linux && cmake --build . --target unit_tests -j$(nproc) \
  && ./unit_tests --gtest_filter='ExprParser*'

# 2. 语义 oracle：用 clang ast-dump 核对树形（本实现的形状应与之一致）
printf 'int f(int a,int b,int c){ return a + b == c && 1 || 2; }\n' > /tmp/oracle.cpp
clang++-18 -Xclang -ast-dump -fsyntax-only -std=c++20 /tmp/oracle.cpp \
  | grep -A8 ReturnStmt
# 预期：树根 BinaryOperator '||'，左子树 '&&' → '==' → '+'，与本项目解析形状一致

# 3. 对比实验：把 + 和 * 的层级函数对调（src/parser.cpp parseAdditiveExpr/
#    parseMultiplicativeExpr 的调用关系互换），重跑单测，
#    ExprParserPrecedence.ArithmeticLevels 立即红掉 —— 验证"优先级即文法层次"
```

## ⑥ clang 源码对照

| clang（llvm-project/clang/lib/Parse） | 本实现（src/parser.cpp） | 简化了什么 |
|---|---|---|
| `ParseExpression` / `ParseRHSOfBinaryExpression`（ParseExpr.cpp，查 `PrecedenceLevels` 表迭代） | `parseOrExpr` ~ `parseMultiplicativeExpr` 函数链 | 把迭代+优先级表改写为 9 层函数嵌套；17 级砍到 7 级（无三目/赋值/逗号/移位/位运算） |
| `ParseCastExpression` 前缀一元分支 | `parseUnaryExpr` | 只保留 `-` 与 `!`；无 `*` 解引用、`&` 取址、`++/--` |
| `ParsePostfixExpressionSuffix` | `parsePostfixExpr` | 只保留 `()`、`.`、`->`；无下标、自增、模板实参后缀 |
| `ParsePrimaryExpression` / `ParsePrimaryExpressionOrUnaryExpression` | `parsePrimaryExpr` | 无数字后缀/字符字面量/用户字面量 |
| `ParseImplicitTemplateId`（<cxx-17 消歧） | `parsePrimaryExpr` 中 `<` 试探法（存档→空跑→回滚） | 判据简化为"类型列表 + `>` + 紧跟 `(` 才算 template-id"（[temp.names] 歧义的简化消解） |
| 错误恢复：`SkipUntil` 同步点 | `Parser::error` 直接抛异常（panic 模式） | 不做恢复，每处错误恰好一条报告 |

## ⑦ 关键 ASCII 总图：优先级链全景

```text
parseExpression
  └─ parseOrExpr            ||                        优先级 1（最低，最晚绑定）
      └─ parseAndExpr       &&                        优先级 2
          └─ parseEqualityExpr        == !=           优先级 3
              └─ parseComparisonExpr  < > <= >=       优先级 4
                  └─ parseAdditiveExpr        + -     优先级 5
                      └─ parseMultiplicativeExpr * / % 优先级 6
                          └─ parseUnaryExpr   - !     优先级 7（右结合）
                              └─ parsePostfixExpr  () . ->  优先级 8
                                  └─ parsePrimaryExpr  字面量/变量/括号/new/this
                                                       （叶子层，最先绑定）

每层模板：
  left := 下一层();
  while (当前 token 是本层运算符) { 消费; left := Bin(op, left, 下一层()); }
  return left;
```

**一句话**：优先级不是被"比较"出来的，而是被"嵌套深度"决定的——
越深的函数越早返回，它消费的运算符就越先绑定。
