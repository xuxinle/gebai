#include "st/ui/svg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numbers>
#include <unordered_map>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"

namespace st::ui::svg {
namespace {

// ————————————————————————————————————————————
// XML 解析（图标子集：元素/属性/文本/注释/CDATA；不抛异常、不认即跳过）
// ————————————————————————————————————————————

struct XmlElement {
  std::string_view name{};
  std::vector<std::pair<std::string_view, std::string_view>> attributes{};
  std::string_view inner{};  // 开始标签到结束标签之间的原文（叶子即文本）
};

/// 光标式文本扫描小工具（不分配；`std::from_chars` 走 `<charconv>` 但本文件统一用它）。
auto parse_double(std::string_view text, double& out) -> bool {
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\n' || *begin == '\r')) {
    ++begin;
  }
  const char* cursor = begin;
  bool digits = false;
  auto scan_digits = [&cursor, end]() {
    while (cursor < end && *cursor >= '0' && *cursor <= '9') ++cursor;
  };
  if (cursor < end && (*cursor == '+' || *cursor == '-')) ++cursor;
  scan_digits();
  if (cursor < end && *cursor == '.') {
    ++cursor;
    scan_digits();
  }
  digits = cursor > begin && cursor != begin + (text.empty() ? 1 : 0);
  // 科学计数法
  if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
    const char* save = cursor;
    ++cursor;
    if (cursor < end && (*cursor == '+' || *cursor == '-')) ++cursor;
    const char* exp_start = cursor;
    scan_digits();
    if (cursor == exp_start) cursor = save;  // 没有指数数字：回退（后面的 e 属于下一词）
  }
  if (!digits && cursor == begin) return false;
  const std::string number(text.substr(0, static_cast<std::size_t>(cursor - text.data())));
  char* parse_end = nullptr;
  const double value = std::strtod(number.c_str(), &parse_end);
  if (parse_end == number.c_str()) return false;
  out = value;
  return true;
}

/// 从 `text` 头部读一个数并推进 `pos`（跳过分隔符：空格/逗号）。
struct NumberCursor {
  std::string_view text;
  std::size_t pos{0};

  auto next(double& out) -> bool {
    while (pos < text.size()) {
      const char ch = text[pos];
      if (ch == ' ' || ch == ',' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++pos;
      } else {
        break;
      }
    }
    if (pos >= text.size()) return false;
    double value = 0.0;
    if (!parse_double(text.substr(pos), value)) return false;
    // parse_double 停在哪：重扫（parse_double 只给值不给长度，这里自己走一遍）
    std::size_t end = pos;
    bool any = false;
    const auto scan_digits = [this, &end, &any]() {
      while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
        any = true;
      }
    };
    if (end < text.size() && (text[end] == '+' || text[end] == '-')) ++end;
    scan_digits();
    if (end < text.size() && text[end] == '.') {
      ++end;
      scan_digits();
    }
    if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
      std::size_t save = end;
      ++end;
      if (end < text.size() && (text[end] == '+' || text[end] == '-')) ++end;
      const std::size_t exp_start = end;
      scan_digits();
      if (end == exp_start) end = save;
    }
    if (!any) {
      // 没有数字（比如 "M" 后直接跟字母）——推进一位防死循环
      ++pos;
      return false;
    }
    pos = end;
    out = value;
    return true;
  }
};

auto is_space(char ch) noexcept -> bool {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/// 找下一个标签（`<`）；跳过注释 `<!-- -->`、CDATA、`<? ?>`、`<!DOCTYPE>`。
/// 返回 nullopt = 再无标签。`tag_begin/tag_end` 是含尖括号的整段。
struct TagSpan {
  std::size_t begin{0};
  std::size_t end{0};  // 结束尖括号之后
};

auto next_tag(std::string_view text, std::size_t& pos) -> std::optional<TagSpan> {
  while (true) {
    const std::size_t lt = text.find('<', pos);
    if (lt == std::string_view::npos) return std::nullopt;
    const std::size_t after = lt + 1;
    if (text.compare(after, 3, "!--") == 0) {
      const std::size_t close = text.find("-->", after);
      if (close == std::string_view::npos) return std::nullopt;
      pos = close + 3;
      continue;
    }
    if (after < text.size() && (text[after] == '?' || text[after] == '!')) {
      const std::size_t close = text.find('>', after);
      if (close == std::string_view::npos) return std::nullopt;
      pos = close + 1;
      continue;
    }
    const std::size_t close = text.find('>', after);
    if (close == std::string_view::npos) return std::nullopt;
    pos = close + 1;
    return TagSpan{lt, close + 1};
  }
}

struct XmlToken {
  enum class Kind : std::uint8_t { Open, Close, SelfClose, Text };
  Kind kind{Kind::Text};
  std::string_view name{};
  std::vector<std::pair<std::string_view, std::string_view>> attributes{};
  std::string_view raw{};  // 该 token 的原文
};

auto parse_tag(std::string_view tag, std::size_t tag_begin_offset, XmlToken& out) -> bool {
  // tag 含 '<' 与 '>'；内部：名字 + 属性（attr="v" | attr='v' | attr=v）
  std::size_t i = 1;  // 跳过 '<'
  if (i < tag.size() && tag[i] == '/') {
    out.kind = XmlToken::Kind::Close;
    ++i;
  } else {
    out.kind = XmlToken::Kind::Open;
  }
  while (i < tag.size() && !is_space(tag[i]) && tag[i] != '>' && tag[i] != '/') ++i;
  out.name = tag.substr(1 + (out.kind == XmlToken::Kind::Close ? 1 : 0),
                        i - 1 - (out.kind == XmlToken::Kind::Close ? 1 : 0));
  // 自闭合判定：'>' 前最后一个非空字符是 '/'
  std::size_t last = tag.size();
  while (last > 0 && (tag[last - 1] == '>' || is_space(tag[last - 1]))) --last;
  const bool self_close = last > 0 && tag[last - 1] == '/';
  if (self_close) out.kind = XmlToken::Kind::SelfClose;
  // 属性
  out.attributes.clear();
  while (i < tag.size()) {
    while (i < tag.size() && is_space(tag[i])) ++i;
    if (i >= tag.size() || tag[i] == '>' || tag[i] == '/') break;
    const std::size_t name_begin = i;
    while (i < tag.size() && !is_space(tag[i]) && tag[i] != '=' && tag[i] != '>' && tag[i] != '/') {
      ++i;
    }
    std::string_view attr_name = tag.substr(name_begin, i - name_begin);
    if (attr_name.empty()) {
      ++i;
      continue;
    }
    std::string_view attr_value{};
    if (i < tag.size() && tag[i] == '=') {
      ++i;
      while (i < tag.size() && is_space(tag[i])) ++i;
      if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
        const char quote = tag[i++];
        const std::size_t value_begin = i;
        while (i < tag.size() && tag[i] != quote) ++i;
        attr_value = tag.substr(value_begin, i - value_begin);
        if (i < tag.size()) ++i;  // 跳过引号
      } else {
        const std::size_t value_begin = i;
        while (i < tag.size() && !is_space(tag[i]) && tag[i] != '>') ++i;
        attr_value = tag.substr(value_begin, i - value_begin);
      }
    }
    out.attributes.emplace_back(attr_name, attr_value);
  }
  (void)tag_begin_offset;
  return !out.name.empty();
}

// ————————————————————————————————————————————
// path d 解析（全指令 + 相对 + 隐式重复 + A 圆弧）
// ————————————————————————————————————————————

/// 把 SVG 弧线端点参数换算为圆心与角度（W3C SVG Implementation Notes F.6.5）。
struct ArcParams {
  math::Point center{};
  float radius_x{0.0f};
  float radius_y{0.0f};
  float rotation_deg{0.0f};
  float theta1{0.0f};
  float theta2{0.0f};
  float delta{0.0f};
};

auto arc_center_form(math::Point from, math::Point to, float rx, float ry, float rotation_deg,
                     bool large_arc, bool sweep) -> std::optional<ArcParams> {
  if (rx <= 0.0f || ry <= 0.0f) return std::nullopt;
  const float phi = rotation_deg * std::numbers::pi_v<float> / 180.0f;
  const float cos_phi = std::cos(phi);
  const float sin_phi = std::sin(phi);
  const float dx = (from.x - to.x) * 0.5f;
  const float dy = (from.y - to.y) * 0.5f;
  const float x1p = cos_phi * dx + sin_phi * dy;
  const float y1p = -sin_phi * dx + cos_phi * dy;
  // 半径不足时放大（F.6.6）
  float rx_sq = rx * rx;
  float ry_sq = ry * ry;
  const float lambda = x1p * x1p / rx_sq + y1p * y1p / ry_sq;
  if (lambda > 1.0f) {
    const float scale = std::sqrt(lambda);
    rx *= scale;
    ry *= scale;
    rx_sq = rx * rx;
    ry_sq = ry * ry;
  }
  const float sign = large_arc != sweep ? 1.0f : -1.0f;
  const float denominator = rx_sq * y1p * y1p + ry_sq * x1p * x1p;
  if (denominator <= 0.0f) return std::nullopt;
  float numerator = rx_sq * ry_sq - rx_sq * y1p * y1p - ry_sq * x1p * x1p;
  if (numerator < 0.0f) numerator = 0.0f;
  const float coef = sign * std::sqrt(numerator / denominator);
  const float cxp = coef * (rx * y1p / ry);
  const float cyp = coef * -(ry * x1p / rx);
  const float cx = cos_phi * cxp - sin_phi * cyp + (from.x + to.x) * 0.5f;
  const float cy = sin_phi * cxp + cos_phi * cyp + (from.y + to.y) * 0.5f;
  const auto angle = [&](float ux, float uy, float vx, float vy) {
    const float dot = ux * vx + uy * vy;
    const float len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    if (len <= 0.0f) return 0.0f;
    float value = dot / len;
    value = std::clamp(value, -1.0f, 1.0f);
    float angle_rad = std::acos(value);
    if (ux * vy - uy * vx < 0.0f) angle_rad = -angle_rad;
    return angle_rad;
  };
  const float theta1 = angle(1.0f, 0.0f, (x1p - cxp) / rx, (y1p - cyp) / ry);
  float delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
  if (!sweep && delta > 0.0f) delta -= 2.0f * std::numbers::pi_v<float>;
  if (sweep && delta < 0.0f) delta += 2.0f * std::numbers::pi_v<float>;
  return ArcParams{math::Point{cx, cy}, rx, ry, rotation_deg, theta1, theta1 + delta, delta};
}

auto arc_to_bezier(math::Point from, const ArcParams& arc, raster::Path& out) -> void {
  // 端点参数 → 中心参数后按 90° 一段转三次贝塞尔（F.6.4）
  const float phi = arc.rotation_deg * std::numbers::pi_v<float> / 180.0f;
  const float cos_phi = std::cos(phi);
  const float sin_phi = std::sin(phi);
  const float total = arc.delta;
  const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(total) /
                                                              (std::numbers::pi_v<float> / 2.0f))));
  const float delta_per = total / static_cast<float>(segments);
  const float k = 4.0f / 3.0f * std::tan(delta_per / 4.0f);
  float theta = arc.theta1;
  float cos_t = std::cos(theta);
  float sin_t = std::sin(theta);
  const auto point_at = [&](float c, float s) {
    const float x = arc.radius_x * c;
    const float y = arc.radius_y * s;
    return math::Point{arc.center.x + x * cos_phi - y * sin_phi,
                       arc.center.y + x * sin_phi + y * cos_phi};
  };
  math::Point current = point_at(cos_t, sin_t);
  // from 理论上等于 current（浮点误差可忽略）；不强制 line_to
  (void)from;
  (void)current;
  for (int seg = 0; seg < segments; ++seg) {
    const float next_theta = theta + delta_per;
    const float cos_n = std::cos(next_theta);
    const float sin_n = std::sin(next_theta);
    const math::Point p1 = point_at(cos_t - k * sin_t, sin_t + k * cos_t);
    const math::Point p2 = point_at(cos_n + k * sin_n, sin_n - k * cos_n);
    const math::Point p3 = point_at(cos_n, sin_n);
    out.cubic_to(p1, p2, p3);
    theta = next_theta;
    cos_t = cos_n;
    sin_t = sin_n;
  }
}

/// 样式属性值解析。
auto attr(const XmlToken& token, std::string_view name) -> std::string_view {
  for (const auto& [key, value] : token.attributes) {
    if (key == name) return value;
  }
  return {};
}

/// 解析 `stroke-linecap` / `stroke-linejoin` 并写入 `style`（元素级与继承级共用一处）。
auto apply_line_style(const XmlToken& token, Style& style) -> void {
  if (const auto cap = attr(token, "stroke-linecap"); !cap.empty()) {
    if (cap == "round") style.line_cap = raster::LineCap::Round;
    else if (cap == "square") style.line_cap = raster::LineCap::Square;
    else style.line_cap = raster::LineCap::Butt;
  }
  if (const auto join = attr(token, "stroke-linejoin"); !join.empty()) {
    if (join == "round") style.line_join = raster::LineJoin::Round;
    else if (join == "bevel") style.line_join = raster::LineJoin::Bevel;
    else style.line_join = raster::LineJoin::Miter;
  }
}


/// CSS 长度：数字或百分比（`length` 是该维度满值——width/height 单独处理）。
auto parse_length_percent(std::string_view text, float full, float& out) -> bool {
  double value = 0.0;
  if (!parse_double(text, value)) return false;
  std::size_t end = 0;
  while (end < text.size() && !is_space(text[end])) ++end;
  const std::string_view unit = text.substr(0, end);
  (void)unit;
  bool percent = false;
  for (const char ch : text) {
    if (ch == '%') percent = true;
  }
  out = percent ? static_cast<float>(value) * full / 100.0f : static_cast<float>(value);
  return true;
}

}  // namespace

// ————————————————————————————————————————————
// 公共 API：颜色 / transform / d 解析
// ————————————————————————————————————————————

auto parse_color(std::string_view text) -> std::optional<math::Color> {
  if (text.empty() || text == "currentColor" || text == "inherit") return std::nullopt;
  if (text[0] == '#') {
    const std::string_view hex = text.substr(1);
    auto hex_value = [](char ch) -> int {
      if (ch >= '0' && ch <= '9') return ch - '0';
      if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
      return -1;
    };
    const auto expand = [&](std::size_t index) -> int {
      const int value = hex_value(hex[index]);
      return value;
    };
    if (hex.size() == 3) {
      const int r = expand(0), g = expand(1), b = expand(2);
      if (r < 0 || g < 0 || b < 0) return std::nullopt;
      return math::Color{static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17),
                         static_cast<std::uint8_t>(b * 17), 255U};
    }
    if (hex.size() == 4) {
      const int r = expand(0), g = expand(1), b = expand(2), a = expand(3);
      if (r < 0 || g < 0 || b < 0 || a < 0) return std::nullopt;
      return math::Color{static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17),
                         static_cast<std::uint8_t>(b * 17), static_cast<std::uint8_t>(a * 17)};
    }
    if (hex.size() == 6) {
      const auto byte_at = [&](std::size_t index) -> std::optional<int> {
        const int high = hex_value(hex[index]);
        const int low = hex_value(hex[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        return high * 16 + low;
      };
      const auto r = byte_at(0), g = byte_at(2), b = byte_at(4);
      if (!r || !g || !b) return std::nullopt;
      return math::Color{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g),
                         static_cast<std::uint8_t>(*b), 255U};
    }
    if (hex.size() == 8) {
      const auto component = [&](std::size_t index) -> int {
        const int high = hex_value(hex[index]);
        const int low = hex_value(hex[index + 1]);
        return (high < 0 || low < 0) ? -1 : high * 16 + low;
      };
      const int r = component(0), g = component(2), b = component(4), a = component(6);
      if (r < 0 || g < 0 || b < 0 || a < 0) return std::nullopt;
      return math::Color{static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                         static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
    }
    return std::nullopt;
  }
  static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 17> kNamed{{
      {"black", 0x000000FFU},   {"silver", 0xC0C0C0FFU}, {"gray", 0x808080FFU},
      {"grey", 0x808080FFU},    {"white", 0xFFFFFFFFU},  {"maroon", 0x800000FFU},
      {"red", 0xFF0000FFU},     {"purple", 0x800080FFU}, {"fuchsia", 0xFF00FFFFU},
      {"green", 0x008000FFU},   {"lime", 0x00FF00FFU},   {"olive", 0x808000FFU},
      {"yellow", 0xFFFF00FFU},  {"navy", 0x000080FFU},   {"blue", 0x0000FFFFU},
      {"teal", 0x008080FFU},    {"aqua", 0x00FFFFFFU},
  }};
  for (const auto& [name, value] : kNamed) {
    if (text == name) return math::Color::from_rgba_hex(value);
  }
  return std::nullopt;
}

auto Transform::translate(float tx, float ty) noexcept -> Transform {
  return Transform{1.0f, 0.0f, 0.0f, 1.0f, tx, ty};
}

auto Transform::scale(float sx, float sy) noexcept -> Transform {
  return Transform{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
}

auto Transform::rotate(float degrees) noexcept -> Transform {
  const float rad = degrees * std::numbers::pi_v<float> / 180.0f;
  return Transform{std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0.0f, 0.0f};
}

auto Transform::matrix(float pa, float pb, float pc, float pd, float pe, float pf) noexcept
    -> Transform {
  return Transform{pa, pb, pc, pd, pe, pf};
}

auto Transform::operator*(const Transform& rhs) const noexcept -> Transform {
  // 本 ∘ rhs（先 rhs 后本）；SVG transform 列表语义 = 依次左乘
  return Transform{a * rhs.a + c * rhs.b, b * rhs.a + d * rhs.b, a * rhs.c + c * rhs.d,
                   b * rhs.c + d * rhs.d, a * rhs.e + c * rhs.f + e, b * rhs.e + d * rhs.f + f};
}

auto Transform::apply(math::Point point) const noexcept -> math::Point {
  return math::Point{a * point.x + c * point.y + e, b * point.x + d * point.y + f};
}

auto Transform::scalar_factor() const noexcept -> float {
  return std::sqrt(std::abs(a * d - b * c));
}

auto parse_path(std::string_view d) -> std::optional<raster::Path> {
  if (d.empty()) return std::nullopt;
  raster::Path path;
  NumberCursor cursor{d, 0};
  math::Point current{0.0f, 0.0f};
  math::Point subpath_start{0.0f, 0.0f};
  char previous_command = ' ';
  // 二次曲线反射控制点（T 用）与三次反射（S 用）
  math::Point last_quad_control{};
  math::Point last_cubic_control{};
  bool has_quad_control = false;
  bool has_cubic_control = false;
  bool started = false;

  const auto need_start = [&](math::Point point) {
    if (!started) {
      path.move_to(point);
      subpath_start = point;
      started = true;
    }
  };

  while (cursor.pos < d.size()) {
    while (cursor.pos < d.size() && is_space(d[cursor.pos])) ++cursor.pos;
    if (cursor.pos >= d.size()) break;
    char command = d[cursor.pos];
    if (command == ',' ) { ++cursor.pos; continue; }
    const bool is_letter = (command >= 'A' && command <= 'Z') || (command >= 'a' && command <= 'z');
    if (is_letter) {
      ++cursor.pos;
      previous_command = command;
    } else {
      // 隐式重复：上一指令延续（M→L、m→l）
      command = previous_command;
      if (command == 'M') command = 'L';
      if (command == 'm') command = 'l';
    }
    const bool relative = command >= 'a' && command <= 'z';
    const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
    double n1 = 0.0, n2 = 0.0, n3 = 0.0, n4 = 0.0, n5 = 0.0, n6 = 0.0, n7 = 0.0;
    switch (kind) {
      case 'M': {
        if (!cursor.next(n1) || !cursor.next(n2)) return std::nullopt;
        const math::Point point = relative ? math::Point{current.x + static_cast<float>(n1),
                                                          current.y + static_cast<float>(n2)}
                                           : math::Point{static_cast<float>(n1), static_cast<float>(n2)};
        if (!started) {
          subpath_start = point;
        }
        path.move_to(point);
        started = true;
        current = point;
        // 后续隐式重复按 L/l
        previous_command = relative ? 'l' : 'L';
        break;
      }
      case 'L': {
        if (!cursor.next(n1) || !cursor.next(n2)) return std::nullopt;
        const math::Point point = relative ? math::Point{current.x + static_cast<float>(n1),
                                                          current.y + static_cast<float>(n2)}
                                           : math::Point{static_cast<float>(n1), static_cast<float>(n2)};
        need_start(point);
        path.line_to(point);
        current = point;
        break;
      }
      case 'H': {
        if (!cursor.next(n1)) return std::nullopt;
        const float x = relative ? current.x + static_cast<float>(n1) : static_cast<float>(n1);
        const math::Point point{x, current.y};
        need_start(point);
        path.line_to(point);
        current = point;
        break;
      }
      case 'V': {
        if (!cursor.next(n1)) return std::nullopt;
        const float y = relative ? current.y + static_cast<float>(n1) : static_cast<float>(n1);
        const math::Point point{current.x, y};
        need_start(point);
        path.line_to(point);
        current = point;
        break;
      }
      case 'C': {
        if (!cursor.next(n1) || !cursor.next(n2) || !cursor.next(n3) || !cursor.next(n4) ||
            !cursor.next(n5) || !cursor.next(n6)) {
          return std::nullopt;
        }
        math::Point c1{static_cast<float>(n1), static_cast<float>(n2)};
        math::Point c2{static_cast<float>(n3), static_cast<float>(n4)};
        math::Point end{static_cast<float>(n5), static_cast<float>(n6)};
        if (relative) {
          c1.x += current.x; c1.y += current.y;
          c2.x += current.x; c2.y += current.y;
          end.x += current.x; end.y += current.y;
        }
        need_start(end);
        path.cubic_to(c1, c2, end);
        last_cubic_control = c2;
        has_cubic_control = true;
        current = end;
        break;
      }
      case 'S': {
        if (!cursor.next(n1) || !cursor.next(n2) || !cursor.next(n3) || !cursor.next(n4)) {
          return std::nullopt;
        }
        math::Point c2{static_cast<float>(n1), static_cast<float>(n2)};
        math::Point end{static_cast<float>(n3), static_cast<float>(n4)};
        if (relative) {
          c2.x += current.x; c2.y += current.y;
          end.x += current.x; end.y += current.y;
        }
        // 反射控制点：上一段三次的第二控制点关于当前点对称
        math::Point c1 = has_cubic_control
                             ? math::Point{2.0f * current.x - last_cubic_control.x,
                                           2.0f * current.y - last_cubic_control.y}
                             : current;
        need_start(end);
        path.cubic_to(c1, c2, end);
        last_cubic_control = c2;
        has_cubic_control = true;
        current = end;
        break;
      }
      case 'Q': {
        if (!cursor.next(n1) || !cursor.next(n2) || !cursor.next(n3) || !cursor.next(n4)) {
          return std::nullopt;
        }
        math::Point control{static_cast<float>(n1), static_cast<float>(n2)};
        math::Point end{static_cast<float>(n3), static_cast<float>(n4)};
        if (relative) {
          control.x += current.x; control.y += current.y;
          end.x += current.x; end.y += current.y;
        }
        need_start(end);
        path.quad_to(control, end);
        last_quad_control = control;
        has_quad_control = true;
        current = end;
        break;
      }
      case 'T': {
        if (!cursor.next(n1) || !cursor.next(n2)) return std::nullopt;
        math::Point end = relative ? math::Point{current.x + static_cast<float>(n1),
                                                  current.y + static_cast<float>(n2)}
                                   : math::Point{static_cast<float>(n1), static_cast<float>(n2)};
        math::Point control = has_quad_control
                                  ? math::Point{2.0f * current.x - last_quad_control.x,
                                                2.0f * current.y - last_quad_control.y}
                                  : current;
        need_start(end);
        path.quad_to(control, end);
        last_quad_control = control;
        has_quad_control = true;
        current = end;
        break;
      }
      case 'A': {
        if (!cursor.next(n1) || !cursor.next(n2) || !cursor.next(n3) || !cursor.next(n4) ||
            !cursor.next(n5) || !cursor.next(n6) || !cursor.next(n7)) {
          return std::nullopt;
        }
        const float rx = std::abs(static_cast<float>(n1));
        const float ry = std::abs(static_cast<float>(n2));
        const float rotation = static_cast<float>(n3);
        const bool large_arc = n4 != 0.0;
        const bool sweep = n5 != 0.0;
        math::Point end = relative ? math::Point{current.x + static_cast<float>(n6),
                                                  current.y + static_cast<float>(n7)}
                                   : math::Point{static_cast<float>(n6), static_cast<float>(n7)};
        need_start(end);
        if (rx > 0.0f && ry > 0.0f &&
            (std::abs(end.x - current.x) > 1e-6f || std::abs(end.y - current.y) > 1e-6f)) {
          if (const auto arc = arc_center_form(current, end, rx, ry, rotation, large_arc, sweep)) {
            arc_to_bezier(current, *arc, path);
          }
        }
        current = end;
        break;
      }
      case 'Z': {
        if (started) {
          path.close();
          current = subpath_start;
        }
        break;
      }
      default:
        // 不认识的指令：跳过一个字符防死循环（宁缺毋滥）
        ++cursor.pos;
        break;
    }
  }
  if (path.is_empty()) return std::nullopt;
  return path;
}

// ————————————————————————————————————————————
// 文档解析：元素树展平 → 节点表（样式级联 + transform + 形状 → path）
// ————————————————————————————————————————————

namespace {

/// 把 transform 属性文本解析为一个矩阵（多个变换函数列表按序组合）。
auto parse_transform(std::string_view text) -> Transform {
  Transform result = Transform::identity();
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t open = text.find('(', pos);
    if (open == std::string_view::npos) break;
    std::size_t name_begin = open;
    while (name_begin > pos && !is_space(text[name_begin - 1])) --name_begin;
    const std::string_view func = text.substr(name_begin, open - name_begin);
    const std::size_t close = text.find(')', open);
    if (close == std::string_view::npos) break;
    NumberCursor numbers{text.substr(open + 1, close - open - 1), 0};
    Transform step = Transform::identity();
    double n1 = 0.0, n2 = 0.0, n3 = 0.0, n4 = 0.0, n5 = 0.0, n6 = 0.0;
    if (func == "translate") {
      if (numbers.next(n1)) {
        numbers.next(n2);  // ty 缺省 0
        step = Transform::translate(static_cast<float>(n1), static_cast<float>(n2));
      }
    } else if (func == "scale") {
      if (numbers.next(n1)) {
        if (!numbers.next(n2)) n2 = n1;  // 均匀缩放
        step = Transform::scale(static_cast<float>(n1), static_cast<float>(n2));
      }
    } else if (func == "rotate") {
      if (numbers.next(n1)) {
        const bool has_center = numbers.next(n2) && numbers.next(n3);
        const float angle = static_cast<float>(n1);
        if (has_center) {
          const float cx = static_cast<float>(n2);
          const float cy = static_cast<float>(n3);
          step = Transform::translate(cx, cy) * Transform::rotate(angle) *
                 Transform::translate(-cx, -cy);
        } else {
          step = Transform::rotate(angle);
        }
      }
    } else if (func == "matrix") {
      if (numbers.next(n1) && numbers.next(n2) && numbers.next(n3) && numbers.next(n4) &&
          numbers.next(n5) && numbers.next(n6)) {
        step = Transform::matrix(static_cast<float>(n1), static_cast<float>(n2),
                                 static_cast<float>(n3), static_cast<float>(n4),
                                 static_cast<float>(n5), static_cast<float>(n6));
      }
    } else if (func == "skewX" || func == "skewY") {
      if (numbers.next(n1)) {
        const float tan_v = std::tan(static_cast<float>(n1) * std::numbers::pi_v<float> / 180.0f);
        if (func == "skewX") {
          step = Transform::matrix(1.0f, 0.0f, tan_v, 1.0f, 0.0f, 0.0f);
        } else {
          step = Transform::matrix(1.0f, tan_v, 0.0f, 1.0f, 0.0f, 0.0f);
        }
      }
    }
    result = result * step;
    pos = close + 1;
  }
  return result;
}

/// 累计 transform 应用于路径（逐控制点）。
auto apply_transform(const raster::Path& path, const Transform& transform) -> raster::Path {
  raster::Path out;
  const auto points = path.raw_points();
  (void)points;
  for (const auto& command : path.commands()) {
    switch (command.kind) {
      case raster::PathCommand::Kind::MoveTo:
        out.move_to(transform.apply(command.p1));
        break;
      case raster::PathCommand::Kind::LineTo:
        out.line_to(transform.apply(command.p1));
        break;
      case raster::PathCommand::Kind::QuadTo:
        out.quad_to(transform.apply(command.p1), transform.apply(command.p2));
        break;
      case raster::PathCommand::Kind::CubicTo:
        out.cubic_to(transform.apply(command.p1), transform.apply(command.p2),
                     transform.apply(command.p3));
        break;
      case raster::PathCommand::Kind::Close:
        out.close();
        break;
    }
  }
  return out;
}

struct StyleContext {
  Style style{};
  Transform transform = Transform::identity();
  float stroke_width_viewbox{1.0f};
  std::string id_scope{};  // <use> 展开时的引用栈（防环）
};

auto apply_shape(const XmlToken& token, const StyleContext& context, float view_w, float view_h,
                 std::vector<Node>& nodes) -> void {
  Node node;
  node.style = context.style;
  node.stroke_width_viewbox = context.stroke_width_viewbox;
  const std::string_view name = token.name;
  const auto transform_text = attr(token, "transform");
  Transform transform = context.transform;
  if (!transform_text.empty()) {
    transform = parse_transform(transform_text) * transform;
  }
  float stroke_width = node.style.stroke_width;
  if (const auto sw = attr(token, "stroke-width"); !sw.empty()) {
    double value = 0.0;
    if (parse_double(sw, value)) stroke_width = static_cast<float>(value);
  }
  node.stroke_width_viewbox = stroke_width * transform.scalar_factor();

  if (name == "path") {
    const auto d = attr(token, "d");
    if (auto path = parse_path(d)) {
      node.path = apply_transform(*path, transform);
    }
  } else if (name == "rect") {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, rx = 0.0f, ry = 0.0f;
    parse_length_percent(attr(token, "x"), view_w, x);
    parse_length_percent(attr(token, "y"), view_h, y);
    parse_length_percent(attr(token, "width"), view_w, w);
    parse_length_percent(attr(token, "height"), view_h, h);
    parse_length_percent(attr(token, "rx"), w, rx);
    parse_length_percent(attr(token, "ry"), h, ry);
    if (w > 0.0f && h > 0.0f) {
      raster::Path path;
      const float radius = std::max(rx, ry) > 0.0f ? std::max(rx, ry) : 0.0f;
      path.add_rounded_rect(math::Rect{x, y, w, h}, radius);
      node.path = apply_transform(path, transform);
    }
  } else if (name == "circle") {
    float cx = 0.0f, cy = 0.0f, r = 0.0f;
    parse_length_percent(attr(token, "cx"), view_w, cx);
    parse_length_percent(attr(token, "cy"), view_h, cy);
    parse_length_percent(attr(token, "r"), std::max(view_w, view_h), r);
    if (r > 0.0f) {
      raster::Path path;
      path.add_circle(math::Point{cx, cy}, r);
      node.path = apply_transform(path, transform);
    }
  } else if (name == "ellipse") {
    float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f;
    parse_length_percent(attr(token, "cx"), view_w, cx);
    parse_length_percent(attr(token, "cy"), view_h, cy);
    parse_length_percent(attr(token, "rx"), view_w, rx);
    parse_length_percent(attr(token, "ry"), view_h, ry);
    if (rx > 0.0f && ry > 0.0f) {
      raster::Path path;
      path.add_ellipse(math::Rect{cx - rx, cy - ry, rx * 2.0f, ry * 2.0f});
      node.path = apply_transform(path, transform);
    }
  } else if (name == "line") {
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    parse_length_percent(attr(token, "x1"), view_w, x1);
    parse_length_percent(attr(token, "y1"), view_h, y1);
    parse_length_percent(attr(token, "x2"), view_w, x2);
    parse_length_percent(attr(token, "y2"), view_h, y2);
    raster::Path path;
    path.move_to(math::Point{x1, y1});
    path.line_to(math::Point{x2, y2});
    node.path = apply_transform(path, transform);
  } else if (name == "polyline" || name == "polygon") {
    NumberCursor numbers{attr(token, "points"), 0};
    double x = 0.0;
    double y = 0.0;
    raster::Path path;
    bool first = true;
    while (numbers.next(x) && numbers.next(y)) {
      const math::Point point{static_cast<float>(x), static_cast<float>(y)};
      if (first) {
        path.move_to(point);
        first = false;
      } else {
        path.line_to(point);
      }
    }
    if (!first) {
      if (name == "polygon") path.close();
      node.path = apply_transform(path, transform);
    }
  }
  if (node.path.is_empty()) return;

  // 元素级样式覆盖（fill/stroke/opacity…）
  const auto fill = attr(token, "fill");
  if (fill == "none") {
    node.style.fill_none = true;
    node.style.fill = std::nullopt;
    node.style.fill_current = false;
  } else if (fill == "currentColor") {
    node.style.fill_none = false;
    node.style.fill = std::nullopt;
    node.style.fill_current = true;
  } else if (auto color = parse_color(fill)) {
    node.style.fill = color;
    node.style.fill_none = false;
    node.style.fill_current = false;
  }
  const auto stroke = attr(token, "stroke");
  if (stroke == "none") {
    node.style.stroke_none = true;
    node.style.stroke = std::nullopt;
    node.style.stroke_current = false;
  } else if (stroke == "currentColor") {
    node.style.stroke_none = false;
    node.style.stroke = std::nullopt;
    node.style.stroke_current = true;
  } else if (auto color = parse_color(stroke)) {
    node.style.stroke = color;
    node.style.stroke_none = false;
    node.style.stroke_current = false;
  }
  double opacity = 0.0;
  if (parse_double(attr(token, "opacity"), opacity)) {
    node.style.opacity *= static_cast<float>(opacity);
  }
  if (parse_double(attr(token, "fill-opacity"), opacity)) {
    node.style.fill_opacity *= static_cast<float>(opacity);
  }
  if (parse_double(attr(token, "stroke-opacity"), opacity)) {
    node.style.stroke_opacity *= static_cast<float>(opacity);
  }
  if (attr(token, "fill-rule") == "evenodd") {
    node.style.fill_rule = FillRule::EvenOdd;
  }
  // cap/join 的解析**只在这一处**（`apply_line_style`）：元素级与 `<g>` 继承级共用，
  // 两处各写一份必然漂移（本文件已有 `fill-rule` 只在继承级解析、元素级漏掉的先例）。
  apply_line_style(token, node.style);
  nodes.push_back(std::move(node));
}

auto style_from_attributes(const XmlToken& token, const Style& inherited) -> Style {
  Style style = inherited;
  if (const auto fill = attr(token, "fill"); !fill.empty()) {
    if (fill == "none") {
      style.fill_none = true;
      style.fill = std::nullopt;
      style.fill_current = false;
    } else if (fill == "currentColor") {
      style.fill_none = false;
      style.fill = std::nullopt;
      style.fill_current = true;
    } else if (auto color = parse_color(fill)) {
      style.fill = color;
      style.fill_none = false;
      style.fill_current = false;
    }
  }
  if (const auto stroke = attr(token, "stroke"); !stroke.empty()) {
    if (stroke == "none") {
      style.stroke_none = true;
      style.stroke = std::nullopt;
      style.stroke_current = false;
    } else if (stroke == "currentColor") {
      style.stroke_none = false;
      style.stroke = std::nullopt;
      style.stroke_current = true;
    } else if (auto color = parse_color(stroke)) {
      style.stroke = color;
      style.stroke_none = false;
      style.stroke_current = false;
    }
  }
  double number = 0.0;
  if (parse_double(attr(token, "stroke-width"), number) && number > 0.0) {
    style.stroke_width = static_cast<float>(number);
  }
  if (parse_double(attr(token, "opacity"), number)) {
    style.opacity *= static_cast<float>(number);
  }
  if (parse_double(attr(token, "fill-opacity"), number)) {
    style.fill_opacity *= static_cast<float>(number);
  }
  if (parse_double(attr(token, "stroke-opacity"), number)) {
    style.stroke_opacity *= static_cast<float>(number);
  }
  if (attr(token, "fill-rule") == "evenodd") style.fill_rule = FillRule::EvenOdd;
  // `stroke-linecap` / `stroke-linejoin` 是**可继承属性**：`<g>` 上声明要传给子元素
  // （Lucide 等图标库正是在根/组上写 `stroke-linecap="round"`）。
  apply_line_style(token, style);
  return style;
}

using SymbolTable = std::map<std::string_view, std::string_view, std::less<>>;

auto flatten(std::string_view scope, const StyleContext& context, float view_w, float view_h,
             const SymbolTable& symbols, const SymbolTable& shapes,
             std::vector<Node>& nodes) -> void;

/// `<use>` 展开：symbol 优先，其次 defs 内的形状元素；防环 + x/y 偏移语义。
auto expand_use(const XmlToken& token, const StyleContext& current, float view_w, float view_h,
                const SymbolTable& symbols, const SymbolTable& shapes,
                std::vector<Node>& nodes) -> void {
  auto href = attr(token, "href");
  if (href.empty()) href = attr(token, "xlink:href");
  if (href.empty() || href.front() != '#') return;
  const std::string_view id = href.substr(1);
  if (current.id_scope.find(std::string("\x01") + std::string(id) + "\x01") !=
      std::string::npos) {
    return;  // 环
  }
  StyleContext use_context = current;
  use_context.id_scope += "\x01" + std::string(id) + "\x01";
  float ux = 0.0f, uy = 0.0f;
  parse_length_percent(attr(token, "x"), view_w, ux);
  parse_length_percent(attr(token, "y"), view_h, uy);
  Transform local = Transform::identity();
  if (const auto t = attr(token, "transform"); !t.empty()) local = parse_transform(t);
  if (ux != 0.0f || uy != 0.0f) local = Transform::translate(ux, uy) * local;
  use_context.transform = local * current.transform;
  if (const auto found = symbols.find(id); found != symbols.end()) {
    // symbol 内联在当前文档里：inner 无自描述 viewBox——大多数图标 sprite 的 symbol
    // 与根同坐标系（0 0 24 24），直接按当前坐标系展平即可；带独立 viewBox 的 symbol
    // 由 IconSet::get 的包裹路径处理（完整文档解析）。
    flatten(found->second, use_context, view_w, view_h, symbols, shapes, nodes);
    return;
  }
  if (const auto found = shapes.find(id); found != shapes.end()) {
    flatten(found->second, use_context, view_w, view_h, symbols, shapes, nodes);
  }
}
/// 展平一层元素序列（`scope` 为待扫描文本；`symbols` 为 symbol 表供 `<use>` 展开）。
auto flatten(std::string_view scope, const StyleContext& context, float view_w, float view_h,
             const SymbolTable& symbols, const SymbolTable& shapes,
             std::vector<Node>& nodes) -> void {
  std::size_t pos = 0;
  StyleContext current = context;
  std::vector<StyleContext> stack{};
  while (true) {
    const std::size_t text_begin = pos;
    const auto tag = next_tag(scope, pos);
    if (!tag) break;
    // 结束标签：弹栈
    XmlToken token;
    if (!parse_tag(scope.substr(tag->begin, tag->end - tag->begin), tag->begin, token)) continue;
    if (token.kind == XmlToken::Kind::Close) {
      if (!stack.empty()) {
        current = stack.back();
        stack.pop_back();
      }
      continue;
    }
    if (token.kind == XmlToken::Kind::Open) {
      // 找匹配结束标签（同名、允许嵌套同名——用深度计数）
      std::size_t depth = 1;
      std::size_t search = pos;
      std::size_t content_end = pos;
      while (depth > 0) {
        const auto inner_tag = next_tag(scope, search);
        if (!inner_tag) {
          content_end = scope.size();
          depth = 0;
          break;
        }
        XmlToken probe;
        if (!parse_tag(scope.substr(inner_tag->begin, inner_tag->end - inner_tag->begin),
                       inner_tag->begin, probe)) {
          continue;
        }
        if (probe.name == token.name) {
          if (probe.kind == XmlToken::Kind::Close) {
            --depth;
            if (depth == 0) content_end = inner_tag->begin;
          } else if (probe.kind == XmlToken::Kind::Open) {
            ++depth;
          }
        }
      }
      const std::string_view inner = scope.substr(pos, content_end - pos);
      pos = content_end;
      // 结束标签跳过：从 search 处已越过它；pos 指到内容结束即可（结束标签在 search 已消费）
      // 需要把 pos 推到结束标签之后：
      {
        std::size_t skip = pos;
        (void)skip;
        pos = search;
      }
      if (token.name == "g") {
        StyleContext child = current;
        child.style = style_from_attributes(token, current.style);
        if (const auto t = attr(token, "transform"); !t.empty()) {
          child.transform = parse_transform(t) * current.transform;
        }
        stack.push_back(current);
        current = child;
        // 递归（g 无自闭合语义）
        flatten(inner, current, view_w, view_h, symbols, shapes, nodes);
        current = stack.back();
        stack.pop_back();
        continue;
      }
      if (token.name == "symbol") {
        // symbol 是**定义**不是绘制——文档流里跳过；只在 <use> 引用时按其 viewBox 展开
        // （展开逻辑在 expand_use → flatten(symbol_inner)，viewBox 映射由 IconSet::get 的
        // `<svg viewBox>` 外壳承担——同一台解析器两条入口，见 IconSet::get）。
        continue;
      }
      if (token.name == "svg") {
        // 嵌套 svg：忽略外壳只展平内容（图标场景罕见，不切坐标系）
        flatten(inner, current, view_w, view_h, symbols, shapes, nodes);
        continue;
      }
      if (token.name == "use") {
        expand_use(token, current, view_w, view_h, symbols, shapes, nodes);
        continue;
      }
      if (token.name == "defs" || token.name == "title" || token.name == "desc") {
        continue;  // defs 里的形状不直接绘制（只被 use 引用）；title/desc 无图形
      }
      // 绘制元素（可能自闭合，也可能带内容——内容里不会有同级图形，直接展平 token）
      apply_shape(token, current, view_w, view_h, nodes);
      if (token.kind == XmlToken::Kind::Open && !inner.empty()) {
        // 个别图标把形状包在中间层（罕见）；递归一层以防漏
        flatten(inner, current, view_w, view_h, symbols, shapes, nodes);
      }
      continue;
    }
    // SelfClose
    if (token.name == "use") {
      expand_use(token, current, view_w, view_h, symbols, shapes, nodes);
    } else if (token.name == "g" || token.name == "svg" || token.name == "symbol" ||
               token.name == "defs" || token.name == "title" || token.name == "desc") {
      // 空容器：跳过
    } else {
      apply_shape(token, current, view_w, view_h, nodes);
    }
    (void)text_begin;
  }
}

auto style_from_attributes(const XmlToken& token, const Style& inherited) -> Style;

/// 第一遍：收集 symbol 表（id → inner XML）与根 viewBox。
struct PreScan {
  std::map<std::string_view, std::string_view, std::less<>> symbols{};
  std::map<std::string_view, std::string_view, std::less<>> shapes{};  // defs 内带 id 的形状
  float view_x{0.0f};
  float view_y{0.0f};
  float view_w{24.0f};
  float view_h{24.0f};
  bool has_view_box{false};
  std::string_view body{};  // 根元素内容（无根则全文）
  Style root_style{};       // 根 <svg> 的 presentation 属性（Lucide 把 fill/stroke 放这里）
};

auto prescan(std::string_view source) -> PreScan {
  PreScan result;
  std::size_t pos = 0;
  // 找根 svg（跳过 xml 声明/注释）
  std::size_t root_content_begin = 0;
  std::size_t root_content_end = source.size();
  std::string_view root_attributes_scope{};
  bool found_root = false;
  while (true) {
    const auto tag = next_tag(source, pos);
    if (!tag) break;
    if (source[tag->begin + 1] == '/') continue;
    XmlToken token;
    if (!parse_tag(source.substr(tag->begin, tag->end - tag->begin), tag->begin, token)) continue;
    if (token.name == "svg") {
      found_root = true;
      root_content_begin = tag->end;
      // viewBox
      if (const auto vb = attr(token, "viewBox"); !vb.empty()) {
        NumberCursor numbers{vb, 0};
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
        if (numbers.next(x) && numbers.next(y) && numbers.next(w) && numbers.next(h)) {
          result.view_x = static_cast<float>(x);
          result.view_y = static_cast<float>(y);
          result.view_w = static_cast<float>(w);
          result.view_h = static_cast<float>(h);
          result.has_view_box = true;
        }
      }
      // 找根的结束标签（最后一个 </svg>——嵌套 svg 罕见，取最后安全）
      const std::size_t close = source.rfind("</svg");
      if (close != std::string_view::npos) {
        root_content_end = close;
      }
      root_attributes_scope = source.substr(root_content_begin, root_content_end - root_content_begin);
      break;
    }
  }
  if (!found_root) {
    result.body = source;
  } else {
    result.body = root_attributes_scope;
    // 根 <svg> 的 presentation 属性 → 初始继承 context
    std::size_t rescan = 0;
    while (true) {
      const auto tag = next_tag(source, rescan);
      if (!tag) break;
      if (source[tag->begin + 1] == '/') continue;
      XmlToken token;
      if (!parse_tag(source.substr(tag->begin, tag->end - tag->begin), tag->begin, token)) {
        continue;
      }
      if (token.name == "svg") {
        result.root_style = style_from_attributes(token, Style{});
        break;
      }
    }
  }
  // symbol 表：全文扫描（含 defs 内）
  std::size_t scan = found_root ? root_content_begin : 0;
  const std::size_t scan_end = found_root ? root_content_end : source.size();
  while (scan < scan_end) {
    const auto tag = next_tag(source, scan);
    if (!tag) break;
    if (source[tag->begin + 1] == '/') continue;
    XmlToken token;
    if (!parse_tag(source.substr(tag->begin, tag->end - tag->begin), tag->begin, token)) continue;
    if (token.name == "symbol") {
      const auto id = attr(token, "id");
      if (!id.empty()) {
        // symbol 内容 = 到匹配 </symbol>
        std::size_t search = tag->end;
        std::size_t depth = 1;
        std::size_t content_end = search;
        while (depth > 0) {
          const auto inner = next_tag(source, search);
          if (!inner) {
            content_end = scan_end;
            break;
          }
          XmlToken probe;
          if (!parse_tag(source.substr(inner->begin, inner->end - inner->begin), inner->begin,
                         probe)) {
            continue;
          }
          if (probe.name == "symbol") {
            if (probe.kind == XmlToken::Kind::Close) {
              --depth;
              if (depth == 0) content_end = inner->begin;
            } else if (probe.kind == XmlToken::Kind::Open) {
              ++depth;
            }
          }
        }
        result.symbols.emplace(id, source.substr(tag->end, content_end - tag->end));
      }
    } else {
      // defs 内的带 id 形状元素（path/rect/…）：记整段标签供 <use> 引用
      const bool is_shape = token.name == "path" || token.name == "rect" ||
                            token.name == "circle" || token.name == "ellipse" ||
                            token.name == "line" || token.name == "polyline" ||
                            token.name == "polygon";
      const auto element_id = attr(token, "id");
      if (is_shape && !element_id.empty()) {
        result.shapes.emplace(element_id, source.substr(tag->begin, tag->end - tag->begin));
      }
    }
  }
  return result;
}

}  // namespace

auto parse(std::string_view source) -> std::optional<Document> {
  if (source.empty()) return std::nullopt;
  const PreScan pre = prescan(source);
  if (pre.view_w <= 0.0f || pre.view_h <= 0.0f) return std::nullopt;
  Document doc;
  doc.view_x = pre.view_x;
  doc.view_y = pre.view_y;
  doc.view_w = pre.view_w;
  doc.view_h = pre.view_h;
  StyleContext context;
  context.style = pre.root_style;  // 根 <svg> 的 presentation 属性进初始 context
  flatten(pre.body, context, pre.view_w, pre.view_h, pre.symbols, pre.shapes, doc.nodes);
  if (doc.is_empty()) return std::nullopt;
  return doc;
}

auto fit_transform(const Document& doc, math::Rect box, bool slice) -> Transform {
  if (doc.view_w <= 0.0f || doc.view_h <= 0.0f || box.width <= 0.0f || box.height <= 0.0f) {
    return Transform::identity();
  }
  const float scale_x = box.width / doc.view_w;
  const float scale_y = box.height / doc.view_h;
  const float scale = slice ? std::max(scale_x, scale_y) : std::min(scale_x, scale_y);
  // xMidYMid（默认对齐）：居中
  const float tx = box.x + (box.width - doc.view_w * scale) * 0.5f - doc.view_x * scale;
  const float ty = box.y + (box.height - doc.view_h * scale) * 0.5f - doc.view_y * scale;
  return Transform{scale, 0.0f, 0.0f, scale, tx, ty};
}

auto stroke_width_px(const Document& doc, math::Rect box) -> float {
  const Transform transform = fit_transform(doc, box);
  return transform.scalar_factor();
}

// ————————————————————————————————————————————
// 渲染
// ————————————————————————————————————————————
namespace {

/// even-odd 填充的降级实现：把孔环从路径里拆出（奇数次填充 → 提出为独立环）。
///
/// 光栅器目前只支持 nonzero（字形同口径）。图标场景的 even-odd 几乎全是「外环 + 孔环」
/// （齿轮、字母形、环形），把**每条闭合子路径按包围盒包含关系**重排：外环正向、
/// 其直接子环反向——环数通常 ≤ 8，代价可忽略。这是子集降级：嵌套奇偶层多于两层时
/// 退化为 nonzero 视觉（不崩溃、不变形，只孔的嵌套层数可能失真）。
auto make_even_odd_path(const raster::Path& path) -> raster::Path {
  // 按子路径拆分
  struct SubPath {
    raster::Path path{};
    math::Rect bounds{};
  };
  std::vector<SubPath> subs;
  raster::Path current;
  const auto flush = [&]() {
    if (!current.is_empty()) {
      subs.push_back(SubPath{current, current.flattened_bounds(0.2f)});
      current = raster::Path{};
    }
  };
  for (const auto& command : path.commands()) {
    switch (command.kind) {
      case raster::PathCommand::Kind::MoveTo:
        flush();
        current.move_to(command.p1);
        break;
      case raster::PathCommand::Kind::LineTo:
        current.line_to(command.p1);
        break;
      case raster::PathCommand::Kind::QuadTo:
        current.quad_to(command.p1, command.p2);
        break;
      case raster::PathCommand::Kind::CubicTo:
        current.cubic_to(command.p1, command.p2, command.p3);
        break;
      case raster::PathCommand::Kind::Close:
        current.close();
        flush();
        break;
    }
  }
  flush();
  if (subs.size() < 2) return path;
  // 包含关系树：环 i 被 j 包含（j 是 i 的父）
  std::vector<int> parent(static_cast<std::size_t>(subs.size()), -1);
  for (std::size_t i = 0; i < subs.size(); ++i) {
    const auto area_of = [](math::Rect rect) noexcept -> float {
      return rect.width * rect.height;
    };
    for (std::size_t j = 0; j < subs.size(); ++j) {
      if (i == j) continue;
      if (subs[j].bounds.contains(subs[i].bounds.center()) &&
          area_of(subs[j].bounds) > area_of(subs[i].bounds)) {
        // 取最小包裹者作父
        if (parent[i] < 0 ||
            area_of(subs[static_cast<std::size_t>(parent[i])].bounds) >
                area_of(subs[j].bounds)) {
          parent[i] = static_cast<int>(j);
        }
      }
    }
  }
  // 深度奇偶：偶数层正向、奇数层反向（等价 even-odd）
  std::vector<int> depth(static_cast<std::size_t>(subs.size()), 0);
  for (std::size_t i = 0; i < subs.size(); ++i) {
    int level = 0;
    int walk = parent[i];
    while (walk >= 0) {
      ++level;
      walk = parent[static_cast<std::size_t>(walk)];
    }
    depth[i] = level;
  }
  raster::Path result;
  for (std::size_t i = 0; i < subs.size(); ++i) {
    if (depth[static_cast<std::size_t>(i)] % 2 == 1) {
      // 反绕向 = 点序反转（flatten 成折线后倒序；填充几何不变、绕向翻转）。
      // 注意不能用 `Path::reverse()`——它把段拆成独立 MoveTo（语义是"断开"，
      // 用在这里会把孔打散成散点）。
      raster::Path reversed;
      const auto polylines = subs[i].path.flatten(0.2f);
      for (const auto& polyline : polylines) {
        if (polyline.points.empty()) continue;
        reversed.move_to(polyline.points.back());
        for (std::size_t p = polyline.points.size() - 1; p-- > 0;) {
          reversed.line_to(polyline.points[p]);
        }
        if (polyline.closed) reversed.close();
      }
      result.add_path(reversed);
    } else {
      result.add_path(subs[i].path);
    }
  }
  return result;
}

}  // namespace

void draw(raster::Surface& canvas, const Document& doc, math::Rect box,
          std::optional<math::Color> override_color) {
  if (doc.is_empty() || box.width <= 0.0f || box.height <= 0.0f) return;
  const Transform transform = fit_transform(doc, box);
  const float stroke_scale = transform.scalar_factor();
  for (const Node& node : doc.nodes) {
    const bool has_fill = !node.style.fill_none;
    // 描边只在**显式指定或 currentColor**时绘制（SVG 缺省 stroke=none）
    const bool has_stroke = !node.style.stroke_none &&
                            (node.style.stroke.has_value() || node.style.stroke_current) &&
                            node.style.stroke_width > 0.0f;
    if (!has_fill && !has_stroke) continue;
    raster::Path path = apply_transform(node.path, transform);
    if (path.is_empty()) continue;
    // 颜色解析次序：显式色 > override（仅覆盖 currentColor/未指定——多色图标不被主题色破坏）
    // > SVG 缺省黑。
    const auto resolve = [&](const std::optional<math::Color>& explicit_color,
                             bool current) -> math::Color {
      if (explicit_color) return *explicit_color;
      if (current && override_color) return *override_color;
      return math::Color{0, 0, 0, 255};
    };
    const float fill_alpha = node.style.opacity * node.style.fill_opacity;
    if (has_fill && fill_alpha > 0.0f) {
      raster::Path fill_path = node.style.fill_rule == FillRule::EvenOdd
                                   ? make_even_odd_path(path)
                                   : path;
      math::Color color = resolve(node.style.fill, node.style.fill_current);
      color.a = static_cast<std::uint8_t>(std::clamp(
          static_cast<float>(color.a) * fill_alpha, 0.0f, 255.0f));
      canvas.fill_path(fill_path, raster::Paint::solid(color));
    }
    const float stroke_alpha = node.style.opacity * node.style.stroke_opacity;
    if (has_stroke && stroke_alpha > 0.0f) {
      math::Color color = resolve(node.style.stroke, node.style.stroke_current);
      color.a = static_cast<std::uint8_t>(std::clamp(
          static_cast<float>(color.a) * stroke_alpha, 0.0f, 255.0f));
      const float width_px = node.stroke_width_viewbox * stroke_scale;
      if (width_px > 0.05f) {
        canvas.stroke_path(path, raster::Paint::solid(color), width_px,
                           raster::StrokeStyle{node.style.line_cap, node.style.line_join});
      }
    }
  }
}

// ————————————————————————————————————————————
// IconSet / IconSetPainter
// ————————————————————————————————————————————

auto IconSet::load(std::string_view source) -> bool {
  if (source.empty()) return false;
  // sprite 根 <svg> 的属性原文：get() 的外壳注入（描边风格所在）
  {
    const std::size_t svg_open = source.find("<svg");
    if (svg_open != std::string_view::npos) {
      const std::size_t close = source.find('>', svg_open);
      if (close != std::string_view::npos) {
        std::string attrs{source.substr(svg_open + 4, close - svg_open - 4)};
        const std::size_t xmlns = attrs.find("xmlns");
        if (xmlns != std::string::npos) {
          const std::size_t q1 = attrs.find('"', xmlns);
          const std::size_t q2 = q1 == std::string::npos ? std::string::npos
                                                         : attrs.find('"', q1 + 1);
          if (q2 != std::string::npos) attrs.erase(xmlns, q2 + 1 - xmlns);
        }
        root_attributes_ = attrs;
      }
    }
  }
  const PreScan pre = prescan(source);
  if (pre.symbols.empty()) return false;
  symbols_.clear();
  for (const auto& [id, inner] : pre.symbols) {
    symbols_.push_back(Symbol{std::string(id), std::string(inner)});
  }
  return !symbols_.empty();
}

auto IconSet::load_single(std::string_view id, std::string_view source) -> bool {
  if (id.empty() || source.empty()) return false;
  if (!parse(source)) return false;
  symbols_.push_back(Symbol{std::string(id), std::string(source)});
  return true;
}

auto IconSet::ids() const -> std::vector<std::string> {
  std::vector<std::string> result;
  result.reserve(symbols_.size());
  for (const auto& symbol : symbols_) result.push_back(symbol.id);
  return result;
}

auto IconSet::has(std::string_view id) const noexcept -> bool {
  for (const auto& symbol : symbols_) {
    if (symbol.id == id) return true;
  }
  return false;
}

auto IconSet::get(std::string_view id) const -> std::optional<Document> {
  for (const auto& symbol : symbols_) {
    if (symbol.id != id) continue;
    // symbol 内容包一层 <svg> 外壳，**带上 sprite 根的 presentation 属性**
    // （Lucide 的 fill="none" stroke="currentColor" 在根上；丢了会整集塌成黑色剪影）。
    // symbol 无自述 viewBox——用图标事实标准 24×24（Codicons/Lucide 同口径）。
    const std::string wrapped = "<svg " + root_attributes_ + " viewBox=\"0 0 24 24\">" +
                                symbol.source + "</svg>";
    return parse(wrapped);
  }
  return std::nullopt;
}

void IconSetPainter::draw(raster::Surface& canvas, std::string_view id, math::Rect box,
                          math::Color color) const {
  if (set_ == nullptr || box.width <= 0.0f || box.height <= 0.0f) return;
  const float scale = canvas.device_scale();
  // 物理像素尺寸量化（1px 对齐；防亚像素差异刷缓存）
  const int size_px = std::max(1, static_cast<int>(std::lround(std::max(box.width, box.height) * scale)));
  ++clock_;
  // 缓存查找
  for (auto& entry : cache_) {
    if (entry.size == size_px && entry.color == color && entry.id == id) {
      entry.used = clock_;
      ++hits_;
      canvas.draw_canvas_at(entry.bitmap, static_cast<int>(std::lround(box.x * scale)),
                            static_cast<int>(std::lround(box.y * scale)));
      return;
    }
  }
  ++misses_;
  // 光栅化到独立画布（scale 倍物理分辨率，矢量重栅）
  auto doc = set_->get(id);
  if (!doc) return;
  raster::Canvas bitmap(size_px, size_px, scale);
  bitmap.clear(math::Color{0, 0, 0, 0});
  svg::draw(bitmap, *doc, math::Rect{0.0f, 0.0f, static_cast<float>(size_px) / scale,
                                     static_cast<float>(size_px) / scale},
            color);
  // LRU 淘汰
  if (cache_.size() >= kMaxEntries) {
    auto oldest = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->used < oldest->used) oldest = it;
    }
    cached_pixels_ -= static_cast<std::size_t>(oldest->bitmap.physical_width()) *
                      static_cast<std::size_t>(oldest->bitmap.physical_height());
    cache_.erase(oldest);
  }
  cached_pixels_ += static_cast<std::size_t>(size_px) * static_cast<std::size_t>(size_px);
  while (cached_pixels_ > kPixelBudget && cache_.size() > 1) {
    auto oldest = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->used < oldest->used) oldest = it;
    }
    cached_pixels_ -= static_cast<std::size_t>(oldest->bitmap.physical_width()) *
                      static_cast<std::size_t>(oldest->bitmap.physical_height());
    cache_.erase(oldest);
  }
  cache_.push_back(Entry{std::string(id), color, size_px, std::move(bitmap), clock_});
  canvas.draw_canvas_at(cache_.back().bitmap, static_cast<int>(std::lround(box.x * scale)),
                        static_cast<int>(std::lround(box.y * scale)));
}

void IconSetPainter::clear_cache() const {
  cache_.clear();
  cached_pixels_ = 0;
}

auto IconSetPainter::cache_stats() const -> CacheStats {
  return CacheStats{cache_.size(), hits_, misses_};
}

}  // namespace st::ui::svg
