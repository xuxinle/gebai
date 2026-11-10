#include "st/ui/components/banner.hpp"

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
using components_internal::tone_from_name;
using components_internal::text_port_of;

namespace {

constexpr float kActionPaddingX = 10.0f;
constexpr float kActionHeight = 24.0f;
constexpr float kActionRadius = 6.0f;

}  // namespace

Banner::Banner() = default;

Banner::Banner(std::string title, Tone tone) : title_(std::move(title)), tone_(tone) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Banner::set_title(std::string title) {
  if (title_ == title) return;
  title_ = std::move(title);
  mark_layout_dirty();
}

void Banner::set_description(std::string description) {
  if (description_ == description) return;
  description_ = std::move(description);
  mark_layout_dirty();
}

void Banner::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

void Banner::set_icon(std::string icon_name) {
  if (icon_ == icon_name) return;
  icon_ = std::move(icon_name);
  mark_dirty();
}

void Banner::set_action(std::string label) {
  if (action_ == label) return;
  action_ = std::move(label);
  mark_layout_dirty();
}

void Banner::set_closable(bool closable) {
  if (closable_ == closable) return;
  closable_ = closable;
  mark_layout_dirty();
}

void Banner::dismiss() {
  if (!visible()) return;
  set_visible(false);
  if (on_dismiss) on_dismiss();
}

auto Banner::default_icon() const -> std::string_view {
  switch (tone_) {
    case Tone::Success: return "success";
    case Tone::Warning: return "warning";
    case Tone::Danger: return "error";
    case Tone::Primary:
    case Tone::Accent: return "info";
    default: return "info";
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Banner::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};   // 底自绘（左侧色条 + 软色底）
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = metrics.radius_md;
  style_.font_size = metrics.font_sm;
  style_.color = tone_color(theme, tone_);
  apply_text_overrides();
}

void Banner::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const float title_font = style_.font_size;
  const float description_font = std::max(style_.font_size - 1.0f, 10.0f);

  // 文本区可用宽 = 总宽 - 色条 - 横向内边距×2 - 图标 - 动作 - 关闭。
  const float right_slots =
      (!action_.empty() ? port.measure_width(action_, description_font) + kActionPaddingX * 2.0f + kGap
                        : 0.0f) +
      (closable_ ? kCloseArea + kGap : 0.0f);
  float content_width =
      constraints.max_width < kUnbounded ? constraints.max_width : 420.0f;
  if (style_.has_explicit_width()) content_width = style_.width;
  if (constraints.max_width < kUnbounded) content_width = std::min(content_width, constraints.max_width);
  content_width = std::clamp(content_width, style_.min_width, style_.max_width);

  const float text_width =
      std::max(content_width - kAccentWidth - kPaddingX * 2.0f - kIconSize - kGap - right_slots, 40.0f);
  // 折行后的高度（标题与说明各自折行）。
  const float line_h = port.line_height(title_font);
  float rows_title = 1.0f;
  if (!title_.empty()) {
    rows_title = static_cast<float>(port.wrap(title_, title_font, text_width).size());
    if (rows_title < 1.0f) rows_title = 1.0f;
  }
  float rows_desc = 0.0f;
  if (!description_.empty()) {
    rows_desc = static_cast<float>(port.wrap(description_, description_font, text_width).size());
    if (rows_desc < 1.0f) rows_desc = 1.0f;
  }
  float natural_height = kPaddingY * 2.0f + rows_title * line_h;
  if (rows_desc > 0.0f) natural_height += rows_desc * port.line_height(description_font) + 3.0f;
  natural_height = std::max(natural_height, kPaddingY * 2.0f + kIconSize);

  float height = style_.has_explicit_height() ? style_.height : natural_height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{content_width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void Banner::relayout(const RenderContext& context, math::Rect rect) const {
  const TextPort& port = text_port_of(context);
  const float description_font = std::max(style_.font_size - 1.0f, 10.0f);
  close_ = math::Rect{};
  action_box_ = math::Rect{};
  if (rect.is_empty()) return;

  const float right = rect.right() - kPaddingX;
  if (closable_) {
    close_ = math::Rect{right - kCloseArea, rect.y + kPaddingY, kCloseArea, kCloseArea};
  }
  if (!action_.empty()) {
    const float width = port.measure_width(action_, description_font) + kActionPaddingX * 2.0f;
    const float anchor = closable_ ? close_.x - kGap : right;
    action_box_ = math::Rect{anchor - width, rect.y + kPaddingY + 2.0f, width, kActionHeight};
  }
}

void Banner::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Banner::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);
  const float title_font = style_.font_size;
  const float description_font = std::max(style_.font_size - 1.0f, 10.0f);
  const math::Color accent = tone_color(context.theme, tone_);

  // 底：软色调 + 左缘一道色条（色条是「这是什么性质的通知」的第一眼线索）。
  fill_round_rect(canvas, bounds_, style_.radius, tone_soft_color(context.theme, tone_));
  const math::Rect accent_rect{bounds_.x, bounds_.y, kAccentWidth, bounds_.height};
  canvas.fill_rect(accent_rect, raster::Paint::solid(accent));

  // 图标
  const float icon_x = bounds_.x + kAccentWidth + kPaddingX;
  const math::Rect icon_box{icon_x, bounds_.y + kPaddingY, kIconSize, kIconSize};
  const std::string glyph = icon_.empty() ? std::string(default_icon()) : icon_;
  Icon::draw(canvas, glyph, icon_box, accent, Icon::effective_stroke(glyph, kIconSize));

  // 文本区：右侧让出动作/关闭的槽位。
  float text_right = bounds_.right() - kPaddingX;
  if (!action_box_.is_empty()) text_right = action_box_.x - kGap;
  else if (!close_.is_empty()) text_right = close_.x - kGap;
  const float text_x = icon_box.right() + kGap;
  const float text_width = std::max(text_right - text_x, 20.0f);

  float y = bounds_.y + kPaddingY;
  if (!title_.empty()) {
    const std::string title = port.ellipsize(title_, title_font, text_width);
    port.draw(canvas, title, math::Point{text_x, y}, title_font, colors.text,
              text::FontRole::Proportional, 0.0f, true);
    y += port.line_height(title_font);
  }
  if (!description_.empty()) {
    const std::string description =
        port.ellipsize(description_, description_font, text_width);
    y += 3.0f;
    port.draw(canvas, description, math::Point{text_x, y}, description_font, colors.text_muted);
  }

  // 动作按钮：描边按钮（次级视觉——通知条的主动作不该比页面主动作更重）。
  if (!action_box_.is_empty()) {
    const math::Color fill = hover_action_ ? colors.surface_hover : colors.surface;
    fill_round_rect(canvas, action_box_, kActionRadius, fill);
    components_internal::paint_outline(canvas, action_box_, kActionRadius, accent, 1.0f);
    const float width = port.measure_width(action_, description_font);
    const float top =
        centered_line_top(port, action_, description_font, action_box_.y, action_box_.height);
    port.draw(canvas, action_, math::Point{action_box_.center().x - width * 0.5f, top},
              description_font, accent);
  }
  // 关闭 ×
  if (!close_.is_empty()) {
    const float glyph_size = 12.0f;
    const math::Rect box{close_.center().x - glyph_size * 0.5f,
                         close_.center().y - glyph_size * 0.5f, glyph_size, glyph_size};
    Icon::draw(canvas, "close", box, hover_close_ ? colors.text : colors.text_faint, 1.5f);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void Banner::activate() {
  if (action_.empty() || !enabled_) return;
  if (on_action_click) on_action_click();
}

auto Banner::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_ || !visible()) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_close_ = false;
      hover_action_ = false;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const bool on_close = closable_ && close_.contains(event.position);
      const bool on_action = !action_box_.is_empty() && action_box_.contains(event.position);
      if (on_close != hover_close_ || on_action != hover_action_) {
        hover_close_ = on_close;
        hover_action_ = on_action;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (closable_ && close_.contains(event.position)) {
        dismiss();
        event.handled = true;
        return true;
      }
      if (!action_box_.is_empty() && action_box_.contains(event.position)) {
        activate();
        event.handled = true;
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto Banner::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Banner::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (closable_ && close_.contains(point)) return MouseCursor::Hand;
  if (!action_box_.is_empty() && action_box_.contains(point)) return MouseCursor::Hand;
  return MouseCursor::Arrow;
}

auto Banner::semantics_text() const -> std::string {
  if (description_.empty()) return title_;
  return title_.empty() ? description_ : title_ + "：" + description_;
}

auto Banner::semantics_value() const -> std::string { return std::string(tone_name(tone_)); }

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Banner::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "title" || name == "text") return title_;
  if (name == "description" || name == "hint") return description_;
  if (name == "tone") return std::string(tone_name(tone_));
  if (name == "icon") return icon_.empty() ? std::string(default_icon()) : icon_;
  if (name == "action" || name == "label") return action_;
  if (name == "closable") return std::string(closable_ ? kFlagTrue : "false");
  return Element::get_property(name);
}

auto Banner::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "title" || name == "text") {
    set_title(std::string(value));
    return true;
  }
  if (name == "description" || name == "hint") {
    set_description(std::string(value));
    return true;
  }
  if (name == "tone") {
    const auto parsed = tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
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
  if (name == "closable") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_closable(*parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto Banner::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"title", "description", "tone", "icon", "action", "closable"});
}

auto Banner::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"visible"});
}

auto Banner::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "dismiss" || action == "close") {
    if (!closable_ && action == "close") return false;
    dismiss();
    return true;
  }
  if (action == "activate" || action == "click") {
    if (action_.empty()) return false;
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Banner);
