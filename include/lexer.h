#pragma once
// =============================================================================
// 阶段 1：词法分析器 (Lexer / Scanner)
// =============================================================================
// 核心职责：将源码字符串逐字符扫描，输出 Token 流。
// 实现方式：手写的确定性有限自动机（DFA），每个字符根据当前状态决定转移。
// =============================================================================
// 【管线位置】
//   源码(.cpp) → [Preprocessor] → ★Lexer★ → [Parser] → [Sema] → … → [CodeGen]
//   输入：预处理（#include 展开、宏替换）完成后的整段源码字符串；
//   输出：std::vector<Token>（定义见 token.h），以一个 Eof token 收尾。
//
// 【对应 C++ 标准章节】
//   [lex.phases] —— 翻译阶段 3：源码字符序列被分解为预处理 token
//                   （本项目把阶段 3 与阶段 7 简化合并，直接产出语法 token）；
//   [lex.token] / [lex.operators] —— token 的划分规则，即本 DFA 所实现的规则。
//
// 【对应 clang 模块】
//   lib/Lex/Lexer.cpp     —— clang 的词法分析器（核心循环 Lexer::LexTokenInternal），
//                            需与预处理器交互、支持 trigraph 等；本项目简化为手写 DFA；
//   include/clang/Lex/Lexer.h —— 对应此处的 Lexer 类声明。
//
// 【理论：为什么手写 DFA】
//   词法分析的对象理论上是正则语言，有限自动机即可识别。教科书路线是
//   正则式 → NFA → DFA（lex/flex 的做法）；工业编译器（clang/gcc）则普遍手写
//   "按首字符分派"的扫描器——等价于手工特化的 DFA，但更易调试、报错更友好。
//   本项目选择手写，正是为了把自动机的每一步都摊开给人看。
// =============================================================================

#include "token.h"
#include <string>
#include <string_view>
#include <vector>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// Lexer：词法分析主体（一次性、单向推进的扫描器）
// ─────────────────────────────────────────────────────────────────────────────
// 使用示例：
//   Lexer lexer("int x = 42;");
//   auto tokens = lexer.tokenizeAll();
//   // → [KwInt][Identifier "x"][Assign][IntLiteral "42"][Semicolon][Eof]
//
// 外层分派图（nextToken() 每次调用的流程）：
//
//               nextToken()
//                   │
//                   ▼
//      skipWhitespaceAndComments()   ← 循环吃掉空白/注释
//                   │
//                   ▼
//            到文件尾了？ ──是──► 返回 Token{Eof}
//                   │否
//                   ▼
//      按首字符 c 分派（只看第一个字符就决定分支）
//        ├─ isdigit(c)            → scanNumber()              如 42
//        ├─ isalpha(c) || c=='_'  → scanIdentifierOrKeyword() 如 x / int
//        ├─ c=='"'                → scanString()              如 "hi"
//        └─ 其他                  → scanOperator()            如 = <= ->
//                   │
//                   ▼
//          返回一个 Token ──── 下一次调用从停下的位置继续
//
// 全部内部状态只有 4 个成员（源码 + 光标 + 行 + 列），前瞻用只读 peek，
// 不回退、不缓存 token——这是手写词法器最经典的形态。
class Lexer {
public:
    // 构造：接管整份源码（按值传参 + 移动，右值实参可零拷贝）。
    // 示例：Lexer lex("int x = 42;"); → 光标停在文件开头，行=列=1
    explicit Lexer(std::string source);

    // 扫描全部 Token（含末尾 Eof）
    // 一次性接口：main.cpp 用它把整个 Token 流交给 Parser。
    // 示例：Lexer("int x;").tokenizeAll() → [KwInt][Identifier "x"][Semicolon][Eof]
    std::vector<Token> tokenizeAll();

    // 逐个获取下一个 Token（流式接口）
    // DFA 的真正外层循环：每调用一次产出一个 token；源码耗尽后永远返回 Eof（幂等）。
    // 示例：源码 "42;" → 第 1 次 Token{IntLiteral,"42"}，第 2 次 Token{Semicolon}，
    //       第 3 次及以后 Token{Eof}
    Token nextToken();

private:
    // ── 扫描器的四个状态（一趟式词法分析的全部状态）──
    // m_source    ：被扫描的源码（完整持有，peek/peekNext 依赖随机访问）；
    // m_pos       ：扫描光标，指向"下一个待读字符"的下标，只前进不回退
    //               （词法分析是单趟的，理论上也不需要回退——最多前瞻一两个字符）；
    // m_line/m_col：光标对应的行/列，在 advance() 中同步维护，用于生成 SourceLocation。
    // 示例：源码 "ab\nc" —— 消费 3 个字符后 m_pos==3、m_line==2、m_col==1（指向 'c'）。
    std::string m_source;
    size_t      m_pos    = 0;
    uint32_t    m_line   = 1;
    uint32_t    m_col    = 1;

    // ── 字符级操作 ──
    // DFA 的"眼睛和手"：peek/peekNext 只读前瞻（看而不消费，保证可以"反悔"），
    // advance 是唯一的消费动作。越界读取统一返回 '\0'，调用方无需处处判边界。
    // 为什么需要前瞻？因为切词要不断做"再看一个字符"的决定：
    // 是 "==" 还是 "="？是 "//" 注释还是除法？——这正是 [lex.pptoken] 最长匹配的要求。

    // 查看当前字符但不消费。示例：源码 "x+1" → peek()=='x'，再 peek() 仍是 'x'
    char peek() const;
    // 查看当前字符的下一个但不消费（判断 "==", "->", "//" 等双字符结构用）。
    // 示例：源码 "->" → peek()=='-'，peekNext()=='>'
    char peekNext() const;
    // 消费并返回当前字符，光标 +1；遇 '\n' 行号 +1、列号重置为 1，否则列号 +1。
    // 示例：源码 "a\nb" → advance()=='a'；再 advance()=='\n'（此时行号 1→2、列号归 1）
    char advance();
    // 源码是否已读完（光标越过最后一个字符）。示例：空源码 → 构造后立即为 true
    bool isAtEnd() const;

    // ── 跳过空白与注释 ──
    // [lex.comment]：注释不是 token，词法上等价于空白，直接"吃掉"即可。
    // 每次 nextToken() 开头调用，保证产出的 token 之间没有杂质。
    void skipWhitespaceAndComments();

    // ── 扫描各类 Token（DFA 的四大分支）──
    // 统一约定：调用时光标指向该 token 的首字符；返回时光标恰好停在该 token
    // 最后一个字符之后，并返回一个完整 Token。

    // 数字字面量（[lex.icon]，简化为十进制整数）：贪心消费连续数字。
    // 示例："42;" → Token{IntLiteral,"42"}，光标停在 ';'
    Token scanNumber();
    // 标识符/关键字（[lex.name]/[lex.key]）：消费字母/数字/下划线后查 kKeywordMap
    // 决定是关键字还是标识符。
    // 示例："if(x" → Token{KwIf,"if"}，光标停在 '('；"foo " → Token{Identifier,"foo"}
    Token scanIdentifierOrKeyword();
    // 字符串字面量（[lex.string]）：消费到未转义的 '"' 为止，中途翻译转义序列。
    // 示例：源码 6 字符 "hi\n" → Token{StringLiteral, "hi"+换行符}，结尾引号被消费
    Token scanString();
    // 运算符/分隔符（[lex.operators]）：按最长匹配决定取双字符还是单字符。
    // 示例："<=" → Token{LessEqual}；"<x" → Token{Less}，光标停在 'x'
    Token scanOperator();

    // ── 辅助 ──
    // 快照当前 (行, 列)。各扫描函数都在开头先拍快照——
    // 保证 token 的位置指向它的"起始处"而不是结束处。
    SourceLocation currentLocation() const;
    // 统一的 Token 装配入口，避免各扫描函数重复聚合初始化。
    // 示例：makeToken(TokenType::KwInt, "int", {1,1}) → Token{KwInt,"int",{1,1}}
    Token makeToken(TokenType type, std::string text, SourceLocation loc) const;
};

} // namespace minicc
