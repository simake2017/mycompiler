#pragma once
// =============================================================================
// include/lexer.h —— 阶段 1：词法分析器 (Lexer / Scanner)
// =============================================================================
// 职责：源码字符串逐字符扫描 ⇒ Token 流。手写 DFA：按【首字符】分派，每个分支产出一个 Token。
//
// 管线位置  源码(.cpp) → Preprocessor → ★Lexer★ → Parser → Sema → … → CodeGen
//           输入 = 预处理后的整段源码；输出 = std::vector<Token>（以 Eof 收尾）
// 标准章节  [lex.phases] 阶段 3/7（合并两级，直接产语法 token）│ [lex.token] / [lex.operators]
// 对照 clang：Lexer::LexTokenInternal —— 即本 DFA 所实现的划分规则
//
// 首字符分派表（扫到什么 ⇒ 走哪条分支 ⇒ 产出什么）：
//   `0-9`        ⇒ scanNumber()              ⇒ "42"   → Token{IntLiteral,"42"}
//   `a-z A-Z _`  ⇒ scanIdentifierOrKeyword() ⇒ "x"    → Token{Identifier,"x"}
//                                            ⇒ "int"  → Token{KwInt,"int"}
//   `"`          ⇒ scanString()              ⇒ "hi\n" → Token{StringLiteral, 转义后的内容}
//   其他         ⇒ scanOperator()            ⇒ "=" → Token{Assign}；"<=" → {LessEqual}
//                                            ⇒ "->" → Token{Arrow}
//   EOF（消费尽）⇒ 不再分派                    ⇒ Token{Eof,""}，★重复调用恒返回 Eof（幂等）
// 空白与 `//` `/*` 在分派之前被 skipWhitespaceAndComments() 吃掉 ⇒ 不产 Token。
//
// 为什么手写而不是 lex/flex：教科书路线是正则式 → NFA → DFA；工业编译器（clang/gcc）却普遍手写
// "按首字符分派"的扫描器 —— 等价于手工特化的 DFA，但更易调试、报错更友好。本项目选手写，
// 正是为了把自动机的每一步都摊开给人看。
// =============================================================================

#include "token.h"
#include <string>
#include <string_view>
#include <vector>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// Lexer：词法分析主体（一次性、单向推进的扫描器）
// ─────────────────────────────────────────────────────────────────────────────
// demo: Lexer("int x = 42;").tokenizeAll() ⇒ [KwInt][Identifier][Assign][IntLiteral][;][Eof]
// 外层分派（nextToken() 每次调用的流程）：
//
//     skipWhitespaceAndComments()      ← 循环吃掉空白/注释
//              │
//        到文件尾了？ ──是──► 返回 Token{Eof}
//              │否
//    按首字符 c 分派（只看第一个字符就决定分支）
//      ├─ isdigit(c)            → scanNumber()              如 42
//      ├─ isalpha(c) || c=='_'  → scanIdentifierOrKeyword() 如 x / int
//      ├─ c=='"'                → scanString()              如 "hi"
//      └─ 其他                  → scanOperator()            如 = <= ->
//              │
//        返回一个 Token ──── 下一次调用从停下的位置继续
//
// 全部内部状态只有 4 个成员（源码 + 光标 + 行 + 列），前瞻用只读 peek、不回退不缓存。
class Lexer {
public:
    // 构造：接管整份源码（按值传参 + 移动，右值实参可零拷贝）。
    // demo: Lexer lex("int x = 42;"); ⇒ 光标停在文件开头，行=列=1
    explicit Lexer(std::string source);

    // 扫描全部 Token（含末尾 Eof）—— main.cpp 用的一次性接口。
    // demo: Lexer("int x;").tokenizeAll() ⇒ [KwInt][Identifier "x"][Semicolon][Eof]
    std::vector<Token> tokenizeAll();

    // 逐个获取下一个 Token（流式接口）：DFA 的真正外层循环。
    // 源码耗尽后【永远】返回 Eof（幂等），调用方循环条件无需特判。
    // demo: "42;" ⇒ 第 1 次 Token{IntLiteral,"42"}，第 2 次 Token{Semicolon}，
    //       第 3 次及以后 Token{Eof}
    Token nextToken();

private:
    // ── 扫描器的四个状态（一趟式词法分析的全部状态）──
    // m_source    ：源码全文（完整持有，peek/peekNext 依赖随机访问）
    // m_pos       ：扫描光标，指向"下一个待读字符"的下标，只前进不回退
    // m_line/m_col：光标对应的行/列，在 advance() 中同步维护，用于生成 SourceLocation
    // demo: 源码 "ab\nc" —— 消费 3 个字符后 m_pos==3、m_line==2、m_col==1（指向 'c'）
    std::string m_source;
    size_t      m_pos    = 0;
    uint32_t    m_line   = 1;
    uint32_t    m_col    = 1;

    // ── 字符级操作：DFA 的"眼睛和手" ──
    // peek/peekNext 只读前瞻（看而不消费，保证可以"反悔"）；advance 是唯一的消费动作。
    // 越界读取统一返回 '\0'，调用方无需处处判边界。
    // 前瞻的必要性：切词要不断做"再看一个字符"的决定 —— "==" 还是 "="？"//" 注释
    // 还是除法？ 这正是 [lex.pptoken] 最长匹配的要求。

    // 查看当前字符但不消费。demo: "x+1" ⇒ peek()=='x'，再 peek() 仍是 'x'
    char peek() const;
    // 查看下一个字符但不消费（判断 "==", "->", "//" 等双字符结构用）。
    // demo: "->" ⇒ peek()=='-'，peekNext()=='>'
    char peekNext() const;
    // 消费并返回当前字符，光标 +1；遇 '\n' 行号 +1、列号归 1，否则列号 +1。
    // demo: "a\nb" ⇒ advance()=='a'；再 advance()=='\n'（行 1→2、列归 1）
    char advance();
    // 源码是否已读完（光标越过最后一个字符）。demo: 空源码 ⇒ 构造后立即为 true
    bool isAtEnd() const;

    // ── 跳过空白与注释 ──
    // [lex.comment]：注释不是 token，词法上等价于空白，直接"吃掉"即可。
    // 每次 nextToken() 开头调用，保证产出的 token 之间没有杂质。
    void skipWhitespaceAndComments();

    // ── 扫描各类 Token（DFA 的四大分支）──
    // 统一约定：调用时光标指向该 token 的首字符；返回时光标恰好停在它之后。

    // 数字字面量（[lex.icon]，简化为十进制整数）：贪心消费连续数字。
    // demo: "42;" ⇒ Token{IntLiteral,"42"}，光标停在 ';'
    Token scanNumber();
    // 标识符/关键字（[lex.name]/[lex.key]）：消费字母/数字/下划线后查 kKeywordMap
    // 决定是关键字还是标识符。
    // demo: "if(x" ⇒ Token{KwIf,"if"}，光标停在 '('；"foo " ⇒ Token{Identifier,"foo"}
    Token scanIdentifierOrKeyword();
    // 字符串字面量（[lex.string]）：消费到未转义的 '"' 为止，途中翻译转义序列。
    // demo: 源码 6 字符 "hi\n" ⇒ Token{StringLiteral, "hi"+换行符}，结尾引号被消费
    Token scanString();
    // 运算符/分隔符（[lex.operators]）：按最长匹配决定取双字符还是单字符。
    // demo: "<=" ⇒ Token{LessEqual}；"<x" ⇒ Token{Less}，光标停在 'x'
    Token scanOperator();

    // ── 辅助 ──
    // 快照当前 (行, 列)：各扫描函数都在开头先拍快照，
    // 保证 token 的位置指向它的"起始处"而不是结束处。
    SourceLocation currentLocation() const;
    // 统一的 Token 装配入口，避免各扫描函数重复聚合初始化。
    // demo: makeToken(KwInt, "int", {1,1}) ⇒ Token{KwInt,"int",{1,1}}
    Token makeToken(TokenType type, std::string text, SourceLocation loc) const;
};

} // namespace minicc
