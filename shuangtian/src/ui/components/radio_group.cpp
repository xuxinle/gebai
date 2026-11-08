#include "st/ui/components/radio_group.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_outline;
using components_internal::text_port_of;

namespace {

constexpr float kIndicatorSize = 16.0f;   ///< 圆圈直径（px）
constexpr float kIndicatorGap = 8.0f;     ///< 圆圈与文本间距（px）
constexpr float kDotSize = 7.0f;          ///< 选中内点直径（px）

}  // namespace

RadioGroup::RadioGroup() = default;

// ————————————————————————————————————————————————————————————————————————————
// 选项与选中
// ————————————————————————————————————————————————————————————————————————————

void RadioGroup::set_options(std::vector<RadioOption> options) {
  // ⚠ **先取旧值，再换列表**。
  //
  // 反过来（先 `options_ = std::move(options)` 再取 `options_[index_]`）读到的是
  // **新列表同一下标**的值——重排后那个位置的选项已经换人了，于是「按值重定位」
  // 退化成「按下标保留」，选中会静默漂到别的选项上。
  // 实测代价：重排 `{甲,乙,丙} → {甲,丙,乙}` 后选中从「乙」漂成「丙」。
  const std::string previous =
      (index_ >= 0 && index_ < static_cast<int>(options_.size()))
          ? std::string(value_of(options_[static_cast<std::size_t>(index_)]))
          : std::string{};
  options_ = std::move(options);
  if (index_ >= 0) {
    int found = -1;
    for (std::size_t i = 0; i < options_.size(); ++i) {
      if (value_of(options_[i]) == previous) {
        found = static_cast<int>(i);
        break;
      }
    }
    index_ = found;   // 新列表里没有这个值了 → 清空选中（而不是保留错误的下标）
  }
  mark_layout_dirty();
}

void RadioGroup::set_labels(const std::vector<std::string>& labels) {
  std::vector<RadioOption> next;
  next.reserve(labels.size());
  for (const auto& label : labels) {
    RadioOption option{};
    option.label = label;
    option.value = label;   // 值与文本同一个：`value` 明确起稳定身份的活
    next.push_back(std::move(option));
  }
  set_options(std::move(next));
}

auto RadioGroup::value_of(const RadioOption& option) const -> std::string_view {
  return option.value.empty() ? std::string_view(option.label) : std::string_view(option.value);
}

auto RadioGroup::value() const -> const std::string& {
  static const std::string kEmpty{};
  if (index_ < 0 || index_ >= static_cast<int>(options_.size())) return kEmpty;
  const RadioOption& option = options_[static_cast<std::size_t>(index_)];
  return option.value.empty() ? option.label : option.value;
}

auto RadioGroup::set_value(std::string_view value) -> bool {
  for (std::size_t i = 0; i < options_.size(); ++i) {
    if (value_of(options_[i]) != value) continue;
    return set_index(static_cast<int>(i));
  }
  return false;
}

auto RadioGroup::set_index(int index) -> bool {
  if (index < 0 || index >= static_cast<int>(options_.size())) return false;
  if (index_ == index) return true;   // 同值不重复派发（避免调用方在回调里递归）
  index_ = index;
  mark_dirty();
  if (on_change) on_change(value());
  return true;
}

void RadioGroup::clear_selection() {
  if (index_ < 0) return;
  index_ = -1;
  mark_dirty();
  if (on_change) on_change(std::string_view{});
}

void RadioGroup::set_direction(Direction direction) {
  if (direction_ == direction) return;
  direction_ = direction;
  mark_layout_dirty();
}

void RadioGroup::set_wrap(bool wrap) {
  if (wrap_ == wrap) return;
  wrap_ = wrap;
  mark_layout_dirty();
}

void RadioGroup::move_selection(int delta) {
  if (options_.empty()) return;
  const auto total = static_cast<int>(options_.size());
  int next = index_;
  // 从当前（或边界）出发找下一个可选项；最多绕一圈。
  for (int step = 0; step < total; ++step) {
    next += delta;
    if (next < 0) next = total - 1;
    if (next >= total) next = 0;
    if (options_[static_cast<std::size_t>(next)].enabled) {
      set_index(next);
      return;
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void RadioGroup::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  font_size_ = metrics.font_sm;
  item_height_ = std::max(kIndicatorSize, metrics.font_sm * 1.5f);
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = font_size_;
  style_.color = theme.colors().text;
  apply_text_overrides();
  font_size_ = style_.font_size;
}

auto RadioGroup::item_width(const RenderContext& context, std::size_t index) const -> float {
  if (index >= options_.size()) return 0.0f;
  const TextPort& port = text_port_of(context);
  return kIndicatorSize + kIndicatorGap +
         port.measure_width(options_[index].label, font_size_);
}

void RadioGroup::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const float available = constraints.max_width < kUnbounded ? constraints.max_width : 1.0e9f;

  if (direction_ == Direction::Vertical) {
    float width = 0.0f;
    for (std::size_t i = 0; i < options_.size(); ++i) {
      width = std::max(width, item_width(context, i));
    }
    const float height = options_.empty()
                             ? 0.0f
                             : static_cast<float>(options_.size()) * item_height_ +
                                   static_cast<float>(options_.size() - 1U) * kGap;
    float final_width = width;
    if (style_.has_explicit_width()) final_width = style_.width;
    if (constraints.max_width < kUnbounded) final_width = std::min(final_width, constraints.max_width);
    measured_ = math::Size{std::clamp(final_width, style_.min_width, style_.max_width), height};
    return;
  }

  // 水平：一行排开；`wrap` 时超宽折行。
  float total = 0.0f;
  float row = 0.0f;
  int rows = options_.empty() ? 0 : 1;
  for (std::size_t i = 0; i < options_.size(); ++i) {
    const float item = item_width(context, i);
    if (row > 0.0f && row + kGap + item > available) {
      if (!wrap_) {
        // 不折行：总宽就是所有项之和（外层给裁剪或滚动）。
        total = std::max(total, row + kGap + item);
        row = row + kGap + item;
        continue;
      }
      total = std::max(total, row);
      row = item;
      ++rows;
    } else {
      row = row == 0.0f ? item : row + kGap + item;
    }
  }
  total = std::max(total, row);
  if (style_.has_explicit_width()) total = style_.width;
  if (constraints.max_width < kUnbounded) total = std::min(total, constraints.max_width);
  const float height = rows == 0 ? 0.0f
                                 : static_cast<float>(rows) * item_height_ +
                                       static_cast<float>(rows - 1) * kRowGap;
  (void)port;
  measured_ =
      math::Size{std::clamp(total, style_.min_width, style_.max_width),
                 std::clamp(height, style_.min_height, style_.max_height)};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void RadioGroup::relayout(const RenderContext& context, math::Rect rect) const {
  rects_.assign(options_.size(), math::Rect{});
  if (rect.is_empty() || options_.empty()) return;

  if (direction_ == Direction::Vertical) {
    float y = rect.y;
    for (std::size_t i = 0; i < options_.size(); ++i) {
      const float width = std::min(item_width(context, i), rect.width);
      rects_[i] = math::Rect{rect.x, y, width, item_height_};
      y += item_height_ + kGap;
    }
    return;
  }

  float x = rect.x;
  float y = rect.y;
  for (std::size_t i = 0; i < options_.size(); ++i) {
    const float width = item_width(context, i);
    if (x > rect.x && wrap_ && x + width > rect.right()) {
      x = rect.x;
      y += item_height_ + kRowGap;
    }
    rects_[i] = math::Rect{x, y, width, item_height_};
    x += width + kGap;
  }
}

void RadioGroup::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

auto RadioGroup::option_rect(std::size_t index) const -> math::Rect {
  return index < rects_.size() ? rects_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void RadioGroup::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || options_.empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  for (std::size_t i = 0; i < options_.size() && i < rects_.size(); ++i) {
    const math::Rect item = rects_[i];
    if (item.is_empty()) continue;
    // 视口剔除（选项多时省一大块绘制）。
    if (item.bottom() < bounds_.y || item.y > bounds_.bottom()) continue;
    const bool selected = static_cast<int>(i) == index_;
    const bool item_enabled = enabled_ && options_[i].enabled;
    const bool item_hover = static_cast<int>(i) == hover_index_ && item_enabled;

    const float indicator_y = item.y + (item.height - kIndicatorSize) * 0.5f;
    const math::Rect indicator{item.x, indicator_y, kIndicatorSize, kIndicatorSize};

    // 圆圈：未选中描边、选中用主色描边 + 内点。
    const math::Color ring = selected ? colors.primary
                             : item_hover ? colors.text_muted
                                          : colors.border;
    fill_round_rect(canvas, indicator, kIndicatorSize * 0.5f, colors.surface);
    if (selected || item_hover) {
      paint_outline(canvas, indicator, kIndicatorSize * 0.5f, ring, selected ? 2.0f : 1.0f);
    } else {
      paint_outline(canvas, indicator, kIndicatorSize * 0.5f, ring, 1.0f);
    }
    if (selected) {
      const math::Rect dot{indicator.center().x - kDotSize * 0.5f,
                           indicator.center().y - kDotSize * 0.5f, kDotSize, kDotSize};
      fill_round_rect(canvas, dot, kDotSize * 0.5f, colors.primary);
    }

    // 文本
    const float text_x = indicator.right() + kIndicatorGap;
    const float text_width = std::max(item.right() - text_x, 0.0f);
    if (text_width <= 0.0f) continue;
    const std::string shown = port.ellipsize(options_[i].label, font_size_, text_width);
    const float top = centered_line_top(port, shown, font_size_, item.y, item.height);
    port.draw(canvas, shown, math::Point{text_x, top}, font_size_,
              item_enabled ? colors.text : colors.text_faint);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void RadioGroup::activate() {
  if (!enabled_ || index_ < 0) return;
  if (on_change) on_change(value());
}

auto RadioGroup::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_index_ = -1;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      int found = -1;
      for (std::size_t i = 0; i < rects_.size(); ++i) {
        if (rects_[i].contains(event.position)) {
          found = static_cast<int>(i);
          break;
        }
      }
      if (found != hover_index_) {
        hover_index_ = found;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      for (std::size_t i = 0; i < rects_.size(); ++i) {
        if (!rects_[i].contains(event.position)) continue;
        if (!options_[i].enabled) {
          event.handled = true;   // 禁用项吃掉点击（不穿透到下层）
          return true;
        }
        set_index(static_cast<int>(i));
        event.handled = true;
        return true;
      }
      return false;
    }
    case EventKind::KeyDown: {
      // 单选组的键盘语义：方向键**移动并选中**，`Home`/`End` 到首尾。
      if (event.key == "ArrowDown" || (event.key == "ArrowRight" && direction_ == Direction::Vertical)) {
        move_selection(1);
        return true;
      }
      if (event.key == "ArrowUp" || (event.key == "ArrowLeft" && direction_ == Direction::Vertical)) {
        move_selection(-1);
        return true;
      }
      if (direction_ == Direction::Horizontal && event.key == "ArrowRight") {
        move_selection(1);
        return true;
      }
      if (direction_ == Direction::Horizontal && event.key == "ArrowLeft") {
        move_selection(-1);
        return true;
      }
      if (event.key == "Home") {
        for (std::size_t i = 0; i < options_.size(); ++i) {
          if (!options_[i].enabled) continue;
          set_index(static_cast<int>(i));
          break;
        }
        return true;
      }
      if (event.key == "End") {
        for (std::size_t i = options_.size(); i > 0; --i) {
          if (!options_[i - 1U].enabled) continue;
          set_index(static_cast<int>(i - 1U));
          break;
        }
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto RadioGroup::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto RadioGroup::cursor_at(math::Point point) const noexcept -> MouseCursor {
  for (const auto& rect : rects_) {
    if (rect.contains(point)) return enabled_ ? MouseCursor::Hand : MouseCursor::Arrow;
  }
  return MouseCursor::Arrow;
}

auto RadioGroup::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t i = 0; i < options_.size(); ++i) {
    if (i > 0) out += ", ";
    out += options_[i].label;
  }
  return out;
}

auto RadioGroup::semantics_value() const -> std::string { return value(); }

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto RadioGroup::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "items" || name == "options") {
    std::string out;
    for (std::size_t i = 0; i < options_.size(); ++i) {
      if (i > 0) out += '|';
      out += options_[i].label;
    }
    return out;
  }
  if (name == "value") return value();
  if (name == "index") return std::to_string(index_);
  if (name == "direction") {
    return std::string(direction_ == Direction::Horizontal ? "horizontal" : "vertical");
  }
  if (name == "wrap") return std::string(wrap_ ? kFlagTrue : "false");
  if (name == "count") return std::to_string(options_.size());
  return std::nullopt;
}

auto RadioGroup::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "items" || name == "options") {
    std::vector<std::string> labels;
    for (const auto& part : st::split(value, '|')) {
      labels.emplace_back(st::trim(part));
    }
    set_labels(labels);
    return true;
  }
  if (name == "value") {
    // 设为**不存在的值**：清空选中（比“静默不动”更符合“我要求它成为这个值”的意图）。
    const std::string target(st::trim(value));
    if (target.empty()) {
      clear_selection();
      return true;
    }
    if (set_value(target)) return true;
    clear_selection();
    return true;
  }
  if (name == "index") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    const auto target = static_cast<int>(*parsed);
    if (target < 0) {
      clear_selection();
      return true;
    }
    return set_index(target);
  }
  if (name == "direction") {
    const std::string text(st::trim(value));
    if (text == "horizontal" || text == "row") set_direction(Direction::Horizontal);
    else if (text == "vertical" || text == "column") set_direction(Direction::Vertical);
    else return false;
    return true;
  }
  if (name == "wrap") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_wrap(*parsed);
    return true;
  }
  return false;
}

auto RadioGroup::property_names() const -> std::vector<std::string_view> {
  return {"items", "value", "index", "direction", "wrap"};
}

auto RadioGroup::readonly_property_names() const -> std::vector<std::string_view> {
  return {"count"};
}

auto RadioGroup::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "select" || action == "choose") {
    const std::string target(st::trim(argument));
    if (target.empty()) return false;
    if (set_value(target)) return true;
    // 也接受索引
    const auto parsed = st::parse_i64(target);
    if (parsed.has_value()) return set_index(static_cast<int>(*parsed));
    return false;
  }
  if (action == "clear") {
    clear_selection();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::RadioGroup);
