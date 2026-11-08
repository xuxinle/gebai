#include "st/ui/components/status_dot.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

StatusDot::StatusDot() = default;

StatusDot::StatusDot(std::string label, State state) : state_value_(state), label_(std::move(label)) {
  if (state_value_ == State::Busy) pulse_ = true;
}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void StatusDot::set_state(State state) {
  if (state_value_ == state) return;
  state_value_ = state;
  // `Busy` 自动呼吸（「正在进行」需要动感）；切走时关掉（避免残留的常驻续帧）。
  pulse_ = state_value_ == State::Busy;
  mark_layout_dirty();
}

void StatusDot::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_layout_dirty();
}

void StatusDot::set_pulse(bool pulse) {
  if (pulse_ == pulse) return;
  pulse_ = pulse;
  mark_dirty();
}

void StatusDot::set_dot_size(float size) {
  const float clamped = std::clamp(size, 4.0f, 32.0f);
  if (dot_size_ == clamped) return;
  dot_size_ = clamped;
  mark_layout_dirty();
}

auto StatusDot::pulse_scale(double time_seconds) const noexcept -> float {
  if (!pulse_) return 1.0f;
  const double turns = time_seconds / static_cast<double>(kPulsePeriodSeconds);
  const double fraction = turns - std::floor(turns);
  // 1.0 → 1.45 → 1.0 的呼吸（半径放大，同时外圈光晕变淡——见绘制）。
  const auto wave = static_cast<float>(std::sin(fraction * 2.0 * 3.14159265358979));
  return 1.0f + 0.45f * (wave + 1.0f) * 0.5f;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void StatusDot::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  switch (state_value_) {
    case State::Ok: dot_color_ = colors.success; break;
    case State::Warn: dot_color_ = colors.warning; break;
    case State::Error: dot_color_ = colors.danger; break;
    case State::Idle: dot_color_ = colors.text_faint; break;
    case State::Busy: dot_color_ = colors.primary; break;
  }
  label_color_ = colors.text_muted;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = metrics.font_xs;
  style_.color = label_color_;
  apply_text_overrides();
}

void StatusDot::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = dot_size_;
  float height = std::max(dot_size_, port.line_height(style_.font_size));
  if (!label_.empty()) {
    width += kGap + port.measure_width(label_, style_.font_size);
  }
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void StatusDot::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  const TextPort& port = text_port_of(context);
  // 点对齐到**文字的墨迹中心**而不是行盒中心：行盒含字体预留空间，
  // 按它居中会让点比文字偏下 1~2px（与 `layout_line` 里那条推导同一原因）。
  const float line_height = port.line_height(style_.font_size);
  const auto geometry = layout_line(port, kInkReferenceSample, style_.font_size, line_height);
  const float ink_center = rect.y + (rect.height - line_height) * 0.5f + geometry.glyph_top +
                           geometry.glyph_height * 0.5f;
  const float top = ink_center - dot_size_ * 0.5f;
  dot_ = math::Rect{rect.x, top, dot_size_, dot_size_};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void StatusDot::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || dot_.is_empty()) return;
  const float scale = pulse_scale(context.time_seconds);

  if (pulse_) {
    // 呼吸：一圈淡光晕 + 本体。光晕随半径放大而变淡（能量守恒的视觉直觉）。
    const float glow_radius = dot_.width * 0.5f * scale * 1.9f;
    const float alpha = 0.28f / std::max(scale, 1.0f);
    canvas.fill_circle(dot_.center(), glow_radius,
                       raster::Paint::solid(dot_color_.with_alpha_f(alpha)));
  }
  const float radius = dot_.width * 0.5f * (pulse_ ? scale : 1.0f);
  canvas.fill_circle(dot_.center(), radius, raster::Paint::solid(dot_color_));

  if (!label_.empty()) {
    const TextPort& port = text_port_of(context);
    const float text_x = dot_.right() + kGap;
    const float top = centered_line_top(port, label_, style_.font_size, bounds_.y, bounds_.height);
    port.draw(canvas, label_, math::Point{text_x, top}, style_.font_size, label_color_);
  }
  if (pulse_) request_animation();
}

auto StatusDot::semantics_value() const -> std::string { return std::string(state_name(state_value_)); }

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto StatusDot::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "state" || name == "value") return std::string(state_name(state_value_));
  if (name == "label" || name == "text") return label_;
  if (name == "pulse") return std::string(pulse_ ? kFlagTrue : "false");
  if (name == "dot_size" || name == "size") return std::format("{}", dot_size_);
  return std::nullopt;
}

auto StatusDot::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "state" || name == "value") {
    const std::string text(st::trim(value));
    if (text == "ok" || text == "online" || text == "success") set_state(State::Ok);
    else if (text == "warn" || text == "warning" || text == "degraded") set_state(State::Warn);
    else if (text == "error" || text == "offline" || text == "fail") set_state(State::Error);
    else if (text == "idle" || text == "unknown") set_state(State::Idle);
    else if (text == "busy" || text == "loading" || text == "pending") set_state(State::Busy);
    else return false;
    return true;
  }
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  if (name == "pulse") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_pulse(*parsed);
    return true;
  }
  if (name == "dot_size" || name == "size") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_dot_size(static_cast<float>(*parsed));
    return true;
  }
  return false;
}

auto StatusDot::property_names() const -> std::vector<std::string_view> {
  return {"state", "label", "pulse", "dot_size"};
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::StatusDot);
