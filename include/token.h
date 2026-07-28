#pragma once
// =============================================================================
// 阶段 1：词法分析 —— Token 定义
// =============================================================================
// Token 是编译器流水线中第一个被生产出来的"零件"。
// 源码字符串被 Lexer 切割成一个个带有类型标签和位置信息的最小语法单元。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <format>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// TokenType：所有可能的 Token 种类
// ─────────────────────────────────────────────────────────────────────────────
enum class TokenType : uint8_t {
    // ── 文件结束 ──
    Eof,

    // ── 字面量 ──
    IntLiteral,       // 42
    StringLiteral,    // "hello"

    // ── 标识符 ──
    Identifier,       // myVar, foo_bar

    // ── 关键字 ──
    KwAuto,           // auto
    KwBool,           // bool
    KwClass,          // class
    KwConst,          // const
    KwDouble,         // double
    KwElse,           // else
    KwFalse,          // false
    KwFor,            // for
    KwIf,             // if
    KwInt,            // int
    KwNullptr,        // nullptr
    KwOverride,       // override
    KwPublic,         // public
    KwPrivate,        // private
    KwProtected,      // protected
    KwReturn,         // return
    KwTemplate,       // template
    KwThis,           // this
    KwTrue,           // true
    KwTypename,       // typename
    KwVirtual,        // virtual
    KwVoid,           // void
    KwWhile,          // while

    // ── 运算符 ──
    Plus,             // +
    Minus,            // -
    Star,             // *
    Slash,            // /
    Percent,          // %
    Ampersand,        // &
    Pipe,             // |
    Caret,            // ^
    Tilde,            // ~
    Bang,             // !
    Assign,           // =
    Less,             // <
    Greater,          // >
    Dot,              // .
    Comma,            // ,
    Colon,            // :
    Semicolon,        // ;
    Arrow,            // ->
    ColonColon,       // ::
    EqualEqual,       // ==
    BangEqual,        // !=
    LessEqual,        // <=
    GreaterEqual,     // >=
    AmpAmp,           // &&
    PipePipe,         // ||
    PlusAssign,       // +=
    MinusAssign,      // -=
    StarAssign,       // *=
    SlashAssign,      // /=

    // ── 分隔符 ──
    LParen,           // (
    RParen,           // )
    LBrace,           // {
    RBrace,           // }
    LBracket,         // [
    RBracket,         // ]
};

// ─────────────────────────────────────────────────────────────────────────────
// 关键字字符串 → TokenType 的映射表（Lexer 初始化时构建一次）
// ─────────────────────────────────────────────────────────────────────────────
inline const std::unordered_map<std::string_view, TokenType> kKeywordMap = {
    {"auto",      TokenType::KwAuto},
    {"bool",      TokenType::KwBool},
    {"class",     TokenType::KwClass},
    {"const",     TokenType::KwConst},
    {"double",    TokenType::KwDouble},
    {"else",      TokenType::KwElse},
    {"false",     TokenType::KwFalse},
    {"for",       TokenType::KwFor},
    {"if",        TokenType::KwIf},
    {"int",       TokenType::KwInt},
    {"nullptr",   TokenType::KwNullptr},
    {"override",  TokenType::KwOverride},
    {"public",    TokenType::KwPublic},
    {"private",   TokenType::KwPrivate},
    {"protected", TokenType::KwProtected},
    {"return",    TokenType::KwReturn},
    {"template",  TokenType::KwTemplate},
    {"this",      TokenType::KwThis},
    {"true",      TokenType::KwTrue},
    {"typename",  TokenType::KwTypename},
    {"virtual",   TokenType::KwVirtual},
    {"void",      TokenType::KwVoid},
    {"while",     TokenType::KwWhile},
};

// ─────────────────────────────────────────────────────────────────────────────
// SourceLocation：源码中的精确位置，用于报错
// ─────────────────────────────────────────────────────────────────────────────
struct SourceLocation {
    uint32_t line   = 1;
    uint32_t column = 1;

    std::string toString() const {
        return std::format("{}:{}", line, column);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Token：词法分析的最小产物
// ─────────────────────────────────────────────────────────────────────────────
struct Token {
    TokenType     type     = TokenType::Eof;
    std::string   text;            // 原始文本（如标识符名、数字文本）
    SourceLocation location;

    bool is(TokenType t) const { return type == t; }
    bool isNot(TokenType t) const { return type != t; }

    // 判断是否为类型关键字（int, double, bool, void, auto）
    bool isTypeKeyword() const {
        return type == TokenType::KwInt
            || type == TokenType::KwDouble
            || type == TokenType::KwBool
            || type == TokenType::KwVoid
            || type == TokenType::KwAuto;
    }

    // 判断是否为访问修饰符
    bool isAccessSpecifier() const {
        return type == TokenType::KwPublic
            || type == TokenType::KwPrivate
            || type == TokenType::KwProtected;
    }

    std::string toString() const {
        return std::format("[{}] '{}' @ {}",
            static_cast<int>(type), text, location.toString());
    }
};

// Token 类型的可读名称（用于错误消息）
const char* tokenTypeName(TokenType t);

} // namespace minicc
