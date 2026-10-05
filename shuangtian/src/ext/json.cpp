#include "st/ext/json.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"

namespace st {

/// 见 `st/ext/json_fwd.hpp` 的声明（为何返回引用而非按值，那里有完整理由）。
/// 与下面的 `null_node()` 同一模式：函数内 `static const`，常量、线程安全初始化。
auto empty_json_object() -> const Json& {
  static const Json instance = Json::object();
  return instance;
}
namespace {

/// 解析失败时的共享空节点（`json_at`/`json_path` 缺键时返回它）。
/// 函数内 `static const`：常量、线程安全初始化，不构成可变全局状态。
[[nodiscard]] auto null_node() noexcept -> const Json& {
  static const Json instance = Json();
  return instance;
}

/// 键查找（仅对象有意义；`nlohmann::json::find` 对非对象返回 `end()`）。
[[nodiscard]] auto find_in(const Json& value, std::string_view key) -> Json::const_iterator {
  if (!value.is_object()) return value.end();
  return value.find(key);
}

}  // namespace

/// 解析前的**嵌套深度预检**（线性扫描，字符串与转义内的括号不算）。
///
/// 为什么需要：nlohmann 的递归下降解析器对极深嵌套（如 10 万个 `[`）会**爆栈崩溃**——
/// 这不是"拒绝一份畸形输入"，而是整个进程被强杀（DoS 面）。旧自研实现有 128 层上限，
/// 换成 nlohmann 后该防护会静默消失，故在此显式补回。
[[nodiscard]] auto depth_within_limit(std::string_view text, std::uint32_t limit) noexcept -> bool {
  std::uint32_t depth = 0;
  bool in_string = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char raw = text[index];
    if (in_string) {
      if (raw == '\\') {
        ++index;  // 跳过被转义的下一个字符
        continue;
      }
      if (raw == '"') in_string = false;
      continue;
    }
    if (raw == '"') {
      in_string = true;
      continue;
    }
    if (raw == '{' || raw == '[') {
      ++depth;
      if (depth > limit) return false;
      continue;
    }
    if (raw == '}' || raw == ']') {
      if (depth > 0) --depth;
    }
  }
  return true;
}

auto json_parse(std::string_view text) -> Result<Json> {
  if (!depth_within_limit(text, kJsonMaxDepth)) {
    return unexpected(ErrorCode::Parse,
                      std::format("JSON 嵌套层数超过上限（{}）", kJsonMaxDepth));
  }
  // `allow_exceptions=false`：解析失败返回 discarded 值而不是抛异常。
  //
  // 但该模式**不产生诊断信息**（只有"失败了"），排障时等于丢了字节位置。
  // 因此这里再跑一次抛出式解析来取 `parse_error::byte` 位置——用 try/catch 接住并转成
  // `Result`。这是本框架里少数"捕获第三方异常"的正当场景：CONVENTIONS §3.1 禁的是
  // **用异常做业务错误**，而不是禁止在边界处把第三方异常翻译成 Result。
  Json parsed = Json::parse(text.begin(), text.end(), /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_discarded()) return parsed;
  try {
    Json diagnostic = Json::parse(text.begin(), text.end());
    (void)diagnostic;  // 第二次解析"成功"说明前一次是被回调截断，按语法错误处理
  } catch (const Json::parse_error& error) {
    return unexpected(ErrorCode::Parse,
                      std::format("JSON 解析失败（第 {} 字节）: {}", error.byte, error.what()));
  } catch (const std::exception& error) {
    return unexpected(ErrorCode::Parse, std::format("JSON 解析失败: {}", error.what()));
  }
  return unexpected(ErrorCode::Parse, "JSON 解析失败（语法错误）");
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
  // `error_handler_t::replace`：非法 UTF-8 用 U+FFFD 替代，而不是抛 type_error——
  // 序列化路径必须"无论内存里是什么字节都能出结果"（控制通道回包不能被一个坏字符串打断）。
  const auto handler = Json::error_handler_t::replace;
  const int indent_width = pretty ? static_cast<int>(indent) : -1;
  const char indent_char = ' ';
  if (pretty) {
    out.append(value.dump(indent_width, indent_char, /*ensure_ascii=*/false, handler));
  } else {
    out.append(value.dump(/*indent=*/-1, indent_char, /*ensure_ascii=*/false, handler));
  }
}

auto json_write_file(std::string_view path, const Json& value, bool pretty) -> Status {
  return fs::write_text(path, json_dump(value, pretty));
}

auto json_find(const Json& value, std::string_view key) noexcept -> const Json* {
  const auto iterator = find_in(value, key);
  if (iterator == value.end()) return nullptr;
  return &(*iterator);
}

auto json_at(const Json& value, std::string_view key) noexcept -> const Json& {
  const Json* found = json_find(value, key);
  return found != nullptr ? *found : null_node();
}

auto json_get_string(const Json& value, std::string_view key, std::string_view fallback) -> std::string {
  return json_as_string(json_at(value, key), fallback);
}

auto json_get_i64(const Json& value, std::string_view key, std::int64_t fallback) -> std::int64_t {
  return json_as_i64(json_at(value, key), fallback);
}

auto json_get_double(const Json& value, std::string_view key, double fallback) -> double {
  return json_as_double(json_at(value, key), fallback);
}

auto json_get_bool(const Json& value, std::string_view key, bool fallback) -> bool {
  return json_as_bool(json_at(value, key), fallback);
}

auto json_get_string_array(const Json& value, std::string_view key) -> std::vector<std::string> {
  std::vector<std::string> out;
  const Json& node = json_at(value, key);
  if (!node.is_array()) return out;
  out.reserve(node.size());
  for (const Json& item : node) {
    if (item.is_string()) out.push_back(item.get<std::string>());
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
  if (value.is_string()) return value.get<std::string>();
  // 数字/布尔也允许按文本读取（配置里 `version: 1.0` 这类写法很常见）
  if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
  if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
  if (value.is_number_float()) return std::format("{}", value.get<double>());
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  return std::string(fallback);
}

auto json_as_i64(const Json& value, std::int64_t fallback) -> std::int64_t {
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    return raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
               ? static_cast<std::int64_t>(raw)
               : fallback;
  }
  if (value.is_number_float()) {
    const double raw = value.get<double>();
    // 只接受"无小数部分"的浮点：有小数就说明调用方读错了字段，别静默截断
    if (raw == std::floor(raw) && std::abs(raw) < 9.0e18) return static_cast<std::int64_t>(raw);
    return fallback;
  }
  if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stoll(text, &consumed);
      return consumed == text.size() ? parsed : fallback;
    } catch (...) {
      return fallback;
    }
  }
  if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
  return fallback;
}

auto json_as_double(const Json& value, double fallback) -> double {
  if (value.is_number()) return value.get<double>();
  if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stod(text, &consumed);
      return consumed == text.size() ? parsed : fallback;
    } catch (...) {
      return fallback;
    }
  }
  if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
  return fallback;
}

auto json_as_bool(const Json& value, bool fallback) -> bool {
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number()) return value.get<double>() != 0.0;
  if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    if (text == "true" || text == "yes" || text == "on" || text == "1") return true;
    if (text == "false" || text == "no" || text == "off" || text == "0") return false;
  }
  return fallback;
}

}  // namespace st
