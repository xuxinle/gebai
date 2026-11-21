#pragma once

/// @file cpp_lex.hpp
/// @brief C++ 源码的**两层净化视图**：去注释（保留字符串内容）/ 再去字符串内容。
///
/// ## 为什么单独一个头，而不是留在 `lint.cpp` 里
///
/// 这份词法状态机原本只服务 lint 规则（L8 花括号计数、L15/L16 看原文），
/// 但“判断这份源码到底用了什么”也需要它——**能力裁剪的判据**
/// （`build.cpp` 的 `target_uses_webview` / `target_uses_script` / `target_uses_three`）。
///
/// 而那个判据**被注释骗过**是真实发生的事：`cefu/src/main.cpp` 里有两处
/// `// 为何：ThreeView 需要真的三维后端…` 的注释，于是判据认定“cefu 用三维”，
/// 1254 KB 的 three.js 就白带了。注释不是使用。
///
/// 抽成共享头的理由是**只留一份词法真相**：lint 与能力判定若各写一套，
/// 必然在某次修复里分家（本仓已有先例：`strip_comments` 与 `strip_string_literals`
/// 两道各自维护状态，裸串里的 `//` 把整行截断，状态机卡到文件末尾）。
///
/// ## 两个视图各管什么
///
/// | 视图 | 内容 | 用途 |
/// |---|---|---|
/// | `uncommented` | 去注释、**保留**字符串内容 | 需要看字面量原文的规则 |
/// | `code` | 再去掉字符串内容（长度对齐、位置不变） | 关键字/标识符判定、花括号计数 |
///
/// 能力判定用 `code`：`"ThreeView"` 这种字符串字面量也不算“用了 ThreeView”。

#include <cstddef>
#include <string>
#include <string_view>

namespace st::pkg::lex {

/// 跨行的词法状态（注释块 + 裸字符串）。
struct RawStringState {
  bool active{false};        ///< 是否正处在一段（可能跨行的）裸字符串里
  std::string delimiter{};   ///< 定界符（无定界符的 `R"( … )"` 为空串）
};

/// 一行的**两层净化结果**（两个视图由**同一次词法扫描**产出）。
struct LexedLine {
  std::string uncommented{};  ///< 去掉注释、**保留**字符串内容
  std::string code{};         ///< 再去掉字符串内容（花括号计数与关键字判定用）
};

/// 跨行的词法状态（注释块 + 裸字符串）。
struct State {
  bool in_block_comment{false};
  RawStringState raw{};
};

/// 词法分析一行，**一次扫描产出两个视图**。
///
/// 状态机（优先级从高到低）：
/// 1. 块注释中 → 找 `*/`，两视图都丢掉正文；
/// 2. 裸字符串中 → 找 `) <定界符> "` 终结；内容进 `uncommented`，不进 `code`
///    （**关键**：裸串里的 `//` 不是行注释，不能截断）；
/// 3. 普通字符串/字符中 → 转义处理；内容进 `uncommented`，不进 `code`；
/// 4. 行注释 → 本行剩余部分丢弃；
/// 5. 裸串开头 `R"delim(` → 记住定界符并进入状态 2；定界符非法则当普通字符串起头
///    （编译器自己会报那行的错，这里只需保证不把状态搞乱）；
/// 6. 否则 → 普通字符，同时进两视图。
[[nodiscard]] auto lex_line(std::string_view line, State& state) -> LexedLine;

/// 整份源码的 `code` 视图（去注释 + 去字符串内容），行间用 `\n` 分隔。
///
/// 能力判定只需要这个：注释与字符串里的名字不算“用了”。
[[nodiscard]] auto strip_to_code(std::string_view source) -> std::string;

}  // namespace st::pkg::lex
