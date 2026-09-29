#include "st/ui/components/slider.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

// —— 几何常量（颜色/间距/字号一律取自 `context.theme` 的 token）——
constexpr float k_track_height = 6.0f;    // 轨道高（圆角 = 高/2，胶囊形）
constexpr float k_knob_size = 16.0f;      // 滑块直径
constexpr float k_default_width = 220.0f; // 未显式指定宽度时的自然宽度
constexpr float k_glow_alpha = 0.22f;     // 拖拽光圈的透明度（primary 派生）

/// 焦点环：控件矩形外扩 `metrics.focus_width / 2`，圆角跟随控件（与 Input 同一口径）。
/// 圆角夹取到「短边一半 - 2」：半径等于半边的圆角路径描边会退化（无极值直线段时描边塌成发丝线，
/// 圆形指示器/圆形滑块尤其明显），故圆形控件用近似圆的圆角矩形画环。
void paint_focus_ring(const RenderContext& context, raster::Canvas& canvas, math::Rect rect,
                      float radius) {
  const float width = context.theme.metrics().focus_width;
  if (width <= 0.0f || rect.is_empty()) return;
  const float offset = width * 0.5f;
  const math::Rect outer = rect.inflate(offset);
  const float limit = std::max(std::min(outer.width, outer.height) * 0.5f - 2.0f, 0.0f);
  raster::Path ring;
  ring.add_rounded_rect(outer, std::min(radius + offset, limit));
  canvas.stroke_path(ring, raster::Paint::solid(context.theme.colors().focus_ring), width);
}

/// 圆角描边：线宽完全落在矩形内侧。
void paint_outline(raster::Canvas& canvas, math::Rect rect, float radius, math::Color color,
                   float width) {
  if (width <= 0.0f || color.a == 0U || rect.is_empty()) return;
  const float half = width * 0.5f;
  raster::Path outline;
  outline.add_rounded_rect(rect.inset(math::Insets::all(half)), radius > half ? radius - half : 0.0f);
  canvas.stroke_path(outline, raster::Paint::solid(color), width);
}

/// 滑块半径：控件被压矮时随之收缩（几何在 measure/paint/命中间保持一致）。
[[nodiscard]] auto slider_knob_radius(math::Rect bounds) noexcept -> float {
  const float limit = std::max(bounds.height, 0.0f) * 0.5f;
  return std::min(k_knob_size * 0.5f, limit);
}

}  // namespace

Slider::Slider(float value) : value_(math::clamp01(value)) { set_focusable(true); }

void Slider::set_value(float value, bool notify) {
  const float clamped = math::clamp01(value);
  if (clamped == value_) return;
  value_ = clamped;
  mark_dirty();
  if (notify && on_change) on_change(value_);
}

void Slider::set_label(std::string label) {
  if (label_ == label) return;
  label_ = std::move(label);
  mark_dirty();
}

void Slider::set_step(float step) {
  const float clamped = std::clamp(step, 0.001f, 1.0f);
  if (clamped == step_) return;
  step_ = clamped;
  mark_dirty();
}

void Slider::nudge(float delta, bool notify) { set_value(value_ + delta, notify); }

void Slider::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.font_size = metrics.font_sm;
  style_.font_weight = FontWeight::Medium;
  style_.color = enabled_ ? colors.text : colors.text_faint;
  style_.radius = theme.metrics().radius_sm;
  style_.background = math::Color{0, 0, 0, 0};  // 轨道自绘，控件自身透明
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
}

void Slider::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  const Metrics& metrics = context.theme.metrics();
  float width = style_.has_explicit_width() ? style_.width : k_default_width;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  float height = style_.has_explicit_height() ? style_.height : metrics.control_height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{std::clamp(width, style_.min_width, style_.max_width),
                         std::clamp(height, style_.min_height, style_.max_height)};
}

auto Slider::track_rect(const RenderContext& context) const -> math::Rect {
  (void)context;
  const float height = std::min(k_track_height, std::max(bounds_.height, 0.0f));
  return math::Rect{bounds_.x, bounds_.y + (bounds_.height - height) * 0.5f, bounds_.width, height};
}

auto Slider::knob_center(const RenderContext& context) const -> math::Point {
  const math::Rect track = track_rect(context);
  const float radius = slider_knob_radius(bounds_);
  const float travel = std::max(track.width - radius * 2.0f, 0.0f);
  return math::Point{track.x + radius + travel * value_, track.center().y};
}

auto Slider::value_at_x(const RenderContext& context, float x) const -> float {
  const math::Rect track = track_rect(context);
  const float radius = slider_knob_radius(bounds_);
  const float travel = track.width - radius * 2.0f;
  if (travel <= 0.0f) return value_;
  return math::clamp01((x - (track.x + radius)) / travel);
}

void Slider::paint_content(const RenderContext& context, raster::Canvas& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const math::Rect track = track_rect(context);
  if (track.is_empty()) return;

  const float radius = track.height * 0.5f;
  const float knob_radius = slider_knob_radius(bounds_);
  const math::Point center = knob_center(context);

  // 未选段：关态底（surface_sunken 与浅底接近，描边保证可见）。
  canvas.fill_rect(track, raster::Paint::solid(colors.surface_sunken), radius);
  paint_outline(canvas, track, radius, colors.border, metrics.border_width);

  // 已选段：primary（hover/press/disabled 逐态解析）。
  math::Color fill = colors.primary;
  if (!enabled_) {
    fill = colors.primary_soft;
  } else if (pressed_) {
    fill = colors.primary_active;
  } else if (hovered_) {
    fill = colors.primary_hover;
  }
  const float filled_width = std::clamp(center.x - track.x, 0.0f, track.width);
  if (filled_width > 0.0f) {
    canvas.fill_rect(math::Rect{track.x, track.y, filled_width, track.height},
                     raster::Paint::solid(fill), radius);
  }

  if (pressed_ && enabled_) {
    // 拖拽光圈（primary 派生），先画再压滑块。
    canvas.fill_circle(center, knob_radius + metrics.space_xs,
                       raster::Paint::solid(colors.primary.with_alpha_f(k_glow_alpha)));
  }

  const Shadow shadow = shadow_sm(context.theme);
  const math::Rect knob{center.x - knob_radius, center.y - knob_radius, knob_radius * 2.0f,
                        knob_radius * 2.0f};
  canvas.draw_shadow(knob, knob_radius, shadow.blur, shadow.color,
                     math::Point{shadow.offset_x, shadow.offset_y});
  canvas.fill_circle(center, knob_radius, raster::Paint::solid(colors.on_primary));
  // 发丝描边：白色滑块压在浅色轨道/深色主题下都有轮廓。
  raster::Path edge;
  edge.add_circle(center, std::max(knob_radius - metrics.border_width * 0.5f, 0.0f));
  canvas.stroke_path(edge, raster::Paint::solid(colors.border_strong), metrics.border_width);
  if (focused_) {
    // 焦点环贴在滑块外沿（半径夹取见 `paint_focus_ring`）。
    paint_focus_ring(context, canvas, knob, knob_radius);
  }
}

auto Slider::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
    case EventKind::Click:
    case EventKind::MouseUp:
      // 按下/点击即跳到该位置；随后按住移动继续跟随。
      set_value(value_at_x(context, event.position.x), true);
      return true;
    case EventKind::MouseMove:
      if (!pressed_) return false;  // 仅在按压拖动中响应
      set_value(value_at_x(context, event.position.x), true);
      return true;
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      if (key == "ArrowLeft" || key == "ArrowDown") {
        nudge(-step_, true);
        return true;
      }
      if (key == "ArrowRight" || key == "ArrowUp") {
        nudge(step_, true);
        return true;
      }
      if (key == "Home") {
        set_value(0.0f, true);
        return true;
      }
      if (key == "End") {
        set_value(1.0f, true);
        return true;
      }
      return false;
    }
    default: return false;
  }
}

void Slider::activate() {
  // 滑杆没有「激活」语义：取值只由拖拽/键盘步进/控制通道驱动。
}

auto Slider::semantics_value() const -> std::string { return std::format("{:.2f}", value_); }

auto Slider::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.focused = focused_;
  return flags;
}

auto Slider::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return std::format("{}", value_);
  if (name == "step") return std::format("{}", step_);
  if (name == "label" || name == "text") return label_;
  return std::nullopt;
}

auto Slider::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = parse_f64(value);
    if (!parsed.has_value()) return false;
    set_value(static_cast<float>(*parsed), false);
    return true;
  }
  if (name == "step") {
    const auto parsed = parse_f64(value);
    if (!parsed.has_value()) return false;
    set_step(static_cast<float>(*parsed));
    return true;
  }
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  return false;
}

auto Slider::property_names() const -> std::vector<std::string_view> {
  return {"value", "step", "label"};
}

auto Slider::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "increment" || action == "increase" || action == "next") {
    nudge(step_, true);
    return true;
  }
  if (action == "decrement" || action == "decrease" || action == "previous") {
    nudge(-step_, true);
    return true;
  }
  if (action == "set") {
    const auto parsed = parse_f64(argument);
    if (!parsed.has_value()) return false;
    set_value(static_cast<float>(*parsed), true);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
