// =============================================================================
// src/lexer.cpp —— 阶段 1：词法分析器实现（手写 DFA）
// =============================================================================
// 核心思想：根据当前字符决定进入哪个"扫描分支"，每个分支消费若干字符、产出一个 Token。
//
// 管线位置  源码 → Preprocessor → ★Lexer★ → Parser → Sema → 模板推导/实例化 → CodeGen
// 标准章节  [lex.phases]（翻译阶段）│ [lex.token] │ [lex.pptoken]（最长匹配的注记在此）
//           [lex.name] [lex.icon] [lex.string] [lex.operators]
// 对照 clang：Lexer::LexTokenInternal —— 本文件 nextToken() 是它的教学简化版。
//
// demo: "int x = 42;" ⇒ [KwInt][Identifier "x"][Assign "="][IntLiteral "42"][Semicolon][Eof]
// 本文件结构
//   tokenTypeName()             —— 枚举 → 可读名（供诊断消息）
//   字符级操作                    peek/peekNext/advance/isAtEnd/currentLocation/makeToken
//   skipWhitespaceAndComments() —— 空白 + // 注释 + /* 注释 */
//   四大扫描分支                  scanNumber / scanIdentifierOrKeyword / scanString / scanOperator
//   调度入口                      nextToken（按首字符分派）/ tokenizeAll（批量扫描）
// =============================================================================

#include "lexer.h"
#include <cctype>
#include <stdexcept>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// tokenTypeName：给每种 Token 一个可读名字
// ─────────────────────────────────────────────────────────────────────────────
// demo: KwInt ⇒ "int"；Arrow ⇒ "->"；Eof ⇒ "EOF"；IntLiteral ⇒ "IntLiteral"
// 约定：关键字/运算符返回"源码形态"，字面量/标识符返回"类别名"，
// 这样 Sema 能直接拼出类似 "期望 ';' 但得到 '}'" 的可读报错。
// ⚠ switch 必须与 token.h 的枚举一一对应，新增种类时两边同步。
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
        case TokenType::KwDecltype:    return "decltype";
        case TokenType::KwDelete:      return "delete";
        case TokenType::KwDouble:      return "double";
        case TokenType::KwDynamicCast: return "dynamic_cast";
        case TokenType::KwElse:        return "else";
        case TokenType::KwEnum:        return "enum";
        case TokenType::KwFalse:       return "false";
        case TokenType::KwFor:         return "for";
        case TokenType::KwIf:          return "if";
        case TokenType::KwInt:         return "int";
        case TokenType::KwNamespace:   return "namespace";
        case TokenType::KwNew:         return "new";
        case TokenType::KwNullptr:     return "nullptr";
        case TokenType::KwOverride:    return "override";
        case TokenType::KwPublic:      return "public";
        case TokenType::KwPrivate:     return "private";
        case TokenType::KwProtected:   return "protected";
        case TokenType::KwReturn:      return "return";
        case TokenType::KwStatic:      return "static";
        case TokenType::KwStruct:      return "struct";
        case TokenType::KwTemplate:    return "template";
        case TokenType::KwThis:        return "this";
        case TokenType::KwTrue:        return "true";
        case TokenType::KwTypedef:     return "typedef";
        case TokenType::KwTypename:    return "typename";
        case TokenType::KwUsing:       return "using";
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
// 按值传参 + std::move：右值实参（如临时 string）零拷贝移动进来，左值最多拷贝一次
// —— 一个签名覆盖两种情况。
// demo: Lexer lex("int x = 42;"); ⇒ m_source 持有该串，m_pos=0，m_line=m_col=1
Lexer::Lexer(std::string source)
    : m_source(std::move(source)) {}

// ─────────────────────────────────────────────────────────────────────────────
// 字符级操作
// ─────────────────────────────────────────────────────────────────────────────
// 手写词法器三件套：只读前瞻（peek/peekNext）+ 消费前进（advance）。
// 前瞻越界统一返回 '\0'，各分支因此可以放心写 peek()=='x' 而不用先判边界。

// 查看当前字符但不消费（状态不变，可反复调用）。
// demo: "x+1" ⇒ peek()=='x'，再 peek() 仍是 'x'
char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return m_source[m_pos];
}

// 前瞻一个字符：用于判断双字符结构（"==", "->", "//", "/*"）。
// demo: "->" ⇒ peek()=='-'，peekNext()=='>'；只剩 1 个字符时返回 '\0'
char Lexer::peekNext() const {
    if (m_pos + 1 >= m_source.size()) return '\0';
    return m_source[m_pos + 1];
}

// 全词法器唯一的"消费"动作：返回当前字符，光标 +1，并维护行/列计数
//（遇 '\n' → 行号 +1、列号归 1；否则列号 +1）。所有 SourceLocation 都由这里积累而来。
// demo: "a\nb" ⇒ advance()=='a'；再 advance()=='\n'（行 1→2、列归 1）
char Lexer::advance() {
    char c = m_source[m_pos++];
    if (c == '\n') { // 在这里面会标识行号和  和 列号
        m_line++;
        m_col = 1;
    } else {
        m_col++;
    }
    return c;
}

// 光标是否已越过最后一个字符（用 >=：消费完最后一个字符后再调一次也安全）。
// demo: 空源码 ⇒ 构造后立即 isAtEnd()==true，nextToken() 直接返回 Eof
bool Lexer::isAtEnd() const {
    return m_pos >= m_source.size(); // 也就是说 是字符文件
}

// 快照当前 (行, 列)。每个扫描函数都在开头先拍快照 ——
// 若等扫完再取，位置会指向 token 之后的字符，报错定位就错了。
SourceLocation Lexer::currentLocation() const {
    return {m_line, m_col};
}

// 统一的 Token 装配入口：聚合初始化 + text 移动，各扫描函数共用。
// demo: makeToken(KwInt, "int", {1,1}) ⇒ Token{KwInt,"int",{1,1}}
Token Lexer::makeToken(TokenType type, std::string text, SourceLocation loc) const {
    return Token{type, std::move(text), loc};
}

// ─────────────────────────────────────────────────────────────────────────────
// 跳过空白和注释（[lex.comment]：注释不是 token，词法上等价一个空格 ⇒ 直接吃掉）
// ─────────────────────────────────────────────────────────────────────────────
// 扫到的字符 ⇒ 动作（不产 Token，连占位 token 都不生成）：
//   ` ` `\t` `\r` `\n` ⇒ advance() 后继续循环
//   `//`               ⇒ 跳到行尾（消费 `//`，再吞到 '\n' 或文件尾）
//   `/*`               ⇒ 吞到 `*/`（★ 直到文件尾仍未见 `*/` ⇒ 当作"注释延续到文件末尾"，
//                         不报错；clang 会报 unterminated comment，属可接受的简化）
//   其他               ⇒ break，交回 nextToken() 分派
// 用 while 而非 if：一个 token 前可能交替出现"空白 //注释 /*注释*/ 空白…"，
// 必须反复跳，直到撞上真正的 token 首字符或文件尾。
// demo: "  // hi\n/*c*/ 42" ⇒ 调用后光标指向 '4'
void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();

        // 空白字符
        // （先转 unsigned char 再传给 isspace：char 直接传负值是未定义行为）
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
                // 注：直到文件尾都没找到 */ 时，简化处理为"注释延续到文件末尾"，
                //     不报错（clang 会报 unterminated comment，属于可接受的简化）。
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
// 状态机（[lex.icon] 的极简版）：
//   起始 ──数字──► [数字态] ──数字──► [数字态] ──非数字/EOF──► 返回 IntLiteral
// 扫到的字符 ⇒ 动作：
//   `0`~`9`  ⇒ advance() 收进 text，继续贪心（这是数字分支内部的"最长匹配"）
//   其他/EOF ⇒ 停，产 Token{IntLiteral, text}
// demo: "42;" ⇒ Token{IntLiteral,"42"}，光标停在 ';'（贪心保证 42 不会被切成 4 和 2）
// 省略：十六进制/八进制/二进制前缀、浮点、后缀（u/l/f…）、数字分隔符 '（教学重点是状态机形态本身）
Token Lexer::scanNumber() {
    // 先拍位置快照：token 位置指向第一个数字（若扫完再取就指向后面的字符了）
    auto loc = currentLocation();
    std::string text;

    // 贪心消费连续数字——数字分支内部的"最长匹配"：
    // 见数字就吃，直到第一个非数字字符才停，保证 42 不会被切成 4 和 2。
    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        text += advance();
    }

    return makeToken(TokenType::IntLiteral, text, loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描标识符或关键字：先按标识符规则扫完整单词，再查关键字表 kKeywordMap ——
// auto/virtual/template 等关键字与普通标识符的区分点就在这里。
// 状态机（[lex.name]：首字符字母/下划线，后续字母/数字/下划线）：
//   起始 ──字母/_──► [词体态] ──字母/数字/_──► [词体态] ──其他字符──► 查 kKeywordMap
// 扫到的字符 ⇒ 动作：
//   字母 / `_`   ⇒ advance() 收进 text（`_` 可作首字符）
//   数字         ⇒ advance() 收进 text（数字可作续字符，但不能开头）
//   其他 / EOF   ⇒ 停，用 text 查 kKeywordMap 分派：
//                    ├─ 命中 "if"     ⇒ Token{KwIf, "if"}
//                    ├─ 命中 "return" ⇒ Token{KwReturn, "return"}
//                    └─ 未中 "foo2"   ⇒ Token{Identifier, "foo2"}
// 好处：新增关键字只改 kKeywordMap 一行、状态机不动。对照 clang：IdentifierTable 哈希查表。
// demo: "if(x" ⇒ Token{KwIf,"if"}；"foo2 " ⇒ Token{Identifier,"foo2"}
Token Lexer::scanIdentifierOrKeyword() {
    auto loc = currentLocation();
    std::string text;

    while (!isAtEnd() && (std::isalnum(static_cast<unsigned char>(peek())) // 字母数字
                          || peek() == '_')) { // 下划线
        text += advance();
    }

    // 查关键字表
    auto it = kKeywordMap.find(text); // 判断是不是关键词
    if (it != kKeywordMap.end()) {
        return makeToken(it->second, text, loc);
    }

    return makeToken(TokenType::Identifier, text, loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描字符串字面量
// ─────────────────────────────────────────────────────────────────────────────
// [lex.string]：字符串字面量由 '"' 开始，到下一个未被转义的 '"' 结束。
// 转义处理：读到 '\' → 再读一个字符，按下表翻译；未知转义原样保留（简化处理）。
// ★ text 存的是"转义翻译后的值"而非原始源码文本 —— CodeGen 因此可直接把内容
//   嵌入 .rodata 段。
// demo: 源码 "hi\n" ⇒ Token{StringLiteral, text == "hi"+换行符}，结尾引号被消费
Token Lexer::scanString() {
    auto loc = currentLocation();
    advance(); // 消费开头的 " 只是往前推进，但是不添加

    std::string text;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance(); // 消费反斜杠：只推进游标，不并入 text
            char escaped = advance();
            // 转义翻译表：n→换行  t→制表符  \\→反斜杠  \"→双引号；
            // default 原样保留（真实 C++ 对未知转义报错，本项目宽容处理）
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

    // 走到文件尾仍未见闭引号 → 报错（[lex.string] 要求字符串在同一行内终结）
    // 示例：源码 `"abc`（缺闭引号）→ 抛 "Unterminated string at 1:1"
    if (isAtEnd()) {
        throw std::runtime_error(
            std::format("Unterminated string at {}", loc.toString()));
    }
    advance(); // 消费结尾的 "

    return makeToken(TokenType::StringLiteral, text, loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 扫描运算符和分隔符：★最长匹配（maximal munch，[lex.pptoken] 注）
// ─────────────────────────────────────────────────────────────────────────────
// 最长匹配原则："若下一个字符合法地把当前序列延长为一个更长的 token，就必须延长"
//   ⇒ `a<=b` 必须切成 a | <= | b，而不能切成 a | < | = | b。
// 扫到的记号 ⇒ 产出（先试 2 字符，组成不了已知双字符运算符再退回 1 字符）：
//   双字符：`<=` `>=` `==` `!=` `&&` `||` `+=` `-=` `*=` `/=` `->` `::`
//           ⇒ 消费前瞻的 n，产对应双字符 token
//   单字符（不消费 n，产对应单字符 token）：
//     `<` `>` `=` `!` `&` `|` `+` `-` `*` `/` `:`      可作双字符首字符的那批
//     `%` `^` `~` `.` `,` `;` `(` `)` `{` `}` `[` `]`  只能单字符的那批
//   其他  ⇒ 抛 "Unexpected character 'x' at 行:列"
// 实现技巧：本项目词表只有 1/2 字符两种 ⇒ "首字符 c + 前瞻 n" 即够
//   （clang 还要处理 `<<=`、`...` 等更长串）。
// demo: "<=" ⇒ Token{LessEqual}；"<x" ⇒ Token{Less}，光标停在 'x'；"->" ⇒ Token{Arrow}
Token Lexer::scanOperator() {
    auto loc = currentLocation();
    char c = advance(); // 会消费
    char n = peek(); //只是取出来 ,中间不能有空格，比如>= 不能是 > =

    // c：已消费的首字符；n：前瞻一字符（只读，匹配成功才 advance() 消费它）
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
    // 走到这里说明 c 无法组成双字符运算符，退回匹配单字符——
    // 这正是最长匹配中"延长不了就取短"的那一面。
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

    // 既非已知运算符也非分隔符 → 非法字符错误（词法阶段就能发现的错误之一）
    // 示例：源码 "int x @ 1;" → 抛 "Unexpected character '@' at 1:7"
    throw std::runtime_error(
        std::format("Unexpected character '{}' at {}", c, loc.toString()));
}

// ─────────────────────────────────────────────────────────────────────────────
// nextToken：词法分析的核心入口
// ─────────────────────────────────────────────────────────────────────────────
// 这是一个"调度器"：根据当前字符的类型分派到对应的扫描函数 ——
// 整个流程即确定性有限自动机（DFA）的外层循环。
// ─────────────────────────────────────────────────────────────────────────────
// 分派表（只看首字符就决定分支；ASCII 流程图见 lexer.h 类注释）：
//   数字 (0-9)        scanNumber()                "42"
//   字母 / 下划线     scanIdentifierOrKeyword()   "foo"、"int"
//   双引号            scanString()                "hi"
//   其他              scanOperator()              "="、";"、"->"
// demo: "int x = 42;" 连续调用 ⇒ {KwInt}{Identifier "x"}{Assign}{IntLiteral "42"}{Semicolon}{Eof}
Token Lexer::nextToken() {
    skipWhitespaceAndComments(); //跳过空白符和注释

    // 源码耗尽：返回 Eof token（而不是报错）——Parser 用它作为文法的统一终止信号；
    // 且重复调用会一直返回 Eof（幂等），调用方循环条件无需特殊处理。
    if (isAtEnd()) {
        return makeToken(TokenType::Eof, "", currentLocation());
    }

    char c = peek();

    // 数字开头 → 数字字面量
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return scanNumber();
    }

    // 字母或下划线开头 → 标识符或关键字
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { // 如果是 字母或者_开头
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
// 反复调用 nextToken() 直到（含）Eof —— main.cpp 使用的接口，
// 把整条 Token 流一次性交给 Parser。
// demo: Lexer("int x;").tokenizeAll() ⇒ [KwInt][Identifier "x"][Semicolon][Eof]
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
