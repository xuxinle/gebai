#include "st/ui/components/kbd.hpp"

#include <algorithm>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_outline;
using components_internal::text_port_of;

Kbd::Kbd() = default;

// ————————————————————————————————————————————————————————————————————————————
// 按键
// ————————————————————————————————————————————————————————————————————————————

void Kbd::set_keys(std::vector<std::string> keys) {
  keys_.clear();
  keys_.reserve(keys.size());
  for (auto& key : keys) {
    const std::string trimmed(st::trim(key));
    if (trimmed.empty()) continue;
    keys_.push_back(trimmed);
  }
  mark_layout_dirty();
}

void Kbd::set_shortcut(std::string_view shortcut) {
  std::vector<std::string> parts;
  for (const auto& part : st::split(shortcut, '+')) {
    parts.emplace_back(st::trim(part));
  }
  set_keys(std::move(parts));
}

auto Kbd::platform_label(std::string_view modifier, Platform platform) -> std::string {
  // 平台解析：`Auto` 按编译目标（本框架是三平台，用条件编译锁定）。
  Platform resolved = platform;
  if (resolved == Platform::Auto) {
#if defined(_WIN32)
    resolved = Platform::Windows;
#elif defined(__APPLE__)
    resolved = Platform::Mac;
#else
    resolved = Platform::Linux;
#endif
  }
  std::string lower(modifier);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

  const bool mac = resolved == Platform::Mac;
  if (lower == "ctrl" || lower == "control") return mac ? "⌃" : "Ctrl";
  if (lower == "cmd" || lower == "command" || lower == "meta" || lower == "super") {
    return mac ? "⌘" : "Win";
  }
  if (lower == "alt" || lower == "option" || lower == "opt") return mac ? "⌥" : "Alt";
  if (lower == "shift") return mac ? "⇧" : "Shift";
  if (lower == "enter" || lower == "return") return mac ? "↩" : "Enter";
  if (lower == "esc" || lower == "escape") return "Esc";
  if (lower == "del" || lower == "delete") return mac ? "⌫" : "Del";
  if (lower == "tab") return mac ? "⇥" : "Tab";
  if (lower == "space") return "Space";
  if (lower == "up") return "↑";
  if (lower == "down") return "↓";
  if (lower == "left") return "←";
  if (lower == "right") return "→";
  return std::string(modifier);
}

auto Kbd::display_keys() const -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(keys_.size());
  for (const auto& key : keys_) out.push_back(platform_label(key, platform_));
  return out;
}

void Kbd::set_separator(Separator separator) {
  if (separator_ == separator) return;
  separator_ = separator;
  mark_layout_dirty();
}

void Kbd::set_platform(Platform platform) {
  if (platform_ == platform) return;
  platform_ = platform;
  mark_layout_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量、落位
// ————————————————————————————————————————————————————————————————————————————

void Kbd::apply_theme(const Theme& theme) {
  cap_fill_ = theme.colors().surface_alt;
  cap_border_ = theme.colors().border;
  cap_text_ = theme.colors().text_muted;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = theme.metrics().font_xs;
  style_.color = cap_text_;
  apply_text_overrides();
  font_size_ = style_.font_size;
}

void Kbd::layout_caps(const RenderContext& context, math::Rect rect) const {
  const TextPort& port = text_port_of(context);
  const std::vector<std::string> labels = display_keys();
  caps_.assign(labels.size(), math::Rect{});
  if (rect.is_empty()) return;

  const float plus_width = port.measure_width("+", font_size_);
  const float cap_height = std::min(kHeight, rect.height);
  const float top = rect.y + (rect.height - cap_height) * 0.5f;
  float x = rect.x;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const float text_width = port.measure_width(labels[i], font_size_);
    const float width = std::max(text_width + kPaddingX * 2.0f, kMinWidth);
    caps_[i] = math::Rect{x, top, width, cap_height};
    x += width;
    if (i + 1U < labels.size()) {
      x += separator_ == Separator::Plus ? plus_width + kCapGap * 2.0f : kCapGap;
    }
  }
}

void Kbd::measure(const RenderContext& context, const Constraints& constraints) {
  layout_caps(context, math::Rect{0.0f, 0.0f, 1.0e6f, kHeight});
  const TextPort& port = text_port_of(context);
  float width = 0.0f;
  if (!caps_.empty()) width = caps_.back().right();
  // `layout_caps` 从 x=0 起算，所以 last.right() 就是总宽。
  width += 0.0f;
  (void)port;
  if (style_.has_explicit_width()) width = style_.width;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void Kbd::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  layout_caps(context, rect);
}

auto Kbd::cap_rect(std::size_t index) const -> math::Rect {
  return index < caps_.size() ? caps_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Kbd::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || keys_.empty()) return;
  layout_caps(context, bounds_);
  const TextPort& port = text_port_of(context);
  const std::vector<std::string> labels = display_keys();

  for (std::size_t i = 0; i < caps_.size() && i < labels.size(); ++i) {
    const math::Rect cap = caps_[i];
    if (cap.is_empty()) continue;
    fill_round_rect(canvas, cap, kRadius, cap_fill_);
    paint_outline(canvas, cap, kRadius, cap_border_, 1.0f);
    const float width = port.measure_width(labels[i], font_size_);
    const float top = centered_line_top(port, labels[i], font_size_, cap.y, cap.height);
    port.draw(canvas, labels[i], math::Point{cap.center().x - width * 0.5f, top}, font_size_,
              cap_text_);
    // 分隔符 `+`
    if (separator_ == Separator::Plus && i + 1U < caps_.size()) {
      const float plus_width = port.measure_width("+", font_size_);
      const float plus_x = cap.right() + kCapGap;
      port.draw(canvas, "+", math::Point{plus_x + (plus_width - plus_width) * 0.5f, top},
                font_size_, cap_text_);
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto Kbd::semantics_text() const -> std::string {
  const std::vector<std::string> labels = display_keys();
  std::string out;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (i > 0) out += '+';
    out += labels[i];
  }
  return out;
}

auto Kbd::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "keys" || name == "shortcut" || name == "text") {
    std::string out;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      if (i > 0) out += '+';
      out += keys_[i];
    }
    return out;
  }
  if (name == "separator") {
    return std::string(separator_ == Separator::Plus ? "plus" : "none");
  }
  if (name == "platform") {
    switch (platform_) {
      case Platform::Auto: return std::string("auto");
      case Platform::Windows: return std::string("windows");
      case Platform::Mac: return std::string("mac");
      case Platform::Linux: return std::string("linux");
    }
    return std::string("auto");
  }
  return Element::get_property(name);
}

auto Kbd::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "keys" || name == "shortcut" || name == "text") {
    set_shortcut(value);
    return true;
  }
  if (name == "separator") {
    const std::string text(st::trim(value));
    if (text == "plus") set_separator(Separator::Plus);
    else if (text == "none" || text == "space") set_separator(Separator::None);
    else return false;
    return true;
  }
  if (name == "platform") {
    const std::string text(st::trim(value));
    if (text == "auto") set_platform(Platform::Auto);
    else if (text == "windows" || text == "win") set_platform(Platform::Windows);
    else if (text == "mac" || text == "macos" || text == "darwin") set_platform(Platform::Mac);
    else if (text == "linux") set_platform(Platform::Linux);
    else return false;
    return true;
  }
  return Element::set_property(name, value);
}

auto Kbd::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"keys", "separator", "platform"});
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Kbd);
