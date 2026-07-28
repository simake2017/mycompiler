#pragma once
// =============================================================================
// 阶段 1：词法分析器 (Lexer / Scanner)
// =============================================================================
// 核心职责：将源码字符串逐字符扫描，输出 Token 流。
// 实现方式：手写的确定性有限自动机（DFA），每个字符根据当前状态决定转移。
// =============================================================================

#include "token.h"
#include <string>
#include <string_view>
#include <vector>

namespace minicc {

class Lexer {
public:
    explicit Lexer(std::string source);

    // 扫描全部 Token（含末尾 Eof）
    std::vector<Token> tokenizeAll();

    // 逐个获取下一个 Token（流式接口）
    Token nextToken();

private:
    std::string m_source;
    size_t      m_pos    = 0;
    uint32_t    m_line   = 1;
    uint32_t    m_col    = 1;

    // ── 字符级操作 ──
    char peek() const;
    char peekNext() const;
    char advance();
    bool isAtEnd() const;

    // ── 跳过空白与注释 ──
    void skipWhitespaceAndComments();

    // ── 扫描各类 Token ──
    Token scanNumber();
    Token scanIdentifierOrKeyword();
    Token scanString();
    Token scanOperator();

    // ── 辅助 ──
    SourceLocation currentLocation() const;
    Token makeToken(TokenType type, std::string text, SourceLocation loc) const;
};

} // namespace minicc
