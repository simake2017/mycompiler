#pragma once
// =============================================================================
// include/preprocessor.h —— 阶段 0：预处理器 (Preprocessor)（理论见 docs/learn/07）
// =============================================================================
// 对应 [cpp.phase] 翻译阶段 2~4：行拼接 → 指令行识别 → 宏展开 + #include 文件并合。
// clang 的真实实现是 token 级的（lib/Lex/PP*.cpp）；本教学版用行/文本级实现，
// 更简单直观，算法骨架（搜索路径、宏重扫描、条件栈）与 clang 一致。
//
// 已支持  #include "…" / <…>（搜索路径）、#pragma once（canonical 去重）、
//         #define/#undef（对象宏 + 函数宏：递归展开、自引用"涂蓝"保护）、
//         #ifdef/#ifndef/#if/#elif/#else/#endif（条件编译栈）、
//         #if 表达式（整数字面量与 defined(X)、! && || == != < > <= >= + - * /、括号）、
//         __LINE__/__FILE__、#error
// 不支持  # 字符串化、## 记号粘贴、变参宏、_Pragma（明确简化，见 docs/learn/07）
//
// 管线位置 —— 整条管线唯一的"文本级"阶段，也是六阶段中的阶段 0：
//   源码.cpp ──► Preprocessor ──► 展开后的纯文本 ──► Lexer ──► Parser ──► Sema ──► ...
//                （本文件）         （等价 gcc -E 输出）
// 之后文本里不再有 '#' 指令、未展开的宏、头文件边界，Lexer 看到的是一整段平铺源码。
//
// clang 对照（函数级对照见 preprocessor.cpp 头注）：
//   指令识别与条件编译栈  lib/Lex/PPDirectives.cpp
//   #include 文件并合     lib/Lex/PPDirectives.cpp (HandleIncludeDirective)
//   头文件搜索路径        lib/Basic/HeaderSearch.cpp
//   宏展开/重扫描/涂蓝    lib/Lex/PPMacroExpansion.cpp
//   #if 常量表达式求值    lib/Lex/PPExpressions.cpp
// =============================================================================

#include <cstddef>     // size_t —— readWord 的签名里用到（见下）。
                       // 必须显式包含，不能依赖 <string> 的传递包含：
                       // IDE 单独解析本头文件时签名会"残缺"，与 .cpp 里的定义对不上。
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minicc {

// 一条宏定义的运行时表示（#define 登记进宏表 m_macros 的条目）。
// demo: #define N 10 ⇒ { name="N", params={}, body="10", functionLike=false }
//       #define MAX(a,b) ((a)>(b)?(a):(b))
//         ⇒ { name="MAX", params={"a","b"}, body="((a)>(b)?(a):(b))", functionLike=true }
struct MacroDef {
    std::string name;
    std::vector<std::string> params;   // 仅函数宏使用
    std::string body;
    bool functionLike = false;
};

// 预处理选项：来自命令行 -I dir（可多次出现，书写顺序 = 搜索优先级）。
// demo: minicc main.cpp -I include -I third_party ⇒ includePaths = {"include", "third_party"}
struct PreprocessorOptions {
    std::vector<std::string> includePaths;  // -I 搜索目录（按顺序）
};

class Preprocessor {
public:
    explicit Preprocessor(PreprocessorOptions opts = {});

    // 预处理入口：读文件 → 展开 → 返回完整源文本，驱动翻译阶段 2~4（[lex.phases]）。
    // 先把 path 的 canonical 路径压入 include 栈，使主文件自身也纳入循环 include 检测。
    // demo: processFile("tests/pp/main.cpp") 的返回值里 util.h 已就地并合、宏已替换
    //       —— 与 `gcc -E` 的输出等价。
    std::string processFile(const std::string& path);

    // 观测接口：被成功并合的头文件数 / 当前宏表（供驱动打印统计信息）
    int includeCount() const { return m_includeCount; }
    const std::unordered_map<std::string, MacroDef>& macros() const { return m_macros; }

private:
    // 白盒单元测试桩（见 tests/unit/test_preprocessor.cpp）：授权测试直接调用
    // processText/expand/substituteParams/evalConstantExpr 等 private 方法，
    // 逐个观察"输入 → 展开结果"。仅供测试，不参与编译产物逻辑。
    friend class PreprocessorTestPeer;

    // -I 搜索目录等选项（来自命令行）
    PreprocessorOptions m_opts;
    // 宏表：名字 → 定义。#define 插入/覆盖，#undef 删除，expand 时查表。
    std::unordered_map<std::string, MacroDef> m_macros;
    std::unordered_set<std::string> m_pragmaOnce;   // 已 pragma once 的 canonical 路径
    std::vector<std::string> m_includeStack;        // 循环 include 检测
    int m_includeCount = 0;
    // 调试：expand 递归深度（仅用于日志缩进，不影响逻辑）
    int m_expandDepth = 0;

    // 处理一段源文本（主文件或某个被包含文件）—— 三步：
    //   ① 行拼接（删除 '\' + 换行，[lex.phases] 翻译阶段 2）
    //   ② 逐行识别 # 指令（define/include/条件编译/pragma/error）
    //   ③ 普通行经 expand 宏展开后写入输出
    // 指令行被消费后输出一个空行，保住原文件行号（报错可定位）。
    // demo（行拼接）：物理行 `#define MAX(a,b) \` + `  ((a)>(b)?(a):(b))`
    //   ⇒ 先拼成一个逻辑行，再识别为完整的 #define，宏体得以跨行书写。
    std::string processText(const std::string& src, const std::string& fileName);

    // 指令处理
    // #define：解析宏名/参数表/宏体并登记进宏表（[cpp.define]）。
    // 关键判定：'(' 紧跟宏名 ⇒ 函数宏；隔了空白 ⇒ 对象宏。
    // demo: rest="N 10" ⇒ 对象宏；rest="MAX(a,b) ((a)>(b)?(a):(b))" ⇒ 函数宏
    void handleDefine(const std::string& rest, const std::string& fileName, int line);
    // #undef：从宏表移除（[cpp.undef]）。此后 #ifdef 该名为假、名字不再展开。
    void handleUndef(const std::string& rest, int line);
    // #include：解析文件名 → 搜索定位 → pragma once 去重 + 循环检测 →
    // 递归 processText，把被包含文件展开后的全文就地并合（[cpp.include]）。
    // demo: #include "util.h" ⇒ 返回 util.h 展开后的全文，直接拼进当前输出
    std::string handleInclude(const std::string& rest, const std::string& fileName, int line);
    // #pragma once：把当前文件 canonical 路径记入 m_pragmaOnce，之后重复 include
    // 同一物理文件直接跳过（头文件去重）。其余 pragma 警告后忽略。
    void handlePragma(const std::string& rest, const std::string& fileName, int line);

    // include 搜索：引号 → 当前文件目录 → -I；尖括号 → -I → /usr/include
    // demo: 当前文件 tests/pp/main.cpp、命令行 -I include：
    //   #include "util.h" ⇒ 先试 tests/pp/util.h，命中即返回
    //   #include <minicc/util.h> ⇒ 试 include/minicc/util.h，再试 /usr/include/...
    // 全部落空时报错，并列出所有尝试过的路径（模仿 clang 的 file-not-found 诊断）。
    std::string resolveInclude(const std::string& name, bool angled,
                               const std::string& currentFile);
    // 读文件全文到字符串（主文件与被包含文件共用；打开失败抛异常）
    std::string readFileContents(const std::string& path) const;

    // 宏展开：递归重扫描 + hide 集（自引用涂蓝，[cpp.rescan] 的简化）
    // demo: #define N 10 ⇒ "arr[N]" 展开成 "arr[10]"
    //       #define MAX(a,b) ((a)>(b)?(a):(b)) ⇒ "MAX(x, y+1)" 展开成 "((x)>(y+1)?(x):(y+1))"
    //       #define A A+1 ⇒ "A" 展开成 "A+1"（其中的 A 已涂蓝不再展开，避免自引用死循环）
    std::string expand(const std::string& text,
                       const std::unordered_set<std::string>& hide,
                       const std::string& fileName, int line);
    // 函数宏实参替换：参数名按【词边界】逐处替换（abc 中的子串 ab 不会被误替换），
    // 字符串字面量内不替换。
    // demo: body="((a)>(b)?(a):(b))"、args={"x","y+1"} ⇒ "((x)>(y+1)?(x):(y+1))"
    std::string substituteParams(const MacroDef& m,
                                 const std::vector<std::string>& args);

    // #if 常量表达式求值（先处理 defined，再展开宏，最后求值）
    // demo: #if defined(DEBUG) && LEVEL > 1（其中 #define LEVEL 2）
    //   ① defined(DEBUG) → 1/0；② LEVEL → 2；③ 求值 "1 && 2 > 1" → 1（分支激活）
    long evalConstantExpr(const std::string& expr, const std::string& fileName, int line);

    // 词法小工具：预处理器不预分词，只在文本上按需读"词"。
    // isWordStart/isWordChar 界定标识符字符集；readWord 从 pos 读一个完整的词
    // （demo: readWord("MAX(a,b)", 0) ⇒ out="MAX"，返回 3，停在 '(' 处）。
    static bool isWordStart(char c);
    static bool isWordChar(char c);
    static size_t readWord(const std::string& s, size_t pos, std::string& out);
    static std::string stripComment(const std::string& line);  // 剥行尾注释（字符串感知）
    static std::string trim(const std::string& s);
};

} // namespace minicc
