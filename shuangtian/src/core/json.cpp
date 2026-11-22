#include "st/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"

namespace st {

/// 见 `st/core/json_fwd.hpp` 的声明（为何返回引用而非按值，那里有完整理由）。
/// 与下面的 `null_node()` 同一模式：函数内 `static const`，常量、线程安全初始化。
auto empty_json_object() -> const Json& {
  static const Json instance = Json::object();
  return instance;
}
namespace {

/// 解析失败时的共享空节点（`json_path` 缺层时返回它）。
/// 函数内 `static const`：常量、线程安全初始化，不构成可变全局状态。
[[nodiscard]] auto null_node() noexcept -> const Json& {
  static const Json instance;
  return instance;
}

}  // namespace

auto json_parse(std::string_view text) -> Result<Json> {
  json_detail::ParseOptions options;
  options.max_depth = kJsonMaxDepth;
  return json_detail::parse(text, options);
}

auto json_parse_file(std::string_view path) -> Result<Json> {
  auto text = fs::read_text(path);
  if (!text) return forward_error(text.error());
  auto parsed = json_parse(*text);
  if (!parsed) {
    return unexpected(parsed.error().code,
                      std::format("{}（文件 {}）", parsed.error().message, path));
  }
  return parsed;
}

auto json_dump(const Json& value, bool pretty, std::uint32_t indent) -> std::string {
  std::string out;
  json_dump_to(out, value, pretty, indent);
  return out;
}

auto json_dump_to(std::string& out, const Json& value, bool pretty, std::uint32_t indent) -> void {
  json_detail::dump_to(out, value, pretty ? indent : 0U, /*ensure_ascii=*/false);
}

auto json_write_file(std::string_view path, const Json& value, bool pretty) -> Status {
  return fs::write_text(path, json_dump(value, pretty));
}

// `json_find` / `json_at` / `json_get_*` 定义在头文件（**热路径，需要内联**）：
// 见 json.hpp —— 实测内联后字面量键能走比较特化。

auto json_get_string_array(const Json& value, std::string_view key) -> std::vector<std::string> {
  std::vector<std::string> out;
  const Json& node = json_at(value, key);
  if (!node.is_array()) return out;
  out.reserve(node.size());
  for (const Json& item : node) {
    if (item.is_string()) out.push_back(item.as_string());
  }
  return out;
}

auto json_path(const Json& value, std::string_view dotted_path) noexcept -> const Json& {
  const Json* current = &value;
  std::size_t begin = 0;
  while (begin <= dotted_path.size()) {
    const std::size_t dot = dotted_path.find('.', begin);
    const std::string_view segment =
        dot == std::string_view::npos ? dotted_path.substr(begin) : dotted_path.substr(begin, dot - begin);
    if (!segment.empty()) {
      current = json_find(*current, segment);
      if (current == nullptr) return null_node();
    }
    if (dot == std::string_view::npos) break;
    begin = dot + 1;
  }
  return *current;
}

auto json_as_string(const Json& value, std::string_view fallback) -> std::string {
  if (value.is_string()) return value.as_string();
  // 数字/布尔也允许按文本读取（配置里 `version: 1.0` 这类写法很常见）
  if (value.is_number_integer()) {
    return value.is_number_unsigned() ? std::to_string(value.as_u64()) : std::to_string(value.as_i64());
  }
  if (value.is_number_float()) return std::format("{}", value.as_double());
  if (value.is_boolean()) return value.as_bool() ? "true" : "false";
  return std::string(fallback);
}

auto json_as_i64(const Json& value, std::int64_t fallback) -> std::int64_t {
  if (value.is_number_integer()) {
    if (value.is_number_unsigned()) {
      const std::uint64_t raw = value.as_u64();
      return raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                 ? static_cast<std::int64_t>(raw)
                 : fallback;
    }
    return value.as_i64();
  }
  if (value.is_number_float()) {
    const double raw = value.as_double();
    // 只接受"无小数部分"的浮点：有小数就说明调用方读错了字段，别静默截断
    if (raw == std::floor(raw) && std::abs(raw) < 9.0e18) return static_cast<std::int64_t>(raw);
    return fallback;
  }
  if (value.is_string()) {
    const std::string& text = value.as_string();
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stoll(text, &consumed);
      return consumed == text.size() ? parsed : fallback;
    } catch (...) {
      return fallback;
    }
  }
  if (value.is_boolean()) return value.as_bool() ? 1 : 0;
  return fallback;
}

auto json_as_double(const Json& value, double fallback) -> double {
  if (value.is_number()) return value.as_double();
  if (value.is_string()) {
    const std::string& text = value.as_string();
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stod(text, &consumed);
      return consumed == text.size() ? parsed : fallback;
    } catch (...) {
      return fallback;
    }
  }
  if (value.is_boolean()) return value.as_bool() ? 1.0 : 0.0;
  return fallback;
}

auto json_as_bool(const Json& value, bool fallback) -> bool {
  if (value.is_boolean()) return value.as_bool();
  if (value.is_number()) return value.as_double() != 0.0;
  if (value.is_string()) {
    const std::string& text = value.as_string();
    if (text == "true" || text == "yes" || text == "on" || text == "1") return true;
    if (text == "false" || text == "no" || text == "off" || text == "0") return false;
  }
  return fallback;
}

}  // namespace st
