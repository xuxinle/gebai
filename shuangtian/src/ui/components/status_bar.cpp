#include "st/ui/components/status_bar.hpp"

#include <algorithm>
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

StatusBar::StatusBar() = default;

// ————————————————————————————————————————————————————————————————————————————
// 段
// ————————————————————————————————————————————————————————————————————————————

void StatusBar::add_left(Segment segment) {
  left_.push_back(std::move(segment));
  mark_layout_dirty();
}

void StatusBar::add_right(Segment segment) {
  right_.push_back(std::move(segment));
  mark_layout_dirty();
}

void StatusBar::set_left(std::vector<Segment> segments) {
  left_ = std::move(segments);
  mark_layout_dirty();
}

void StatusBar::set_right(std::vector<Segment> segments) {
  right_ = std::move(segments);
  mark_layout_dirty();
}

void StatusBar::clear() {
  if (left_.empty() && right_.empty()) return;
  left_.clear();
  right_.clear();
  mark_layout_dirty();
}

void StatusBar::set_separators(bool separators) {
  if (separators_ == separators) return;
  separators_ = separators;
  mark_layout_dirty();
}

auto StatusBar::id_of(const Segment& segment) -> std::string_view {
  return segment.id.empty() ? std::string_view(segment.text) : std::string_view(segment.id);
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量
// ————————————————————————————————————————————————————————————————————————————

void StatusBar::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  font_size_ = metrics.font_xs;
  style_.background = theme.colors().surface_alt;
  style_.border_color = theme.colors().border;
  style_.border_width = 0.0f;   // 状态栏靠底色区分；顶部那条线在 `paint_content` 里画
  style_.radius = 0.0f;
  style_.font_size = font_size_;
  style_.color = theme.colors().text_muted;
  apply_text_overrides();
  font_size_ = style_.font_size;
}

auto StatusBar::segment_width(const RenderContext& context, const Segment& segment) const -> float {
  const TextPort& port = text_port_of(context);
  float width = port.measure_width(segment.text, font_size_);
  if (!segment.icon.empty()) width += kIconSize + kIconGap;
  return width;
}

void StatusBar::measure(const RenderContext& context, const Constraints& constraints) {
  float width = 0.0f;
  for (std::size_t i = 0; i < left_.size(); ++i) {
    width += segment_width(context, left_[i]);
    if (i + 1U < left_.size()) width += separators_ ? kSeparatorGap * 2.0f + 0.5f : kSegmentGap;
  }
  for (std::size_t i = 0; i < right_.size(); ++i) {
    width += segment_width(context, right_[i]);
    if (i + 1U < right_.size()) width += separators_ ? kSeparatorGap * 2.0f + 0.5f : kSegmentGap;
  }
  width += kPaddingX * 2.0f;
  // 自然宽之上再留一段“中间空白”的期望值（让状态栏看起来是满宽的框）。
  if (constraints.max_width < kUnbounded) width = constraints.max_width;
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void StatusBar::relayout(const RenderContext& context, math::Rect rect) const {
  left_boxes_.assign(left_.size(), math::Rect{});
  right_boxes_.assign(right_.size(), math::Rect{});
  middle_gap_ = 0.0f;
  if (rect.is_empty()) return;

  // 左区：从左缘往右排。
  float x = rect.x + kPaddingX;
  for (std::size_t i = 0; i < left_.size(); ++i) {
    const float width = segment_width(context, left_[i]);
    left_boxes_[i] = math::Rect{x, rect.y, width, rect.height};
    x += width;
    if (i + 1U < left_.size()) x += separators_ ? kSeparatorGap * 2.0f + 0.5f : kSegmentGap;
  }
  const float left_end = x;

  // 右区：从右缘往左排。
  float right_edge = rect.right() - kPaddingX;
  for (std::size_t i = right_.size(); i > 0; --i) {
    const std::size_t index = i - 1U;
    const float width = segment_width(context, right_[index]);
    right_edge -= width;
    right_boxes_[index] = math::Rect{right_edge, rect.y, width, rect.height};
    if (index > 0) right_edge -= separators_ ? kSeparatorGap * 2.0f + 0.5f : kSegmentGap;
  }
  // 中间空白：左区末端到右区起始。**允许为负**（表示两侧挤在一起了），
  // 夹到 0 并以此作为“是否该压缩”的判据。
  middle_gap_ = std::max(right_edge - left_end, 0.0f);
}

void StatusBar::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

auto StatusBar::left_rect(std::size_t index) const -> math::Rect {
  return index < left_boxes_.size() ? left_boxes_[index] : math::Rect{};
}

auto StatusBar::right_rect(std::size_t index) const -> math::Rect {
  return index < right_boxes_.size() ? right_boxes_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void StatusBar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  // 顶部一道细线：把状态栏与主内容分开（状态栏自己不描边，避免四边都出墨）。
  canvas.fill_rect(math::Rect{bounds_.x, bounds_.y, bounds_.width, 0.5f},
                   raster::Paint::solid(colors.border));

  const auto draw_segment = [&](const Segment& segment, const math::Rect& box, bool hover) {
    if (box.is_empty()) return;
    float text_x = box.x;
    if (!segment.icon.empty()) {
      const math::Rect icon_box{box.x, box.y + (box.height - kIconSize) * 0.5f, kIconSize,
                                kIconSize};
      Icon::draw(canvas, segment.icon, icon_box, tone_color(context.theme, segment.tone),
                 Icon::effective_stroke(segment.icon, kIconSize));
      text_x = icon_box.right() + kIconGap;
    }
    const float available = std::max(box.right() - text_x, 0.0f);
    if (available <= 1.0f) return;
    const std::string shown = port.ellipsize(segment.text, font_size_, available);
    const float top = centered_line_top(port, shown, font_size_, box.y, box.height);
    math::Color color = tone_color(context.theme, segment.tone);
    if (segment.clickable && hover) color = colors.primary;
    port.draw(canvas, shown, math::Point{text_x, top}, font_size_, color);
  };

  for (std::size_t i = 0; i < left_.size() && i < left_boxes_.size(); ++i) {
    draw_segment(left_[i], left_boxes_[i], static_cast<int>(i) == hover_left_);
  }
  for (std::size_t i = 0; i < right_.size() && i < right_boxes_.size(); ++i) {
    draw_segment(right_[i], right_boxes_[i], static_cast<int>(i) == hover_right_);
  }

  // 竖分隔线（段之间）。
  if (separators_) {
    const auto draw_separator = [&](float x) {
      canvas.fill_rect(math::Rect{x - 0.25f, bounds_.y + 6.0f, 0.5f, bounds_.height - 12.0f},
                       raster::Paint::solid(colors.border));
    };
    for (std::size_t i = 0; i + 1U < left_boxes_.size(); ++i) {
      draw_separator(left_boxes_[i].right() + kSeparatorGap);
    }
    for (std::size_t i = 0; i + 1U < right_boxes_.size(); ++i) {
      draw_separator(right_boxes_[i + 1U].x - kSeparatorGap);
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void StatusBar::hit_segment(math::Point point, bool& is_left, std::size_t& index) const {
  is_left = false;
  index = 0;
  for (std::size_t i = 0; i < left_boxes_.size(); ++i) {
    if (!left_boxes_[i].contains(point)) continue;
    is_left = true;
    index = i;
    return;
  }
  for (std::size_t i = 0; i < right_boxes_.size(); ++i) {
    if (!right_boxes_[i].contains(point)) continue;
    is_left = false;
    index = i;
    return;
  }
  index = static_cast<std::size_t>(-1);   // 没命中
}

auto StatusBar::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_left_ = -1;
      hover_right_ = -1;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      bool is_left = false;
      std::size_t index = 0;
      hit_segment(event.position, is_left, index);
      const bool valid = index != static_cast<std::size_t>(-1);
      int next_left = -1;
      int next_right = -1;
      if (valid) {
        const bool clickable = is_left ? (index < left_.size() && left_[index].clickable)
                                       : (index < right_.size() && right_[index].clickable);
        if (clickable) {
          if (is_left) next_left = static_cast<int>(index);
          else next_right = static_cast<int>(index);
        }
      }
      if (next_left != hover_left_ || next_right != hover_right_) {
        hover_left_ = next_left;
        hover_right_ = next_right;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      bool is_left = false;
      std::size_t index = 0;
      hit_segment(event.position, is_left, index);
      if (index == static_cast<std::size_t>(-1)) return false;
      const Segment& segment = is_left ? left_[index] : right_[index];
      if (!segment.clickable) return false;
      if (on_segment_click) on_segment_click(id_of(segment));
      event.handled = true;
      return true;
    }
    default: return false;
  }
}

auto StatusBar::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto StatusBar::cursor_at(math::Point point) const noexcept -> MouseCursor {
  bool is_left = false;
  std::size_t index = 0;
  hit_segment(point, is_left, index);
  if (index == static_cast<std::size_t>(-1)) return MouseCursor::Arrow;
  const bool clickable = is_left ? (index < left_.size() && left_[index].clickable)
                                 : (index < right_.size() && right_[index].clickable);
  return clickable ? MouseCursor::Hand : MouseCursor::Arrow;
}

auto StatusBar::semantics_text() const -> std::string {
  std::string out;
  for (const auto& segment : left_) {
    if (!out.empty()) out += " | ";
    out += segment.text;
  }
  for (const auto& segment : right_) {
    if (!out.empty()) out += " | ";
    out += segment.text;
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto StatusBar::get_property(std::string_view name) const -> std::optional<std::string> {
  const auto join = [](const std::vector<Segment>& segments) {
    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i) {
      if (i > 0) out += '|';
      out += segments[i].text;
    }
    return out;
  };
  if (name == "left") return join(left_);
  if (name == "right") return join(right_);
  if (name == "text" || name == "content") return semantics_text();
  if (name == "separators") return std::string(separators_ ? kFlagTrue : "false");
  if (name == "height") return std::to_string(static_cast<int>(measured_.height));
  return Element::get_property(name);
}

auto StatusBar::set_property(std::string_view name, std::string_view value) -> bool {
  const auto parse = [](std::string_view text) {
    std::vector<Segment> segments;
    for (const auto& part : st::split(text, '|')) {
      Segment segment{};
      segment.text = std::string(st::trim(part));
      if (segment.text.empty()) continue;
      segments.push_back(std::move(segment));
    }
    return segments;
  };
  if (name == "left") {
    set_left(parse(value));
    return true;
  }
  if (name == "right") {
    set_right(parse(value));
    return true;
  }
  if (name == "separators") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_separators(*parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto StatusBar::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"left", "right", "separators"});
}

auto StatusBar::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"text", "height"});
}

auto StatusBar::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "click" || action == "activate") {
    if (argument.empty()) return false;
    // 找可点段并派发。
    for (const auto& segment : left_) {
      if (!segment.clickable || id_of(segment) != argument) continue;
      if (on_segment_click) on_segment_click(id_of(segment));
      return true;
    }
    for (const auto& segment : right_) {
      if (!segment.clickable || id_of(segment) != argument) continue;
      if (on_segment_click) on_segment_click(id_of(segment));
      return true;
    }
    return false;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::StatusBar);
