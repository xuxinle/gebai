#include "st/ui/components/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;

Skeleton::Skeleton() = default;

Skeleton::Skeleton(std::size_t lines) : lines_(std::max<std::size_t>(lines, 1U)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Skeleton::set_shape(Shape shape) {
  if (shape_ == shape) return;
  shape_ = shape;
  if (shape_ != Shape::Text) lines_ = 1;
  mark_layout_dirty();
}

void Skeleton::set_lines(std::size_t lines) {
  const std::size_t clamped = std::max<std::size_t>(lines, 1U);
  if (lines_ == clamped) return;
  lines_ = clamped;
  mark_layout_dirty();
}

void Skeleton::set_last_line_ratio(float ratio) {
  const float clamped = std::clamp(ratio, 0.05f, 1.0f);
  if (last_line_ratio_ == clamped) return;
  last_line_ratio_ = clamped;
  mark_dirty();
  relayout();
}

void Skeleton::set_pulse(bool pulse) {
  if (pulse_ == pulse) return;
  pulse_ = pulse;
  mark_dirty();
}

void Skeleton::set_radius(float radius) {
  const float clamped = std::max(radius, 0.0f);
  if (radius_ == clamped) return;
  radius_ = clamped;
  mark_dirty();
}

auto Skeleton::pulse_factor(double time_seconds) const noexcept -> float {
  if (!pulse_ || kPulsePeriodSeconds <= 0.0f) return 1.0f;
  // 正弦呼吸：`0` 相位最亮、半周期最暗。用 `cos` 让首帧（t=0）为最亮——
  // 首帧最暗会让「刚打开就一片灰」看起来像没加载好。
  const double turns = time_seconds / static_cast<double>(kPulsePeriodSeconds);
  const double fraction = turns - std::floor(turns);
  const auto wave = static_cast<float>(std::cos(fraction * 2.0 * 3.14159265358979));
  return 1.0f - kPulseDepth * (1.0f - wave) * 0.5f;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Skeleton::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  block_color_ = colors.surface_alt;
  base_color_ = colors.surface;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = radius_;
}

void Skeleton::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const std::size_t count = shape_ == Shape::Text ? lines_ : 1U;
  const float natural_height =
      shape_ == Shape::Text
          ? static_cast<float>(count) * kLineHeight +
                static_cast<float>(count > 0U ? count - 1U : 0U) * kLineGap
          : (shape_ == Shape::Circle ? kLineHeight * 2.0f : kLineHeight * 3.0f);

  float width = style_.has_explicit_width() ? style_.width
                                            : (constraints.max_width < kUnbounded
                                                   ? constraints.max_width
                                                   : 240.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : natural_height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void Skeleton::relayout() const {
  blocks_.clear();
  if (bounds_.is_empty()) return;
  switch (shape_) {
    case Shape::Circle: {
      const float side = std::min(bounds_.width, bounds_.height);
      blocks_.emplace_back(bounds_.x, bounds_.y + (bounds_.height - side) * 0.5f, side, side);
      break;
    }
    case Shape::Rect: {
      blocks_.push_back(bounds_);
      break;
    }
    case Shape::Text: {
      // 文本行：行数给定则均分高度（让骨架与真实段落同高，切换时不跳）。
      const std::size_t count = std::max<std::size_t>(lines_, 1U);
      const float total_gap = kLineGap * static_cast<float>(count > 0U ? count - 1U : 0U);
      const float line_height =
          std::max((bounds_.height - total_gap) / static_cast<float>(count), 4.0f);
      float y = bounds_.y;
      for (std::size_t index = 0; index < count; ++index) {
        float width = bounds_.width;
        if (index + 1U == count) width *= last_line_ratio_;
        blocks_.emplace_back(bounds_.x, y, width, line_height);
        y += line_height + kLineGap;
      }
      break;
    }
  }
}

void Skeleton::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout();
}

auto Skeleton::block_rect(std::size_t index) const -> math::Rect {
  return index < blocks_.size() ? blocks_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Skeleton::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout();
  const float factor = pulse_factor(context.time_seconds);
  // 底色按系数向 `surface` 靠（不是简单调 alpha：调 alpha 会让占位块透出背后的东西，
  // 在深色主题上看起来像“脏斑”）。
  const auto shade = [&](math::Color color) -> math::Color {
    const auto mix = [factor](std::uint8_t top, std::uint8_t bottom) -> std::uint8_t {
      return static_cast<std::uint8_t>(static_cast<float>(bottom) +
                                       (static_cast<float>(top) - static_cast<float>(bottom)) *
                                           factor +
                                       0.5f);
    };
    return math::Color{mix(color.r, base_color_.r), mix(color.g, base_color_.g),
                       mix(color.b, base_color_.b), color.a};
  };

  for (std::size_t index = 0; index < blocks_.size(); ++index) {
    const math::Rect rect = blocks_[index];
    const float radius = shape_ == Shape::Circle ? rect.width * 0.5f : radius_;
    fill_round_rect(canvas, rect, radius, shade(block_color_));
  }
  // 呼吸期间需要续帧（否则画面会停在某一相位）。
  if (pulse_ && kPulseDepth > 0.0f) request_animation();
}

auto Skeleton::semantics_text() const -> std::string { return "loading"; }

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Skeleton::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "lines" || name == "count") return std::to_string(lines_);
  if (name == "shape") {
    switch (shape_) {
      case Shape::Rect: return std::string("rect");
      case Shape::Circle: return std::string("circle");
      case Shape::Text: return std::string("text");
    }
    return std::string("text");
  }
  if (name == "last_line_ratio") return std::format("{}", last_line_ratio_);
  if (name == "pulse") return std::string(pulse_ ? kFlagTrue : "false");
  if (name == "radius") return std::format("{}", radius_);
  if (name == "height") return std::format("{}", measured_.height);
  return Element::get_property(name);
}

auto Skeleton::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "lines" || name == "count") {
    const auto parsed = st::parse_u64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_lines(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (name == "shape") {
    const std::string text(st::trim(value));
    if (text == "rect" || text == "rectangle" || text == "block") set_shape(Shape::Rect);
    else if (text == "circle" || text == "avatar") set_shape(Shape::Circle);
    else if (text == "text" || text == "line" || text == "lines") set_shape(Shape::Text);
    else return false;
    return true;
  }
  if (name == "last_line_ratio") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_last_line_ratio(static_cast<float>(*parsed));
    return true;
  }
  if (name == "pulse") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_pulse(*parsed);
    return true;
  }
  if (name == "radius") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_radius(static_cast<float>(*parsed));
    return true;
  }
  return Element::set_property(name, value);
}

auto Skeleton::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"lines", "shape", "last_line_ratio", "pulse", "radius"});
}

auto Skeleton::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"height"});
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Skeleton);
