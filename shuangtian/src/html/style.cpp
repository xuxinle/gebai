#include "st/html/style.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::html {

namespace {

[[nodiscard]] auto is_space(char ch) noexcept -> bool {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

[[nodiscard]] auto trim_view(std::string_view text) -> std::string_view {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

[[nodiscard]] auto lower_ascii(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
  }
  return out;
}

[[nodiscard]] auto split_ws_into_words(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> words;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_space(text[index])) ++index;
    const std::size_t begin = index;
    while (index < text.size() && !is_space(text[index])) ++index;
    if (index > begin) words.emplace_back(text.substr(begin, index - begin));
  }
  return words;
}

// ————————————————————————————————————————————————————————————————————————————
// 取值解析
// ————————————————————————————————————————————————————————————————————————————

/// 长度：`"12"` / `"12px"` / `"12.5px"` / `"0"`。`em`/`%` 由调用方给基准换算。
[[nodiscard]] auto parse_length(std::string_view text, float em_base, float percent_base)
    -> std::optional<float> {
  const std::string_view value = trim_view(text);
  if (value.empty()) return std::nullopt;
  if (value == "auto" || value == "none" || value == "normal") return std::nullopt;
  std::size_t index = 0;
  bool negative = false;
  if (value[index] == '-' || value[index] == '+') {
    negative = value[index] == '-';
    ++index;
  }
  std::size_t digits = 0;
  double number = 0.0;
  while (index < value.size() && ((value[index] >= '0' && value[index] <= '9') || value[index] == '.')) {
    if (value[index] == '.') {
      ++index;
      double scale = 0.1;
      while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
        number += static_cast<double>(value[index] - '0') * scale;
        scale *= 0.1;
        ++index;
      }
      break;
    }
    number = number * 10.0 + static_cast<double>(value[index] - '0');
    ++index;
    ++digits;
  }
  if (digits == 0 && index < value.size() && value[index] == '.') return std::nullopt;
  const std::string_view unit = value.substr(index);
  float result = static_cast<float>(number);
  if (unit == "px" || unit.empty()) {
    // 无单位在 CSS 里只对 0 合法；这里宽松处理（网页里 `width:100` 很常见）。
  } else if (unit == "em" || unit == "rem") {
    result *= em_base > 0.0f ? em_base : 16.0f;
  } else if (unit == "%") {
    if (percent_base <= 0.0f) return std::nullopt;
    result = static_cast<float>(number) / 100.0f * percent_base;
  } else if (unit == "pt") {
    result *= 96.0f / 72.0f;
  } else {
    return std::nullopt;   // vw/vh/ch/ex… 不支持（明确忽略）
  }
  return negative ? -result : result;
}

/// 颜色：`#rgb` / `#rrggbb` / `#rrggbbaa` / `rgb()` / `rgba()` / 常见命名色 / `transparent`。
[[nodiscard]] auto parse_color(std::string_view text) -> std::optional<math::Color> {
  const std::string_view value = trim_view(text);
  if (value.empty()) return std::nullopt;
  const std::string lower = lower_ascii(value);

  if (lower == "transparent") return math::Color{0, 0, 0, 0};
  if (lower == "currentcolor" || lower == "inherit") return std::nullopt;

  if (lower[0] == '#') {
    const std::string_view hex = value.substr(1);
    const auto digit = [](char ch) -> int {
      if (ch >= '0' && ch <= '9') return ch - '0';
      if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
      return -1;
    };
    if (hex.size() == 3U || hex.size() == 4U) {
      int values[4] = {0, 0, 0, 15};
      for (std::size_t index = 0; index < hex.size(); ++index) {
        const int high = digit(hex[index]);
        if (high < 0) return std::nullopt;
        values[index] = high * 16 + high;
      }
      return math::Color{static_cast<std::uint8_t>(values[0]), static_cast<std::uint8_t>(values[1]),
                         static_cast<std::uint8_t>(values[2]),
                         static_cast<std::uint8_t>(values[3])};
    }
    if (hex.size() == 6U || hex.size() == 8U) {
      int values[8] = {0};
      for (std::size_t index = 0; index < hex.size(); ++index) {
        const int high = digit(hex[index]);
        if (high < 0) return std::nullopt;
        values[index] = high;
      }
      const auto pair_at = [&values](std::size_t at) -> std::uint8_t {
        return static_cast<std::uint8_t>(values[at] * 16 + values[at + 1]);
      };
      return math::Color{pair_at(0), pair_at(2), pair_at(4),
                         hex.size() == 8U ? pair_at(6) : static_cast<std::uint8_t>(255)};
    }
    return std::nullopt;
  }

  if (lower.rfind("rgb", 0) == 0) {
    const std::size_t open = lower.find('(');
    const std::size_t close = lower.find(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
      return std::nullopt;
    }
    const std::string_view body = lower.substr(open + 1, close - open - 1);
    std::vector<float> channel;
    std::size_t index = 0;
    while (index < body.size()) {
      while (index < body.size() && (is_space(body[index]) || body[index] == ',')) ++index;
      const std::size_t begin = index;
      while (index < body.size() && !is_space(body[index]) && body[index] != ',') ++index;
      if (index > begin) {
        const auto number = parse_length(body.substr(begin, index - begin), 0.0f, 0.0f);
        if (!number.has_value()) return std::nullopt;
        channel.push_back(*number);
      }
    }
    if (channel.size() < 3U) return std::nullopt;
    const auto to_byte = [](float channel_value) -> std::uint8_t {
      return static_cast<std::uint8_t>(std::clamp(channel_value, 0.0f, 255.0f) + 0.5f);
    };
    const float alpha = channel.size() >= 4U ? std::clamp(channel[3], 0.0f, 1.0f) * 255.0f : 255.0f;
    return math::Color{to_byte(channel[0]), to_byte(channel[1]), to_byte(channel[2]),
                       to_byte(alpha)};
  }

  struct Named {
    std::string_view name;
    std::uint32_t rgb;
  };
  static constexpr Named kNamed[] = {
      {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},   {"green", 0x008000},
      {"blue", 0x0000FF},  {"gray", 0x808080},  {"grey", 0x808080},  {"silver", 0xC0C0C0},
      {"yellow", 0xFFFF00}, {"orange", 0xFFA500}, {"purple", 0x800080}, {"pink", 0xFFC0CB},
      {"brown", 0xA52A2A}, {"cyan", 0x00FFFF},  {"magenta", 0xFF00FF}, {"lime", 0x00FF00},
      {"navy", 0x000080},  {"teal", 0x008080},  {"olive", 0x808000},  {"maroon", 0x800000},
      {"gold", 0xFFD700},  {"coral", 0xFF7F50}, {"salmon", 0xFA8072}, {"crimson", 0xDC143C},
      {"tomato", 0xFF6347}, {"khaki", 0xF0E68C}, {"indigo", 0x4B0082}, {"violet", 0xEE82EE},
      {"beige", 0xF5F5DC}, {"ivory", 0xFFFFF0}, {"silver", 0xC0C0C0}, {"whitesmoke", 0xF5F5F5},
      {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"darkgray", 0xA9A9A9},
      {"darkgrey", 0xA9A9A9}, {"dodgerblue", 0x1E90FF}, {"steelblue", 0x4682B4},
      {"skyblue", 0x87CEEB}, {"royalblue", 0x4169E1}, {"seagreen", 0x2E8B57},
      {"forestgreen", 0x228B22}, {"darkgreen", 0x006400}, {"darkred", 0x8B0000},
      {"darkblue", 0x00008B}, {"rebeccapurple", 0x663399}, {"grey", 0x808080},
      {"transparent", 0x000000},
  };
  for (const Named& named : kNamed) {
    if (lower == named.name) {
      return math::Color{static_cast<std::uint8_t>((named.rgb >> 16U) & 0xFFU),
                         static_cast<std::uint8_t>((named.rgb >> 8U) & 0xFFU),
                         static_cast<std::uint8_t>(named.rgb & 0xFFU), 255};
    }
  }
  return std::nullopt;
}

/// 一行/两行/四行的边距简写（`4px` / `4px 8px` / `4px 8px 4px 8px`）。
struct Edges {
  float top{0.0f};
  float right{0.0f};
  float bottom{0.0f};
  float left{0.0f};
};

[[nodiscard]] auto parse_edges(std::string_view text, float em_base, float percent_base)
    -> std::optional<Edges> {
  std::vector<float> values;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_space(text[index])) ++index;
    const std::size_t begin = index;
    while (index < text.size() && !is_space(text[index])) ++index;
    if (index == begin) break;
    const auto value = parse_length(text.substr(begin, index - begin), em_base, percent_base);
    if (!value.has_value()) return std::nullopt;
    values.push_back(*value);
  }
  if (values.empty()) return std::nullopt;
  Edges edges{};
  switch (values.size()) {
    case 1U: edges = Edges{values[0], values[0], values[0], values[0]}; break;
    case 2U: edges = Edges{values[0], values[1], values[0], values[1]}; break;
    case 3U: edges = Edges{values[0], values[1], values[2], values[1]}; break;
    default: edges = Edges{values[0], values[1], values[2], values[3]}; break;
  }
  return edges;
}

/// 边框简写（`1px solid #ccc` / `none` / `2px dashed red`）→ 宽度 + 颜色。
struct Border {
  float width{0.0f};
  math::Color color{0, 0, 0, 0};
  bool has_width{false};
};

[[nodiscard]] auto parse_border(std::string_view text, float em_base) -> Border {
  Border border{};
  const auto words = split_ws_into_words(text);
  for (const std::string& word : words) {
    const std::string lower = lower_ascii(word);
    if (lower == "none" || lower == "hidden") {
      border.width = 0.0f;
      border.has_width = true;
      continue;
    }
    if (lower == "solid" || lower == "dashed" || lower == "dotted" || lower == "double") continue;
    // ⚠ **先判颜色，再判长度**——顺序反了会把 `#2563eb` 当宽度：
    // `parse_length` 会在开头跳过非数字、把后续 `2563` 当成数值（`#` 被跳过后
    // `2563eb` 的 `2563` 可解析），于是 `border-left: 4px solid #2563eb` 的宽度
    // 变成 2563px——画面上是一块盖住半屏的色块。实测就踩了这个。
    if (const auto color = parse_color(word); color.has_value()) {
      border.color = *color;
      continue;
    }
    // 颜色字面量（`#` 开头 / `rgb(` / 命名色）绝不当作长度。
    if (!word.empty() && (word[0] == '#' || lower.rfind("rgb", 0) == 0)) continue;
    if (const auto width = parse_length(word, em_base, 0.0f); width.has_value()) {
      border.width = *width;
      border.has_width = true;
    }
  }
  return border;
}

/// 选择器特异性（id×100 + class×10 + tag×1）。
[[nodiscard]] auto specificity_of(const std::vector<CompoundSelector>& parts) noexcept -> int {
  int total = 0;
  for (const CompoundSelector& part : parts) {
    if (!part.id.empty()) total += 100;
    total += static_cast<int>(part.classes.size()) * 10;
    if (!part.tag.empty() && part.tag != "*") total += 1;
  }
  return total;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// CSS 解析
// ————————————————————————————————————————————————————————————————————————————

auto parse_inline_style(std::string_view text) -> std::vector<Declaration> {
  std::vector<Declaration> declarations;
  std::size_t index = 0;
  while (index < text.size()) {
    const std::size_t semi = text.find(';', index);
    const std::string_view chunk =
        semi == std::string_view::npos ? text.substr(index) : text.substr(index, semi - index);
    index = semi == std::string_view::npos ? text.size() : semi + 1;
    const std::size_t colon = chunk.find(':');
    if (colon == std::string_view::npos) continue;
    Declaration declaration{};
    declaration.property = lower_ascii(trim_view(chunk.substr(0, colon)));
    declaration.value = std::string(trim_view(chunk.substr(colon + 1)));
    if (declaration.property.empty() || declaration.value.empty()) continue;
    // `!important` 后缀
    const std::string lower_value = lower_ascii(declaration.value);
    const std::size_t bang = lower_value.find("!important");
    if (bang != std::string::npos) {
      declaration.important = true;
      declaration.value = std::string(trim_view(declaration.value.substr(0, bang)));
    }
    declarations.push_back(std::move(declaration));
  }
  return declarations;
}

namespace {

/// 解析一个复合选择器（`div.card#main` / `*` / `.a.b`）。
[[nodiscard]] auto parse_compound(std::string_view text) -> std::optional<CompoundSelector> {
  CompoundSelector compound{};
  std::size_t index = 0;
  // 开头的 tag / `*`
  const std::size_t tag_begin = index;
  while (index < text.size() && text[index] != '.' && text[index] != '#' && text[index] != ':' &&
         text[index] != '[') {
    ++index;
  }
  if (index > tag_begin) {
    compound.tag = lower_ascii(trim_view(text.substr(tag_begin, index - tag_begin)));
    if (compound.tag.empty()) return std::nullopt;
  }
  while (index < text.size()) {
    const char kind = text[index];
    if (kind == ':' || kind == '[') return std::nullopt;   // 伪类/属性选择器：明确不支持
    ++index;
    const std::size_t begin = index;
    while (index < text.size() && text[index] != '.' && text[index] != '#' && text[index] != ':' &&
           text[index] != '[') {
      ++index;
    }
    const std::string name(trim_view(text.substr(begin, index - begin)));
    if (name.empty()) return std::nullopt;
    if (kind == '.') {
      compound.classes.push_back(lower_ascii(name));
    } else if (kind == '#') {
      // CSS 的 id 是**大小写敏感**的；这里保守按原样保存。
      compound.id = std::string(name);
    } else {
      return std::nullopt;
    }
  }
  if (compound.tag.empty() && compound.id.empty() && compound.classes.empty()) return std::nullopt;
  return compound;
}

/// 解析一个选择器（含空格与 `>` 组合符）。
[[nodiscard]] auto parse_selector(std::string_view text) -> std::optional<Selector> {
  Selector selector{};
  std::size_t index = 0;
  bool expect_child = false;
  while (index < text.size()) {
    while (index < text.size() && is_space(text[index])) ++index;
    if (index >= text.size()) break;
    if (text[index] == '>') {
      expect_child = true;
      ++index;
      continue;
    }
    const std::size_t begin = index;
    while (index < text.size() && !is_space(text[index]) && text[index] != '>') ++index;
    const auto compound = parse_compound(text.substr(begin, index - begin));
    if (!compound.has_value()) return std::nullopt;
    if (!selector.parts.empty()) selector.child_combinator.push_back(expect_child);
    expect_child = false;
    selector.parts.push_back(*compound);
  }
  if (selector.parts.empty()) return std::nullopt;
  if (selector.child_combinator.size() + 1U != selector.parts.size()) return std::nullopt;
  selector.specificity = specificity_of(selector.parts);
  return selector;
}

}  // namespace

namespace {

/// 一帧的停点：`from` / `to` / `50%`（返回值夹到 `[0,1]`）。
[[nodiscard]] auto parse_keyframe_offset(std::string_view text) -> std::optional<float> {
  const std::string_view value = trim_view(text);
  if (value.empty()) return std::nullopt;
  if (value == "from") return 0.0f;
  if (value == "to") return 1.0f;
  // ⚠ **不能借 `parse_length` 解百分比**：它对 `percent_base == 0` 直接返回 `nullopt`
  //（那条分支的存在理由是“百分比宽高要容器尺寸”）。而关键帧停点 `50%` 的百分比
  // 基数是**1**（它本身就是比例），所以这里自己读数。
  if (value.back() != '%') return std::nullopt;
  const auto number = parse_length(value.substr(0, value.size() - 1), 0.0f, 0.0f);
  if (!number.has_value()) return std::nullopt;
  return std::clamp(*number / 100.0f, 0.0f, 1.0f);
}

/// 把一条声明写进关键帧（**只认可动画的几项**，其余忽略）。
void apply_keyframe_declaration(KeyframeSpec& frame, std::string_view property,
                               std::string_view value) {
  const std::string_view raw = trim_view(value);
  if (raw.empty()) return;
  if (property == "opacity") {
    if (const auto number = parse_length(raw, 0.0f, 0.0f); number.has_value()) {
      frame.opacity = std::clamp(*number, 0.0f, 1.0f);
    }
    return;
  }
  if (property == "background" || property == "background-color") {
    for (const std::string& word : split_ws_into_words(raw)) {
      if (const auto color = parse_color(word); color.has_value()) {
        frame.background = *color;
        return;
      }
    }
    return;
  }
  if (property == "color") {
    if (const auto color = parse_color(raw); color.has_value()) frame.color = *color;
    return;
  }
  if (property == "border-radius") {
    if (const auto number = parse_length(raw, 0.0f, 0.0f); number.has_value()) {
      frame.radius = *number;
    }
    return;
  }
  // `transform`：只接这四种最常见写法（`translateX/Y`、`translate`、`scale`）。
  // 完整 transform 语法（矩阵/旋转/斜切）不接：本层没有变换引擎，
  // 而那些写法在展示型网页里占比极低。
  if (property != "transform") return;
  const auto words = split_ws_into_words(raw);
  for (const std::string& word : words) {
    const std::string lower = lower_ascii(word);
    const auto number_in = [&lower](std::string_view prefix) -> std::optional<float> {
      if (lower.rfind(prefix, 0) != 0) return std::nullopt;
      // ⚠ `prefix` 本身已含 `(`（如 `"translatex("`）：从这里往后找 `(` 永远找不到，
      // 于是所有 transform 函数静默失败——实测症状是“关键帧解析出来了，但位移是 0”。
      const std::size_t open = prefix.size() - 1U;
      const std::size_t close = lower.rfind(')');
      if (open == std::string::npos || close == std::string::npos || close <= open) {
        return std::nullopt;
      }
      const std::string_view body =
          std::string_view(lower).substr(open + 1, close - open - 1);
      std::size_t cursor = 0;
      while (cursor < body.size() && is_space(body[cursor])) ++cursor;
      std::size_t end = cursor;
      while (end < body.size() && !is_space(body[end]) && body[end] != ',') ++end;
      return parse_length(body.substr(cursor, end - cursor), 0.0f, 0.0f);
    };
    if (const auto value_x = number_in("translatex("); value_x.has_value()) {
      frame.translate_x = *value_x;
      continue;
    }
    if (const auto value_y = number_in("translatey("); value_y.has_value()) {
      frame.translate_y = *value_y;
      continue;
    }
    if (const auto value_scale = number_in("scale("); value_scale.has_value()) {
      frame.scale = *value_scale;
      continue;
    }
    if (const auto value_scale = number_in("scalex("); value_scale.has_value()) {
      frame.scale = *value_scale;   // 等比近似：本层不做非等比缩放
      continue;
    }
    if (const auto value_scale = number_in("scaley("); value_scale.has_value()) {
      frame.scale = *value_scale;
      continue;
    }
    if (const auto value_translate = number_in("translate("); value_translate.has_value()) {
      // `translate(x, y)`：第一个分量当 X；Y 只在 x 为 0 时单独取（与 CSS 简写同义）。
      frame.translate_x = *value_translate;
      if (std::abs(*value_translate) < 1e-6F) {
        frame.translate_x = std::nullopt;
      }
    }
  }
}

/// 时间值：`0.3s` / `300ms` / `0`。
[[nodiscard]] auto parse_seconds(std::string_view text) -> std::optional<double> {
  const std::string_view value = trim_view(text);
  if (value.empty()) return std::nullopt;
  if (value.ends_with("ms")) {
    const auto number = parse_length(value.substr(0, value.size() - 2), 0.0f, 0.0f);
    if (!number.has_value()) return std::nullopt;
    return static_cast<double>(*number) / 1000.0;
  }
  if (value.ends_with("s")) {
    const auto number = parse_length(value.substr(0, value.size() - 1), 0.0f, 0.0f);
    if (!number.has_value()) return std::nullopt;
    return static_cast<double>(*number);
  }
  const auto number = parse_length(value, 0.0f, 0.0f);
  if (!number.has_value()) return std::nullopt;
  // **无单位只认 `0`**：CSS 里 `animation: 2 fadeIn` 的 `2` 是循环次数，
  // 不是“2 秒”——无条件接受无单位时间会把次数吃成时长（实测踩到）。
  if (*number != 0.0F) return std::nullopt;
  return 0.0;
}

}  // namespace

auto StyleSheet::parse(std::string_view css) -> StyleSheet {
  StyleSheet sheet{};
  std::size_t index = 0;
  std::size_t order = 0;
  while (index < css.size()) {
    // 跳过空白与注释（注释已在 `clean_style_text` 去过，这里兜底 `//` 行注释）。
    while (index < css.size() && is_space(css[index])) ++index;
    if (index >= css.size()) break;
    // `@keyframes` → 收集成 `KeyframesSpec`；其余 at-rule 整段跳过（不猜语义）。
    if (css[index] == '@') {
      const std::size_t open = css.find('{', index);
      const std::size_t semi = css.find(';', index);
      if (semi != std::string_view::npos &&
          (open == std::string_view::npos || semi < open)) {
        index = semi + 1;
        continue;
      }
      if (open == std::string_view::npos) break;
      // 找到配对的 `}`
      int depth = 0;
      std::size_t at = open;
      for (; at < css.size(); ++at) {
        if (css[at] == '{') ++depth;
        if (css[at] == '}') {
          --depth;
          if (depth == 0) break;
        }
      }
      const std::string_view head = trim_view(css.substr(index, open - index));
      const std::string_view body = at < css.size() ? css.substr(open + 1, at - open - 1)
                                                    : std::string_view{};
      // 前缀写法（`-webkit-keyframes`）不认：只接标准 `@keyframes`。
      if (head.rfind("@keyframes", 0) == 0) {
        KeyframesSpec spec{};
        spec.name = std::string(trim_view(head.substr(10)));
        if (!spec.name.empty()) {
          std::size_t cursor = 0;
          while (cursor < body.size()) {
            const std::size_t frame_open = body.find('{', cursor);
            if (frame_open == std::string_view::npos) break;
            const std::size_t frame_close = body.find('}', frame_open);
            if (frame_close == std::string_view::npos) break;
            const std::string_view stops = body.substr(cursor, frame_open - cursor);
            const std::string_view declarations =
                body.substr(frame_open + 1, frame_close - frame_open - 1);
            cursor = frame_close + 1;
            // 一帧可以列多个停点：`0%, 100% { … }` —— 展开成多个同值帧
            //（否则 `shake` 那种写法只有最后一个停点生效，动效完全不一样）。
            std::size_t stop_at = 0;
            while (stop_at <= stops.size()) {
              const std::size_t comma = stops.find(',', stop_at);
              const std::string_view piece = comma == std::string_view::npos
                                                 ? stops.substr(stop_at)
                                                 : stops.substr(stop_at, comma - stop_at);
              const auto offset = parse_keyframe_offset(piece);
              if (offset.has_value()) {
                KeyframeSpec frame{};
                frame.offset = *offset;
                for (const Declaration& declaration : parse_inline_style(declarations)) {
                  apply_keyframe_declaration(frame, declaration.property, declaration.value);
                }
                spec.frames.push_back(std::move(frame));
              }
              if (comma == std::string_view::npos) break;
              stop_at = comma + 1;
            }
          }
        }
        if (!spec.name.empty() && !spec.frames.empty()) {
          // 同名后者覆盖：CSS 里后定义的关键帧赢。
          std::erase_if(sheet.keyframes,
                        [&spec](const KeyframesSpec& existing) { return existing.name == spec.name; });
          sheet.keyframes.push_back(std::move(spec));
        }
      }
      index = at < css.size() ? at + 1 : css.size();
      continue;
    }

    const std::size_t open = css.find('{', index);
    if (open == std::string_view::npos) break;
    const std::size_t close = css.find('}', open);
    if (close == std::string_view::npos) break;

    const std::string_view selector_text = css.substr(index, open - index);
    const std::string_view body_text = css.substr(open + 1, close - open - 1);
    index = close + 1;

    Rule rule{};
    rule.order = order++;
    // 选择器列表（逗号分隔；逐个解析，任一条无效就丢那一条——但不丢整组）。
    std::size_t at = 0;
    while (at <= selector_text.size()) {
      const std::size_t comma = selector_text.find(',', at);
      const std::string_view piece = comma == std::string_view::npos
                                         ? selector_text.substr(at)
                                         : selector_text.substr(at, comma - at);
      if (const auto parsed = parse_selector(trim_view(piece)); parsed.has_value()) {
        rule.selectors.push_back(*parsed);
      }
      if (comma == std::string_view::npos) break;
      at = comma + 1;
    }
    rule.declarations = parse_inline_style(body_text);
    if (!rule.selectors.empty() && !rule.declarations.empty()) {
      sheet.rules.push_back(std::move(rule));
    }
  }
  return sheet;
}

// ————————————————————————————————————————————————————————————————————————————
// 用户代理默认样式
// ————————————————————————————————————————————————————————————————————————————

auto StyleResolver::initial_style() -> Style {
  Style style{};
  style.color = math::Color{17, 17, 17, 255};
  style.background = math::Color{255, 255, 255, 255};
  style.has_background = true;
  style.font_size = 16.0f;
  style.font_weight = 400;
  style.line_height = 1.45f;
  style.display = Display::Block;
  style.white_space = WhiteSpace::Normal;
  return style;
}

/// 浏览器 UA 默认样式的**那一小部分会明显影响观感的**（标题字号/段落边距/列表缩进）。
///
/// 为什么必须有：没有 UA 样式，`<h1>` 与 `<p>` 会长得一模一样——一份
/// 「有标题有正文」的网页会被渲染成一片等大的文字，用户看到的是「预览坏了」。
auto StyleResolver::user_agent_style(std::string_view tag) -> Style {
  Style style{};
  style.display = Display::Inline;

  // 标题：字号阶梯 + 粗体 + 上下边距（数值取浏览器常见档位）。
  struct HeadingSpec {
    std::string_view tag;
    float size;
    float margin;
  };
  static constexpr HeadingSpec kHeadings[] = {{"h1", 32.0f, 0.67f}, {"h2", 24.0f, 0.83f},
                                              {"h3", 18.72f, 1.0f}, {"h4", 16.0f, 1.33f},
                                              {"h5", 13.28f, 1.67f}, {"h6", 10.72f, 2.33f}};
  for (const HeadingSpec& heading : kHeadings) {
    if (tag == heading.tag) {
      style.display = Display::Block;
      style.font_size = heading.size;
      style.font_weight = 700;
      style.line_height = 1.25f;
      style.margin_top = heading.size * 0.8f;
      style.margin_bottom = heading.size * 0.6f;
      style.heading_level = static_cast<std::uint8_t>(tag[1] - '0');
      return style;
    }
  }

  if (tag == "p") {
    style.display = Display::Block;
    style.margin_top = 8.0f;
    style.margin_bottom = 8.0f;
    return style;
  }
  if (tag == "div" || tag == "section" || tag == "article" || tag == "aside" ||
      tag == "header" || tag == "footer" || tag == "main" || tag == "nav" || tag == "form" ||
      tag == "fieldset" || tag == "figure" || tag == "address" || tag == "center" ||
      tag == "details" || tag == "body" || tag == "html") {
    style.display = Display::Block;
    return style;
  }
  if (tag == "ul" || tag == "ol") {
    style.display = Display::Block;
    style.margin_top = 8.0f;
    style.margin_bottom = 8.0f;
    style.padding_left = 28.0f;
    style.list_style = tag == "ol" ? ListStyle::Decimal : ListStyle::Disc;
    return style;
  }
  if (tag == "li") {
    style.display = Display::ListItem;
    style.margin_top = 2.0f;
    style.margin_bottom = 2.0f;
    return style;
  }
  if (tag == "blockquote") {
    style.display = Display::Block;
    style.margin_top = 8.0f;
    style.margin_bottom = 8.0f;
    style.margin_left = 20.0f;
    style.margin_right = 20.0f;
    style.padding_left = 12.0f;
    style.border_width = 3.0f;
    style.border_color = math::Color{220, 220, 220, 255};
    style.border_left_only = true;
    return style;
  }
  if (tag == "pre") {
    style.display = Display::Block;
    style.monospace = true;
    style.font_size = 13.0f;
    style.white_space = WhiteSpace::Pre;
    style.margin_top = 8.0f;
    style.margin_bottom = 8.0f;
    style.padding_top = 8.0f;
    style.padding_bottom = 8.0f;
    style.padding_left = 10.0f;
    style.padding_right = 10.0f;
    style.background = math::Color{245, 245, 245, 255};
    style.has_background = true;
    style.border_width = 1.0f;
    style.border_color = math::Color{225, 225, 225, 255};
    style.radius = 4.0f;
    return style;
  }
  if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt") {
    style.monospace = true;
    style.font_size = 13.0f;
    return style;
  }
  if (tag == "hr") {
    style.display = Display::Block;
    style.margin_top = 8.0f;
    style.margin_bottom = 8.0f;
    style.height = 1.0f;
    style.background = math::Color{221, 221, 221, 255};
    style.has_background = true;
    return style;
  }
  if (tag == "b" || tag == "strong") {
    style.font_weight = 700;
    return style;
  }
  if (tag == "i" || tag == "em" || tag == "cite" || tag == "var") {
    style.italic = true;
    return style;
  }
  if (tag == "u" || tag == "ins") {
    style.underline = true;
    return style;
  }
  if (tag == "s" || tag == "del" || tag == "strike") {
    style.strikethrough = true;
    return style;
  }
  if (tag == "small") {
    style.font_size = 12.0f;
    return style;
  }
  if (tag == "big") {
    style.font_size = 20.0f;
    return style;
  }
  if (tag == "a") {
    style.color = math::Color{0, 102, 204, 255};
    style.underline = true;
    return style;
  }
  if (tag == "table") {
    style.display = Display::Table;
    style.margin_top = 8.0f;
    style.margin_bottom = 8.0f;
    return style;
  }
  if (tag == "thead" || tag == "tbody") {
    style.display = Display::TableRow;
    return style;
  }
  if (tag == "tr") {
    style.display = Display::TableRow;
    return style;
  }
  if (tag == "td" || tag == "th") {
    style.display = Display::TableCell;
    style.padding_top = 5.0f;
    style.padding_bottom = 5.0f;
    style.padding_left = 8.0f;
    style.padding_right = 8.0f;
    style.border_width = 1.0f;
    style.border_color = math::Color{225, 225, 225, 255};
    if (tag == "th") style.font_weight = 700;
    return style;
  }
  if (tag == "img") {
    style.display = Display::InlineBlock;
    return style;
  }
  if (tag == "br") {
    style.display = Display::Inline;
    return style;
  }
  if (tag == "script" || tag == "style" || tag == "head" || tag == "meta" || tag == "link" ||
      tag == "title" || tag == "base" || tag == "noscript") {
    style.display = Display::None;
    return style;
  }
  if (tag == "input" || tag == "button" || tag == "select" || tag == "textarea") {
    style.display = Display::InlineBlock;
    return style;
  }
  if (tag == "span" || tag == "label" || tag == "abbr" || tag == "time" || tag == "sub" ||
      tag == "sup" || tag == "mark" || tag == "q") {
    style.display = Display::Inline;
    return style;
  }
  // 其余未知标签：按行内处理（保守：不打断文档流）。
  return style;
}

// ————————————————————————————————————————————————————————————————————————————
// 解析器
// ————————————————————————————————————————————————————————————————————————————

StyleResolver::StyleResolver(const StyleSheet& sheet, const Style& initial)
    : sheet_(sheet), initial_(initial) {}

auto StyleResolver::style_of(const Node* node) const -> const Style& {
  const auto found = styles_.find(node);
  return found == styles_.end() ? initial_ : found->second;
}

auto find_keyframes(const StyleSheet& sheet, std::string_view name) -> const KeyframesSpec* {
  if (name.empty()) return nullptr;
  for (const KeyframesSpec& spec : sheet.keyframes) {
    if (spec.name == name) return &spec;
  }
  return nullptr;
}

auto StyleResolver::matches_compound(const Node& node, const CompoundSelector& part) -> bool {
  if (node.kind != NodeKind::Element) return false;
  if (!part.tag.empty() && part.tag != "*" && node.tag != part.tag) return false;
  if (!part.id.empty()) {
    const auto id = node.attr("id");
    if (!id.has_value() || *id != part.id) return false;
  }
  for (const std::string& cls : part.classes) {
    if (!node.has_class(cls)) return false;
  }
  return true;
}

auto StyleResolver::matches_selector(const Node& node, const std::vector<const Node*>& ancestors,
                                     const Selector& selector) -> bool {
  if (selector.parts.empty()) return false;
  const std::size_t last = selector.parts.size() - 1U;
  if (!matches_compound(node, selector.parts[last])) return false;
  // 从右往左回扫祖先。`ancestors` 是**从根到父**的顺序。
  int ancestor_index = static_cast<int>(ancestors.size()) - 1;
  for (std::size_t part = last; part-- > 0;) {
    const bool direct = selector.child_combinator[part];
    bool matched = false;
    if (direct) {
      if (ancestor_index >= 0 &&
          matches_compound(*ancestors[static_cast<std::size_t>(ancestor_index)],
                           selector.parts[part])) {
        matched = true;
        --ancestor_index;
      }
    } else {
      while (ancestor_index >= 0) {
        if (matches_compound(*ancestors[static_cast<std::size_t>(ancestor_index)],
                             selector.parts[part])) {
          matched = true;
          --ancestor_index;
          break;
        }
        --ancestor_index;
      }
    }
    if (!matched) return false;
  }
  return true;
}

void StyleResolver::resolve_node(const Node& node, const Style& parent_style, int depth) {
  if (depth > depth_limit_) return;
  if (node.kind == NodeKind::Text) {
    // 文本节点不自己算样式：沿用父元素（排版层查父节点的样式即可）。
    styles_[&node] = parent_style;
    return;
  }

  // ① 起点：UA 默认值（按标签），再叠继承。
  Style style = user_agent_style(node.tag);
  // 可继承属性（与浏览器一致的那一批）。
  style.color = parent_style.color;
  style.font_size = parent_style.font_size;
  style.font_weight = parent_style.font_weight;
  style.italic = parent_style.italic;
  style.line_height = parent_style.line_height;
  style.letter_spacing = parent_style.letter_spacing;
  style.text_align = parent_style.text_align;
  style.white_space = parent_style.white_space;
  style.monospace = parent_style.monospace;
  style.list_style = parent_style.list_style;

  // ② UA 默认值里**显式设过**的那些字段要盖回继承值——UA 表里给标题设了字号，
  //    就该盖住父级字号。判据是「UA 结果与空 Style 的对应字段不同」。
  const Style blank{};
  const Style ua = user_agent_style(node.tag);
  const auto prefer_ua = [&ua, &blank](float ua_value, float blank_value, float inherited) {
    return ua_value != blank_value ? ua_value : inherited;
  };
  style.font_size = prefer_ua(ua.font_size, blank.font_size, parent_style.font_size);
  style.font_weight = static_cast<std::uint16_t>(
      prefer_ua(static_cast<float>(ua.font_weight), static_cast<float>(blank.font_weight),
                static_cast<float>(parent_style.font_weight)));
  style.monospace = ua.monospace != blank.monospace ? ua.monospace : parent_style.monospace;
  style.color = (ua.color.r != blank.color.r || ua.color.g != blank.color.g ||
                 ua.color.b != blank.color.b || ua.color.a != blank.color.a)
                    ? ua.color
                    : parent_style.color;
  // UA 的 display/边距/背景/边框等**非继承属性**直接用 UA 值（已在 `style` 里）。

  // ③ 样式表规则：按优先级排序后依次应用。
  ancestors_.push_back(&node);
  std::vector<Candidate> candidates;
  const float em_base = style.font_size > 0.0f ? style.font_size : 16.0f;
  const auto add_declaration = [&candidates](const Declaration& declaration, int base) {
    Candidate candidate{};
    candidate.property = declaration.property;
    candidate.value = declaration.value;
    candidate.precedence = base + (declaration.important ? 100000 : 0);
    candidates.push_back(std::move(candidate));
  };
  // 内联 style 的基线优先级高于任何无 important 的选择器（CSS 规则如此）。
  if (const auto inline_style = node.attr("style"); inline_style.has_value()) {
    for (const Declaration& declaration : parse_inline_style(*inline_style)) {
      add_declaration(declaration, 50000);
    }
  }
  for (const Rule& rule : sheet_.rules) {
    int best = -1;
    for (const Selector& selector : rule.selectors) {
      if (!matches_selector(node, ancestors_, selector)) continue;
      best = std::max(best, selector.specificity);
    }
    if (best < 0) continue;
    // 同特异性的规则：后出现的赢（`rule.order` 进优先级低位）。
    const int base = best * 1000 + static_cast<int>(std::min<std::size_t>(rule.order, 99U));
    for (const Declaration& declaration : rule.declarations) add_declaration(declaration, base);
  }
  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                            const Candidate& right) {
    return left.precedence < right.precedence;
  });

  // ④ 逐条应用（后应用覆盖前面的——已排序）。
  for (const Candidate& candidate : candidates) {
    apply_declaration(style, candidate.property, candidate.value, em_base);
  }

  styles_[&node] = style;
  for (const Node& child : node.children) {
    resolve_node(child, style, depth + 1);
  }
  ancestors_.pop_back();
}

void StyleResolver::apply_declaration(Style& style, std::string_view property,
                                      std::string_view value, float em_base) {
  const std::string_view raw = trim_view(value);
  if (raw.empty()) return;
  const float percent_base = 0.0f;   // 百分比宽度/高度需要容器宽高：排版层给，这里不支持

  if (property == "animation" || property == "animation-name") {
    // `animation` 简写：`name duration timing delay iteration-count direction fill-mode`。
    // 逐词**分类**（不按位置）——CSS 允许这些分量任意顺序，按位置解会把
    // `animation: 0.4s ease-out fadeInUp` 解错（名字取成 `0.4s`）。
    AnimationSpec spec{};
    for (const std::string& word : split_ws_into_words(raw)) {
      const std::string lower = lower_ascii(word);
      if (lower == "none") {
        style.animation.reset();
        return;
      }
      if (lower == "infinite") {
        spec.iterations = 0;
        continue;
      }
      if (lower == "normal" || lower == "reverse" || lower == "alternate" ||
          lower == "alternate-reverse") {
        spec.direction = lower == "reverse"          ? AnimationSpec::Direction::Reverse
                         : lower == "alternate"        ? AnimationSpec::Direction::Alternate
                         : lower == "alternate-reverse" ? AnimationSpec::Direction::AlternateReverse
                                                        : AnimationSpec::Direction::Normal;
        continue;
      }
      if (lower == "forwards" || lower == "backwards" || lower == "both") {
        spec.fill = lower == "forwards" ? AnimationSpec::Fill::Forwards
                    : lower == "backwards" ? AnimationSpec::Fill::Backwards
                                            : AnimationSpec::Fill::Both;
        continue;
      }
      if (lower == "linear" || lower == "ease" || lower == "ease-in" ||
          lower == "ease-out" || lower == "ease-in-out" ||
          lower.rfind("cubic-bezier(", 0) == 0 || lower.rfind("steps(", 0) == 0) {
        spec.timing = lower;
        continue;
      }
      if (const auto seconds = parse_seconds(word); seconds.has_value()) {
        // 第一个时间是 duration，第二个是 delay（CSS 同规则）。
        if (spec.duration <= 0.0) {
          spec.duration = *seconds;
        } else {
          spec.delay = *seconds;
        }
        continue;
      }
      const auto count = parse_length(word, 0.0f, 0.0f);
      if (count.has_value()) {
        spec.iterations = static_cast<int>(*count);
        continue;
      }
      // 其余按名字（最后一个未被认领的标识符就是动画名）。
      if (!word.empty() &&
          (word[0] == '_' || std::isalpha(static_cast<unsigned char>(word[0])) != 0)) {
        spec.name = word;
      }
    }
    if (property == "animation-name" && spec.name.empty()) spec.name = std::string(raw);
    if (spec.name.empty()) return;
    style.animation = std::move(spec);
    return;
  }
  if (property == "animation-duration") {
    if (const auto seconds = parse_seconds(raw); seconds.has_value()) {
      if (!style.animation.has_value()) style.animation.emplace();
      style.animation->duration = *seconds;
    }
    return;
  }
  if (property == "animation-delay") {
    if (const auto seconds = parse_seconds(raw); seconds.has_value()) {
      if (!style.animation.has_value()) style.animation.emplace();
      style.animation->delay = *seconds;
    }
    return;
  }
  if (property == "animation-iteration-count") {
    if (!style.animation.has_value()) style.animation.emplace();
    if (lower_ascii(raw) == "infinite") {
      style.animation->iterations = 0;
      return;
    }
    if (const auto count = parse_length(raw, 0.0f, 0.0f); count.has_value()) {
      style.animation->iterations = static_cast<int>(*count);
    }
    return;
  }
  if (property == "animation-timing-function") {
    if (!style.animation.has_value()) style.animation.emplace();
    style.animation->timing = std::string(raw);
    return;
  }
  if (property == "animation-fill-mode") {
    if (!style.animation.has_value()) style.animation.emplace();
    const std::string lower = lower_ascii(raw);
    style.animation->fill = lower == "forwards"  ? AnimationSpec::Fill::Forwards
                            : lower == "backwards" ? AnimationSpec::Fill::Backwards
                            : lower == "both"      ? AnimationSpec::Fill::Both
                                                   : AnimationSpec::Fill::None;
    return;
  }
  if (property == "animation-direction") {
    if (!style.animation.has_value()) style.animation.emplace();
    const std::string lower = lower_ascii(raw);
    style.animation->direction = lower == "reverse" ? AnimationSpec::Direction::Reverse
                                 : lower == "alternate"
                                     ? AnimationSpec::Direction::Alternate
                                 : lower == "alternate-reverse"
                                     ? AnimationSpec::Direction::AlternateReverse
                                     : AnimationSpec::Direction::Normal;
    return;
  }
  if (property == "transition" || property == "transition-duration") {
    // 过渡只接**时长**（其余分量取缺省：`all` + `ease-out`）。逐属性曲线、
    // 逐属性延迟在展示型内容里几乎不用，而接全它们要再一张解析表。
    for (const std::string& word : split_ws_into_words(raw)) {
      const auto seconds = parse_seconds(word);
      if (!seconds.has_value()) continue;
      if (*seconds > 0.0) style.transition_duration = *seconds;
      break;   // 第一个时间就是 duration（第二个是 delay，不接）
    }
    return;
  }

  if (property == "color") {
    if (const auto color = parse_color(raw); color.has_value()) style.color = *color;
    return;
  }
  if (property == "background" || property == "background-color") {
    // `background: #fff url(…)` 这类简写：只取颜色部分（不取网络图）。
    const auto words = split_ws_into_words(raw);
    for (const std::string& word : words) {
      if (const auto color = parse_color(word); color.has_value()) {
        style.background = *color;
        style.has_background = color->a != 0U;
        return;
      }
      if (const std::string_view lower = lower_ascii(word);
          lower.rfind("url(", 0) == 0 || lower == "none") {
        return;
      }
    }
    if (raw == "none") {
      style.has_background = false;
    }
    return;
  }
  if (property == "font-size") {
    if (const auto size = parse_length(raw, em_base, percent_base); size.has_value() && *size > 0.0f) {
      style.font_size = *size;
      return;
    }
    const std::string lower = lower_ascii(raw);
    // 关键字字号（相对父级）
    struct Keyword {
      std::string_view name;
      float factor;
    };
    static constexpr Keyword kKeywords[] = {{"xx-small", 0.6f}, {"x-small", 0.75f},
                                            {"small", 0.83f},   {"medium", 1.0f},
                                            {"large", 1.2f},    {"x-large", 1.5f},
                                            {"xx-large", 2.0f}, {"smaller", 0.83f},
                                            {"larger", 1.2f}};
    for (const Keyword& keyword : kKeywords) {
      if (lower == keyword.name) {
        style.font_size = em_base * keyword.factor;
        return;
      }
    }
    return;
  }
  if (property == "font-weight") {
    const std::string lower = lower_ascii(raw);
    if (lower == "bold" || lower == "bolder") {
      style.font_weight = 700;
    } else if (lower == "normal" || lower == "lighter") {
      style.font_weight = 400;
    } else if (const auto weight = parse_length(raw, 0.0f, 0.0f); weight.has_value()) {
      style.font_weight = static_cast<std::uint16_t>(std::clamp(*weight, 100.0f, 900.0f));
    }
    return;
  }
  if (property == "font-style") {
    const std::string lower = lower_ascii(raw);
    style.italic = lower == "italic" || lower == "oblique";
    return;
  }
  if (property == "font-family") {
    const std::string lower = lower_ascii(raw);
    style.monospace = lower.find("mono") != std::string::npos ||
                      lower.find("courier") != std::string::npos ||
                      lower.find("consolas") != std::string::npos;
    return;
  }
  if (property == "line-height") {
    if (const auto length = parse_length(raw, em_base, 0.0f); length.has_value()) {
      // 无单位是倍数，带单位是绝对值（换算成倍数）。
      const std::string lower = lower_ascii(raw);
      const bool unitless = lower.find("px") == std::string::npos &&
                            lower.find("em") == std::string::npos &&
                            lower.find("%") == std::string::npos &&
                            lower.find("pt") == std::string::npos;
      if (unitless) {
        if (*length > 0.1f && *length < 10.0f) style.line_height = *length;
      } else if (lower.find('%') != std::string::npos) {
        style.line_height = std::max(*length / 100.0f, 0.8f);
      } else if (style.font_size > 0.0f) {
        style.line_height = std::clamp(*length / style.font_size, 0.8f, 4.0f);
      }
    }
    return;
  }
  if (property == "letter-spacing") {
    if (const auto spacing = parse_length(raw, em_base, 0.0f); spacing.has_value()) {
      style.letter_spacing = *spacing;
    }
    return;
  }
  if (property == "text-align") {
    const std::string lower = lower_ascii(raw);
    if (lower == "left" || lower == "start") style.text_align = TextAlign::Start;
    else if (lower == "right" || lower == "end") style.text_align = TextAlign::End;
    else if (lower == "center") style.text_align = TextAlign::Center;
    else if (lower == "justify") style.text_align = TextAlign::Justify;
    return;
  }
  if (property == "text-decoration" || property == "text-decoration-line") {
    const std::string lower = lower_ascii(raw);
    if (lower.find("none") != std::string::npos) {
      style.underline = false;
      style.strikethrough = false;
    }
    if (lower.find("underline") != std::string::npos) style.underline = true;
    if (lower.find("line-through") != std::string::npos) style.strikethrough = true;
    return;
  }
  if (property == "white-space") {
    const std::string lower = lower_ascii(raw);
    if (lower == "pre") style.white_space = WhiteSpace::Pre;
    else if (lower == "pre-wrap") style.white_space = WhiteSpace::PreWrap;
    else if (lower == "nowrap") style.white_space = WhiteSpace::NoWrap;
    else style.white_space = WhiteSpace::Normal;
    return;
  }
  if (property == "display") {
    const std::string lower = lower_ascii(raw);
    if (lower == "none") style.display = Display::None;
    else if (lower == "block") style.display = Display::Block;
    else if (lower == "inline") style.display = Display::Inline;
    else if (lower == "inline-block") style.display = Display::InlineBlock;
    else if (lower == "list-item") style.display = Display::ListItem;
    else if (lower == "table") style.display = Display::Table;
    else if (lower == "table-row") style.display = Display::TableRow;
    else if (lower == "table-cell") style.display = Display::TableCell;
    // flex/grid/inline-flex… 不支持：保持原值（忽略那一条声明）。
    return;
  }
  if (property == "list-style-type" || property == "list-style") {
    const std::string lower = lower_ascii(raw);
    if (lower.find("none") != std::string::npos) style.list_style = ListStyle::None;
    else if (lower.find("decimal") != std::string::npos) style.list_style = ListStyle::Decimal;
    else if (lower.find("circle") != std::string::npos) style.list_style = ListStyle::Circle;
    else if (lower.find("square") != std::string::npos) style.list_style = ListStyle::Square;
    else if (lower.find("disc") != std::string::npos) style.list_style = ListStyle::Disc;
    return;
  }
  if (property == "width") {
    if (const auto width = parse_length(raw, em_base, percent_base); width.has_value()) {
      style.width = std::max(*width, 0.0f);
    }
    return;
  }
  if (property == "height") {
    if (const auto height = parse_length(raw, em_base, percent_base); height.has_value()) {
      style.height = std::max(*height, 0.0f);
    }
    return;
  }
  if (property == "max-width") {
    if (const auto width = parse_length(raw, em_base, percent_base); width.has_value()) {
      style.max_width = std::max(*width, 0.0f);
    }
    return;
  }
  if (property == "padding") {
    if (const auto edges = parse_edges(raw, em_base, percent_base); edges.has_value()) {
      style.padding_top = edges->top;
      style.padding_right = edges->right;
      style.padding_bottom = edges->bottom;
      style.padding_left = edges->left;
    }
    return;
  }
  if (property == "padding-top" || property == "padding-right" || property == "padding-bottom" ||
      property == "padding-left") {
    if (const auto length = parse_length(raw, em_base, percent_base); length.has_value()) {
      if (property == "padding-top") style.padding_top = *length;
      else if (property == "padding-right") style.padding_right = *length;
      else if (property == "padding-bottom") style.padding_bottom = *length;
      else style.padding_left = *length;
    }
    return;
  }
  if (property == "margin") {
    if (const auto edges = parse_edges(raw, em_base, percent_base); edges.has_value()) {
      style.margin_top = edges->top;
      style.margin_right = edges->right;
      style.margin_bottom = edges->bottom;
      style.margin_left = edges->left;
    }
    return;
  }
  if (property == "margin-top" || property == "margin-right" || property == "margin-bottom" ||
      property == "margin-left") {
    const std::string lower = lower_ascii(raw);
    if (lower == "auto") return;   // 自动居中需要容器宽：排版层处理（此处不猜）
    if (const auto length = parse_length(raw, em_base, percent_base); length.has_value()) {
      if (property == "margin-top") style.margin_top = *length;
      else if (property == "margin-right") style.margin_right = *length;
      else if (property == "margin-bottom") style.margin_bottom = *length;
      else style.margin_left = *length;
    }
    return;
  }
  if (property == "border") {
    const Border border = parse_border(raw, em_base);
    if (border.has_width) style.border_width = border.width;
    if (border.color.a != 0U) style.border_color = border.color;
    return;
  }
  if (property == "border-width") {
    if (const auto width = parse_length(raw, em_base, 0.0f); width.has_value()) {
      style.border_width = std::max(*width, 0.0f);
    }
    return;
  }
  if (property == "border-color") {
    if (const auto color = parse_color(raw); color.has_value()) style.border_color = *color;
    return;
  }
  if (property == "border-radius") {
    if (const auto radius = parse_length(raw, em_base, 0.0f); radius.has_value()) {
      style.radius = std::max(*radius, 0.0f);
    }
    return;
  }
  if (property == "border-left") {
    // 引用块/提示条最常见的写法：只给左边一道线。
    const Border border = parse_border(raw, em_base);
    if (border.has_width) {
      style.border_width = border.width;
      if (border.color.a != 0U) style.border_color = border.color;
      // ⚠ 这条**必须设**：否则绘制侧会把 `border-left` 当成矩形描边，
      // 画出一个四边都有框的盒子（读起来像“套了个框”而不是“引用”）。
      style.border_left_only = border.width > 0.0f;
    }
    return;
  }
  // 其余属性（box-shadow / transition / transform / float / position / flex…）
  // **明确不支持**：忽略这一条，不影响其余声明。
}

auto StyleResolver::resolve_tree(const Node& root) -> StyleResolver& {
  styles_.clear();
  ancestors_.clear();
  Style root_style = initial_;
  // 根若是元素，用它的 UA 默认样式作为基础。
  if (root.kind == NodeKind::Element) {
    const Style ua = user_agent_style(root.tag);
    root_style.display = ua.display;
    root_style.white_space = ua.white_space;
  }
  for (const Node& child : root.children) {
    resolve_node(child, root_style, 0);
  }
  return *this;
}

}  // namespace st::html
