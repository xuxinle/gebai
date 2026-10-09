#include "st/lsp/actions.hpp"

#include <utility>

namespace st::lsp {

auto formatting_options_json(const FormattingOptions& options) -> Json {
  Json result = Json::object();
  result["tabSize"] = options.tab_size;
  result["insertSpaces"] = options.insert_spaces;
  // 可选字段：**给了才带**（带 `false` 与不带语义不同——前者明确要求"不要做"，
  // 后者是"没意见"。server 对前者的处理是强制的，后者才是自由裁量）。
  if (options.trim_trailing_whitespace) result["trimTrailingWhitespace"] = true;
  if (options.insert_final_newline) result["insertFinalNewline"] = true;
  // 额外选项合入（不覆盖上面三个——它们是协议定义的核心字段）。
  if (options.extras.is_object()) {
    for (auto iterator = options.extras.begin(); iterator != options.extras.end(); ++iterator) {
      if (result.contains(iterator.key())) continue;
      result[iterator.key()] = iterator.value();
    }
  }
  return result;
}

auto parse_formatting_edits(const Json& result) -> std::vector<TextEdit> {
  return parse_text_edits(result);   // 与普通 `TextEdit[]` 同形
}

auto parse_prepare_rename(const Json& result) -> std::optional<PrepareRenameResult> {
  if (result.is_null()) return std::nullopt;   // 明确"此处不可重命名"
  if (!result.is_object()) return std::nullopt;
  PrepareRenameResult prepared{};
  // 形态一：`{range, placeholder}`（协议的"完整"形态）。
  if (const Json* range = json_find(result, "range"); range != nullptr) {
    prepared.range = range_from_json(*range);
    prepared.placeholder = json_get_string(result, "placeholder");
    return prepared;
  }
  // 形态二：**裸 `Range`**（`{start:{...}, end:{...}}`）——clangd 就是这么回的
  //（实测：它给 `{"start":{"line":7,"character":9},"end":{...}}`，既没有 `range`
  //  包裹也没有 `placeholder`）。漏掉这一形态的后果：**clangd 下重命名永远被判
  //  "此处没有可重命名的符号"**——功能看上去完全没接上，而协议层其实一切正常。
  if (json_find(result, "start") != nullptr && json_find(result, "end") != nullptr) {
    prepared.range = range_from_json(result);
    return prepared;
  }
  // 形态二：`{defaultBehavior: true}` —— server 在说"别问了，直接发 rename"。
  // 返回 nullopt 会让调用方以为"不可重命名"——但那与"不确定"是两件事。
  // 这里用**空 placeholder + 默认区间**表达"可以试，但没有预填"：
  // 调用方看到 `has_value()` 为真但 placeholder 为空，就知道该允许尝试并留空输入框。
  if (json_get_bool(result, "defaultBehavior")) return PrepareRenameResult{};
  return std::nullopt;
}

auto text_position_params(std::string_view uri, std::uint32_t line, std::uint32_t character)
    -> Json {
  Json params = Json::object();
  Json item = Json::object();
  item["uri"] = std::string(uri);
  params["textDocument"] = std::move(item);
  Json position = Json::object();
  position["line"] = line;
  position["character"] = character;
  params["position"] = std::move(position);
  return params;
}

}  // namespace st::lsp
