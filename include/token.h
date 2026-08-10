#pragma once
// =============================================================================
// 阶段 1：词法分析 —— Token 定义
// =============================================================================
// Token 是编译器流水线中第一个被生产出来的"零件"。
// 源码字符串被 Lexer 切割成一个个带有类型标签和位置信息的最小语法单元。
// =============================================================================
// 【管线位置】
//   源码(.cpp) → [Preprocessor] → [Lexer] → [Parser] → [Sema] → [模板推导/实例化] → [CodeGen](.s)
//                                └── 本文件定义 Lexer 交给 Parser 的"数据格式"：
//                                    Parser 从此不再看见字符，只看见这里定义的 Token。
//
// 【对应 C++ 标准章节】
//   [lex.phases]  —— 翻译阶段：词法分析对应阶段 3（把源码分解为预处理 token）
//                    与阶段 7（预处理完成后的正式词法分析）；
//   [lex.token]   —— token 的定义：语言的最小词法单元，分五类——标识符、关键字、
//                    字面量、运算符、分隔符（下面的 TokenType 就是这五类的扁平枚举）；
//   [lex.pptoken] —— 预处理 token（预处理阶段的形态），本项目为简化，预处理后仍复用
//                    同一套 Token 表示。
//
// 【对应 clang 模块】
//   include/clang/Basic/TokenKinds.def —— 全部 token 种类定义（tok::kw_int、tok::equal…），
//                                         对应此处的 TokenType（本项目大幅精简）；
//   include/clang/Basic/Token.h        —— clang::Token（种类/长度/位置 + 字面量数据），
//                                         对应此处的 Token 结构。
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
// 设计理由：
//   1. 用"扁平枚举"列全所有 token 种类——词法理论上这叫文法的"终结符集合"
//      (terminals)。Parser 的每条文法规则都建立在这些终结符之上。
//      （clang 对应 tok:: 命名空间 + TokenKinds.def 宏展开的大枚举，思路相同、种类更多。）
//   2. 底层类型 uint8_t：种类不超过 256 个，一字节足够；Token 数量巨大、随流传递，
//      尺寸小对缓存友好。
//   3. 排列顺序有语义：Eof 放第一个（所有循环都以它收尾），其后按
//      字面量 → 标识符 → 关键字 → 运算符 → 分隔符 分组，便于阅读与维护。
//
// 整体示例（感受一次"切词"）：
//   源码：  int x = 42;
//   Token 流：[KwInt] [Identifier "x"] [Assign "="] [IntLiteral "42"] [Semicolon ";"] [Eof]
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
// 设计理由（关键字清单由标准 [lex.key] 规定）：
//   Lexer 扫描时不"预判"关键字——先按标识符规则扫出完整单词，再查这张表：
//   命中 → 关键字 token，未命中 → 普通标识符。这是经典两步法
//   （clang 同样是先扫出标识符再查 IdentifierTable 的完美哈希表），
//   好处：不必为每个关键字写状态机；新增关键字只需在这里加一行。
//   示例："template" → TokenType::KwTemplate；"templata"（未命中）→ TokenType::Identifier。
//   inline const：头文件内定义、多个翻译单元共享且只构建一次。
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
// 设计理由：
//   Token 流是"破坏性"的——源码切成 token 后原始行号列号就丢了，
//   所以每个 token 必须自带位置快照，供后续诊断（如 Sema 报"3:7 处变量未声明"）。
//   clang 对应 SourceLocation（FileID + 偏移的编码），本项目简化为 (行, 列) 二元组，
//   行列都从 1 开始（与编辑器显示一致）。
//   示例：源码第一行 "int x" 中，token x 的位置是 {1, 5}。
struct SourceLocation {
    uint32_t line   = 1;
    uint32_t column = 1;

    // 格式化为 "行:列"（如 "3:7"），直接拼进诊断消息。
    // 示例：SourceLocation{12, 5}.toString() → "12:5"
    std::string toString() const {
        return std::format("{}:{}", line, column);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Token：词法分析的最小产物
// ─────────────────────────────────────────────────────────────────────────────
// 设计理由：
//   Token = 类型标签 + 原始文本 + 位置，最小三元组。
//   注意：这里不存字面量的"数值"——把 "42" 换算成 int 是 Parser/Sema 的事，
//   词法分析只负责"切词"，这正是 [lex.token] 的职责边界。
//   text 用 std::string 值语义：简单直接，避免 string_view 悬空到源码生命周期的问题
//   （clang::Token 只存偏移+长度、按需回查源码文本，本项目为教学选择直接拷贝）。
//
// 完整示例：源码 `int x = 42;` →
//   Token{KwInt,      "int", {1,1}}
//   Token{Identifier, "x",   {1,5}}
//   Token{Assign,     "=",   {1,7}}
//   Token{IntLiteral, "42",  {1,9}}
//   Token{Semicolon,  ";",   {1,11}}
//   Token{Eof,        "",    {1,12}}
struct Token {
    TokenType     type     = TokenType::Eof;
    std::string   text;            // 原始文本（如标识符名、数字文本）
    SourceLocation location;

    // 种类判断 / 取反判断（Parser 中高频使用的快捷方法）。
    // 示例：Token{KwIf,...}.is(TokenType::KwIf) → true；
    //       isNot 让循环条件更易读：while (tok.isNot(TokenType::Eof)) {...}
    bool is(TokenType t) const { return type == t; }
    bool isNot(TokenType t) const { return type != t; }

    // 判断是否为类型关键字（int, double, bool, void, auto）
    // Parser 识别声明（"类型 + 名字"）时用。
    // 示例：Token{KwInt}.isTypeKeyword() → true；Token{KwClass} → false
    //       （class 引入的是类定义而非内置类型名，故不在此列）
    bool isTypeKeyword() const {
        return type == TokenType::KwInt
            || type == TokenType::KwDouble
            || type == TokenType::KwBool
            || type == TokenType::KwVoid
            || type == TokenType::KwAuto;
    }

    // 判断是否为访问修饰符
    // Parser 解析类体中 "public/private/protected :" 成员区段时用。
    // 示例：Token{KwPublic}.isAccessSpecifier() → true；Token{KwClass} → false
    bool isAccessSpecifier() const {
        return type == TokenType::KwPublic
            || type == TokenType::KwPrivate
            || type == TokenType::KwProtected;
    }

    // 调试输出：[类型枚举数值] '原始文本' @ 行:列
    // 示例：Token{KwInt,"int",{1,1}}.toString() → "[13] 'int' @ 1:1"
    //       （KwInt 在枚举中排第 13，注意调试时对照枚举顺序读数值的含义）
    std::string toString() const {
        return std::format("[{}] '{}' @ {}",
            static_cast<int>(type), text, location.toString());
    }
};

// Token 类型的可读名称（用于错误消息）
// 示例：tokenTypeName(TokenType::KwInt) → "int"；tokenTypeName(TokenType::Arrow) → "->"
// 实现见 src/lexer.cpp 中的大 switch 表，与枚举一一对应（新增种类时两边都要补）。
const char* tokenTypeName(TokenType t);

} // namespace minicc
