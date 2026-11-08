#include "st/ui/components/search_box.hpp"

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

using components_internal::fill_round_rect;
using components_internal::text_port_of;

SearchBox::SearchBox() = default;

SearchBox::SearchBox(std::string placeholder) : placeholder_(std::move(placeholder)) {}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void SearchBox::set_value(std::string value) {
  if (value_ == value) return;
  value_ = std::move(value);
  caret_ = value_.size();
  mark_layout_dirty();
  if (on_change) on_change(value_);
}

void SearchBox::set_placeholder(std::string placeholder) {
  if (placeholder_ == placeholder) return;
  placeholder_ = std::move(placeholder);
  mark_layout_dirty();
}

void SearchBox::set_count(int count) {
  if (count_ == count) return;
  count_ = count;
  mark_layout_dirty();
}

void SearchBox::set_clearable(bool clearable) {
  if (clearable_ == clearable) return;
  clearable_ = clearable;
  mark_layout_dirty();
}

void SearchBox::clear() {
  if (on_clear) on_clear();
  if (value_.empty()) return;
  value_.clear();
  caret_ = 0;
  mark_layout_dirty();
  if (on_change) on_change(value_);
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void SearchBox::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.background = colors.surface_alt;
  style_.border_color = colors.border;
  style_.border_width = 1.0f;
  style_.radius = kRadius;
  style_.font_size = metrics.font_sm;
  style_.color = colors.text;
  apply_text_overrides();
}

void SearchBox::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const std::string sample = value_.empty() ? placeholder_ : value_;
  float width = port.measure_width(sample, style_.font_size) + kPaddingX * 2.0f +
                kIconSize + kGap + (clearable_ ? kClearArea + kGap : 0.0f) + 40.0f;
  width = std::max(width, 160.0f);
  if (style_.has_explicit_width()) width = style_.width;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void SearchBox::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  const TextPort& port = text_port_of(context);
  icon_ = math::Rect{rect.x + kPaddingX, rect.y + (rect.height - kIconSize) * 0.5f, kIconSize,
                     kIconSize};

  // 右到左让位：计数值 → 清除位 → 输入区。
  float right = rect.right() - kPaddingX;
  count_box_ = math::Rect{};
  if (count_ >= 0) {
    const std::string label = std::format("{} 项", count_);
    const float width = port.measure_width(label, style_.font_size - 1.0f);
    count_box_ = math::Rect{right - width, rect.y, width, rect.height};
    right = count_box_.x - kGap;
  }
  clear_ = math::Rect{};
  const bool show_clear = clearable_ && !value_.empty();
  if (show_clear) {
    clear_ = math::Rect{right - kClearArea, rect.y + (rect.height - kClearArea) * 0.5f,
                        kClearArea, kClearArea};
    right = clear_.x - kGap;
  }
  const float field_x = icon_.right() + kGap;
  field_ = math::Rect{field_x, rect.y, std::max(right - field_x, 0.0f), rect.height};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void SearchBox::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  Icon::draw(canvas, "search", icon_, colors.text_faint, Icon::effective_stroke("search", kIconSize));

  // 文本：有内容画内容（左对齐、超宽裁左侧——搜索时更关心**刚敲的字**）；
  // 空内容画占位符。
  const bool empty = value_.empty();
  const std::string shown = empty ? placeholder_ : value_;
  const math::Color color = empty ? colors.text_faint : (enabled_ ? colors.text : colors.text_faint);
  const float available = field_.width - kPaddingX * 0.5f;
  // 超宽时保留尾部：从前往后剥（搜索框里"刚输入的部分"信息量最大）。
  std::string visible = shown;
  while (!visible.empty() && port.measure_width(visible, style_.font_size) > available) {
    const std::size_t next = st::utf8_next(visible, 0);
    if (next == 0U) break;
    visible.erase(0, next);
  }
  // 裁掉前缀时补一个省略号（让用户知道左边还有内容）。
  const bool clipped = visible.size() < shown.size();
  if (clipped) visible.insert(0, "…");
  const float top = centered_line_top(port, visible, style_.font_size, bounds_.y, bounds_.height);
  port.draw(canvas, visible, math::Point{field_.x, top}, style_.font_size, color);

  // 光标
  if (focused_ && enabled_) {
    if (std::fmod(context.time_seconds, 1.0) < 0.5) {
      canvas.fill_rect(math::Rect{caret_x(context), bounds_.y + 7.0f, 1.0f, bounds_.height - 14.0f},
                       raster::Paint::solid(colors.text));
    }
    request_animation();
  }

  // 清除 ×
  if (!clear_.is_empty()) {
    const float glyph = 12.0f;
    const math::Rect box{clear_.center().x - glyph * 0.5f, clear_.center().y - glyph * 0.5f,
                         glyph, glyph};
    Icon::draw(canvas, "close", box, hover_clear_ ? colors.text : colors.text_faint, 1.5f);
  }

  // 结果计数
  if (!count_box_.is_empty() && count_ >= 0) {
    const std::string label = std::format("{} 项", count_);
    const float width = port.measure_width(label, style_.font_size - 1.0f);
    const float count_top =
        centered_line_top(port, label, style_.font_size - 1.0f, bounds_.y, bounds_.height);
    port.draw(canvas, label, math::Point{count_box_.right() - width, count_top},
              style_.font_size - 1.0f, colors.text_faint);
  }
}

auto SearchBox::caret_x(const RenderContext& context) const -> float {
  const TextPort& port = text_port_of(context);
  // 与绘制同口径：超宽时取尾部的宽。
  const std::string head = value_.substr(0, std::min(caret_, value_.size()));
  const float available = field_.width - kPaddingX * 0.5f;
  const float full = port.measure_width(value_, style_.font_size);
  if (full <= available) return field_.x + port.measure_width(head, style_.font_size);
  // 已裁剪：光标贴右缘（近似即可——用户正在敲，光标就该在末尾）。
  return field_.right() - kPaddingX * 0.5f;
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void SearchBox::activate() {
  if (!enabled_) return;
  if (on_submit) on_submit(value_);
}

auto SearchBox::handle_key(const Event& event) -> bool {
  if (event.key == "Enter") {
    if (on_submit) on_submit(value_);
    return true;
  }
  if (event.key == "Escape") {
    // `Esc` 只在**有内容**时清空（空内容时 Esc 应交给上层，如关对话框）。
    if (value_.empty()) return false;
    clear();
    return true;
  }
  if (event.key == "Backspace") {
    if (caret_ == 0U) return true;
    const std::size_t begin = st::utf8_prev(value_, caret_);
    value_.erase(begin, caret_ - begin);
    caret_ = begin;
    mark_layout_dirty();
    if (on_change) on_change(value_);
    return true;
  }
  if (event.key == "Delete") {
    if (caret_ >= value_.size()) return true;
    const std::size_t end = st::utf8_next(value_, caret_);
    value_.erase(caret_, end - caret_);
    mark_layout_dirty();
    if (on_change) on_change(value_);
    return true;
  }
  if (event.key == "ArrowLeft") {
    caret_ = st::utf8_prev(value_, caret_);
    mark_dirty();
    return true;
  }
  if (event.key == "ArrowRight") {
    caret_ = st::utf8_next(value_, caret_);
    mark_dirty();
    return true;
  }
  if (event.key == "Home") {
    caret_ = 0;
    mark_dirty();
    return true;
  }
  if (event.key == "End") {
    caret_ = value_.size();
    mark_dirty();
    return true;
  }
  return false;
}

void SearchBox::insert_text(std::string_view inserted) {
  if (inserted.empty()) return;
  caret_ = std::min(caret_, value_.size());
  value_.insert(caret_, inserted);
  caret_ += inserted.size();
  mark_layout_dirty();
  if (on_change) on_change(value_);
}

auto SearchBox::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_clear_ = false;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const bool over = !clear_.is_empty() && clear_.contains(event.position);
      if (over != hover_clear_) {
        hover_clear_ = over;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (!clear_.is_empty() && clear_.contains(event.position)) {
        clear();
        event.handled = true;
        return true;
      }
      return false;
    }
    case EventKind::TextInput: {
      insert_text(event.text);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: return handle_key(event);
    default: return false;
  }
}

auto SearchBox::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto SearchBox::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (!clear_.is_empty() && clear_.contains(point)) return MouseCursor::Hand;
  return MouseCursor::Text;
}

auto SearchBox::semantics_value() const -> std::string {
  if (count_ < 0) return value_;
  return std::format("{}（{} 项）", value_, count_);
}

auto SearchBox::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = true;
  return flags;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto SearchBox::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value" || name == "text") return value_;
  if (name == "placeholder") return placeholder_;
  if (name == "count") return std::to_string(count_);
  if (name == "clearable") return std::string(clearable_ ? kFlagTrue : "false");
  return std::nullopt;
}

auto SearchBox::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "text") {
    set_value(std::string(value));
    return true;
  }
  if (name == "placeholder") {
    set_placeholder(std::string(value));
    return true;
  }
  if (name == "count") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_count(static_cast<int>(*parsed));
    return true;
  }
  if (name == "clearable") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_clearable(*parsed);
    return true;
  }
  return false;
}

auto SearchBox::property_names() const -> std::vector<std::string_view> {
  return {"value", "placeholder", "count", "clearable"};
}

auto SearchBox::readonly_property_names() const -> std::vector<std::string_view> { return {}; }

auto SearchBox::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "clear") {
    clear();
    return true;
  }
  if (action == "submit" || action == "activate" || action == "click") {
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::SearchBox);
