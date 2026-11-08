#include "st/ui/components/rating.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

Rating::Rating() = default;

Rating::Rating(int max_stars) : max_(std::clamp(max_stars, 1, 10)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Rating::set_value(float value) {
  const float step = allow_half_ ? 0.5f : 1.0f;
  float clamped = std::clamp(value, 0.0f, static_cast<float>(max_));
  // 吸附到步长（否则 3.7 会显示成「三颗半多一点」，看起来像渲染错）。
  clamped = std::round(clamped / step) * step;
  clamped = std::clamp(clamped, 0.0f, static_cast<float>(max_));
  if (std::abs(clamped - value_) < 1e-6f) return;
  value_ = clamped;
  mark_dirty();
  if (on_change) on_change(value_);
}

void Rating::set_max(int max_stars) {
  const int clamped = std::clamp(max_stars, 1, 10);
  if (max_ == clamped) return;
  max_ = clamped;
  value_ = std::min(value_, static_cast<float>(max_));
  mark_layout_dirty();
}

void Rating::set_readonly(bool readonly) {
  if (readonly_ == readonly) return;
  readonly_ = readonly;
  if (readonly_) hover_value_ = -1.0f;
  mark_dirty();
}

void Rating::set_allow_half(bool allow) {
  if (allow_half_ == allow) return;
  allow_half_ = allow;
  // 关掉半星时把值吸附到整档（否则会留下一个画不出来也点不到的 3.5）。
  set_value(value_);
  mark_dirty();
}

void Rating::set_allow_clear(bool allow) {
  if (allow_clear_ == allow) return;
  allow_clear_ = allow;
  mark_dirty();
}

void Rating::set_show_value(bool show) {
  if (show_value_ == show) return;
  show_value_ = show;
  mark_layout_dirty();
}

void Rating::set_star_size(float size) {
  const float clamped = std::clamp(size, 8.0f, 64.0f);
  if (star_size_ == clamped) return;
  star_size_ = clamped;
  mark_layout_dirty();
}

auto Rating::displayed_value() const noexcept -> float {
  if (readonly_) return value_;
  return hover_value_ >= 0.0f ? hover_value_ : value_;
}

void Rating::commit(float value) {
  if (readonly_) return;
  // 再次点当前值 → 清零（`allow_clear`）。
  if (allow_clear_ && std::abs(value - value_) < 1e-6f) {
    set_value(0.0f);
    return;
  }
  set_value(value);
}

auto Rating::value_at(math::Point point) const -> float {
  for (std::size_t i = 0; i < star_boxes_.size(); ++i) {
    const math::Rect& box = star_boxes_[i];
    if (!box.contains(point)) continue;
    const float half_point = box.x + box.width * 0.5f;
    if (allow_half_ && point.x < half_point) return static_cast<float>(i) + 0.5f;
    return static_cast<float>(i) + 1.0f;
  }
  return -1.0f;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量、落位
// ————————————————————————————————————————————————————————————————————————————

void Rating::apply_theme(const Theme& theme) {
  filled_color_ = theme.colors().warning;   // 星用 warning 色（金色系约定）
  empty_color_ = theme.colors().border;
  text_color_ = theme.colors().text_muted;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = theme.metrics().font_xs;
  style_.color = text_color_;
  apply_text_overrides();
}

void Rating::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = static_cast<float>(max_) * star_size_ +
                static_cast<float>(max_ > 0 ? max_ - 1 : 0) * kStarGap;
  if (show_value_) {
    // 预留 `max.5` 这种最宽形态（值变化时宽度不跳）。
    const std::string sample = std::format("{}.5", max_);
    width += kValueGap + port.measure_width(sample, style_.font_size);
  }
  if (style_.has_explicit_width()) width = style_.width;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height()
                     ? style_.height
                     : std::max(star_size_, port.line_height(style_.font_size));
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void Rating::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  const TextPort& port = text_port_of(context);
  star_boxes_.assign(static_cast<std::size_t>(max_), math::Rect{});
  value_box_ = math::Rect{};
  if (rect.is_empty()) return;

  const float top = rect.y + (rect.height - star_size_) * 0.5f;
  float x = rect.x;
  for (int i = 0; i < max_; ++i) {
    star_boxes_[static_cast<std::size_t>(i)] = math::Rect{x, top, star_size_, star_size_};
    x += star_size_ + kStarGap;
  }
  if (show_value_) {
    const std::string sample = std::format("{}.5", max_);
    const float width = port.measure_width(sample, style_.font_size);
    value_box_ = math::Rect{x - kStarGap + kValueGap, rect.y, width, rect.height};
  }
}

auto Rating::star_rect(std::size_t index) const -> math::Rect {
  return index < star_boxes_.size() ? star_boxes_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Rating::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || star_boxes_.empty()) return;
  const float shown = displayed_value();

  for (std::size_t i = 0; i < star_boxes_.size(); ++i) {
    const math::Rect box = star_boxes_[i];
    if (box.is_empty()) continue;
    const float filled = std::clamp(shown - static_cast<float>(i), 0.0f, 1.0f);
    // 空星（底）
    Icon::draw(canvas, "star", box, empty_color_,
               Icon::effective_stroke("star", star_size_));
    if (filled <= 0.0f) continue;
    if (filled >= 0.999f) {
      Icon::draw(canvas, "star", box, filled_color_,
                 Icon::effective_stroke("star", star_size_));
      continue;
    }
    // ⚠ 半星：用**裁剪**画左半边，而不是画一颗小的。
    // 画小星会让半星看起来比满星瘦一圈（形状不同），像字体回退错误。
    canvas.push_clip_rect(math::Rect{box.x, box.y, box.width * filled, box.height});
    Icon::draw(canvas, "star", box, filled_color_,
               Icon::effective_stroke("star", star_size_));
    canvas.pop_clip();
  }

  if (show_value_ && !value_box_.is_empty()) {
    const TextPort& port = text_port_of(context);
    const float display = readonly_ ? value_ : shown;
    const std::string label = std::abs(display - std::round(display)) < 1e-3f
                                  ? std::format("{}", static_cast<int>(std::lround(display)))
                                  : std::format("{:.1f}", display);
    const float top =
        centered_line_top(port, label, style_.font_size, value_box_.y, value_box_.height);
    port.draw(canvas, label, math::Point{value_box_.x, top}, style_.font_size, text_color_);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto Rating::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_ || readonly_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_value_ = -1.0f;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const float target = value_at(event.position);
      if (std::abs(target - hover_value_) > 1e-6f) {
        hover_value_ = target;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      const float target = value_at(event.position);
      if (target < 0.0f) return false;
      commit(target);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      const float step = allow_half_ ? 0.5f : 1.0f;
      if (event.key == "ArrowRight" || event.key == "ArrowUp") {
        set_value(value_ + step);
        return true;
      }
      if (event.key == "ArrowLeft" || event.key == "ArrowDown") {
        set_value(value_ - step);
        return true;
      }
      if (event.key == "Home") {
        set_value(0.0f);
        return true;
      }
      if (event.key == "End") {
        set_value(static_cast<float>(max_));
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto Rating::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Rating::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (readonly_) return MouseCursor::Arrow;
  for (const auto& box : star_boxes_) {
    if (box.contains(point)) return MouseCursor::Hand;
  }
  return MouseCursor::Arrow;
}

auto Rating::semantics_text() const -> std::string { return "评分"; }

auto Rating::semantics_value() const -> std::string {
  return std::format("{} / {}", value_, max_);
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Rating::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return std::format("{}", value_);
  if (name == "max") return std::to_string(max_);
  if (name == "readonly") return std::string(readonly_ ? kFlagTrue : "false");
  if (name == "allow_half") return std::string(allow_half_ ? kFlagTrue : "false");
  if (name == "allow_clear") return std::string(allow_clear_ ? kFlagTrue : "false");
  if (name == "show_value") return std::string(show_value_ ? kFlagTrue : "false");
  if (name == "star_size") return std::format("{}", star_size_);
  return std::nullopt;
}

auto Rating::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_value(static_cast<float>(*parsed));
    return true;
  }
  if (name == "max") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_max(static_cast<int>(*parsed));
    return true;
  }
  if (name == "readonly") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_readonly(*parsed);
    return true;
  }
  if (name == "allow_half") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_allow_half(*parsed);
    return true;
  }
  if (name == "allow_clear") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_allow_clear(*parsed);
    return true;
  }
  if (name == "show_value") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_show_value(*parsed);
    return true;
  }
  if (name == "star_size") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_star_size(static_cast<float>(*parsed));
    return true;
  }
  return false;
}

auto Rating::property_names() const -> std::vector<std::string_view> {
  return {"value", "max", "readonly", "allow_half", "allow_clear", "show_value", "star_size"};
}

auto Rating::readonly_property_names() const -> std::vector<std::string_view> { return {}; }

auto Rating::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "set" || action == "clear") {
    if (action == "clear") {
      set_value(0.0f);
      return true;
    }
    const auto parsed = st::parse_f64(st::trim(argument));
    if (!parsed.has_value()) return false;
    set_value(static_cast<float>(*parsed));
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Rating);
