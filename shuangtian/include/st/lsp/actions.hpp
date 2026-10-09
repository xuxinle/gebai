#pragma once

/// 编辑类动作的语义层（LSP 阶段 7）：**格式化**与**重命名符号**。
///
/// 与 `completion`/`navigation` 同层同理由：协议 → 视图的翻译器，单测喂 JSON 即可跑。
/// 应用编辑本身在 `st/lsp/edit`（两个特性共用）。
///
/// ## 格式化
///
/// 三种请求，语义不同、**不能互相替代**（server 对哪种支持是分开声明的）：
/// - `textDocument/formatting`：整篇格式化；
/// - `textDocument/rangeFormatting`：格式化选中区；
/// - `textDocument/onTypeFormatting`：输入时格式化（如敲 `}` 自动重排缩进）。
///
/// 本层只解析结果（`TextEdit[]`）；`FormattingOptions` 的构造在这里
/// （`tabSize`/`insertSpaces` 必须与编辑器的显示口径一致——否则格式化后的缩进
/// 在编辑器里看起来是错的，尽管 server 严格按我们给的参数做了）。
///
/// ## 重命名
///
/// `textDocument/prepareRename` → `textDocument/rename` 两步：
/// 第一步告诉调用方"这里能不能重命名、重命名的是哪个名字"（用于预填输入框），
/// 第二步才真正返回 `WorkspaceEdit`。
/// **跳过第一步也能工作**（直接 rename），但拿不到"当前名字"用于预填——
/// 输入框空着让用户自己敲全名，体验差一截。本层两个都解析。
///
/// ⚠ 重命名返回的是 **`WorkspaceEdit`**（可能跨文件！）——改一个头文件里的函数名
/// 会同时影响所有调用点。应用时必须逐个文件处理，且**每个文件各自从后往前应用**
/// （`st/lsp/edit` 已保证）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/edit.hpp"
#include "st/lsp/protocol.hpp"

namespace st::lsp {

/// 格式化选项（协议 `FormattingOptions` + 我们的默认值）。
struct FormattingOptions {
  std::uint32_t tab_size{4};
  bool insert_spaces{true};
  /// 是否裁剪行尾空白（协议 `trimTrailingWhitespace`；可选字段，server 忽略也无妨）。
  bool trim_trailing_whitespace{false};
  /// 是否在文件末尾插空行（协议 `insertFinalNewline`）。
  bool insert_final_newline{false};
  /// 额外选项（原样传给 server，如 clangd 不读但别的 server 认）。
  Json extras{};
};

/// 由编辑器配置构造协议参数。**`tab_size`/`insert_spaces` 必须与编辑器一致**：
/// server 按我们给的参数产出缩进，编辑器按自己的显示——两者不一致时格式化结果
/// 在编辑器里看起来是错的（而 server 没错，排查方向会被带偏）。
[[nodiscard]] auto formatting_options_json(const FormattingOptions& options) -> Json;

/// 解析格式化响应（`TextEdit[]`；无编辑返回空列表）。
[[nodiscard]] auto parse_formatting_edits(const Json& result) -> std::vector<TextEdit>;

/// 重命名预备结果（协议 `PrepareRenameResult`）。
struct PrepareRenameResult {
  /// 当前名字（用于预填输入框）。
  std::string placeholder{};
  /// 可重命名的区间（server 有时返回 `{range, placeholder}`，有时只给 `range`）。
  Range range{};
};

/// 解析 `textDocument/prepareRename` 的响应。
///
/// 三种形态：`{range, placeholder}` / `{defaultBehavior: true}`（server 表示
/// "直接试吧"）/ `null`（此位置不可重命名）。后两种都返回 `nullopt`——
/// 调用方据此决定"禁用重命名入口"或"不预填但允许尝试"（两者语义不同，见实现注释）。
[[nodiscard]] auto parse_prepare_rename(const Json& result) -> std::optional<PrepareRenameResult>;

/// 协议文档位置参数（`{textDocument:{uri}, position:{line,character}}`）。
[[nodiscard]] auto text_position_params(std::string_view uri, std::uint32_t line,
                                        std::uint32_t character) -> Json;

}  // namespace st::lsp
