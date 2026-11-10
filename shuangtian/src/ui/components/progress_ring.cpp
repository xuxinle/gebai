#include "st/ui/components/progress_ring.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "st/core/string.hpp"
#include "st/math/path.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

/// 把「从 12 点开始、顺时针」的角度换成画布坐标点。
///
/// ⚠ 这是本组件最容易做错的一处：`sin/cos` 的自然零点在 **3 点方向**，
/// 直接拿来画会得到「从右边开始、逆时针」的弧——与钟表直觉相反。
/// 所以先减 90°（转到 12 点），再用 `+sin` 让角度增长往**顺时针**走（y 轴向下）。
auto point_on_circle(math::Point center, float radius, float angle) -> math::Point {
  const float cx = -std::sin(angle);
  const float cy = std::cos(angle);
  return math::Point{center.x + radius * cx, center.y + radius * cy};
}

}  // namespace

ProgressRing::ProgressRing() = default;

ProgressRing::ProgressRing(float value) : value_(std::clamp(value, 0.0f, 1.0f)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void ProgressRing::set_value(float value) {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  if (std::abs(clamped - value_) < 1e-6f) return;
  value_ = clamped;
  // 到达 100% 触发一次（回落再涨上去会重新武装）。
  if (value_ >= 0.999f) {
    if (!completed_) {
      completed_ = true;
      if (on_complete) on_complete();
    }
  } else {
    completed_ = false;
  }
  mark_dirty();
}

void ProgressRing::set_indeterminate(bool indeterminate) {
  if (indeterminate_ == indeterminate) return;
  indeterminate_ = indeterminate;
  if (indeterminate_) completed_ = false;
  mark_dirty();
}

void ProgressRing::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  mark_layout_dirty();
}

void ProgressRing::set_thickness(float thickness) {
  const float clamped = std::clamp(thickness, 1.0f, 20.0f);
  if (thickness_ == clamped) return;
  thickness_ = clamped;
  mark_dirty();
}

void ProgressRing::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

auto ProgressRing::percent() const noexcept -> int {
  return static_cast<int>(std::lround(value_ * 100.0f));
}

auto ProgressRing::sweep_start(double time_seconds) const noexcept -> float {
  if (!indeterminate_) return 0.0f;
  const double turns = time_seconds / static_cast<double>(kIndeterminatePeriod);
  const double fraction = turns - std::floor(turns);
  return static_cast<float>(fraction) * 2.0f * std::numbers::pi_v<float>;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量
// ————————————————————————————————————————————————————————————————————————————

void ProgressRing::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  track_color_ = theme.colors().surface_alt;
  progress_color_ = tone_color(theme, tone_);
  text_color_ = theme.colors().text;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = metrics.font_xs;
  style_.color = text_color_;
  apply_text_overrides();
}

void ProgressRing::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float side = style_.has_explicit_width() ? style_.width : kDefaultSize;
  if (style_.has_explicit_height()) side = std::min(side, style_.height);
  if (side <= 0.0f) side = kDefaultSize;
  if (constraints.max_width < kUnbounded) side = std::min(side, constraints.max_width);
  if (constraints.max_height < kUnbounded) side = std::min(side, constraints.max_height);
  side = std::clamp(side, style_.min_width, style_.max_width);
  // 环是正圆：宽高一致（非正方会被拉成椭圆）。
  measured_ = math::Size{side, side};
}

auto ProgressRing::ring_rect() const -> math::Rect {
  const float side = std::min(bounds_.width, bounds_.height);
  return math::Rect{bounds_.x + (bounds_.width - side) * 0.5f,
                    bounds_.y + (bounds_.height - side) * 0.5f, side, side};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void ProgressRing::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const math::Rect ring = ring_rect();
  if (ring.is_empty()) return;
  const float radius = (std::min(ring.width, ring.height) - thickness_) * 0.5f;
  if (radius <= 0.0f) return;
  const math::Point center = ring.center();

  // 轨道：完整一圈淡色（让用户看得出"总共多长"）。
  st::math::Path track;
  track.move_to(point_on_circle(center, radius, 0.0f));
  constexpr int kSegments = 72;
  for (int i = 1; i <= kSegments; ++i) {
    const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) /
                        static_cast<float>(kSegments);
    track.line_to(point_on_circle(center, radius, angle));
  }
  track.close();
  canvas.stroke_path(track, raster::Paint::solid(track_color_), thickness_);

  // 进度弧。
  float start = 0.0f;
  float span = 0.0f;
  if (indeterminate_) {
    start = sweep_start(context.time_seconds);
    span = kIndeterminateSpan * 2.0f * std::numbers::pi_v<float>;
    request_animation();
  } else {
    span = value_ * 2.0f * std::numbers::pi_v<float>;
  }
  if (span > 0.001f) {
    st::math::Path arc;
    const int segments = std::max(static_cast<int>(std::ceil(span / 0.08f)), 2);
    arc.move_to(point_on_circle(center, radius, start));
    for (int i = 1; i <= segments; ++i) {
      const float angle = start + span * static_cast<float>(i) / static_cast<float>(segments);
      arc.line_to(point_on_circle(center, radius, angle));
    }
    canvas.stroke_path(arc, raster::Paint::solid(progress_color_), thickness_);
  }

  // 中心文本：确定态显示百分比（或自定义文本）；不确定态**不显示**……
  // （不知道进度还显示一个数字是在撒谎，宁可空着）。
  const std::string label = indeterminate_ ? std::string{} : (text_.empty() ? std::format("{}%", percent()) : text_);
  if (label.empty()) return;
  const TextPort& port = text_port_of(context);
  // 文本要能塞进内圆：放不下就缩字号（缩到 8px 为止）。
  float font = style_.font_size;
  const float usable = radius * 2.0f * 0.72f;
  const float natural = port.measure_width(label, font);
  if (natural > usable && natural > 0.0f) {
    font = std::max(font * (usable / natural), 8.0f);
  }
  const float width = port.measure_width(label, font);
  const float top = centered_line_top(port, label, font, ring.y, ring.height);
  port.draw(canvas, label, math::Point{center.x - width * 0.5f, top}, font, text_color_);
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto ProgressRing::semantics_text() const -> std::string {
  return indeterminate_ ? std::string("进行中") : text_;
}

auto ProgressRing::semantics_value() const -> std::string {
  return indeterminate_ ? std::string{} : std::format("{}%", percent());
}

auto ProgressRing::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return std::format("{}", value_);
  if (name == "percent") return std::to_string(percent());
  if (name == "indeterminate") return std::string(indeterminate_ ? kFlagTrue : "false");
  if (name == "text") return text_;
  if (name == "thickness") return std::format("{}", thickness_);
  if (name == "tone") return std::string(tone_name(tone_));
  return Element::get_property(name);
}

auto ProgressRing::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    // 允许 `50` 与 `0.5` 两种写法：`50` 明显是百分比（>1 时按百分比解释）。
    const double raw = *parsed;
    set_value(static_cast<float>(raw > 1.0 ? raw / 100.0 : raw));
    return true;
  }
  if (name == "indeterminate") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_indeterminate(*parsed);
    return true;
  }
  if (name == "text") {
    set_text(std::string(value));
    return true;
  }
  if (name == "thickness") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_thickness(static_cast<float>(*parsed));
    return true;
  }
  if (name == "tone") {
    const auto parsed = components_internal::tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto ProgressRing::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"value", "indeterminate", "text", "thickness", "tone"});
}

auto ProgressRing::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"percent"});
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::ProgressRing);
