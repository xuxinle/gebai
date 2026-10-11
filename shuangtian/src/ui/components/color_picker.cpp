#include "st/ui/components/color_picker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 十六进制数字 → 值（非法返回 -1）。
[[nodiscard]] auto hex_digit(char ch) -> int {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

}  // namespace

ColorPicker::ColorPicker() {
  style_.width = kDefaultWidth;
  style_.height = kDefaultHeight;
  set_focusable(true);   // 十六进制输入要能聚焦收键盘
}

auto ColorPicker::hsv_to_rgb(float hue, float saturation, float value) -> math::Color {
  // 标准六段换算。`hue` 先归一到 `[0,360)`——外部可能传来负值或 >360
  //（色相环联动时常见），不归一会落进错误的分段。
  float h = std::fmod(hue, 360.0f);
  if (h < 0.0f) h += 360.0f;
  const float s = std::clamp(saturation, 0.0f, 1.0f);
  const float v = std::clamp(value, 0.0f, 1.0f);

  const float c = v * s;
  const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
  const float m = v - c;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  if (h < 60.0f) {
    r = c; g = x;
  } else if (h < 120.0f) {
    r = x; g = c;
  } else if (h < 180.0f) {
    g = c; b = x;
  } else if (h < 240.0f) {
    g = x; b = c;
  } else if (h < 300.0f) {
    r = x; b = c;
  } else {
    r = c; b = x;
  }
  // 四舍五入而不是截断：截断会让 `hsv_to_rgb(rgb_to_hsv(c))` 系统性偏暗一档。
  const auto to_byte = [](float channel) -> std::uint8_t {
    return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
  };
  return math::Color::rgba(to_byte(r + m), to_byte(g + m), to_byte(b + m), 255);
}

void ColorPicker::rgb_to_hsv(math::Color color) {
  const float r = static_cast<float>(color.r) / 255.0f;
  const float g = static_cast<float>(color.g) / 255.0f;
  const float b = static_cast<float>(color.b) / 255.0f;
  const float max_channel = std::max({r, g, b});
  const float min_channel = std::min({r, g, b});
  const float delta = max_channel - min_channel;

  value_ = max_channel;
  saturation_ = max_channel <= 0.0f ? 0.0f : delta / max_channel;

  // ⚠ 灰阶（`delta == 0`）时色相**数学上无定义**。此时**保留原 `hue_`**而不是设 0：
  // 设 0 会把「我刚选的红」在拖到纯灰时悄悄变成「红→黑」的色相基准，
  // 用户再把 V 拉回来就成了红色而不是他原来的颜色。
  if (delta <= 1.0e-6f) return;
  if (max_channel == r) {
    hue_ = 60.0f * std::fmod((g - b) / delta, 6.0f);
  } else if (max_channel == g) {
    hue_ = 60.0f * ((b - r) / delta + 2.0f);
  } else {
    hue_ = 60.0f * ((r - g) / delta + 4.0f);
  }
  if (hue_ < 0.0f) hue_ += 360.0f;
}

void ColorPicker::sync_from_color() {
  rgb_to_hsv(color_);
  hex_buffer_ = hex_text();
  hex_invalid_ = false;
}

void ColorPicker::set_color(math::Color color, bool notify) {
  if (color_ == color) return;
  color_ = color;
  sync_from_color();
  mark_dirty();
  if (notify && on_change) on_change(color_);
}

void ColorPicker::set_hsv(float hue, float saturation, float value, bool notify) {
  const float h = hue;
  const float s = std::clamp(saturation, 0.0f, 1.0f);
  const float v = std::clamp(value, 0.0f, 1.0f);
  const math::Color next = hsv_to_rgb(h, s, v);
  // 即便换算出来的 RGB 与当前相同（如 S/V 变化但落在同一字节值），也要更新 HSV
  // 中间量——否则拖动时「手感」会跳（下一个事件用的是旧 HSV）。
  const bool changed = next != color_;
  hue_ = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f);
  saturation_ = s;
  value_ = v;
  color_ = next;
  hex_buffer_ = hex_text();
  hex_invalid_ = false;
  mark_dirty();
  if (notify && changed && on_change) on_change(color_);
}

auto ColorPicker::hex_text() const -> std::string {
  if (color_.a == 255U) return std::format("#{:02X}{:02X}{:02X}", color_.r, color_.g, color_.b);
  return std::format("#{:02X}{:02X}{:02X}{:02X}", color_.r, color_.g, color_.b, color_.a);
}

auto ColorPicker::set_hex_text(std::string_view text) -> bool {
  std::string_view body = text;
  if (!body.empty() && body.front() == '#') body.remove_prefix(1);
  // 只认 6 位与 8 位。**不认 3 位缩写**：`#abc` 是 `#aabbcc` 还是别的口径有歧义，
  // 而歧义在读代码时看不出来、在用户输入时会变成"我输的和看到的不是一个颜色"。
  if (body.size() != 6U && body.size() != 8U) {
    hex_invalid_ = true;
    mark_dirty();
    return false;
  }
  std::uint32_t packed = 0U;
  for (const char ch : body) {
    const int digit = hex_digit(ch);
    if (digit < 0) {
      hex_invalid_ = true;
      mark_dirty();
      return false;
    }
    packed = (packed << 4U) | static_cast<std::uint32_t>(digit);
  }
  math::Color next{};
  if (body.size() == 6U) {
    next = math::Color::rgb(static_cast<std::uint8_t>((packed >> 16U) & 0xFFU),
                            static_cast<std::uint8_t>((packed >> 8U) & 0xFFU),
                            static_cast<std::uint8_t>(packed & 0xFFU));
  } else {
    // 8 位一律 `RRGGBBAA`，**不按数值大小猜**（见头文件里 `from_hex` 的坑）。
    next = math::Color::from_rgba_hex(packed);
  }
  hex_invalid_ = false;
  set_color(next, true);
  hex_buffer_ = hex_text();   // 规范化大小写与 `#`（用户输 `ff8800` 会回显 `#FF8800`）
  mark_dirty();
  return true;
}

auto ColorPicker::current_hex_valid() const -> bool {
  return !hex_invalid_;
}

void ColorPicker::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  border_ = colors.border;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  focus_ring_ = colors.primary;
  font_size_ = theme.metrics().font_sm;
}

void ColorPicker::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float width = style_.has_explicit_width() ? style_.width
                     : bounds_.width > 0.0f        ? bounds_.width
                                                   : std::min(kDefaultWidth, constraints.max_width);
  const float height = style_.has_explicit_height() ? style_.height
                      : bounds_.height > 0.0f        ? bounds_.height
                                                     : std::min(kDefaultHeight, constraints.max_height);
  measured_ = math::Size{std::max(0.0f, width), std::max(0.0f, height)};
}

auto ColorPicker::hue_bar_rect() const -> math::Rect {
  return math::Rect{bounds_.x, bounds_.y + bounds_.height - kHueBarHeight - kHexRowHeight - kGap,
                    bounds_.width, kHueBarHeight};
}

auto ColorPicker::sv_square_rect() const -> math::Rect {
  const math::Rect bar = hue_bar_rect();
  return math::Rect{bounds_.x, bounds_.y, bounds_.width, std::max(0.0f, bar.y - kGap - bounds_.y)};
}

auto ColorPicker::swatch_rect() const -> math::Rect {
  return math::Rect{bounds_.x, bounds_.y + bounds_.height - kHexRowHeight, kSwatchSize,
                    kHexRowHeight};
}

auto ColorPicker::eyedropper_rect() const -> math::Rect {
  return math::Rect{bounds_.right() - kSwatchSize, bounds_.y + bounds_.height - kHexRowHeight,
                    kSwatchSize, kHexRowHeight};
}

auto ColorPicker::hex_rect() const -> math::Rect {
  const math::Rect swatch = swatch_rect();
  const float left = swatch.right() + kGap;
  const float right = bounds_.right() - kSwatchSize - kGap;
  return math::Rect{left, swatch.y, std::max(0.0f, right - left), kHexRowHeight};
}

void ColorPicker::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
}

void ColorPicker::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const math::Rect sv = sv_square_rect();
  const math::Rect bar = hue_bar_rect();
  const math::Rect hex = hex_rect();
  const math::Rect swatch = swatch_rect();

  // —— SV 块：右→左为饱和度高→低，上→下为明度高→低 ——
  //
  // 逐行渐变（不是逐像素）：128 行足够平滑，而逐像素会让每帧多几千次 fill_rect。
  // 每行内部再用水平渐变（`Gradient::linear`）从白（S=0）到纯色（S=1）。
  if (!sv.is_empty()) {
    const int rows = std::max(1, static_cast<int>(sv.height));
    for (int row = 0; row < rows; ++row) {
      const float ratio = static_cast<float>(row) / static_cast<float>(std::max(1, rows - 1));
      const float v = 1.0f - ratio;
      const float y = sv.y + static_cast<float>(row);
      const math::Color full = hsv_to_rgb(hue_, 1.0f, v);
      canvas.fill_rect(math::Rect{sv.x, y, sv.width, 1.0f},
                       raster::Paint::solid(full), 0.0f);
      // S=0 那一端是灰（V 决定），用半透明白叠上去比再算一次渐变省事且等效。
      const math::Color gray = hsv_to_rgb(hue_, 0.0f, v);
      if (gray.r != full.r || gray.g != full.g || gray.b != full.b) {
        // 用一系列竖条近似水平渐变（16 档足够；再多看不出差别）。
        constexpr int kSteps = 16;
        for (int step = 0; step < kSteps; ++step) {
          const float t = static_cast<float>(step) / static_cast<float>(kSteps);
          const float alpha = (1.0f - t) * (1.0f - t);   // 二次衰减更接近 HSV 的观感
          const float x = sv.x + sv.width * t;
          const float w = sv.width / static_cast<float>(kSteps) + 1.0f;
          canvas.fill_rect(math::Rect{x, y, w, 1.0f},
                           raster::Paint::solid(gray.with_alpha_f(alpha)), 0.0f);
        }
      }
    }
    // 选中位置的十字标记（白描边 + 黑内圈：任何底色上都看得见）。
    const float mark_x = sv.x + sv.width * saturation_;
    const float mark_y = sv.y + sv.height * (1.0f - value_);
    canvas.fill_rect(math::Rect{mark_x - 5.0f, mark_y - 5.0f, 10.0f, 10.0f},
                     raster::Paint::solid(math::Color::rgba(0, 0, 0, 180)), 5.0f);
    canvas.fill_rect(math::Rect{mark_x - 4.0f, mark_y - 4.0f, 8.0f, 8.0f},
                     raster::Paint::solid(math::Color::rgba(255, 255, 255, 230)), 4.0f);
  }

  // —— 色相条：0..360 铺成 12 段（每 30° 一段足够平滑）——
  if (!bar.is_empty()) {
    constexpr int kSegments = 24;
    for (int index = 0; index < kSegments; ++index) {
      const float t0 = static_cast<float>(index) / static_cast<float>(kSegments);
      const float x = bar.x + bar.width * t0;
      const float w = bar.width / static_cast<float>(kSegments) + 1.0f;
      canvas.fill_rect(math::Rect{x, bar.y, w, bar.height},
                       raster::Paint::solid(hsv_to_rgb(t0 * 360.0f, 1.0f, 1.0f)), 0.0f);
    }
    const float cursor_x = bar.x + bar.width * (hue_ / 360.0f);
    canvas.fill_rect(math::Rect{cursor_x - 2.0f, bar.y - 1.0f, 4.0f, bar.height + 2.0f},
                     raster::Paint::solid(math::Color::rgba(255, 255, 255, 235)), 2.0f);
    canvas.fill_rect(math::Rect{cursor_x - 1.0f, bar.y, 2.0f, bar.height},
                     raster::Paint::solid(math::Color::rgba(0, 0, 0, 160)), 1.0f);
  }

  // —— 色块预览（当前颜色，棋盘格打底显透明）——
  if (!swatch.is_empty()) {
    canvas.fill_rect(swatch, raster::Paint::solid(surface_), 3.0f);
    const float half = swatch.height * 0.5f;
    for (int row = 0; row < 2; ++row) {
      for (int col = 0; col < 2; ++col) {
        const math::Color tone = (row + col) % 2 == 0 ? math::Color::rgb(200, 200, 200)
                                                      : math::Color::rgb(240, 240, 240);
        canvas.fill_rect(math::Rect{swatch.x + 2.0f + static_cast<float>(col) * half * 0.5f,
                                    swatch.y + 2.0f + static_cast<float>(row) * half * 0.5f,
                                    half * 0.5f, half * 0.5f},
                         raster::Paint::solid(tone), 0.0f);
      }
    }
    canvas.fill_rect(math::Rect{swatch.x + 2.0f, swatch.y + 2.0f, swatch.width - 4.0f,
                                swatch.height - 4.0f},
                     raster::Paint::solid(color_), 2.0f);
    components_internal::paint_outline(canvas, swatch, 3.0f, border_, 1.0f);
  }

  // —— 十六进制输入 ——
  if (!hex.is_empty()) {
    canvas.fill_rect(hex, raster::Paint::solid(surface_), 3.0f);
    // 非法输入用 danger 色描边**明确说出来**（静默兜底是最糟的失败方式）。
    components_internal::paint_outline(canvas, hex, 3.0f,
                                       hex_invalid_ ? math::Color::rgb(220, 60, 60)
                                                    : (focused() ? focus_ring_ : border_),
                                       focused() ? 1.5f : 1.0f);
    const TextPort& port = components_internal::text_port_of(context);
    const std::string shown = hex_buffer_.empty() ? hex_text() : hex_buffer_;
    const float line = port.line_height(font_size_);
    port.draw(canvas, shown,
              math::Point{hex.x + 6.0f, hex.y + (hex.height - line) * 0.5f}, font_size_,
              hex_invalid_ ? math::Color::rgb(220, 60, 60) : text_color_);
  }

  // —— 吸管按钮（无源时**如实禁用**：画成弱色且不响应）——
  const math::Rect dropper = eyedropper_rect();
  if (!dropper.is_empty()) {
    const bool enabled = has_eyedropper();
    components_internal::paint_outline(canvas, dropper, 3.0f, enabled ? border_ : border_, 1.0f);
    Icon::draw(canvas, "eyedropper",
               math::Rect{dropper.x + 4.0f, dropper.y + 4.0f, dropper.width - 8.0f,
                          dropper.height - 8.0f},
               enabled ? text_color_ : text_muted_, 0.0f);
  }
}

auto ColorPicker::on_event(const RenderContext& /*context*/, Event& event) -> bool {
  const math::Rect sv = sv_square_rect();
  const math::Rect bar = hue_bar_rect();
  const math::Rect hex = hex_rect();
  const math::Rect dropper = eyedropper_rect();

  switch (event.kind) {
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      if (sv.contains(event.position)) {
        dragging_ = Drag::Sv;
        const float s = (event.position.x - sv.x) / std::max(1.0f, sv.width);
        const float v = 1.0f - (event.position.y - sv.y) / std::max(1.0f, sv.height);
        set_hsv(hue_, s, v);
        event.handled = true;
        return true;
      }
      if (bar.contains(event.position)) {
        dragging_ = Drag::Hue;
        set_hsv((event.position.x - bar.x) / std::max(1.0f, bar.width) * 360.0f, saturation_,
                value_);
        event.handled = true;
        return true;
      }
      if (hex.contains(event.position)) {
        // 请求**框架焦点**（不自己维护 shadow 状态）：`TextInput` 只投给
        // `UiRoot::focused_`（见 `ui_root.cpp` 的分派），自己记一个 `hex_focused_`
        // 会与框架焦点分家——表现是"点了输入框敲字没反应"（实测踩到）。
        if (auto* host = this->host(); host != nullptr) {
          (void)host->set_keyboard_focus(this);
        } else {
          // 未上树（单测/嵌入式）：至少让自己能收键盘。
          set_focused(true);
        }
        // **聚焦时清空编辑缓冲**（前提是它当前就是"颜色文本"、不在编辑中）：
        // 不清的话用户接着敲会把新内容**追加**到旧值后面（`#101010` + 敲
        // `#FF0000` = `#101010#FF0000`），而超长会被解析拒绝——表现是
        // "点了输入框输入新颜色却没反应"（实测踩到）。
        //
        // 为何判一下"是不是等于当前颜色"：用户输了一半跑去看别处、回来接着输，
        // 那半截**不该**被清掉。只有"显示的就是颜色本身"才说明没在编辑。
        if (hex_buffer_ == hex_text()) hex_buffer_.clear();
        hex_invalid_ = false;
        mark_dirty();
        event.handled = true;
        return true;
      }
      if (dropper.contains(event.position)) {
        // 无源时**如实拒绝**（不假装可用）——但也不吞掉事件，交给上层。
        if (!has_eyedropper()) return false;
        if (const auto picked = eyedropper_(event.position); picked.has_value()) {
          set_color(*picked, true);
          mark_dirty();
        }
        event.handled = true;
        return true;
      }
      // 点组件其他位置：不需要在此改焦点——`UiRoot::set_focus` 是唯一入口
      //（它会发 FocusOut/FocusIn），自己清会与它分家。
      return false;
    }
    case EventKind::MouseMove: {
      if (dragging_ == Drag::None) return false;
      if (dragging_ == Drag::Sv) {
        const float s = (event.position.x - sv.x) / std::max(1.0f, sv.width);
        const float v = 1.0f - (event.position.y - sv.y) / std::max(1.0f, sv.height);
        set_hsv(hue_, s, v);
      } else {
        set_hsv((event.position.x - bar.x) / std::max(1.0f, bar.width) * 360.0f, saturation_,
                value_);
      }
      event.handled = true;
      return true;
    }
    case EventKind::MouseUp: {
      if (dragging_ == Drag::None) return false;
      dragging_ = Drag::None;
      event.handled = true;
      return true;
    }
    case EventKind::TextInput: {
      if (!focused()) return false;
      // 只收十六进制字符：别的字符进缓冲区会让输入框显示"看着对、其实是错的"内容。
      for (const char ch : event.text) {
        if (hex_digit(ch) < 0 && ch != '#') return false;
      }
      hex_buffer_ += event.text;
      // 够长就即时提交（6 或 8 位）——用户不用按回车也能看到颜色变化。
      const std::size_t digits = hex_buffer_.starts_with("#") ? hex_buffer_.size() - 1
                                                              : hex_buffer_.size();
      if (digits == 6U || digits == 8U) {
        (void)set_hex_text(hex_buffer_);
      } else {
        hex_invalid_ = false;   // 敲到一半不算错
      }
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      if (!focused()) return false;
      if (event.key == "Backspace") {
        if (!hex_buffer_.empty()) hex_buffer_.pop_back();
        hex_invalid_ = false;
        mark_dirty();
        event.handled = true;
        return true;
      }
      if (event.key == "Enter") {
        // 回车提交：这是"我输完了"的明确信号，失败就**保留原值并标红**。
        (void)set_hex_text(hex_buffer_);
        event.handled = true;
        return true;
      }
      return false;
    }
    case EventKind::FocusIn:
    case EventKind::FocusOut: {
      mark_dirty();   // 焦点环要重绘
      return false;   // 不吞：框架自己也要做焦点簿记
    }
    default:
      return false;
  }
}

auto ColorPicker::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "color") return hex_text();
  if (name == "hue") return std::format("{:.1f}", static_cast<double>(hue_));
  if (name == "saturation") return std::format("{:.3f}", static_cast<double>(saturation_));
  if (name == "value") return std::format("{:.3f}", static_cast<double>(value_));
  if (name == "eyedropper") return has_eyedropper() ? "true" : "false";
  return Element::get_property(name);
}

auto ColorPicker::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "color") return set_hex_text(value);
  const auto parse_float = [&value]() -> std::optional<float> {
    try {
      std::size_t used = 0;
      const float parsed = std::stof(std::string(value), &used);
      if (used != value.size()) return std::nullopt;
      return parsed;
    } catch (...) {
      return std::nullopt;
    }
  };
  if (name == "hue" || name == "saturation" || name == "value") {
    if (const auto parsed = parse_float(); parsed.has_value()) {
      if (name == "hue") set_hsv(*parsed, saturation_, value_);
      else if (name == "saturation") set_hsv(hue_, *parsed, value_);
      else set_hsv(hue_, saturation_, *parsed);
      return true;
    }
    return false;
  }
  return Element::set_property(name, value);
}

auto ColorPicker::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(), {"color", "hue", "saturation", "value", "eyedropper"});
  return names;
}

auto ColorPicker::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "set_color") return set_hex_text(argument);
  if (action == "eyedropper") {
    if (!has_eyedropper()) return false;
    // 无鼠标位置：以组件中心为探针点（脚本/键盘路径）。
    if (const auto picked = eyedropper_(bounds_.center()); picked.has_value()) {
      set_color(*picked, true);
      mark_dirty();
      return true;
    }
    return false;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
