#include "st/lsp/completion.hpp"

#include <algorithm>
#include <cctype>

namespace st::lsp {

namespace {

/// 从 `documentation` 字段取文本（`string` 或 `{kind,value}` 两种形态）。
[[nodiscard]] auto documentation_text(const Json& item) -> std::string {
  const Json* docs = json_find(item, "documentation");
  if (docs == nullptr) return {};
  if (docs->is_string()) return docs->get<std::string>();
  if (docs->is_object()) return json_get_string(*docs, "value");
  return {};
}

/// 从 `textEdit` 取插入文本（`TextEdit` 或 `InsertReplaceEdit` 两种形态）。
/// 返回 `{has_range, text}`；`range` 的具体区间本层不解析（应用侧用它的起点去替换）。
[[nodiscard]] auto text_edit_of(const Json& item) -> std::pair<bool, std::string> {
  const Json* edit = json_find(item, "textEdit");
  if (edit == nullptr || !edit->is_object()) return {false, {}};
  const std::string text = json_get_string(*edit, "newText");
  // `InsertReplaceEdit` 有两个区间（`insert`/`replace`），都算"有区间"。
  const bool has_range = json_find(*edit, "range") != nullptr ||
                         (json_find(*edit, "insert") != nullptr &&
                          json_find(*edit, "replace") != nullptr);
  return {has_range, text};
}

}  // namespace

auto completion_badge(std::int64_t kind) -> std::string_view {
  // 协议枚举（`CompletionItemKind`）：1=Text 2=Method 3=Function 4=Constructor
  // 5=Field 6=Variable 7=Class 8=Interface 9=Module 10=Property 11=Unit
  // 12=Value 13=Enum 14=Keyword 15=Snippet 16=Color 17=File 18=Reference
  // 19=Folder 20=EnumMember 21=Constant 22=Struct 23=Event 24=Operator 25=TypeParameter
  switch (kind) {
    case 2: return "m";    // 方法
    case 3: return "f";    // 函数
    case 4: return "c";    // 构造
    case 5: return "·";    // 字段
    case 6: return "v";    // 变量
    case 7: return "C";    // 类
    case 8: return "I";    // 接口
    case 9: return "M";    // 模块
    case 10: return "p";   // 属性
    case 12: return "=";   // 值
    case 13: return "E";   // 枚举
    case 14: return "k";   // 关键字
    case 15: return "S";   // 片段
    case 20: return "e";   // 枚举成员
    case 21: return "n";   // 常量
    case 22: return "T";   // 结构体
    case 23: return "!";   // 事件
    case 24: return "o";   // 运算符
    case 25: return "t";   // 类型参数
    default: return {};    // 未知/文本：不画徽标（空比错好）
  }
}

auto expand_snippet_minimal(std::string_view text) -> std::string {
  std::string result;
  result.reserve(text.size());
  for (std::size_t at = 0; at < text.size(); ++at) {
    const char current = text[at];
    if (current == '\\' && at + 1 < text.size()) {
      // 转义：`\$` → `$`（snippet 里 `$` 的字面写法）。
      const char next = text[at + 1];
      if (next == '$' || next == '}' || next == '\\') {
        result += next;
        ++at;
        continue;
      }
      result += current;
      continue;
    }
    if (current != '$') {
      result += current;
      continue;
    }
    // `$` 之后：`${N:default}` / `${N|a,b|}` / `${N}` / `$N` / `$NAME`
    std::size_t cursor = at + 1;
    std::string placeholder_default;
    bool has_default = false;
    if (cursor < text.size() && text[cursor] == '{') {
      ++cursor;
      // 跳数字（tabstop 序号）。
      while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
        ++cursor;
      }
      if (cursor < text.size() && text[cursor] == ':') {
        // 默认值到匹配的 `}`（不做嵌套花括号的完整支持——snippet 里少见）。
        ++cursor;
        std::size_t depth = 1;
        std::size_t value_begin = cursor;
        while (cursor < text.size() && depth > 0) {
          if (text[cursor] == '{') ++depth;
          else if (text[cursor] == '}') {
            --depth;
            if (depth == 0) break;
          }
          ++cursor;
        }
        placeholder_default = std::string(text.substr(value_begin, cursor - value_begin));
        has_default = true;
      } else if (cursor < text.size() && text[cursor] == '|') {
        // 选择占位 `${N|a,b|}`：取第一个选项。
        ++cursor;
        const std::size_t comma = text.find_first_of(",|", cursor);
        if (comma != std::string_view::npos && comma > cursor) {
          placeholder_default = std::string(text.substr(cursor, comma - cursor));
          has_default = true;
        }
        while (cursor < text.size() && text[cursor] != '}') ++cursor;
      }
      // 跳过到 `}`（`${N}` 形态）。
      while (cursor < text.size() && text[cursor] != '}') ++cursor;
      if (cursor < text.size()) ++cursor;
    } else {
      // `$NAME` 或 `$N`：整个变量名/数字都吃掉。
      while (cursor < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[cursor])) != 0 || text[cursor] == '_')) {
        ++cursor;
      }
    }
    if (has_default) result += placeholder_default;
    at = cursor - 1;   // 循环会再 ++
  }
  return result;
}

auto parse_completion(const Json& result) -> std::vector<CompletionEntry> {
  const Json* items = nullptr;
  if (result.is_array()) {
    items = &result;
  } else if (result.is_object()) {
    items = json_find(result, "items");
  }
  if (items == nullptr || !items->is_array()) return {};

  std::vector<CompletionEntry> entries;
  entries.reserve(items->size());
  for (const auto& item : *items) {
    if (!item.is_object()) continue;
    CompletionEntry entry{};
    entry.label = json_get_string(item, "label");
    if (entry.label.empty()) continue;   // 没有标签的候选无法显示
    entry.detail = json_get_string(item, "detail");
    entry.filter_text = json_get_string(item, "filterText");
    entry.sort_text = json_get_string(item, "sortText");
    entry.documentation = documentation_text(item);
    entry.badge = std::string(
        completion_badge(json_get_i64(item, "kind", 0)));

    // 插入文本优先级：`textEdit.newText`（带区间，最准）> `insertText` > `label`。
    const auto [has_edit, edit_text] = text_edit_of(item);
    if (has_edit && !edit_text.empty()) {
      entry.insert_text = edit_text;
    } else if (const std::string literal = json_get_string(item, "insertText");
               !literal.empty()) {
      entry.insert_text = literal;
    } else {
      entry.insert_text = entry.label;
    }
    // Snippet 展开（`insertTextFormat == 2`；缺省是 1=PlainText）。
    if (json_get_i64(item, "insertTextFormat", 1) == 2) {
      entry.insert_text = expand_snippet_minimal(entry.insert_text);
    }
    entry.raw = item;
    entries.push_back(std::move(entry));
  }
  return entries;
}

auto completion_is_incomplete(const Json& result) -> bool {
  if (!result.is_object()) return false;
  return json_get_bool(result, "isIncomplete", false);
}

auto parse_completion_detail(const Json& item) -> std::string {
  std::string detail = documentation_text(item);
  const std::string short_detail = json_get_string(item, "detail");
  if (detail.empty()) return short_detail;
  if (short_detail.empty()) return detail;
  return short_detail + "\n" + detail;
}

auto should_trigger(const std::vector<std::string>& trigger_characters, char input) -> bool {
  const std::string_view text(&input, 1);
  return std::find(trigger_characters.begin(), trigger_characters.end(), text) !=
         trigger_characters.end();
}

auto word_range_before_cursor(std::string_view text, std::size_t cursor)
    -> std::pair<std::size_t, std::size_t> {
  const std::size_t end = std::min(cursor, text.size());
  // 标识符字符（与 C 家族一致：字母/数字/下划线；非 ASCII 按"字面字符"算——
  // UTF-8 多字节的高位字节 >= 0x80，直接算进词里，避免把中文标识符切断）。
  const auto is_word = [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c >= 0x80U;
  };
  std::size_t begin = end;
  while (begin > 0 && is_word(static_cast<unsigned char>(text[begin - 1]))) --begin;
  return {begin, end};
}

}  // namespace st::lsp
