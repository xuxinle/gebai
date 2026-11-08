#include "st/ui/components/empty_state.hpp"

#include <algorithm>
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

using components_internal::fill_round_rect;
using components_internal::text_port_of;

namespace {

/// 主按钮的尺寸与内边距（与 `Button` 的 Small 档同一套数值：空态里的按钮不该比别处大）。
constexpr float kActionHeight = 30.0f;
constexpr float kActionPaddingX = 16.0f;
constexpr float kActionRadius = 7.0f;
/// 图标与标题之间、标题与说明之间、说明与按钮之间的间距（按档位缩放）。
constexpr float kGapIcon = 12.0f;
constexpr float kGapText = 6.0f;
constexpr float kGapAction = 16.0f;

}  // namespace

EmptyState::EmptyState() = default;

EmptyState::EmptyState(std::string title, std::string description)
    : title_(std::move(title)), description_(std::move(description)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void EmptyState::set_title(std::string title) {
  if (title_ == title) return;
  title_ = std::move(title);
  mark_layout_dirty();
}

void EmptyState::set_description(std::string description) {
  if (description_ == description) return;
  description_ = std::move(description);
  mark_layout_dirty();
}

void EmptyState::set_icon(std::string icon_name) {
  if (icon_ == icon_name) return;
  icon_ = std::move(icon_name);
  mark_layout_dirty();
}

void EmptyState::set_action(std::string label) {
  if (action_ == label) return;
  action_ = std::move(label);
  mark_layout_dirty();
}

void EmptyState::set_size(Size size) {
  if (size_ == size) return;
  size_ = size;
  mark_layout_dirty();
}

auto EmptyState::icon_size() const noexcept -> float {
  switch (size_) {
    case Size::Small: return 24.0f;
    case Size::Medium: return 36.0f;
    case Size::Large: return 52.0f;
    case Size::Hero: return 72.0f;
  }
  return 36.0f;
}

auto EmptyState::title_font_size() const -> float { return title_font_; }
auto EmptyState::description_font_size() const -> float { return description_font_; }

auto EmptyState::vertical_padding() const noexcept -> float {
  switch (size_) {
    case Size::Small: return 16.0f;
    case Size::Medium: return 28.0f;
    case Size::Large: return 44.0f;
    case Size::Hero: return 64.0f;
  }
  return 28.0f;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void EmptyState::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.color = theme.colors().text;
  switch (size_) {
    case Size::Small:
      title_font_ = metrics.font_sm;
      description_font_ = metrics.font_xs;
      break;
    case Size::Medium:
      title_font_ = metrics.font_base;
      description_font_ = metrics.font_sm;
      break;
    case Size::Large:
      title_font_ = metrics.font_lg;
      description_font_ = metrics.font_base;
      break;
    case Size::Hero:
      title_font_ = metrics.font_xl;
      description_font_ = metrics.font_base;
      break;
  }
  style_.font_size = title_font_;
  apply_text_overrides();
  // 字号可能被显式覆盖（`set_text_size`）——以覆盖为准。
  title_font_ = style_.font_size;
}

void EmptyState::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float natural_width = 0.0f;
  if (!title_.empty()) natural_width = std::max(natural_width, port.measure_width(title_, title_font_));
  if (!description_.empty()) {
    natural_width =
        std::max(natural_width, port.measure_width(description_, description_font_));
  }
  if (!action_.empty()) {
    natural_width = std::max(natural_width,
                             port.measure_width(action_, description_font_) + kActionPaddingX * 2.0f);
  }
  natural_width = std::max(natural_width, icon_size());

  float natural_height = vertical_padding() * 2.0f;
  if (!icon_.empty()) natural_height += icon_size() + kGapIcon;
  if (!title_.empty()) natural_height += port.line_height(title_font_) + kGapText;
  if (!description_.empty()) natural_height += port.line_height(description_font_);
  if (!action_.empty()) natural_height += kGapAction + kActionHeight;

  float width = style_.has_explicit_width() ? style_.width
                                            : (constraints.max_width < kUnbounded
                                                   ? constraints.max_width
                                                   : natural_width + 48.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : natural_height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位（绘制/命中同一份）
// ————————————————————————————————————————————————————————————————————————————

void EmptyState::relayout(const RenderContext& context, math::Rect rect) const {
  const TextPort& port = text_port_of(context);
  icon_box_ = math::Rect{};
  title_box_ = math::Rect{};
  description_box_ = math::Rect{};
  action_box_ = math::Rect{};
  if (rect.is_empty()) return;

  // 各块的自然高（用于垂直居中整组内容）。
  const float icon_h = icon_.empty() ? 0.0f : icon_size();
  const float title_h = title_.empty() ? 0.0f : port.line_height(title_font_);
  const float description_h = description_.empty() ? 0.0f : port.line_height(description_font_);
  const float action_h = action_.empty() ? 0.0f : kActionHeight;
  float group = icon_h + title_h + description_h + action_h;
  if (icon_h > 0.0f && title_h > 0.0f) group += kGapIcon;
  if (title_h > 0.0f && description_h > 0.0f) group += kGapText;
  if (action_h > 0.0f && group > action_h) group += kGapAction;

  // 内容少时居中；内容多时至少留出档位留白（否则一眼看去贴边）。
  float y = rect.y + std::max((rect.height - group) * 0.5f, vertical_padding());

  if (icon_h > 0.0f) {
    icon_box_ = math::Rect{rect.center().x - icon_h * 0.5f, y, icon_h, icon_h};
    y += icon_h + (title_h > 0.0f ? kGapIcon : 0.0f);
  }
  if (title_h > 0.0f) {
    title_box_ = math::Rect{rect.x, y, rect.width, title_h};
    y += title_h + (description_h > 0.0f ? kGapText : 0.0f);
  }
  if (description_h > 0.0f) {
    description_box_ = math::Rect{rect.x, y, rect.width, description_h};
    y += description_h;
  }
  if (action_h > 0.0f) {
    const float action_width =
        port.measure_width(action_, description_font_) + kActionPaddingX * 2.0f;
    y += kGapAction;
    action_box_ =
        math::Rect{rect.center().x - action_width * 0.5f, y, action_width, kActionHeight};
  }
}

void EmptyState::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void EmptyState::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  // 图标：弱色调（它是氛围，不该比标题抢眼）。
  if (!icon_box_.is_empty() && !icon_.empty()) {
    Icon::draw(canvas, icon_, icon_box_, colors.text_faint,
               Icon::effective_stroke(icon_, icon_box_.width));
  }
  // 标题：居中、Medium 字重（与卡片标题同口径）。
  if (!title_box_.is_empty() && !title_.empty()) {
    const float width = port.measure_width(title_, title_font_);
    const float top = centered_line_top(port, title_, title_font_, title_box_.y, title_box_.height);
    port.draw(canvas, title_, math::Point{title_box_.center().x - width * 0.5f, top}, title_font_,
              colors.text, text::FontRole::Proportional, 0.0f, false);
  }
  // 说明：次要色调、居中。
  if (!description_box_.is_empty() && !description_.empty()) {
    const float width = port.measure_width(description_, description_font_);
    const float top = centered_line_top(port, description_, description_font_,
                                        description_box_.y, description_box_.height);
    port.draw(canvas, description_, math::Point{description_box_.center().x - width * 0.5f, top},
              description_font_, colors.text_muted);
  }
  // 主按钮：主色实底（空态的动作是主要出路，用最醒目的档）。
  if (!action_box_.is_empty() && !action_.empty()) {
    const math::Color fill = !enabled_            ? colors.primary_soft
                             : hover_action_      ? colors.primary_hover
                                                  : colors.primary;
    fill_round_rect(canvas, action_box_, kActionRadius, fill);
    const float width = port.measure_width(action_, description_font_);
    const float top =
        centered_line_top(port, action_, description_font_, action_box_.y, action_box_.height);
    port.draw(canvas, action_, math::Point{action_box_.center().x - width * 0.5f, top},
              description_font_, colors.on_primary);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void EmptyState::activate() {
  if (action_.empty() || !enabled_) return;
  if (on_action) on_action();
}

auto EmptyState::on_event(const RenderContext& context, Event& event) -> bool {
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
    case EventKind::KeyDown: {
      if (event.key != "Enter" && event.key != " ") return false;
      if (action_.empty()) return false;
      activate();
      return true;
    }
    default: return false;
  }
}

auto EmptyState::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto EmptyState::cursor_at(math::Point point) const noexcept -> MouseCursor {
  return (!action_box_.is_empty() && action_box_.contains(point)) ? MouseCursor::Hand : MouseCursor::Arrow;
}

auto EmptyState::semantics_value() const -> std::string { return action_; }

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto EmptyState::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "title" || name == "text") return title_;
  if (name == "description" || name == "hint") return description_;
  if (name == "icon") return icon_;
  if (name == "action" || name == "label") return action_;
  if (name == "size" || name == "scale") {
    switch (size_) {
      case Size::Small: return std::string("small");
      case Size::Medium: return std::string("medium");
      case Size::Large: return std::string("large");
      case Size::Hero: return std::string("hero");
    }
    return std::string("medium");
  }
  return std::nullopt;
}

auto EmptyState::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "title" || name == "text") {
    set_title(std::string(value));
    return true;
  }
  if (name == "description" || name == "hint") {
    set_description(std::string(value));
    return true;
  }
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  if (name == "action" || name == "label") {
    set_action(std::string(value));
    return true;
  }
  if (name == "size" || name == "scale") {
    const std::string text(st::trim(value));
    if (text == "small" || text == "sm") set_size(Size::Small);
    else if (text == "medium" || text == "md") set_size(Size::Medium);
    else if (text == "large" || text == "lg") set_size(Size::Large);
    else if (text == "hero" || text == "xl") set_size(Size::Hero);
    else return false;
    return true;
  }
  return false;
}

auto EmptyState::property_names() const -> std::vector<std::string_view> {
  return {"title", "description", "icon", "action", "size"};
}

auto EmptyState::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "activate" || action == "click") {
    if (action_.empty()) return false;
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::EmptyState);
