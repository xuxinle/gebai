#include "st/ui/components/link.hpp"

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

namespace {

/// 下划线相对**基线**的偏移（px）。
///
/// ⚠ 用基线而不是「墨迹底」：墨迹底由 `g`/`p` 这类**下伸字形**决定，
/// 直接按它画会让下划线穿过下伸部（看起来像删除线）。基线 + 2px 落在
/// 下伸部之上、字母底之下，是排印惯例。
constexpr float kUnderlineOffset{2.0f};
constexpr float kUnderlineThickness{1.0f};
constexpr float kUnderlineThicknessHover{1.6f};

}  // namespace

Link::Link() = default;

Link::Link(std::string text) : text_(std::move(text)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Link::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  mark_layout_dirty();
}

void Link::set_url(std::string url) {
  if (url_ == url) return;
  url_ = std::move(url);
  mark_dirty();
}

void Link::set_external(bool external) {
  if (external_ == external) return;
  external_ = external;
  mark_layout_dirty();
}

void Link::set_tone(Tone tone) {
  if (tone_ == tone) return;
  tone_ = tone;
  mark_dirty();
}

void Link::set_underline(bool underline) {
  if (underline_ == underline) return;
  underline_ = underline;
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量、落位
// ————————————————————————————————————————————————————————————————————————————

void Link::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  font_size_ = metrics.font_sm;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = font_size_;
  style_.color = tone_color(theme, tone_);
  apply_text_overrides();
  font_size_ = style_.font_size;
}

void Link::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = port.measure_width(text_, font_size_);
  if (external_) width += kExternalGap + kExternalSize;
  if (style_.has_explicit_width()) width = style_.width;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : port.line_height(font_size_);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void Link::relayout(const RenderContext& context, math::Rect rect) const {
  const TextPort& port = text_port_of(context);
  text_box_ = math::Rect{};
  underline_box_ = math::Rect{};
  external_box_ = math::Rect{};
  shown_.clear();
  if (rect.is_empty() || text_.empty()) return;

  // `↗` 先占位：截断时文字要让出它，否则标记会被挤出容器。
  const float reserved = external_ ? kExternalGap + kExternalSize : 0.0f;
  const float available = std::max(rect.width - reserved, 0.0f);
  shown_ = port.ellipsize(text_, font_size_, available);
  const float text_width = port.measure_width(shown_, font_size_);

  const auto geometry = layout_line(port, kInkReferenceSample, font_size_, rect.height);
  text_box_ = math::Rect{rect.x, rect.y, text_width, rect.height};

  if (external_) {
    const float top = rect.y + (rect.height - kExternalSize) * 0.5f;
    external_box_ =
        math::Rect{rect.x + text_width + kExternalGap, top, kExternalSize, kExternalSize};
  }

  if (underline_ && !shown_.empty()) {
    // ⚠ 下划线宽度跟**截断后**的文字走，且贴基线（不按行盒底）。
    const float y = rect.y + geometry.baseline + kUnderlineOffset;
    underline_box_ = math::Rect{rect.x, y, text_width, hover_ ? kUnderlineThicknessHover
                                                              : kUnderlineThickness};
  }
}

void Link::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Link::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || text_.empty()) return;
  relayout(context, bounds_);
  const TextPort& port = text_port_of(context);
  const Palette& colors = context.theme.colors();

  // 悬停：颜色加深到主色（弱视用户还要靠下划线加粗，见 `relayout`）。
  const math::Color color = !enabled_ ? colors.text_faint
                              : hover_  ? colors.primary
                                        : tone_color(context.theme, tone_);
  const float top = centered_line_top(port, shown_, font_size_, bounds_.y, bounds_.height);
  port.draw(canvas, shown_, math::Point{text_box_.x, top}, font_size_, color,
            text::FontRole::Proportional, 0.0f, false);

  if (!underline_box_.is_empty()) {
    canvas.fill_rect(underline_box_, raster::Paint::solid(color));
  }

  // `↗` 外部链接标记：用图标集里的箭头（与文字同色，跟随悬停）。
  if (!external_box_.is_empty()) {
    Icon::draw(canvas, "external", external_box_, color,
               Icon::effective_stroke("external", kExternalSize));
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void Link::activate() {
  if (!enabled_) return;
  if (on_click) on_click(url_);
}

auto Link::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
      hover_ = true;
      mark_dirty();
      return false;
    case EventKind::HoverOut:
      hover_ = false;
      mark_dirty();
      return false;
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (!bounds_.contains(event.position)) return false;
      activate();
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      if (event.key != "Enter" && event.key != " ") return false;
      activate();
      return true;
    }
    default: return false;
  }
}

auto Link::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Link::cursor_at(math::Point point) const noexcept -> MouseCursor {
  return bounds_.contains(point) && enabled_ ? MouseCursor::Hand : MouseCursor::Arrow;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Link::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "label") return text_;
  if (name == "url" || name == "href") return url_;
  if (name == "external") return std::string(external_ ? kFlagTrue : "false");
  if (name == "underline") return std::string(underline_ ? kFlagTrue : "false");
  if (name == "tone") return std::string(tone_name(tone_));
  return Element::get_property(name);
}

auto Link::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "label") {
    set_text(std::string(value));
    return true;
  }
  if (name == "url" || name == "href") {
    set_url(std::string(value));
    return true;
  }
  if (name == "external") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_external(*parsed);
    return true;
  }
  if (name == "underline") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_underline(*parsed);
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

auto Link::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"text", "url", "external", "underline", "tone"});
}

auto Link::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "click" || action == "activate") {
    activate();
    return true;
  }
  (void)argument;
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Link);
