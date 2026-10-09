#pragma once

/// 编辑应用层（LSP 阶段 7）：把 server 给的 `TextEdit` / `WorkspaceEdit` 落到文本上。
///
/// 为什么独立成层：**格式化与重命名符号**（以及未来的任何编辑类特性）最终都归到
/// 这一件事——"把若干区间替换成新文本"。把这层做对，各特性只剩"取参数、发请求、收结果"。
///
/// ## 应用顺序：必须从后往前
///
/// 这是本层最容易做错的地方：多个 edit 的 `range` 都是**基于同一份原始文本**给的，
/// 若从前往后应用，第一个替换改变了后面所有偏移——第二个 edit 就写在错误位置。
/// 实测症状很隐蔽：格式化一个文件时前几处对、后面开始错位，越往后越乱。
/// 本层统一按起点**降序**应用（同起点的先应用 range 大的，避免小 range 破坏大 range 的边界）。
///
/// ## 重叠与冲突
///
/// 协议没说 edit 之间不会重叠，但重叠应用的结果无定义。本层**如实报告**重叠
/// （`apply_edits` 返回 `nullopt` 并给出原因），不猜"哪个优先"——格式化/重命名产生的
/// edit 应当互不重叠，真重叠了说明理解有偏差，静默处理只会掩盖问题。
///
/// ## 位置换算
///
/// 协议给的是 `line`/`character`（UTF-16 码元），本层负责转成字节偏移
/// （复用 `st/lsp/protocol` 的 `position_to_offset`）。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/protocol.hpp"

namespace st::lsp {

/// 一条文本编辑（协议 `TextEdit` 的可用子集）。
struct TextEdit {
  Range range{};
  std::string new_text{};
};

/// 解析协议 `TextEdit[]`。
[[nodiscard]] auto parse_text_edits(const Json& value) -> std::vector<TextEdit>;

/// 一个文件的编辑集合（`WorkspaceEdit` 的一项）。
struct FileEdits {
  std::string uri{};
  std::vector<TextEdit> edits{};
  /// 协议 `TextDocumentEdit.textDocument.version`（可空：server 未给版本时不做校验）。
  std::optional<std::int64_t> version{};
};

/// 解析协议 `WorkspaceEdit`。
///
/// 两种形态：`changes`（`{uri: TextEdit[]}`，老形态，**无版本**）与 `documentChanges`
/// （`TextDocumentEdit[]`，带版本；还可能含 `CreateFile`/`RenameFile`/`DeleteFile`
/// 这类**资源操作**——本层只解析文本编辑，资源操作**如实跳过并记录**
/// （静默忽略会让"重命名文件"类动作看起来成功却没生效）。
struct WorkspaceEdit {
  std::vector<FileEdits> files{};
  /// 被跳过的资源操作描述（`create`/`rename`/`delete`），供 UI 如实提示。
  std::vector<std::string> skipped_resource_ops{};
};

[[nodiscard]] auto parse_workspace_edit(const Json& value) -> WorkspaceEdit;

/// 把一组 edit 应用到文本上。成功返回新文本；失败返回 `nullopt` 并填 `error`。
///
/// 失败情形（**都不静默**）：
/// - 区间越界（`line`/`character` 超出文本）；
/// - 区间起点 > 终点（协议不该出现，出现即 server 有 bug）；
/// - 两处 edit 区间重叠（应用结果无定义）。
[[nodiscard]] auto apply_edits(std::string_view text, const std::vector<TextEdit>& edits,
                               std::string* error = nullptr) -> std::optional<std::string>;

/// 就地应用（成功才写回；`text` 不变则返回 false）。
[[nodiscard]] auto apply_edits_in_place(std::string& text, const std::vector<TextEdit>& edits,
                                        std::string* error = nullptr) -> bool;

}  // namespace st::lsp
