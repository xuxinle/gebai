#include "st/lsp/edit.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace st::lsp {

auto parse_text_edits(const Json& value) -> std::vector<TextEdit> {
  std::vector<TextEdit> edits;
  if (!value.is_array()) return edits;
  edits.reserve(value.size());
  for (const auto& item : value) {
    if (!item.is_object()) continue;
    TextEdit edit{};
    edit.range = range_from_json(json_at(item, "range"));
    edit.new_text = json_get_string(item, "newText");
    edits.push_back(std::move(edit));
  }
  return edits;
}

auto parse_workspace_edit(const Json& value) -> WorkspaceEdit {
  WorkspaceEdit result{};
  if (!value.is_object()) return result;

  // 形态一：`documentChanges`（带版本的现代形态；优先——它是超集）。
  if (const Json* changes = json_find(value, "documentChanges");
      changes != nullptr && changes->is_array()) {
    for (const auto& item : *changes) {
      if (!item.is_object()) continue;
      // 资源操作（`CreateFile`/`RenameFile`/`DeleteFile`）：**如实记录并跳过**。
      // 静默忽略会让"重命名文件"这类动作看起来成功却没生效——那比报错更坏。
      if (const std::string kind = json_get_string(item, "kind"); !kind.empty()) {
        // 资源操作的 uri 字段名随操作类型不同：`Create`/`Delete` 用 `uri`，
        // `Rename` 用 `oldUri`/`newUri`——两种都试，只用于给用户看的那行描述。
        std::string uri = json_get_string(json_at(item, "options"), "uri");
        if (uri.empty()) uri = json_get_string(json_at(item, "options"), "oldUri");
        result.skipped_resource_ops.push_back(
            uri.empty() ? kind : std::format("{} {}", kind, uri));
        continue;
      }
      FileEdits file{};
      const Json& document = json_at(item, "textDocument");
      file.uri = json_get_string(document, "uri");
      if (const Json* version = json_find(document, "version");
          version != nullptr && version->is_number_integer()) {
        file.version = version->get<std::int64_t>();
      }
      file.edits = parse_text_edits(json_at(item, "edits"));
      if (!file.uri.empty()) result.files.push_back(std::move(file));
    }
    if (!result.files.empty() || !result.skipped_resource_ops.empty()) return result;
  }

  // 形态二：`changes`（`{uri: TextEdit[]}`，老形态，无版本）。
  if (const Json* changes = json_find(value, "changes");
      changes != nullptr && changes->is_object()) {
    for (auto iterator = changes->begin(); iterator != changes->end(); ++iterator) {
      FileEdits file{};
      file.uri = iterator.key();
      file.edits = parse_text_edits(iterator.value());
      if (!file.uri.empty()) result.files.push_back(std::move(file));
    }
  }
  return result;
}

namespace {

/// 把一条 edit 的区间换算成字节偏移。
[[nodiscard]] auto edit_offsets(std::string_view text, const TextEdit& edit, std::size_t* begin,
                                std::size_t* end) -> bool {
  *begin = position_to_offset(text, edit.range.start);
  *end = position_to_offset(text, edit.range.end);
  return *begin <= *end;
}

}  // namespace

auto apply_edits(std::string_view text, const std::vector<TextEdit>& edits, std::string* error)
    -> std::optional<std::string> {
  const auto fail = [error](std::string message) -> std::optional<std::string> {
    if (error != nullptr) *error = std::move(message);
    return std::nullopt;
  };
  if (edits.empty()) return std::string(text);

  // ① 换算全部偏移（一次到位，后面按偏移排序/应用）。
  struct Resolved {
    std::size_t begin{0};
    std::size_t end{0};
    const TextEdit* edit{nullptr};
  };
  std::vector<Resolved> resolved;
  resolved.reserve(edits.size());
  for (const auto& edit : edits) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!edit_offsets(text, edit, &begin, &end)) {
      return fail(std::format("编辑区间起点晚于终点（{}:{} → {}:{}）",
                              edit.range.start.line, edit.range.start.character,
                              edit.range.end.line, edit.range.end.character));
    }
    resolved.push_back(Resolved{begin, end, &edit});
  }

  // ② 降序应用：从后往前改，前面的偏移才不被影响——这是本层的关键约定，
  //    反过来写会让越靠后的 edit 越错（实测格式化时"前几处对、后面全乱"）。
  //    同起点时**先应用 range 大的**（否则小 range 的替换会破坏大 range 的边界）。
  std::sort(resolved.begin(), resolved.end(), [](const Resolved& a, const Resolved& b) {
    if (a.begin != b.begin) return a.begin > b.begin;
    return (a.end - a.begin) > (b.end - b.begin);
  });

  // ③ 重叠检查（排序后相邻比较即可覆盖）。
  //
  // **同起点也算冲突**：`[0,4)` 与 `[0,1)` 是嵌套关系，先应用哪个都讲得通
  //（先大后小 → 小范围作用于已改文本；先小后大 → 大范围覆盖小范围结果），
  // 结果**不确定**。协议下 server 不该产生这种组合（真实的重命名/格式化不会），
  // 所以这里如实拒绝而不是挑一个"看起来合理"的顺序——猜错会静默改坏代码。
  for (std::size_t index = 0; index + 1 < resolved.size(); ++index) {
    const Resolved& later = resolved[index];      // 起点更靠后（降序）
    const Resolved& earlier = resolved[index + 1];
    if (earlier.end > later.begin) {
      return fail(std::format("两处编辑区间重叠（[{}..{}] 与 [{}..{}]）", earlier.begin,
                              earlier.end, later.begin, later.end));
    }
  }

  std::string result(text);
  for (const Resolved& item : resolved) {
    result.replace(item.begin, item.end - item.begin, item.edit->new_text);
  }
  return result;
}

auto apply_edits_in_place(std::string& text, const std::vector<TextEdit>& edits,
                          std::string* error) -> bool {
  auto applied = apply_edits(text, edits, error);
  if (!applied.has_value()) return false;
  if (*applied == text) return false;   // 无变化：不白写（调用方据此决定是否标脏）
  text = std::move(*applied);
  return true;
}

}  // namespace st::lsp
