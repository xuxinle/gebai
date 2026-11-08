#include "st/ui/components/toolbar.hpp"

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
using components_internal::paint_outline;
using components_internal::text_port_of;

namespace {

constexpr float kOverflowRadius = 6.0f;

}  // namespace

Toolbar::Toolbar() = default;

// ————————————————————————————————————————————————————————————————————————————
// 条目
// ————————————————————————————————————————————————————————————————————————————

void Toolbar::add_item(std::unique_ptr<Element> item, bool pinned) {
  if (item == nullptr) return;
  Entry entry{};
  entry.kind = Kind::Item;
  entry.pinned = pinned;
  entry.element = std::move(item);
  items_.push_back(std::move(entry));
  mark_layout_dirty();
}

void Toolbar::add_separator() {
  Entry entry{};
  entry.kind = Kind::Separator;
  items_.push_back(std::move(entry));
  mark_layout_dirty();
}

void Toolbar::clear_items() {
  if (items_.empty()) return;
  items_.clear();
  mark_layout_dirty();
}

auto Toolbar::item_at(std::size_t index) const noexcept -> Element* {
  if (index >= items_.size()) return nullptr;
  return items_[index].element.get();
}

auto Toolbar::kind_at(std::size_t index) const noexcept -> Kind {
  return index < items_.size() ? items_[index].kind : Kind::Item;
}

auto Toolbar::item_rect(std::size_t index) const -> math::Rect {
  return index < rects_.size() ? rects_[index] : math::Rect{};
}

auto Toolbar::is_collapsed(std::size_t index) const -> bool {
  if (index >= rects_.size()) return false;
  // 折叠项没有矩形（不画、不命中）。
  return items_[index].kind == Kind::Item && rects_[index].is_empty();
}

void Toolbar::set_overflow_label(std::string label) {
  if (overflow_label_ == label) return;
  overflow_label_ = std::move(label);
  mark_layout_dirty();
}

void Toolbar::show_overflow() {
  if (collapsed_ == 0U) return;
  if (on_overflow) on_overflow();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Toolbar::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = theme.colors().surface_alt;
  style_.border_color = theme.colors().border;
  style_.border_width = 0.0f;   // 工具条靠底色区分，不描边（描边会和内容争）
  style_.radius = 0.0f;
  style_.font_size = metrics.font_sm;
  style_.color = theme.colors().text;
  apply_text_overrides();
}

void Toolbar::resolve_collapse(const RenderContext& context, float available_width) const {
  // 依次把条目的自然宽量出来（缓存到 `items_` 的非 const 侧需要 mutable，
  // 这里直接在局部重算——条目数少，量宽是纯函数，不值得引入 mutable 状态）。
  std::vector<float> widths(items_.size(), 0.0f);
  float total = 0.0f;
  for (std::size_t i = 0; i < items_.size(); ++i) {
    float width = 0.0f;
    if (items_[i].kind == Kind::Separator) {
      width = kGroupGap * 2.0f;   // 分隔两侧各留一个分组间距
    } else if (items_[i].element != nullptr) {
      Constraints inner{};
      inner.max_width = available_width;
      inner.available_width = available_width;
      inner.max_height = kHeight;
      inner.available_height = kHeight;
      items_[i].element->measure(context, inner);
      width = items_[i].element->measured_size().width;
      if (width <= 0.0f) width = 24.0f;   // 未给尺寸的按钮给个最小占位
    }
    widths[i] = width;
    total += width;
    if (i > 0 && items_[i].kind != Kind::Separator && items_[i - 1].kind != Kind::Separator) {
      total += kItemGap;
    }
  }

  collapsed_ = 0;
  if (total <= available_width) return;

  // 装不下：从右往左折（右侧通常次要），跳过 pinned 与分隔。
  const float budget = available_width - kOverflowWidth - kItemGap;
  float used = total;
  for (std::size_t i = items_.size(); i > 0; --i) {
    if (used <= budget) break;
    const std::size_t index = i - 1U;
    if (items_[index].kind == Kind::Separator || items_[index].pinned) continue;
    used -= widths[index];
    if (index > 0) used -= kItemGap;
    ++collapsed_;
  }
}

void Toolbar::measure(const RenderContext& context, const Constraints& constraints) {
  const float available =
      constraints.max_width < kUnbounded ? constraints.max_width : 1.0e9f;
  resolve_collapse(context, std::max(available - kPaddingX * 2.0f, 1.0f));

  // 自然宽 = 全部放得下的宽（父级决定要不要压缩）。
  float natural = kPaddingX * 2.0f;
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == Kind::Separator) {
      natural += kGroupGap * 2.0f;
      continue;
    }
    if (items_[i].element == nullptr) continue;
    Constraints inner{};
    inner.max_width = available;
    inner.available_width = available;
    inner.max_height = kHeight;
    inner.available_height = kHeight;
    items_[i].element->measure(context, inner);
    natural += items_[i].element->measured_size().width;
    if (i + 1U < items_.size() && items_[i + 1U].kind != Kind::Separator) natural += kItemGap;
  }
  if (constraints.max_width < kUnbounded) natural = std::min(natural, constraints.max_width);
  natural = std::clamp(natural, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{natural, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void Toolbar::relayout(const RenderContext& context, math::Rect rect) const {
  rects_.assign(items_.size(), math::Rect{});
  overflow_box_ = math::Rect{};
  if (rect.is_empty()) return;

  const float inner_width = std::max(rect.width - kPaddingX * 2.0f, 0.0f);
  resolve_collapse(context, inner_width);
  if (collapsed_ > 0U) {
    overflow_box_ = math::Rect{rect.right() - kPaddingX - kOverflowWidth,
                               rect.y + (rect.height - 24.0f) * 0.5f, kOverflowWidth, 24.0f};
  }

  // 从左往右铺。
  //
  // 折叠集合已由 `resolve_collapse` 从**右往左**数出来（`collapsed_` 项），
  // 这里按那个绝对下标集合判定，**没有第二套判据**。
  //
  // 为何刻意不留“放不下就跳过”的兜底：兜底会**掩盖**折叠集合算错的事实——
  // 两套判据恰好得出同一结果时，改坏哪一套测试都是绿的（逆向验证实测）。
  // 单一判据 + 回归断言，错了才会红。
  std::vector<bool> folded(items_.size(), false);
  {
    std::size_t remaining = collapsed_;
    for (std::size_t i = items_.size(); i > 0 && remaining > 0U; --i) {
      const std::size_t index = i - 1U;
      if (items_[index].kind == Kind::Separator || items_[index].pinned) continue;
      folded[index] = true;
      --remaining;
    }
  }

  float x = rect.x + kPaddingX;
  const float right_limit =
      collapsed_ > 0U ? overflow_box_.x - kItemGap : rect.right() - kPaddingX;
  for (std::size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == Kind::Separator) {
      const float separator_x = x + kGroupGap - kItemGap * 0.5f;
      if (separator_x < right_limit) {
        rects_[i] = math::Rect{separator_x, rect.y + (rect.height - kSeparatorHeight) * 0.5f,
                               0.5f, kSeparatorHeight};
        x += kGroupGap * 2.0f;
      }
      continue;
    }
    if (items_[i].element == nullptr || folded[i]) continue;
    const float width = items_[i].element->measured_size().width;
    const float height = items_[i].element->measured_size().height;
    rects_[i] = math::Rect{x, rect.y + (rect.height - height) * 0.5f, width, height};
    items_[i].element->arrange(context, rects_[i]);
    x += width + kItemGap;
  }
}

void Toolbar::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Toolbar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();

  // 分隔线自绘（`rects_` 里那个 0.5px 宽的矩形）。
  for (std::size_t i = 0; i < items_.size() && i < rects_.size(); ++i) {
    if (items_[i].kind != Kind::Separator || rects_[i].is_empty()) continue;
    canvas.fill_rect(rects_[i], raster::Paint::solid(colors.border));
  }

  // 子按钮由宿主绘制（`Element::paint` 会走 children_）——但这里的条目**不是**
  // children_（工具条自己持有），所以要显式逐项绘制。
  for (std::size_t i = 0; i < items_.size() && i < rects_.size(); ++i) {
    if (items_[i].kind == Kind::Separator || rects_[i].is_empty()) continue;
    if (items_[i].element == nullptr) continue;
    items_[i].element->paint(context, canvas);
  }

  // 溢出按钮。
  if (!overflow_box_.is_empty()) {
    fill_round_rect(canvas, overflow_box_, kOverflowRadius,
                    hover_overflow_ ? colors.surface_hover : colors.surface);
    paint_outline(canvas, overflow_box_, kOverflowRadius, colors.border, 1.0f);
    const std::string label = std::format("{}{}", overflow_label_, collapsed_);
    const TextPort& port = text_port_of(context);
    const float width = port.measure_width(label, style_.font_size);
    const float top = centered_line_top(port, label, style_.font_size, overflow_box_.y,
                                        overflow_box_.height);
    port.draw(canvas, label, math::Point{overflow_box_.center().x - width * 0.5f, top},
              style_.font_size, colors.text_muted);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto Toolbar::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_overflow_ = false;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const bool over = !overflow_box_.is_empty() && overflow_box_.contains(event.position);
      if (over != hover_overflow_) {
        hover_overflow_ = over;
        mark_dirty();
      }
      break;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (!overflow_box_.is_empty() && overflow_box_.contains(event.position)) {
        show_overflow();
        event.handled = true;
        return true;
      }
      break;
    }
    default: break;
  }
  // 把事件转给可见条目（工具条的条目不是 children_，需要显式派发）。
  for (std::size_t i = 0; i < items_.size() && i < rects_.size(); ++i) {
    if (items_[i].element == nullptr || rects_[i].is_empty()) continue;
    if (items_[i].element->on_event(context, event)) return true;
  }
  return false;
}

auto Toolbar::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Toolbar::cursor_at(math::Point point) const noexcept -> MouseCursor {
  for (std::size_t i = 0; i < items_.size() && i < rects_.size(); ++i) {
    if (items_[i].element == nullptr || rects_[i].is_empty()) continue;
    if (rects_[i].contains(point)) return items_[i].element->cursor_at(point);
  }
  if (!overflow_box_.is_empty() && overflow_box_.contains(point)) return MouseCursor::Hand;
  return MouseCursor::Arrow;
}

auto Toolbar::semantics_text() const -> std::string {
  std::string out;
  for (const auto& entry : items_) {
    if (entry.kind == Kind::Separator || entry.element == nullptr) continue;
    if (!out.empty()) out += ", ";
    out += entry.element->semantics_text();
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Toolbar::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "items" || name == "text") {
    std::string out;
    for (const auto& entry : items_) {
      if (entry.kind == Kind::Separator || entry.element == nullptr) continue;
      if (!out.empty()) out += '|';
      out += entry.element->semantics_text();
    }
    return out;
  }
  if (name == "count" || name == "item_count") return std::to_string(items_.size());
  if (name == "collapsed") return std::to_string(collapsed_);
  if (name == "overflow_label") return overflow_label_;
  return std::nullopt;
}

auto Toolbar::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "overflow_label") {
    set_overflow_label(std::string(value));
    return true;
  }
  // `items` / `count` 只读：条目是**拥有所有权**的对象，不能靠字符串重建
  // （那会丢掉每个按钮的回调与状态）。
  return false;
}

auto Toolbar::property_names() const -> std::vector<std::string_view> {
  return {"overflow_label"};
}

auto Toolbar::readonly_property_names() const -> std::vector<std::string_view> {
  return {"items", "count", "collapsed"};
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::Toolbar);
