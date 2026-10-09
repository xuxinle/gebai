#pragma once

/// 导航类请求的**语义层**（LSP 阶段 5）：定义跳转 / 引用 / 悬停 / 文档符号。
///
/// 与 `st/lsp/completion` 同一分层理由：这里是"协议 → 视图"的翻译器，
/// 单测喂 JSON 就能跑，不需要进程与 UI。
///
/// ## 三种返回形态的坑
///
/// 协议对"位置类"响应留了三种形态，server 按喜好选：
/// 1. `Location`（单个对象）——`textDocument/definition` 最常见；
/// 2. `Location[]`（数组）——多结果；
/// 3. `LocationLink[]`（带 `targetUri`/`targetSelectionRange` 的数组）
///    ——`textDocument/definition` 的"现代"形态，`clangd` 与 `rust-analyzer` 会用。
///
/// **`LocationLink` 必须优先取 `targetSelectionRange`**（而不是 `targetRange`）：
/// 前者是"标识符本身"，后者是"整个声明"（函数体会被整个框住）。跳到 `targetRange`
/// 的起点在多数情况下相同，但 `targetSelectionRange` 能给出准确的**列**——
/// 实测差异：跳到一个函数定义时，用 `targetRange` 落在函数签名第一列（错位）。
///
/// ## URI 与路径
///
/// 响应里一律是 `file://` URI。本层**不转路径**（那是应用的事——它才知道
/// 文件系统口径），原样带出 URI，由调用方用 `LspClient::uri_to_path` 转。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/protocol.hpp"

namespace st::lsp {

/// 一个位置（文件 + 区间）。
struct Location {
  std::string uri{};
  Range range{};
  /// `LocationLink` 的 `targetSelectionRange`（标识符本身；普通 `Location` 时为空，
  /// 调用方应退回 `range`）。
  std::optional<Range> selection{};
};

/// 解析 `textDocument/definition` / `references` / `implementation` 等的响应。
///
/// 三种形态（单个 / 数组 / LocationLink 数组）统一成一个列表；无法识别返回空。
[[nodiscard]] auto parse_locations(const Json& result) -> std::vector<Location>;

/// 悬停内容。
struct HoverInfo {
  /// 文本内容（MarkupContent 取 `value`；MarkedString 数组被拼接）。
  std::string text{};
  /// 该悬停适用的区间（可空——server 可能不给）。
  std::optional<Range> range{};
};

/// 解析 `textDocument/hover` 的响应（`null` 结果返回 `nullopt`）。
[[nodiscard]] auto parse_hover(const Json& result) -> std::optional<HoverInfo>;

/// 文档符号（大纲视图用）。
struct DocumentSymbol {
  std::string name{};
  std::string detail{};
  /// 协议 `SymbolKind`（1=File 2=Module 3=Namespace 4=Package 5=Class 6=Method
  /// 7=Property 8=Field 9=Constructor 10=Enum 11=Interface 12=Function
  /// 13=Variable 14=Constant 15=String 16=Number 17=Boolean 18=Array …）。
  std::int64_t kind{0};
  Range range{};
  /// 子符号（`DocumentSymbol[]` 形态才有；`SymbolInformation[]` 是扁平的）。
  std::vector<DocumentSymbol> children{};
  /// 缩进深度（扁平形态由调用方按容器算；本层对嵌套形态直接填）。
  std::size_t depth{0};
};

/// 解析 `textDocument/documentSymbol` 的响应。
///
/// 两种形态：**层级**（`DocumentSymbol`，带 `children`，靠 `range` 嵌套）与
/// **扁平**（`SymbolInformation`，带 `location`）。本层把扁平形态转成层级 0，
/// 层级形态递归展开并填 `depth`。
[[nodiscard]] auto parse_document_symbols(const Json& result) -> std::vector<DocumentSymbol>;

/// 工作区符号（`workspace/symbol`）。
struct WorkspaceSymbol {
  std::string name{};
  std::string container{};   ///< `containerName`（如类名）
  std::int64_t kind{0};
  std::string uri{};
  Range range{};
};

/// 解析 `workspace/symbol` 的响应。
[[nodiscard]] auto parse_workspace_symbols(const Json& result) -> std::vector<WorkspaceSymbol>;

/// 符号类型 → 徽标（与补全同一套单字符风格）。
[[nodiscard]] auto symbol_badge(std::int64_t kind) -> std::string_view;

/// 取"光标处的标识符"（用于把位置请求发到词的起点——server 对词中间的容忍度不一）。
///
/// 返回 `{begin, end}`；光标不在词内时返回 `{cursor, cursor}`（调用方原样发光标位置）。
[[nodiscard]] auto identifier_at(std::string_view text, std::size_t cursor)
    -> std::pair<std::size_t, std::size_t>;

}  // namespace st::lsp
