// =============================================================================
// 阶段 1：词法分析器实现
// =============================================================================
// 这是一个手写的确定性有限自动机（DFA）。
// 核心思想：根据当前字符决定进入哪个"扫描分支"，每个分支负责消费
// 若干字符并产出一个完整 Token。
// =============================================================================

#include "lexer.h"
#include <cctype>
#include <stdexcept>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// tokenTypeName：给每种 Token 一个可读名字
// ─────────────────────────────────────────────────────────────────────────────
const char* tokenTypeName(TokenType t) {
    switch (t) {
        case TokenType::Eof:           return "EOF";
        case TokenType::IntLiteral:    return "IntLiteral";
        case TokenType::StringLiteral: return "StringLiteral";
        case TokenType::Identifier:    return "Identifier";
        case TokenType::KwAuto:        return "auto";
        case TokenType::KwBool:        return "bool";
        case TokenType::KwClass:       return "class";
        case TokenType::KwConst:       return "const";
        case TokenType::KwDouble:      return "double";
        case TokenType::KwElse:        return "else";
        case TokenType::KwFalse:       return "false";
        case TokenType::KwFor:         return "for";
        case TokenType::KwIf:          return "if";
        case TokenType::KwInt:         return "int";
        case TokenType::KwNullptr:     return "nullptr";
        case TokenType::KwOverride:    return "override";
        case TokenType::KwPublic:      return "public";
        case TokenType::KwPrivate:     return "private";
        case TokenType::KwProtected:   return "protected";
        case TokenType::KwReturn:      return "return";
        case TokenType::KwTemplate:    return "template";
        case TokenType::KwThis:        return "this";
        case TokenType::KwTrue:        return "true";
        case TokenType::KwTypename:    return "typename";
        case TokenType::KwVirtual:     return "virtual";
        case TokenType::KwVoid:        return "void";
        case TokenType::KwWhile:       return "while";
        case TokenType::Plus:          return "+";
        case TokenType::Minus:         return "-";
        case TokenType::Star:          return "*";
        case TokenType::Slash:         return "/";
        case TokenType::Percent:       return "%";
        case TokenType::Ampersand:     return "&";
        case TokenType::Pipe:          return "|";
        case TokenType::Caret:         return "^";
        case TokenType::Tilde:         return "~";
        case TokenType::Bang:          return "!";
        case TokenType::Assign:        return "=";
        case TokenType::Less:          return "<";
        case TokenType::Greater:       return ">";
        case TokenType::Dot:           return ".";
        case TokenType::Comma:         return ",";
        case TokenType::Colon:         return ":";
        case TokenType::Semicolon:     return ";";
        case TokenType::Arrow:         return "->";
        case TokenType::ColonColon:    return "::";
        case TokenType::EqualEqual:    return "==";
        case TokenType::BangEqual:     return "!=";
        case TokenType::LessEqual:     return "<=";
        case TokenType::GreaterEqual:  return ">=";
        case TokenType::AmpAmp:        return "&&";
        case TokenType::PipePipe:      return "||";
        case TokenType::PlusAssign:    return "+=";
        case TokenType::MinusAssign:   return "-=";
        case TokenType::StarAssign:    return "*=";
        case TokenType::SlashAssign:   return "/=";
        case TokenType::LParen:        return "(";
        case TokenType::RParen:        return ")";
        case TokenType::LBrace:        return "{";
        case TokenType::RBrace:        return "}";
        case TokenType::LBracket:      return "[";
        case TokenType::RBracket:      return "]";
    }
    return "Unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// 构造函数
// ─────────────────────────────────────────────────────────────────────────────
Lexer::Lexer(std::string source)
    : m_source(std::move(source)) {}

// ─────────────────────────────────────────────────────────────────────────────
// 字符级操作
// ─────────────────────────────────────────────────────────────────────────────
char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return m_source[m_pos];
}

char Lexer::peekNext() const {
    if (m_pos + 1 >= m_source.size()) return '\0';
    return m_source[m_pos + 1];
}

char Lexer::advance() {
    char c = m_source[m_pos++];
    if (c == '\n') {
        m_line++;
        m_col = 1;
    } else {
        m_col++;
    }
    return c;
}

bool Lexer::isAtEnd() const {
    return m_pos >= m_source.size();
}

SourceLocation Lexer::currentLocation() const {
    return {m_line, m_col};
}

Token Lexer::makeToken(TokenType type, std::string text, SourceLocation loc) const {
    return Token{type, std::move(text), loc};
}

// ─────────────────────────────────────────────────────────────────────────────
// 跳过空白和注释
// ─────────────────────────────────────────────────────────────────────────────
// C++ 有两种注释：
//   1. 单行注释：// ...
//   2. 多行注释：/* ... */
// ─────────────────────────────────────────────────────────────────────────────
void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();

        // 空白字符
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // 注释
        if (c == '/') {
            if (peekNext() == '/') {
                // 单行注释：跳到行尾
                advance(); advance(); // 消费 //
                while (!isAtEnd() && peek() != '\n') advance();
                continue;
            }
            if (peekNext() == '*') {
                // 多行注释：找到 */
                advance(); advance(); // 消费 /*
                while (!isAtEnd()) {
                    if (peek() == '*' && peekNext() == '/') {
                        advance(); advance(); // 消费 */
                        break;
                    }
                    advance();
                }
                continue;
            }
        }

        break; // 非空白非注释，停止
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描数字字面量（仅支持整数）
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::scanNumber() {
    auto loc = currentLocation();
    std::string text;

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        text += advance();
    }

    return makeToken(TokenType::IntLiteral, text, loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描标识符或关键字
// ─────────────────────────────────────────────────────────────────────────────
// 核心逻辑：先按标识符规则（字母/数字/下划线）扫描完整单词，
// 然后查关键字表判断是关键字还是普通标识符。
// 这就是 auto/virtual/template 等关键字与普通标识符的区分点。
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::scanIdentifierOrKeyword() {
    auto loc = currentLocation();
    std::string text;

    while (!isAtEnd() && (std::isalnum(static_cast<unsigned char>(peek())) // 字母数字
                          || peek() == '_')) { // 下划线
        text += advance();
    }

    // 查关键字表
    auto it = kKeywordMap.find(text);
    if (it != kKeywordMap.end()) {
        return makeToken(it->second, text, loc);
    }

    return makeToken(TokenType::Identifier, text, loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描字符串字面量
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::scanString() {
    auto loc = currentLocation();
    advance(); // 消费开头的 "

    std::string text;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') { // wangyang 这里是读取到内容是 \(反斜线)的意思
            advance(); // 消费反斜杠
            char escaped = advance();
            switch (escaped) {
                case 'n':  text += '\n'; break;
                case 't':  text += '\t'; break;
                case '\\': text += '\\'; break;
                case '"':  text += '"';  break;
                default:   text += escaped; break;
            }
        } else {
            text += advance();
        }
    }

    if (isAtEnd()) {
        throw std::runtime_error(
            std::format("Unterminated string at {}", loc.toString()));
    }
    advance(); // 消费结尾的 "

    return makeToken(TokenType::StringLiteral, text, loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描运算符和分隔符
// ─────────────────────────────────────────────────────────────────────────────
// 采用"最长匹配"原则：先看两个字符是否能组成运算符，不能则取单字符。
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::scanOperator() {
    auto loc = currentLocation();
    char c = advance();
    char n = peek();

    // 双字符运算符
    switch (c) {
        case '-':
            if (n == '>') { advance(); return makeToken(TokenType::Arrow, "->", loc); }
            if (n == '=') { advance(); return makeToken(TokenType::MinusAssign, "-=", loc); }
            return makeToken(TokenType::Minus, "-", loc);
        case '+':
            if (n == '=') { advance(); return makeToken(TokenType::PlusAssign, "+=", loc); }
            return makeToken(TokenType::Plus, "+", loc);
        case '*':
            if (n == '=') { advance(); return makeToken(TokenType::StarAssign, "*=", loc); }
            return makeToken(TokenType::Star, "*", loc);
        case '/':
            if (n == '=') { advance(); return makeToken(TokenType::SlashAssign, "/=", loc); }
            return makeToken(TokenType::Slash, "/", loc);
        case '=':
            if (n == '=') { advance(); return makeToken(TokenType::EqualEqual, "==", loc); }
            return makeToken(TokenType::Assign, "=", loc);
        case '!':
            if (n == '=') { advance(); return makeToken(TokenType::BangEqual, "!=", loc); }
            return makeToken(TokenType::Bang, "!", loc);
        case '<':
            if (n == '=') { advance(); return makeToken(TokenType::LessEqual, "<=", loc); }
            return makeToken(TokenType::Less, "<", loc);
        case '>':
            if (n == '=') { advance(); return makeToken(TokenType::GreaterEqual, ">=", loc); }
            return makeToken(TokenType::Greater, ">", loc);
        case '&':
            if (n == '&') { advance(); return makeToken(TokenType::AmpAmp, "&&", loc); }
            return makeToken(TokenType::Ampersand, "&", loc);
        case '|':
            if (n == '|') { advance(); return makeToken(TokenType::PipePipe, "||", loc); }
            return makeToken(TokenType::Pipe, "|", loc);
        case ':':
            if (n == ':') { advance(); return makeToken(TokenType::ColonColon, "::", loc); }
            return makeToken(TokenType::Colon, ":", loc);
        default:
            break;
    }

    // 单字符运算符/分隔符
    switch (c) {
        case '%': return makeToken(TokenType::Percent, "%", loc);
        case '^': return makeToken(TokenType::Caret, "^", loc);
        case '~': return makeToken(TokenType::Tilde, "~", loc);
        case '.': return makeToken(TokenType::Dot, ".", loc);
        case ',': return makeToken(TokenType::Comma, ",", loc);
        case ';': return makeToken(TokenType::Semicolon, ";", loc);
        case '(': return makeToken(TokenType::LParen, "(", loc);
        case ')': return makeToken(TokenType::RParen, ")", loc);
        case '{': return makeToken(TokenType::LBrace, "{", loc);
        case '}': return makeToken(TokenType::RBrace, "}", loc);
        case '[': return makeToken(TokenType::LBracket, "[", loc);
        case ']': return makeToken(TokenType::RBracket, "]", loc);
        default: break;
    }

    throw std::runtime_error(
        std::format("Unexpected character '{}' at {}", c, loc.toString()));
}

// ─────────────────────────────────────────────────────────────────────────────
// nextToken：词法分析的核心入口
// ─────────────────────────────────────────────────────────────────────────────
// 这是一个简单的"调度器"：根据当前字符的类型，分派到对应的扫描函数。
// 整个流程可以看作一个确定性有限自动机（DFA）的外层循环。
// ─────────────────────────────────────────────────────────────────────────────
Token Lexer::nextToken() {
    skipWhitespaceAndComments(); //跳过空白符和注释

    if (isAtEnd()) {
        return makeToken(TokenType::Eof, "", currentLocation());
    }

    char c = peek();

    // 数字开头 → 数字字面量
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return scanNumber();
    }

    // 字母或下划线开头 → 标识符或关键字
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return scanIdentifierOrKeyword();
    }

    // 双引号开头 → 字符串字面量
    if (c == '"') {
        return scanString();
    }

    // 其他 → 运算符或分隔符
    return scanOperator();
}

// ─────────────────────────────────────────────────────────────────────────────
// tokenizeAll：一次性扫描所有 Token
// ─────────────────────────────────────────────────────────────────────────────
std::vector<Token> Lexer::tokenizeAll() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = nextToken();
        tokens.push_back(tok);
        if (tok.is(TokenType::Eof)) break;
    }
    return tokens;
}

} // namespace minicc
