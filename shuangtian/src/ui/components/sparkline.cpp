#include "st/ui/components/sparkline.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

Sparkline::Sparkline() = default;

Sparkline::Sparkline(std::vector<double> values) : values_(std::move(values)) {
  update_range();
}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Sparkline::set_values(std::vector<double> values) {
  values_ = std::move(values);
  update_range();
  mark_layout_dirty();
}

void Sparkline::set_values_csv(std::string_view csv) {
  std::vector<double> next;
  for (const auto& part : st::split(csv, ',')) {
    const std::string_view trimmed = st::trim(part);
    if (trimmed.empty()) continue;
    if (const auto parsed = st::parse_f64(trimmed); parsed.has_value()) {
      next.push_back(*parsed);
    }
  }
  set_values(std::move(next));
}

void Sparkline::add_value(double value) {
  values_.push_back(value);
  update_range();
  mark_layout_dirty();
}

void Sparkline::clear_values() {
  if (values_.empty()) return;
  values_.clear();
  update_range();
  mark_layout_dirty();
}

void Sparkline::set_shape(Shape shape) {
  if (shape_ == shape) return;
  shape_ = shape;
  mark_dirty();
}

void Sparkline::set_range(double min_value, double max_value) {
  // 倒置的区间自动纠正（调用方传反了不该静默画出空白图）。
  if (min_value > max_value) std::swap(min_value, max_value);
  has_range_ = true;
  range_min_ = min_value;
  range_max_ = max_value;
  update_range();
  mark_dirty();
}

void Sparkline::clear_range() {
  if (!has_range_) return;
  has_range_ = false;
  update_range();
  mark_dirty();
}

void Sparkline::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

void Sparkline::set_baseline(bool baseline) {
  if (baseline_ == baseline) return;
  baseline_ = baseline;
  mark_dirty();
}

void Sparkline::set_emphasis_tail(std::size_t count) {
  if (emphasis_tail_ == count) return;
  emphasis_tail_ = count;
  mark_dirty();
}

void Sparkline::update_range() {
  flat_ = true;
  if (has_range_) {
    data_min_ = range_min_;
    data_max_ = range_max_;
    flat_ = std::abs(range_max_ - range_min_) < 1e-12;
    return;
  }
  if (values_.empty()) {
    data_min_ = 0.0;
    data_max_ = 1.0;
    flat_ = true;
    return;
  }
  double lowest = std::numeric_limits<double>::max();
  double highest = std::numeric_limits<double>::lowest();
  for (const double value : values_) {
    lowest = std::min(lowest, value);
    highest = std::max(highest, value);
  }
  data_min_ = lowest;
  data_max_ = highest;
  flat_ = std::abs(highest - lowest) < 1e-12;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量
// ————————————————————————————————————————————————————————————————————————————

void Sparkline::apply_theme(const Theme& theme) {
  stroke_color_ = tone_color(theme, tone_);
  fill_color_ = tone_soft_color(theme, tone_);
  muted_color_ = theme.colors().border;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
}

void Sparkline::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = style_.has_explicit_width() ? style_.width
                                            : (constraints.max_width < kUnbounded
                                                   ? constraints.max_width
                                                   : 120.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kDefaultHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

auto Sparkline::value_to_y(double value, math::Rect rect) const -> float {
  // 笔画内缩半个线宽：否则最高点/最低点会被边界裁掉（峰谷被削平）。
  const float inset = stroke_ * 0.5f + 0.5f;
  const float top = rect.y + inset;
  const float bottom = rect.bottom() - inset;
  if (bottom <= top) return top;
  if (flat_) return (top + bottom) * 0.5f;   // 全等数据落中线（不是贴底）
  const double span = data_max_ - data_min_;
  if (span <= 0.0) return (top + bottom) * 0.5f;
  const double normalized = (value - data_min_) / span;
  return bottom - static_cast<float>(normalized) * (bottom - top);
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Sparkline::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  if (bounds_.is_empty() || values_.empty()) return;
  const math::Rect area = bounds_;

  if (baseline_) {
    const float zero_y = value_to_y(0.0, area);
    canvas.fill_rect(math::Rect{area.x, zero_y, area.width, 0.5f},
                     raster::Paint::solid(muted_color_));
  }

  const auto x_of = [&](std::size_t index) -> float {
    if (values_.size() <= 1U) return area.center().x;
    const float usable = area.width - stroke_ - 1.0f;
    return area.x + stroke_ * 0.5f + usable * static_cast<float>(index) /
                                                 static_cast<float>(values_.size() - 1U);
  };

  if (shape_ == Shape::Bar) {
    // 柱状：每点一柱。宽度按点数均分，间距取柱宽的固定比例。
    const float slot = area.width / static_cast<float>(values_.size());
    const float bar_width = std::max(slot * (1.0f - kBarGapRatio), 1.0f);
    const float base_y = area.bottom() - 0.5f;
    for (std::size_t i = 0; i < values_.size(); ++i) {
      const float y = value_to_y(values_[i], area);
      const float top = std::min(y, base_y);
      const float height = std::max(base_y - top, 0.5f);
      const bool emphasized =
          emphasis_tail_ > 0U && i + emphasis_tail_ >= values_.size();
      canvas.fill_rect(
          math::Rect{area.x + slot * static_cast<float>(i) + (slot - bar_width) * 0.5f, top,
                     bar_width, height},
          raster::Paint::solid(emphasized ? stroke_color_ : fill_color_));
    }
    return;
  }

  // 折线 / 面积：逐点建路径。
  //
  // ⚠ 点数远多于像素列时的处理：直接连线会画出锯齿噪点。
  // 这里**不降采样**（保持实现简单、峰谷不丢），而是依赖光栅器的抗锯齿；
  // 若将来数据量到万级，应改为「每像素列取 min/max」两趟扫描（见头文件说明）。
  st::math::Path path;
  const math::Point first{x_of(0), value_to_y(values_[0], area)};
  path.move_to(first);
  for (std::size_t i = 1; i < values_.size(); ++i) {
    path.line_to(math::Point{x_of(i), value_to_y(values_[i], area)});
  }

  if (shape_ == Shape::Area) {
    // 面积：闭合到底边再填充（否则填充会跨过整个框）。
    st::math::Path filled = path;
    filled.line_to(math::Point{x_of(values_.size() - 1U), area.bottom() - 0.5f});
    filled.line_to(math::Point{first.x, area.bottom() - 0.5f});
    filled.close();
    canvas.fill_path(filled, raster::Paint::solid(fill_color_));
  }

  // 描线：先整条淡色，再把「尾部强调段」重画成主色（尾段语义是“最近的数据”）。
  canvas.stroke_path(path, raster::Paint::solid(stroke_color_), stroke_);
  if (emphasis_tail_ > 0U && emphasis_tail_ < values_.size()) {
    const std::size_t begin = values_.size() - emphasis_tail_;
    st::math::Path tail;
    tail.move_to(math::Point{x_of(begin), value_to_y(values_[begin], area)});
    for (std::size_t i = begin + 1U; i < values_.size(); ++i) {
      tail.line_to(math::Point{x_of(i), value_to_y(values_[i], area)});
    }
    canvas.fill_path(tail, raster::Paint::solid(stroke_color_));
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto Sparkline::semantics_text() const -> std::string {
  return std::format("{} 个数据点", values_.size());
}

auto Sparkline::semantics_value() const -> std::string {
  if (values_.empty()) return {};
  return std::format("{} .. {}", data_min_, data_max_);
}

auto Sparkline::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "values" || name == "data") {
    std::string out;
    for (std::size_t i = 0; i < values_.size(); ++i) {
      if (i > 0) out += ',';
      out += std::format("{}", values_[i]);
    }
    return out;
  }
  if (name == "shape") {
    switch (shape_) {
      case Shape::Line: return std::string("line");
      case Shape::Area: return std::string("area");
      case Shape::Bar: return std::string("bar");
    }
    return std::string("line");
  }
  if (name == "tone") return std::string(tone_name(tone_));
  if (name == "baseline") return std::string(baseline_ ? kFlagTrue : "false");
  if (name == "emphasis_tail") return std::to_string(emphasis_tail_);
  if (name == "count" || name == "length") return std::to_string(values_.size());
  if (name == "data_min") return std::format("{}", data_min_);
  if (name == "data_max") return std::format("{}", data_max_);
  if (name == "min") return has_range_ ? std::optional<std::string>(std::format("{}", range_min_))
                                       : std::nullopt;
  if (name == "max") return has_range_ ? std::optional<std::string>(std::format("{}", range_max_))
                                       : std::nullopt;
  return std::nullopt;
}

auto Sparkline::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "values" || name == "data") {
    set_values_csv(value);
    return true;
  }
  if (name == "shape") {
    const std::string text(st::trim(value));
    if (text == "line") set_shape(Shape::Line);
    else if (text == "area") set_shape(Shape::Area);
    else if (text == "bar" || text == "column") set_shape(Shape::Bar);
    else return false;
    return true;
  }
  if (name == "tone") {
    const auto parsed = components_internal::tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "baseline") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_baseline(*parsed);
    return true;
  }
  if (name == "emphasis_tail") {
    const auto parsed = st::parse_u64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_emphasis_tail(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (name == "min" || name == "max") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    const double other = name == "min" ? (has_range_ ? range_max_ : data_max_) : (has_range_ ? range_min_ : data_min_);
    if (name == "min") set_range(*parsed, other);
    else set_range(other, *parsed);
    return true;
  }
  return false;
}

auto Sparkline::property_names() const -> std::vector<std::string_view> {
  return {"values", "shape", "tone", "baseline", "emphasis_tail", "min", "max"};
}

auto Sparkline::readonly_property_names() const -> std::vector<std::string_view> {
  return {"count", "data_min", "data_max"};
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Sparkline);
