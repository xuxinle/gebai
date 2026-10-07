#include "st/core/string.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace st {
namespace {

[[nodiscard]] constexpr auto is_ascii_space(char value) noexcept -> bool {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

}  // namespace

auto is_ascii(std::string_view text) noexcept -> bool {
  for (const char raw : text) {
    if (static_cast<unsigned char>(raw) >= 0x80U) return false;
  }
  return true;
}

auto ascii_lower(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    out.push_back(raw >= 'A' && raw <= 'Z' ? static_cast<char>(raw - 'A' + 'a') : raw);
  }
  return out;
}

auto ascii_upper(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    out.push_back(raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw);
  }
  return out;
}

// `lower/upper`：ASCII 快路径（与 ascii_* 同实现；不做 Unicode 语义折叠，见头文件注释）。
auto lower(std::string_view text) -> std::string { return ascii_lower(text); }

auto upper(std::string_view text) -> std::string { return ascii_upper(text); }

auto ascii_iequals(std::string_view a, std::string_view b) noexcept -> bool {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char left = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
    const char right = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
    if (left != right) return false;
  }
  return true;
}

auto ascii_starts_with_ci(std::string_view text, std::string_view prefix) noexcept -> bool {
  if (prefix.size() > text.size()) return false;
  return ascii_iequals(text.substr(0, prefix.size()), prefix);
}

auto trim_start(std::string_view text) -> std::string_view {
  std::size_t begin = 0;
  while (begin < text.size() && is_ascii_space(text[begin])) ++begin;
  return text.substr(begin);
}

auto trim_end(std::string_view text) -> std::string_view {
  std::size_t end = text.size();
  while (end > 0 && is_ascii_space(text[end - 1])) --end;
  return text.substr(0, end);
}

auto trim(std::string_view text) -> std::string_view {
  return trim_end(trim_start(text));
}

auto split(std::string_view text, char sep) -> std::vector<std::string_view> {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (true) {
    const std::size_t pos = text.find(sep, begin);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      break;
    }
    parts.push_back(text.substr(begin, pos - begin));
    begin = pos + 1;
  }
  return parts;
}

auto split_str(std::string_view text, std::string_view sep) -> std::vector<std::string_view> {
  std::vector<std::string_view> parts;
  if (sep.empty()) {
    parts.push_back(text);
    return parts;
  }
  std::size_t begin = 0;
  while (true) {
    const std::size_t pos = text.find(sep, begin);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      break;
    }
    parts.push_back(text.substr(begin, pos - begin));
    begin = pos + sep.size();
  }
  return parts;
}

auto split_whitespace(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> parts;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_ascii_space(text[index])) ++index;
    const std::size_t begin = index;
    while (index < text.size() && !is_ascii_space(text[index])) ++index;
    if (index > begin) parts.push_back(text.substr(begin, index - begin));
  }
  return parts;
}

auto join(const std::vector<std::string_view>& parts, std::string_view sep) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out.append(sep);
    out.append(parts[i]);
  }
  return out;
}

auto replace_all(std::string_view text, std::string_view from, std::string_view to) -> std::string {
  if (from.empty()) return std::string(text);
  std::string out;
  std::size_t begin = 0;
  while (true) {
    const std::size_t pos = text.find(from, begin);
    if (pos == std::string_view::npos) {
      out.append(text.substr(begin));
      break;
    }
    out.append(text.substr(begin, pos - begin));
    out.append(to);
    begin = pos + from.size();
  }
  return out;
}

namespace {

template <class T>
[[nodiscard]] auto parse_integral(std::string_view text) -> std::optional<T> {
  if (text.empty()) return std::nullopt;
  T value{};
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) return std::nullopt;
  return value;
}

}  // namespace

auto parse_i64(std::string_view text) -> std::optional<std::int64_t> {
  return parse_integral<std::int64_t>(text);
}

auto parse_u64(std::string_view text) -> std::optional<std::uint64_t> {
  return parse_integral<std::uint64_t>(text);
}

auto parse_f64(std::string_view text) -> std::optional<double> {
  if (text.empty()) return std::nullopt;
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  double value{};
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) return std::nullopt;
  return value;
}

auto parse_bool(std::string_view text) -> std::optional<bool> {
  const std::string lowered = ascii_lower(trim(text));
  if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") return true;
  if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") return false;
  return std::nullopt;
}

auto to_string(std::int64_t value) -> std::string { return std::format("{}", value); }
auto to_string(std::uint64_t value) -> std::string { return std::format("{}", value); }

auto to_string(double value) -> std::string {
  if (value == std::floor(value) && std::abs(value) < 1e15) {
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  return std::format("{:.6g}", value);
}

auto decode_utf8(std::string_view text, std::size_t& index) -> Codepoint {
  if (index >= text.size()) {
    index = text.size();
    return Codepoint{U'\0', 0};
  }
  const auto byte_at = [&text](std::size_t at) -> std::uint32_t {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[at]));
  };
  const std::uint32_t first = byte_at(index);
  if (first < 0x80U) {
    ++index;
    return Codepoint{static_cast<char32_t>(first), 1};
  }
  std::uint32_t needed = 0;
  std::uint32_t accumulator = 0;
  std::uint32_t minimum = 0;
  if ((first & 0xE0U) == 0xC0U) {
    needed = 1;
    accumulator = first & 0x1FU;
    minimum = 0x80U;
  } else if ((first & 0xF0U) == 0xE0U) {
    needed = 2;
    accumulator = first & 0x0FU;
    minimum = 0x800U;
  } else if ((first & 0xF8U) == 0xF0U) {
    needed = 3;
    accumulator = first & 0x07U;
    minimum = 0x10000U;
  } else {
    ++index;
    return Codepoint{U'\uFFFD', 1};
  }
  if (index + needed >= text.size() + 0 && text.size() - index - 1 < needed) {
    ++index;
    return Codepoint{U'\uFFFD', 1};
  }
  for (std::uint32_t step = 1; step <= needed; ++step) {
    const std::uint32_t continuation = byte_at(index + step);
    if ((continuation & 0xC0U) != 0x80U) {
      ++index;
      return Codepoint{U'\uFFFD', 1};
    }
    accumulator = (accumulator << 6U) | (continuation & 0x3FU);
  }
  if (accumulator < minimum || accumulator > 0x10FFFFU ||
      (accumulator >= 0xD800U && accumulator <= 0xDFFFU)) {
    ++index;
    return Codepoint{U'\uFFFD', 1};
  }
  index += static_cast<std::size_t>(needed) + 1;
  return Codepoint{static_cast<char32_t>(accumulator), static_cast<std::uint8_t>(needed + 1)};
}

auto encode_utf8(char32_t codepoint, std::string& out) -> void {
  const std::uint32_t value = static_cast<std::uint32_t>(codepoint);
  if (value < 0x80U) {
    out.push_back(static_cast<char>(value));
  } else if (value < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

auto utf8_encode(std::u32string_view codepoints) -> std::string {
  std::string out;
  out.reserve(codepoints.size());
  for (const char32_t codepoint : codepoints) encode_utf8(codepoint, out);
  return out;
}

auto utf8_decode(std::string_view text) -> std::u32string {
  std::u32string out;
  out.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    const Codepoint codepoint = decode_utf8(text, index);
    if (codepoint.bytes == 0) break;
    out.push_back(codepoint.value);
  }
  return out;
}

auto utf8_length(std::string_view text) noexcept -> std::size_t {
  std::size_t count = 0;
  for (const char raw : text) {
    if ((static_cast<unsigned char>(raw) & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

auto utf8_offset(std::string_view text, std::size_t index) noexcept -> std::size_t {
  std::size_t seen = 0;
  std::size_t offset = 0;
  while (offset < text.size() && seen < index) {
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) ++offset;
    ++seen;
  }
  return offset;
}

auto utf8_prev(std::string_view text, std::size_t index) noexcept -> std::size_t {
  if (index == 0U || text.empty()) return 0;
  // 光标在（或越过）文末：向左一步 = 最后一个码点的起点。
  std::size_t cursor = index >= text.size() ? text.size() - 1U : index - 1U;
  while (cursor > 0U && (static_cast<unsigned char>(text[cursor]) & 0xC0U) == 0x80U) --cursor;
  return cursor;
}

auto utf8_next(std::string_view text, std::size_t index) noexcept -> std::size_t {
  if (index + 1U > text.size()) return text.size();
  std::size_t cursor = index + 1U;
  while (cursor < text.size() && (static_cast<unsigned char>(text[cursor]) & 0xC0U) == 0x80U) ++cursor;
  return cursor;
}

auto utf8_slice(std::string_view text, std::size_t begin, std::size_t count) -> std::string_view {
  const std::size_t start = utf8_offset(text, begin);
  const std::size_t stop = utf8_offset(text, begin + count);
  return text.substr(start, stop - start);
}

auto utf8_is_valid(std::string_view text) noexcept -> bool {
  std::size_t index = 0;
  while (index < text.size()) {
    const std::size_t before = index;
    const Codepoint codepoint = decode_utf8(text, index);
    if (codepoint.value == U'\uFFFD' && codepoint.bytes == 1 &&
        static_cast<unsigned char>(text[before]) >= 0x80U) {
      return false;
    }
  }
  return true;
}

auto escape_control(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    if (byte >= 0x20U && byte < 0x7FU) {
      out.push_back(raw);
    } else if (raw == '\n') {
      out.append("\\n");
    } else if (raw == '\t') {
      out.append("\\t");
    } else if (raw == '\r') {
      out.append("\\r");
    } else {
      out.append(std::format("\\x{:02X}", static_cast<unsigned int>(byte)));
    }
  }
  return out;
}

}  // namespace st
