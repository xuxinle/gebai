#include "st/ui/components/divider_label.hpp"

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

using components_internal::text_port_of;

DividerLabel::DividerLabel() = default;

DividerLabel::DividerLabel(std::string text) : text_(std::move(text)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void DividerLabel::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  mark_layout_dirty();
}

void DividerLabel::set_align(Align align) {
  if (align_ == align) return;
  align_ = align;
  mark_layout_dirty();
}

void DividerLabel::set_action(std::string label) {
  if (action_ == label) return;
  action_ = std::move(label);
  mark_layout_dirty();
}

void DividerLabel::set_line_visible(bool visible) {
  if (line_visible_ == visible) return;
  line_visible_ = visible;
  mark_layout_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void DividerLabel::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  font_size_ = metrics.font_xs;
  action_font_ = metrics.font_xs;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = font_size_;
  style_.color = theme.colors().text_muted;
  apply_text_overrides();
  font_size_ = style_.font_size;
}

void DividerLabel::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float natural = 0.0f;
  if (!text_.empty()) natural += port.measure_width(text_, font_size_);
  if (!action_.empty()) natural += port.measure_width(action_, action_font_) + kActionGap;
  // 线至少要留出可辨长度（否则看起来只是“标题后面跟着一截断线”）。
  natural += kGap * 2.0f + 40.0f;

  float width = style_.has_explicit_width() ? style_.width
                                            : (constraints.max_width < kUnbounded
                                                   ? constraints.max_width
                                                   : natural);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void DividerLabel::relayout(const RenderContext& context, math::Rect rect) const {
  const TextPort& port = text_port_of(context);
  left_line_ = math::Rect{};
  right_line_ = math::Rect{};
  text_box_ = math::Rect{};
  action_box_ = math::Rect{};
  if (rect.is_empty()) return;

  // 线与**文字墨迹中心**对齐（不是行盒中心）。
  //
  // 为何：文字经 `centered_line_top` 落位时居中的是行盒，而字形实际占据的墨迹带
  // 在行盒里**偏下**（字体上方预留了空格）。按行盒中心画线，视觉上就是「线在文字上方」
  // （实测截图：『高级设置』的分隔线压在上边缘）。
  const auto geometry = layout_line(port, kInkReferenceSample, font_size_, rect.height);
  const float line_y = rect.y + geometry.glyph_top + geometry.glyph_height * 0.5f -
                       kLineThickness * 0.5f;

  // 右侧动作先占位（贴右缘）。
  float right_limit = rect.right();
  if (!action_.empty()) {
    const float action_width = port.measure_width(action_, action_font_);
    action_box_ = math::Rect{rect.right() - action_width, rect.y, action_width, rect.height};
    right_limit = action_box_.x - kActionGap;
  }

  const float text_width =
      text_.empty() ? 0.0f : port.measure_width(text_, font_size_);
  // 文字宽不能超过可用宽（长标题时线要缩到 0，而不是文字溢出）。
  const float available = std::max(right_limit - rect.x, 0.0f);
  const float text_clamped = std::min(text_width, std::max(available - kGap * 2.0f, 0.0f));

  float text_x = rect.x;
  switch (align_) {
    case Align::Left: text_x = rect.x; break;
    case Align::Center: text_x = rect.x + (available - text_clamped) * 0.5f; break;
    case Align::Right: text_x = right_limit - text_clamped; break;
  }
  if (!text_.empty()) {
    text_box_ = math::Rect{text_x, rect.y, text_clamped, rect.height};
  }

  if (!line_visible_) return;

  // 左线段：从容器左缘到文字左侧减间隙（`Left` 对齐时长度为 0，不画）。
  const float left_end = text_.empty() ? right_limit : text_box_.x - kGap;
  if (left_end - rect.x > 0.5f) {
    left_line_ = math::Rect{rect.x, line_y, left_end - rect.x, kLineThickness};
  }
  // 右线段：从文字右侧加间隙到右边界。
  const float right_begin = (text_.empty() ? left_end : text_box_.right() + kGap);
  if (right_limit - right_begin > 0.5f) {
    right_line_ = math::Rect{right_begin, line_y, right_limit - right_begin, kLineThickness};
  }
}

void DividerLabel::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void DividerLabel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  if (!left_line_.is_empty()) {
    canvas.fill_rect(left_line_, raster::Paint::solid(colors.border));
  }
  if (!right_line_.is_empty()) {
    canvas.fill_rect(right_line_, raster::Paint::solid(colors.border));
  }

  if (!text_box_.is_empty() && !text_.empty()) {
    const std::string shown = port.ellipsize(text_, font_size_, text_box_.width);
    const float top =
        centered_line_top(port, shown, font_size_, text_box_.y, text_box_.height);
    port.draw(canvas, shown, math::Point{text_box_.x, top}, font_size_, colors.text_muted);
  }

  if (!action_box_.is_empty() && !action_.empty()) {
    const float width = port.measure_width(action_, action_font_);
    const float top =
        centered_line_top(port, action_, action_font_, action_box_.y, action_box_.height);
    port.draw(canvas, action_, math::Point{action_box_.right() - width, top}, action_font_,
              hover_action_ ? colors.primary : colors.text_muted);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void DividerLabel::activate() {
  if (action_.empty() || !enabled_) return;
  if (on_action) on_action();
}

auto DividerLabel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_action_ = false;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const bool over = !action_box_.is_empty() && action_box_.contains(event.position);
      if (over != hover_action_) {
        hover_action_ = over;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (action_box_.is_empty() || !action_box_.contains(event.position)) return false;
      activate();
      event.handled = true;
      return true;
    }
    default: return false;
  }
}

auto DividerLabel::cursor_at(math::Point point) const noexcept -> MouseCursor {
  return (!action_box_.is_empty() && action_box_.contains(point)) ? MouseCursor::Hand
                                                                 : MouseCursor::Arrow;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto DividerLabel::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "label") return text_;
  if (name == "action") return action_;
  if (name == "align") {
    switch (align_) {
      case Align::Left: return std::string("left");
      case Align::Center: return std::string("center");
      case Align::Right: return std::string("right");
    }
    return std::string("left");
  }
  if (name == "line") return std::string(line_visible_ ? kFlagTrue : "false");
  return Element::get_property(name);
}

auto DividerLabel::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "label") {
    set_text(std::string(value));
    return true;
  }
  if (name == "action") {
    set_action(std::string(value));
    return true;
  }
  if (name == "align") {
    const std::string text(st::trim(value));
    if (text == "left" || text == "start") set_align(Align::Left);
    else if (text == "center" || text == "middle") set_align(Align::Center);
    else if (text == "right" || text == "end") set_align(Align::Right);
    else return false;
    return true;
  }
  if (name == "line") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_line_visible(*parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto DividerLabel::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"text", "action", "align", "line"});
}

auto DividerLabel::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "activate" || action == "click") {
    if (action_.empty()) return false;
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::DividerLabel);
