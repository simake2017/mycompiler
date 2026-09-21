#pragma once
// =============================================================================
// include/token.h —— 阶段 1：词法分析的产物（Token 定义）
// =============================================================================
// 管线位置：源码(.cpp) → Preprocessor → ★Lexer★ → Parser → Sema → … → CodeGen(.s)
//   本文件定义 Lexer 交给 Parser 的"数据格式"：Parser 从此不再看见字符，只看见 Token。
//   Token 是流水线上第一个"零件" —— 带类型标签与位置信息的最小语法单元。
//
// 标准章节  [lex.phases] 阶段 3/7（切词）│ [lex.token] 五类终结符 │ [lex.pptoken]
// 对应 clang Basic/TokenKinds.def（tok:: 大枚举 ／ 本项目 TokenType）
//            Basic/Token.h（种类+长度+位置 ／ 本项目 Token 结构）
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <format>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// TokenType：所有可能的 Token 种类 —— 文法"终结符集合"(terminals) 的扁平枚举
// ─────────────────────────────────────────────────────────────────────────────
// 底层 uint8_t：种类不足 256，一字节足够；Token 量大且随流传递，尺寸小对缓存友好。
// 顺序有语义：Eof 居首（所有循环以它收尾），其后按
//   字面量 → 标识符 → 关键字 → 运算符 → 分隔符 分组。
// clang 对应 tok:: 命名空间 + TokenKinds.def 宏展开的大枚举，思路相同、种类更多。
//
// demo: `int x = 42;` ⇒ [KwInt][Identifier "x"][Assign "="][IntLiteral "42"][Semicolon][Eof]
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
    KwDecltype,       // decltype
    KwDelete,         // delete
    KwDouble,         // double
    KwDynamicCast,    // dynamic_cast
    KwElse,           // else
    KwEnum,           // enum
    KwFalse,          // false
    KwFor,            // for
    KwIf,             // if
    KwInt,            // int
    KwNamespace,      // namespace
    KwNew,            // new
    KwNullptr,        // nullptr
    KwOverride,       // override
    KwPublic,         // public
    KwPrivate,        // private
    KwProtected,      // protected
    KwReturn,         // return
    KwStruct,         // struct
    KwTemplate,       // template
    KwThis,           // this
    KwTrue,           // true
    KwTypedef,        // typedef
    KwTypename,       // typename
    KwUsing,          // using
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
// 关键字字符串 → TokenType 的映射表（关键字清单由标准 [lex.key] 规定）
// ─────────────────────────────────────────────────────────────────────────────
// 两步法：先按标识符规则扫出完整单词，再查这张表 —— 命中即关键字，未中即 Identifier。
// clang 同思路（扫出标识符再查 IdentifierTable 完美哈希）。好处：不必为每个关键字
// 写状态机，新增关键字只需在这里加一行。
// demo: "template" ⇒ KwTemplate；"templata"（未命中）⇒ Identifier
// inline const：头文件内定义、多个翻译单元共享且只构建一次。
inline const std::unordered_map<std::string_view, TokenType> kKeywordMap = {
    {"auto",      TokenType::KwAuto},
    {"bool",      TokenType::KwBool},
    {"class",     TokenType::KwClass},
    {"const",     TokenType::KwConst},
    {"decltype",  TokenType::KwDecltype},
    {"delete",    TokenType::KwDelete},
    {"double",    TokenType::KwDouble},
    {"dynamic_cast", TokenType::KwDynamicCast},
    {"else",      TokenType::KwElse},
    {"enum",      TokenType::KwEnum},
    {"false",     TokenType::KwFalse},
    {"for",       TokenType::KwFor},
    {"if",        TokenType::KwIf},
    {"int",       TokenType::KwInt},
    {"namespace", TokenType::KwNamespace},
    {"new",       TokenType::KwNew},
    {"nullptr",   TokenType::KwNullptr},
    {"override",  TokenType::KwOverride},
    {"public",    TokenType::KwPublic},
    {"private",   TokenType::KwPrivate},
    {"protected", TokenType::KwProtected},
    {"return",    TokenType::KwReturn},
    {"struct",    TokenType::KwStruct},
    {"template",  TokenType::KwTemplate},
    {"this",      TokenType::KwThis},
    {"true",      TokenType::KwTrue},
    {"typedef",   TokenType::KwTypedef},
    {"typename",  TokenType::KwTypename},
    {"using",     TokenType::KwUsing},
    {"virtual",   TokenType::KwVirtual},
    {"void",      TokenType::KwVoid},
    {"while",     TokenType::KwWhile},
};

// ─────────────────────────────────────────────────────────────────────────────
// SourceLocation：源码中的精确位置，用于报错
// ─────────────────────────────────────────────────────────────────────────────
// Token 流是"破坏性"的 —— 切成 token 后原始行列号就丢了，故每个 token 自带位置快照，
// 供后续诊断（如 Sema 报"3:7 处变量未声明"）。
// clang 是 FileID + 偏移的编码（SourceLocation）；本项目简化为 (行, 列)，均从 1 开始。
// demo: 首行 "int x" 中 token x 的位置是 {1,5}；SourceLocation{12,5}.toString() ⇒ "12:5"
struct SourceLocation {
    uint32_t line   = 1;
    uint32_t column = 1;

    // 格式化为 "行:列"，直接拼进诊断消息。demo: {12,5}.toString() ⇒ "12:5"
    std::string toString() const {
        return std::format("{}:{}", line, column);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Token：词法分析的最小产物 = 类型标签 + 原始文本 + 位置
// ─────────────────────────────────────────────────────────────────────────────
// 不存字面量的"数值"：把 "42" 换算成 int 是 Parser/Sema 的事（[lex.token] 的职责边界）。
// text 用 std::string 值语义，避免 string_view 悬空到源码生命周期
//（clang::Token 只存偏移+长度、按需回查源码文本，本项目为教学选择直接拷贝）。
//
// demo: `int x = 42;` ⇒ Token{KwInt,"int",{1,1}} {Identifier,"x",{1,5}} {Assign,"=",{1,7}}
//                       {IntLiteral,"42",{1,9}} {Semicolon,";",{1,11}} {Eof,"",{1,12}}
struct Token {
    TokenType     type     = TokenType::Eof;
    std::string   text;            // 原始文本（如标识符名、数字文本）
    SourceLocation location;

    // 种类判断 / 取反判断（Parser 中高频使用的快捷方法）。
    // demo: tok.is(TokenType::KwIf) ⇒ true；isNot 让循环条件更易读：
    //       while (tok.isNot(TokenType::Eof)) {...}
    bool is(TokenType t) const { return type == t; }
    bool isNot(TokenType t) const { return type != t; }

    // 判断是否为类型关键字（int, double, bool, void, auto, const, decltype）
    // Parser 识别声明（"类型 + 名字"）时用。
    // demo: Token{KwInt}.isTypeKeyword() ⇒ true；Token{KwClass} ⇒ false
    //       （class 引入的是类定义而非内置类型名，故不在此列）
    // ★ KwConst 必须在此列：const 是【类型说明符】的开头（[dcl.type]：
    //   type-specifier-seq 可为 `const` + 类型），`const int x = 1;` 是一条正经的声明。
    //   ⚠ 顶层声明走的是"试探性 parseType + 回滚"、不看本函数 —— 同一语义在两条
    //   路径上判定，改这里（或改那条前瞻）时必须两边一起核对。
    bool isTypeKeyword() const {
        return type == TokenType::KwInt
            || type == TokenType::KwDouble
            || type == TokenType::KwBool
            || type == TokenType::KwVoid
            || type == TokenType::KwAuto
            || type == TokenType::KwConst      // const int x; —— [dcl.type] 类型说明符
            || type == TokenType::KwDecltype;   // decltype(e) 也是类型说明符 [dcl.type.decltype]
    }

    // 判断是否为访问修饰符（Parser 解析类体中 "public/private/protected :" 成员区段时用）
    // demo: Token{KwPublic}.isAccessSpecifier() ⇒ true；Token{KwClass} ⇒ false
    bool isAccessSpecifier() const {
        return type == TokenType::KwPublic
            || type == TokenType::KwPrivate
            || type == TokenType::KwProtected;
    }

    // 调试输出：[类型枚举数值] '原始文本' @ 行:列
    // demo: Token{KwInt,"int",{1,1}}.toString() ⇒ "[13] 'int' @ 1:1"
    //       （数值即枚举序号，改枚举顺序会让这条输出整体漂移）
    std::string toString() const {
        return std::format("[{}] '{}' @ {}",
            static_cast<int>(type), text, location.toString());
    }
};

// Token 类型的可读名称（用于错误消息）。实现见 src/lexer.cpp 的大 switch 表，
// 与枚举一一对应 —— 新增种类时两边都要补。
// demo: tokenTypeName(KwInt) ⇒ "int"；tokenTypeName(Arrow) ⇒ "->"
// 约定：关键字/运算符返回"源码形态"，字面量/标识符返回"类别名"，
// 便于 Sema 直接拼出类似 "期望 ';' 但得到 '}'" 的可读报错。
const char* tokenTypeName(TokenType t);

} // namespace minicc
