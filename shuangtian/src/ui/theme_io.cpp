#include "st/ui/theme_io.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/ext/json.hpp"

namespace st::ui {
namespace {

/// 十六进制数字 → 值；非法字符返回 -1。
[[nodiscard]] auto hex_digit(char value) -> int {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

/// 取 `text` 的第 `index` 个两位十六进制字节（`prefix` 已剥离）。
[[nodiscard]] auto hex_byte(std::string_view text, std::size_t index, std::uint8_t& out) -> bool {
  const int high = hex_digit(text[index]);
  const int low = hex_digit(text[index + 1]);
  if (high < 0 || low < 0) return false;
  out = static_cast<std::uint8_t>(high * 16 + low);
  return true;
}

/// 解析 `0..255` 的十进制通道值（`rgba()` 用）。
[[nodiscard]] auto parse_channel(std::string_view text, double& out) -> bool {
  if (text.empty()) return false;
  double value = 0.0;
  bool seen_digit = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t') continue;
    if (c < '0' || c > '9') return false;
    seen_digit = true;
    value = value * 10.0 + static_cast<double>(c - '0');
    if (value > 255.0) return false;
  }
  if (!seen_digit) return false;
  out = value;
  return true;
}

/// 去掉首尾空白。
///
/// 放在这里（而不是文件后半段）：它被 `parse_alpha` 与 `parse_color` 共用，
/// 而 C++ 的"先声明后用"要求定义在前（实测把 `trim` 留在后面就会编译失败）。
[[nodiscard]] auto trim(std::string_view text) -> std::string_view {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                           text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

/// 解析 alpha 通道，接受 CSS 里真实存在的三种写法：
///
/// - `0.5` 小数（0..1）—— CSS 的规范形式，也是从样式表里搬值最可能碰到的；
/// - `128` 整数（0..255）—— 十六进制思维下顺手写的；
/// - `50%` 百分比。
///
/// **区分小数与整数的判据只能是「有没有小数点」**，不是值的大小：
/// 实测踩到——早期实现把 `0.5` 当成“通道值 0.5 → 四舍五入后 1/255”，
/// 于是半透明底色变成了几乎全透明，而参数看起来一切正常。
[[nodiscard]] auto parse_alpha(std::string_view text, std::uint8_t& out) -> bool {
  text = trim(text);
  if (text.empty()) return false;
  bool is_percent = false;
  bool is_fraction = false;
  if (text.back() == '%') {
    text.remove_suffix(1);
    is_percent = true;
  }
  // 数值部分：可选的小数点 + 若干数字（不接受负号/指数：颜色通道没有负数）。
  double value = 0.0;
  double place = 1.0;
  bool seen_digit = false;
  bool after_dot = false;
  bool seen_dot = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t') continue;
    if (c == '.') {
      if (seen_dot) return false;  // 一个小数点
      seen_dot = true;
      after_dot = true;
      is_fraction = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    const double digit = static_cast<double>(c - '0');
    if (after_dot) {
      place *= 0.1;
      value += digit * place;
    } else {
      value = value * 10.0 + digit;
    }
    seen_digit = true;
  }
  if (!seen_digit) return false;
  // **换算用除法不用乘法**：百分比的 scale 写成 `2.55` 会引入浮点误差
  // （实测 `50%` 得 127.4999… → 四舍五入到 127，而正确答案是 127.5 → 128）。
  // `value / 100.0 * 255.0` 在常见整百分比下是精确的。
  if (is_percent) value = value / 100.0 * 255.0;
  // 小数形式（`0.5`）是 0..1 的比例；整数（`128`）已经是 0..255 尺度。
  if (is_fraction) value *= 255.0;
  if (value < 0.0) value = 0.0;
  if (value > 255.0) value = 255.0;
  out = static_cast<std::uint8_t>(std::lround(value));
  return true;
}

/// 解析颜色串：`#RGB` / `#RRGGBB` / `#RRGGBBAA` / `rgb(r,g,b)` / `rgba(r,g,b,a)`。
///
/// 为什么不支持 CSS 的 `hsl()` 与具名色：设计令牌是给**代码与主题文件**用的，
/// 这两种写法在"数量少、要精确"的场景里没有收益，而每多一种语法就多一处歧义
/// （如 `#ABC` 是三位的简写还是三位有效数字）。
[[nodiscard]] auto parse_color(std::string_view text, math::Color& out) -> bool {
  text = trim(text);
  if (text.empty()) return false;
  if (text.front() == '#') {
    text.remove_prefix(1);
    if (text.size() == 3) {
      std::uint8_t r = 0;
      std::uint8_t g = 0;
      std::uint8_t b = 0;
      const auto expand = [](char c, std::uint8_t& value) -> bool {
        const int digit = hex_digit(c);
        if (digit < 0) return false;
        value = static_cast<std::uint8_t>(digit * 16 + digit);
        return true;
      };
      if (!expand(text[0], r) || !expand(text[1], g) || !expand(text[2], b)) return false;
      out = math::Color{r, g, b, 255};
      return true;
    }
    if (text.size() == 6 || text.size() == 8) {
      std::uint8_t r = 0;
      std::uint8_t g = 0;
      std::uint8_t b = 0;
      std::uint8_t a = 255;
      if (!hex_byte(text, 0, r) || !hex_byte(text, 2, g) || !hex_byte(text, 4, b)) return false;
      if (text.size() == 8 && !hex_byte(text, 6, a)) return false;
      out = math::Color{r, g, b, a};
      return true;
    }
    return false;
  }
  // rgb()/rgba()：从 CSS 直接搬值时要能粘（歌白的主题文件就是 rgba 写法）。
  const bool has_alpha = text.starts_with("rgba(") || text.starts_with("RGBA(");
  const bool has_rgb = text.starts_with("rgb(") || text.starts_with("RGB(");
  if (!has_alpha && !has_rgb) return false;
  // **左括号位置必须找出来，不能写成 `3`**：`rgb(` 的左括号在下标 3，而 `rgba(`
  // 在下标 4——写死 3 时 rgba 的 body 会多带一个 `(`，首个通道永远解析失败。
  // 实测就是这个：`rgb(255,0,0)` 全绿、`rgba(255,0,0,128)` 三种写法全红。
  const std::size_t open = text.find('(');
  const std::size_t close = text.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
    return false;
  }
  std::string_view body = text.substr(open + 1, close - open - 1);
  std::vector<std::string_view> parts;
  while (true) {
    const std::size_t comma = body.find(',');
    if (comma == std::string_view::npos) {
      parts.push_back(trim(body));
      break;
    }
    parts.push_back(trim(body.substr(0, comma)));
    body.remove_prefix(comma + 1);
  }
  const std::size_t expected = has_alpha ? 4U : 3U;
  if (parts.size() != expected) return false;
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  if (!parse_channel(parts[0], r) || !parse_channel(parts[1], g) || !parse_channel(parts[2], b)) {
    return false;
  }
  std::uint8_t alpha = 255;
  if (has_alpha && !parse_alpha(parts[3], alpha)) return false;
  out = math::Color{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                    static_cast<std::uint8_t>(b), alpha};
  return true;
}

/// 颜色 → 规范十六进制串（`#RRGGBBAA`，或 alpha 为 255 时 `#RRGGBB`）。
[[nodiscard]] auto color_to_hex_string(const math::Color& color) -> std::string {
  if (color.a == 255U) return std::format("#{:02X}{:02X}{:02X}", color.r, color.g, color.b);
  return std::format("#{:02X}{:02X}{:02X}{:02X}", color.r, color.g, color.b, color.a);
}

}  // namespace

auto theme_mode_from_name(std::string_view name) -> std::optional<ThemeMode> {
  if (name == "light") return ThemeMode::Light;
  if (name == "dark") return ThemeMode::Dark;
  return std::nullopt;
}

auto theme_mode_name(ThemeMode mode) noexcept -> std::string_view {
  return mode == ThemeMode::Dark ? "dark" : "light";
}

auto theme_to_json(const Theme& theme) -> Json {
  // 用 `Json::object()` 而不是 `Json x{...}`：花括号会命中 initializer_list 构造，
  // 得到「含一个对象的数组」而不是对象（`CONVENTIONS.md` §3.8 有事故记录）。
  Json root = Json::object();
  root["name"] = theme.name();
  root["base"] = std::string(theme_mode_name(theme.mode()));
  Json colors = Json::object();
  for (const std::string_view name : palette_token_names()) {
    if (const auto color = palette_token(theme.colors(), name); color.has_value()) {
      colors[std::string(name)] = color_to_hex_string(*color);
    }
  }
  root["colors"] = std::move(colors);
  Json metrics = Json::object();
  for (const std::string_view name : metric_token_names()) {
    if (const auto value = metric_token(theme.metrics(), name); value.has_value()) {
      metrics[std::string(name)] = static_cast<double>(*value);
    }
  }
  root["metrics"] = std::move(metrics);
  root["font_family"] = theme.font_family();
  return root;
}

auto apply_theme_overrides(Theme& theme, const Json& json) -> Status {
  if (!json.is_object()) {
    return unexpected(ErrorCode::Invalid, "主题覆盖必须是 JSON 对象");
  }  if (const auto* base_value = json_find(json, "base"); base_value != nullptr) {
    // `base` 允许出现，但**必须与当前模式一致**——它描述的是"这份主题建立在哪个基准上"，
    // 与现状不符就说明调用方以为在改另一套主题（静默照做会改错对象）。
    // 允许一致值出现，是为了让 `theme` 返回的 token 快照能直接改两笔再喂回 `set`
    // （导出 → 改 → 应用，最常见的用法）。
    const std::string base = json_as_string(*base_value, "");
    const auto mode = theme_mode_from_name(base);
    if (!mode.has_value()) {
      return unexpected(ErrorCode::Invalid,
                        std::format("未知基准主题 base=\"{}\"（可用：light / dark）", base));
    }
    if (*mode != theme.mode()) {
      return unexpected(
          ErrorCode::Invalid,
          std::format("base=\"{}\" 与当前主题模式（{}）不符：切模式请用 theme 的 \"mode\"", base,
                      theme_mode_name(theme.mode())));
    }
  }
  // 未知键**一律报错**（见头文件的说明）：拼错被静默忽略会变成"改了但没生效"。
  //
  // `unknown` 是**集合语义**：同一个键可能被两条路径各推一次（显式扫描一次、
  // `set_*_token` 再失败一次），去重后的报错才读得懂
  // （实测输出 `colors.primry、colors.primry`，看着像有两个错）。
  std::vector<std::string> unknown;
  const auto note_unknown = [&unknown](std::string entry) {
    for (const std::string& seen : unknown) {
      if (seen == entry) return;
    }
    unknown.push_back(std::move(entry));
  };
  const auto reject_unknown = [&note_unknown](const Json& object, std::string_view section,
                                              const std::vector<std::string_view>& known) {
    for (auto it = object.begin(); it != object.end(); ++it) {
      const std::string& key = it.key();
      bool found = false;
      for (const std::string_view candidate : known) {
        if (candidate == key) {
          found = true;
          break;
        }
      }
      if (!found) note_unknown(std::format("{}.{}", section, key));
    }
  };

  if (const auto* colors = json_find(json, "colors"); colors != nullptr) {
    if (!colors->is_object()) return unexpected(ErrorCode::Invalid, "colors 必须是对象");
    reject_unknown(*colors, "colors", palette_token_names());
    for (auto it = colors->begin(); it != colors->end(); ++it) {
      const std::string& key = it.key();
      if (!it.value().is_string()) {
        return unexpected(ErrorCode::Invalid, std::format("colors.{} 必须是颜色字符串", key));
      }
      math::Color color{};
      if (!parse_color(json_as_string(it.value(), ""), color)) {
        return unexpected(ErrorCode::Invalid,
                          std::format("colors.{} 不是合法颜色：\"{}\"（可用 #RRGGBB / "
                                      "#RRGGBBAA / rgb() / rgba()）",
                                      key, json_as_string(it.value(), "")));
      }
      if (!set_palette_token(theme.colors(), key, color)) {
        note_unknown(std::format("colors.{}", key));
      }
    }
  }

  if (const auto* metrics = json_find(json, "metrics"); metrics != nullptr) {
    if (!metrics->is_object()) return unexpected(ErrorCode::Invalid, "metrics 必须是对象");
    reject_unknown(*metrics, "metrics", metric_token_names());
    for (auto it = metrics->begin(); it != metrics->end(); ++it) {
      const std::string& key = it.key();
      if (!it.value().is_number()) {
        return unexpected(ErrorCode::Invalid, std::format("metrics.{} 必须是数字", key));
      }
      const float value = static_cast<float>(json_as_double(it.value(), 0.0));
      if (!set_metric_token(theme.metrics(), key, value)) {
        note_unknown(std::format("metrics.{}", key));
      }
    }
  }

  if (const auto* name_value = json_find(json, "name"); name_value != nullptr) {
    theme.set_name(json_as_string(*name_value, theme.name()));
  }
  if (const auto* family = json_find(json, "font_family"); family != nullptr) {
    theme.set_font_family(json_as_string(*family, theme.font_family()));
  }

  if (!unknown.empty()) {
    std::string joined;
    for (std::size_t i = 0; i < unknown.size(); ++i) {
      if (i > 0) joined += "、";
      joined += unknown[i];
    }
    return unexpected(ErrorCode::Invalid, std::format("未知主题 token：{}", joined));
  }

  // 顶层也只接受认识的键（`name`/`base`/`colors`/`metrics`/`font_family`）——
  // 顶层拼错比 token 拼错更常见（如写成 `colour`）。
  for (auto it = json.begin(); it != json.end(); ++it) {
    const std::string& key = it.key();
    if (key != "name" && key != "base" && key != "colors" && key != "metrics" &&
        key != "font_family") {
      return unexpected(ErrorCode::Invalid,
                        std::format("未知主题字段：{}（可用：name / base / colors / metrics / "
                                    "font_family）",
                                    key));
    }
  }
  return {};
}

auto merge_overrides(const Json& base, const Json& patch) -> Result<Json> {
  if (!base.is_object() || !patch.is_object()) {
    return unexpected(ErrorCode::Invalid, "主题覆盖必须是 JSON 对象");
  }
  // 先校验 `patch` 的形状（未知 token 由 `apply_theme_overrides` 负责——
  // 它掌握了权威的 token 名表，这里不重复一份）。
  for (const std::string_view section : {"colors", "metrics"}) {
    if (const auto* value = json_find(patch, section); value != nullptr && !value->is_object()) {
      return unexpected(ErrorCode::Invalid, std::format("{} 必须是对象", section));
    }
  }
  Json merged = base;
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    const std::string& key = it.key();
    // `colors`/`metrics` 是**逐键并合**的（它们才是稀疏覆盖的载体）；
    // 其余标量（`name`/`font_family`）直接以 `patch` 为准。
    if (key == "colors" || key == "metrics") {
      Json section = json_find(merged, key) != nullptr ? *json_find(merged, key) : Json::object();
      if (!section.is_object()) section = Json::object();
      for (auto entry = it.value().begin(); entry != it.value().end(); ++entry) {
        section[entry.key()] = entry.value();
      }
      merged[key] = std::move(section);
    } else {
      merged[key] = it.value();
    }
  }
  return merged;
}

auto theme_from_json(const Json& json, ThemeMode fallback) -> Result<Theme> {
  if (!json.is_object()) {
    return unexpected(ErrorCode::Invalid, "主题必须是 JSON 对象");
  }
  if (const auto* base_value = json_find(json, "base"); base_value != nullptr) {
    const std::string base = json_as_string(*base_value, "dark");
    const auto mode = theme_mode_from_name(base);
    if (!mode.has_value()) {
      return unexpected(ErrorCode::Invalid,
                        std::format("未知基准主题 base=\"{}\"（可用：light / dark）", base));
    }
    fallback = *mode;
  }
  Theme theme = Theme::by_mode(fallback);
  // `apply_theme_overrides` 见到 `base` 会报错，而这里 `base` 已经消费掉了——
  // 因此先把它从一份拷贝里摘除。
  Json body = json;
  body.erase("base");
  const auto applied = apply_theme_overrides(theme, body);
  if (!applied) return forward_error(applied.error());
  return theme;
}

auto parse_theme_json(std::string_view text, ThemeMode fallback) -> Result<Theme> {
  const auto parsed = json_parse(text);
  if (!parsed) return forward_error(parsed.error());
  return theme_from_json(*parsed, fallback);
}

auto load_theme_json_file(std::string_view path) -> Result<Json> {
  if (path.empty()) return unexpected(ErrorCode::Invalid, "主题文件路径为空");
  const auto text = fs::read_text(path);
  if (!text) {
    return unexpected(text.error().code,
                      std::format("读取主题文件失败：{}（{}）", path, text.error().message));
  }
  const auto parsed = json_parse(*text);
  if (!parsed) {
    return unexpected(parsed.error().code,
                      std::format("解析主题文件失败：{}（{}）", path, parsed.error().message));
  }
  return *parsed;
}

auto load_theme_file(std::string_view path, ThemeMode fallback) -> Result<Theme> {
  const auto json = load_theme_json_file(path);
  if (!json) return forward_error(json.error());
  const auto theme = theme_from_json(*json, fallback);
  if (!theme) {
    return unexpected(theme.error().code,
                      std::format("解析主题文件失败：{}（{}）", path, theme.error().message));
  }
  return theme;
}

auto write_theme_file(std::string_view path, const Theme& theme) -> Status {
  return json_write_file(path, theme_to_json(theme), /*pretty=*/true);
}

}  // namespace st::ui
