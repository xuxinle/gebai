#pragma once

/// Markdown 层的高亮入口——**转发到通用高亮引擎**（`st/text/highlight.hpp`）。
///
/// 历史上高亮实现在本层（只服务 Markdown 代码块）；现在它已升级为「规则驱动 + 可注册自定义语言」
/// 的通用引擎并下沉到 `text` 层，供代码编辑器与 Markdown 渲染共用**同一份实现与规则集**。
/// 本头文件保留原 API 形态（`st::md::highlight` / `st::md::TokenKind`），既有调用方无需改动。

#include <string>
#include <string_view>
#include <vector>

#include "st/text/highlight.hpp"

namespace st::md {

using TokenKind = st::text::TokenKind;
using Token = st::text::Token;

/// 对 `code` 按 `language` 做词法着色（语言名/别名/扩展名均可；未知语言返回单个 `Plain`）。
[[nodiscard]] inline auto highlight(std::string_view code, std::string_view language)
    -> std::vector<Token> {
  return st::text::highlight(code, language);
}

/// 支持的语言名（规范名 + 别名 + 扩展名，升序）。
[[nodiscard]] inline auto supported_languages() -> std::vector<std::string> {
  return st::text::supported_languages();
}

}  // namespace st::md
