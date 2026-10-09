#include "st/lsp/navigation.hpp"

#include <algorithm>
#include <cctype>

namespace st::lsp {

namespace {

/// 解析一条 `Location` / `LocationLink`（按字段存在性自动判别）。
[[nodiscard]] auto parse_one_location(const Json& item) -> std::optional<Location> {
  if (!item.is_object()) return std::nullopt;
  Location location{};
  // `LocationLink` 用 `targetUri`（+ `targetSelectionRange`）；`Location` 用 `uri`。
  if (const std::string target = json_get_string(item, "targetUri"); !target.empty()) {
    location.uri = target;
    if (const Json* selection = json_find(item, "targetSelectionRange"); selection != nullptr) {
      location.selection = range_from_json(*selection);
    }
    // `targetRange` 作为兜底：`selection` 缺失时它就是最接近的区间。
    if (const Json* target_range = json_find(item, "targetRange"); target_range != nullptr) {
      location.range = range_from_json(*target_range);
    } else if (location.selection.has_value()) {
      location.range = *location.selection;
    }
    return location.uri.empty() ? std::nullopt : std::optional<Location>(std::move(location));
  }
  location.uri = json_get_string(item, "uri");
  if (location.uri.empty()) return std::nullopt;
  location.range = range_from_json(json_at(item, "range"));
  return location;
}

/// 递归展开 `DocumentSymbol.children`。
void collect_symbols(const Json& item, std::size_t depth, std::vector<DocumentSymbol>& out) {
  if (!item.is_object()) return;
  DocumentSymbol symbol{};
  symbol.name = json_get_string(item, "name");
  symbol.detail = json_get_string(item, "detail");
  symbol.kind = json_get_i64(item, "kind", 0);
  symbol.depth = depth;
  // 层级形态用 `range`；扁平形态（`SymbolInformation`）用 `location.range`。
  if (const Json* range = json_find(item, "range"); range != nullptr) {
    symbol.range = range_from_json(*range);
  } else if (const Json* location = json_find(item, "location"); location != nullptr) {
    symbol.range = range_from_json(json_at(*location, "range"));
  }
  out.push_back(std::move(symbol));
  // 递归子节点（子节点写在 `push_back` 之后，因此父在子前——大纲顺序天然正确）。
  if (const Json* children = json_find(item, "children");
      children != nullptr && children->is_array()) {
    for (const auto& child : *children) collect_symbols(child, depth + 1, out);
  }
}

}  // namespace

auto parse_locations(const Json& result) -> std::vector<Location> {
  std::vector<Location> locations;
  if (result.is_null()) return locations;
  if (result.is_array()) {
    for (const auto& item : result) {
      if (auto parsed = parse_one_location(item); parsed.has_value()) {
        locations.push_back(std::move(*parsed));
      }
    }
    return locations;
  }
  if (result.is_object()) {
    if (auto parsed = parse_one_location(result); parsed.has_value()) {
      locations.push_back(std::move(*parsed));
    }
  }
  return locations;
}

auto parse_hover(const Json& result) -> std::optional<HoverInfo> {
  if (!result.is_object()) return std::nullopt;   // `null` / 非对象 = 无悬停
  HoverInfo info{};
  const Json* contents = json_find(result, "contents");
  if (contents == nullptr) return std::nullopt;
  if (contents->is_string()) {
    info.text = contents->get<std::string>();
  } else if (contents->is_object()) {
    // `MarkupContent`：`{kind: "markdown"|"plaintext", value: "..."}`
    info.text = json_get_string(*contents, "value");
  } else if (contents->is_array()) {
    // `MarkedString[]`（已废弃但仍在用）：元素是字符串或 `{language, value}`。
    for (const auto& item : *contents) {
      if (!info.text.empty()) info.text += "\n";
      if (item.is_string()) {
        info.text += item.get<std::string>();
      } else if (item.is_object()) {
        info.text += json_get_string(item, "value");
      }
    }
  }
  if (info.text.empty()) return std::nullopt;   // 空内容按"无悬停"处理（弹空框不如不弹）
  if (const Json* range = json_find(result, "range"); range != nullptr) {
    info.range = range_from_json(*range);
  }
  return info;
}

auto parse_document_symbols(const Json& result) -> std::vector<DocumentSymbol> {
  std::vector<DocumentSymbol> symbols;
  if (!result.is_array()) return symbols;
  for (const auto& item : result) collect_symbols(item, 0, symbols);
  return symbols;
}

auto parse_workspace_symbols(const Json& result) -> std::vector<WorkspaceSymbol> {
  std::vector<WorkspaceSymbol> symbols;
  // 两种形态：数组，或 `{symbols: [...]}`（部分实现如此）。
  const Json* items = result.is_array() ? &result : json_find(result, "symbols");
  if (items == nullptr || !items->is_array()) return symbols;
  symbols.reserve(items->size());
  for (const auto& item : *items) {
    if (!item.is_object()) continue;
    WorkspaceSymbol symbol{};
    symbol.name = json_get_string(item, "name");
    symbol.container = json_get_string(item, "containerName");
    symbol.kind = json_get_i64(item, "kind", 0);
    const Json* location = json_find(item, "location");
    if (location != nullptr) {
      symbol.uri = json_get_string(*location, "uri");
      symbol.range = range_from_json(json_at(*location, "range"));
    }
    if (symbol.name.empty()) continue;
    symbols.push_back(std::move(symbol));
  }
  return symbols;
}

auto symbol_badge(std::int64_t kind) -> std::string_view {
  // 与补全的徽标风格一致（单字符，等宽友好）。
  switch (kind) {
    case 1: return "F";    // File
    case 2: return "M";    // Module
    case 3: return "N";    // Namespace
    case 5: return "C";    // Class
    case 6: return "m";    // Method
    case 7: return "p";    // Property
    case 8: return "·";    // Field
    case 9: return "c";    // Constructor
    case 10: return "E";   // Enum
    case 11: return "I";   // Interface
    case 12: return "ƒ";   // Function（用 ƒ 与"方法 m"区分开）
    case 13: return "v";   // Variable
    case 14: return "n";   // Constant
    case 22: return "T";   // Struct
    default: return "•";
  }
}

auto identifier_at(std::string_view text, std::size_t cursor)
    -> std::pair<std::size_t, std::size_t> {
  const auto is_word = [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c >= 0x80U;
  };
  const std::size_t size = text.size();
  if (size == 0) return {0, 0};
  std::size_t at = std::min(cursor, size);
  // 光标可能停在词的**末尾之后**（编辑器里光标在字符间）——两种都要能取到词。
  if (at == size || !is_word(static_cast<unsigned char>(text[at]))) {
    if (at > 0 && is_word(static_cast<unsigned char>(text[at - 1]))) --at;
  }
  if (at >= size || !is_word(static_cast<unsigned char>(text[at]))) return {cursor, cursor};
  std::size_t begin = at;
  while (begin > 0 && is_word(static_cast<unsigned char>(text[begin - 1]))) --begin;
  std::size_t end = at;
  while (end < size && is_word(static_cast<unsigned char>(text[end]))) ++end;
  return {begin, end};
}

}  // namespace st::lsp
