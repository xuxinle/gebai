#include "st/ui/components/pagination.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_focus_ring;
using components_internal::text_port_of;

namespace {

/// 省略号占位标记（`items()` 里用 0 表示）。
constexpr int kEllipsis = 0;

/// 页码格最小宽度：至少能放下 3 位数（`100`）——到 999 页之前宽度不变，
/// 翻页时页码条不会左右抖。
constexpr float kMinCellWidth = 34.0f;

}  // namespace

Pagination::Pagination() {
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Center;
  set_focusable(true);
}

// ————————————————————————————————————————————————————————————————————————————
// 数据与折叠规则
// ————————————————————————————————————————————————————————————————————————————

void Pagination::set_page(int page, bool notify) {
  const int clamped = std::clamp(page, 1, std::max(page_count_, 1));
  if (clamped == page_) return;
  page_ = clamped;
  mark_dirty();
  if (notify && on_change) on_change(page_);
}

void Pagination::set_page_count(int count) {
  const int clamped = std::max(count, 1);
  if (clamped == page_count_) return;
  page_count_ = clamped;
  if (page_ > page_count_) page_ = page_count_;   // 数据变化不是用户翻页 → 不派发 on_change
  if (page_ < 1) page_ = 1;
  mark_dirty();
}

void Pagination::set_sibling_count(int siblings) {
  const int clamped = std::max(siblings, 0);
  if (clamped == sibling_count_) return;
  sibling_count_ = clamped;
  mark_dirty();
}

void Pagination::set_show_edges(bool value) {
  if (show_edges_ == value) return;
  show_edges_ = value;
  mark_dirty();
}

auto Pagination::items() const -> std::vector<int> {
  // 折叠规则（唯一实现；绘制/命中/属性都取它）：
  //
  // - `n <= 2s+5` → 全部直出（页数少时折叠只会更难读）；
  // - 否则窗口固定 `2s+1` 个页码，**靠边时向中间撑开**而不是被夹窄：
  //   否则翻到首页时页码条突然变短、按钮位置集体跳一次，连点两下很容易点错。
  // - 空缺只有 1 个位置时直接画那个页码（`…` 与页码同宽，画 `…` 是白丢一个可点目标）。
  const int total = std::max(page_count_, 1);
  const int page = std::clamp(page_, 1, total);
  const int siblings = std::max(sibling_count_, 0);

  if (!show_edges_ || total <= 2 * siblings + 5) {
    std::vector<int> all;
    all.reserve(static_cast<std::size_t>(total));
    for (int index = 1; index <= total; ++index) all.push_back(index);
    return all;
  }

  const int window = 2 * siblings + 1;
  // 夹到 `[1, total]`（**不是** `[2, total-1]`）：首/末页本身就是窗口的一部分时才自然，
  // 夹到内侧会把「第 1 页」挤成「[2,3,4]」——多占一个位置却没多出有用信息。
  const int lo = std::clamp(page - siblings, 1, total - window + 1);
  const int hi = lo + window - 1;

  std::vector<int> out;
  // 窗口左端在 1 或 2 时不插 `…`：那些位置页码本来就连续（画 `…` 是白丢可点目标）。
  if (lo > 2) {
    out.push_back(1);
    out.push_back(kEllipsis);
  } else if (lo == 2) {
    out.push_back(1);
  }
  for (int index = lo; index <= hi; ++index) out.push_back(index);
  if (hi < total - 1) {
    out.push_back(kEllipsis);
    out.push_back(total);
  } else if (hi == total - 1) {
    out.push_back(total);
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Pagination::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = kRadius;
  style_.font_size = metrics.font_sm;
  style_.color = theme.colors().text;
  apply_text_overrides();
}

void Pagination::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;

  // 每个页码格至少 kCellSize，且要放得下自己的文字（三位数是常见的最大宽度）。
  float cell = kCellSize;
  for (const int item : items()) {
    if (item == kEllipsis) {
      cell = std::max(cell, 24.0f);
      continue;
    }
    cell = std::max(cell, port.measure_width(std::to_string(item), size) + 16.0f);
  }
  cell = std::max(cell, kMinCellWidth);
  // 「上一页/下一页」是两个图标格（同宽）。
  const float natural = cell * static_cast<float>(items().size() + 2) +
                        kGap * static_cast<float>(items().size() + 1);

  float width = style_.has_explicit_width() ? style_.width : natural;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void Pagination::relayout() const {
  slots_.clear();
  items_ = items();
  prev_ = math::Rect{};
  next_ = math::Rect{};
  if (bounds_.is_empty() || items_.empty()) return;

  // 格宽：按可用宽均分（受 `kCellSize` 下限保护）。横向排满时页码条与控件同宽，
  // 视觉上是一整条，而不是一段居中的小块。
  const std::size_t total_slots = items_.size() + 2U;
  const float gaps = kGap * static_cast<float>(total_slots - 1U);
  const float available = std::max(bounds_.width - gaps, 0.0f);
  const float cell = std::max(available / static_cast<float>(total_slots), 12.0f);
  const float used = cell * static_cast<float>(total_slots) + gaps;
  // 富余宽居中（显式设了更宽时不要让控件贴左）。
  float x = bounds_.x + std::max((bounds_.width - used) * 0.5f, 0.0f);
  const float y = bounds_.y + (bounds_.height - std::min(kHeight, bounds_.height)) * 0.5f;
  const float height = std::min(kHeight, bounds_.height);

  prev_ = math::Rect{x, y, cell, height};
  x += cell + kGap;

  slots_.reserve(items_.size());
  for (std::size_t slot = 0; slot < items_.size(); ++slot) {
    slots_.emplace_back(x, y, cell, height);
    x += cell + kGap;
  }
  next_ = math::Rect{x, y, cell, height};
}

void Pagination::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout();
}

auto Pagination::slot_rect(std::size_t slot) const -> math::Rect {
  relayout();
  return slot < slots_.size() ? slots_[slot] : math::Rect{};
}

auto Pagination::prev_rect() const -> math::Rect {
  relayout();
  return prev_;
}

auto Pagination::next_rect() const -> math::Rect {
  relayout();
  return next_;
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Pagination::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout();
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;
  const math::Color dim = enabled_ ? colors.text_muted : colors.text_faint;

  // 上一页 / 下一页：到了两端就**画成禁用**（不是隐藏——位置恒定，用户不会找不到）。
  const bool can_prev = enabled_ && page_ > 1;
  const bool can_next = enabled_ && page_ < page_count_;
  const auto paint_arrow = [&](math::Rect rect, const char* icon, bool usable, bool hovered) {
    if (rect.is_empty()) return;
    if (hovered && usable) fill_round_rect(canvas, rect, kRadius, colors.surface_hover);
    const math::Color ink = usable ? (hovered ? colors.text : colors.text_muted) : colors.text_faint;
    const float glyph = std::min(rect.width, rect.height) * 0.5f;
    const math::Rect box{rect.center().x - glyph * 0.5f, rect.center().y - glyph * 0.5f, glyph,
                         glyph};
    Icon::draw(canvas, icon, box, ink, Icon::effective_stroke(icon, glyph));
  };
  paint_arrow(prev_, "chevron-left", can_prev, hover_prev_);
  paint_arrow(next_, "chevron-right", can_next, hover_next_);

  for (std::size_t slot = 0; slot < slots_.size() && slot < items_.size(); ++slot) {
    const math::Rect rect = slots_[slot];
    const int item = items_[slot];
    const bool hovered = static_cast<int>(slot) == hover_slot_;
    if (item == kEllipsis) {
      // `…`：三个小圆点（与文字 `…` 的字面宽无关，几何与格宽对齐）。
      const float gap = 4.0f;
      const float start_x = rect.center().x - gap;
      for (int dot = 0; dot < 3; ++dot) {
        canvas.fill_circle(
            math::Point{start_x + static_cast<float>(dot) * gap, rect.center().y}, 1.5f,
            raster::Paint::solid(colors.text_faint));
      }
      continue;
    }
    const bool active = item == page_;
    if (active) {
      fill_round_rect(canvas, rect, kRadius, colors.primary);
    } else if (hovered && enabled_) {
      fill_round_rect(canvas, rect, kRadius, colors.surface_hover);
    }
    const std::string label = std::to_string(item);
    const float text_width = port.measure_width(label, size);
    const float text_x = rect.center().x - text_width * 0.5f;
    const float top = centered_line_top(port, label, size, rect.y, rect.height);
    port.draw(canvas, label, math::Point{text_x, top}, size,
              active ? colors.on_primary : (enabled_ ? colors.text : dim));
  }

  if (focused_) paint_focus_ring(context, canvas, bounds_, style_.radius);
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto Pagination::slot_at(math::Point point) const -> int {
  relayout();
  for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
    if (slots_[slot].contains(point)) return static_cast<int>(slot);
  }
  return -1;
}

auto Pagination::page_at(math::Point point) const -> int {
  const int slot = slot_at(point);
  if (slot < 0 || static_cast<std::size_t>(slot) >= items_.size()) return 0;
  return items_[static_cast<std::size_t>(slot)];
}

auto Pagination::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      relayout();
      const int slot = slot_at(event.position);
      const bool on_prev = prev_.contains(event.position) && page_ > 1;
      const bool on_next = next_.contains(event.position) && page_ < page_count_;
      if (slot != hover_slot_ || on_prev != hover_prev_ || on_next != hover_next_) {
        hover_slot_ = slot;
        hover_prev_ = on_prev;
        hover_next_ = on_next;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      relayout();
      if (prev_.contains(event.position)) {
        if (page_ <= 1) return false;
        set_page(page_ - 1, true);
        event.handled = true;
        return true;
      }
      if (next_.contains(event.position)) {
        if (page_ >= page_count_) return false;
        set_page(page_ + 1, true);
        event.handled = true;
        return true;
      }
      const int page = page_at(event.position);
      if (page <= 0) return false;   // 点 `…` 不翻页（它只是占位）
      if (page == page_) return false;
      set_page(page, true);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      if (key == "ArrowLeft" || key == "PageUp") {
        if (page_ <= 1) return false;
        set_page(page_ - 1, true);
        return true;
      }
      if (key == "ArrowRight" || key == "PageDown") {
        if (page_ >= page_count_) return false;
        set_page(page_ + 1, true);
        return true;
      }
      if (key == "Home") {
        if (page_ == 1) return false;
        set_page(1, true);
        return true;
      }
      if (key == "End") {
        if (page_ == page_count_) return false;
        set_page(page_count_, true);
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto Pagination::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto Pagination::cursor_at(math::Point point) const noexcept -> MouseCursor {
  relayout();
  if (prev_.contains(point) && page_ > 1) return MouseCursor::Hand;
  if (next_.contains(point) && page_ < page_count_) return MouseCursor::Hand;
  return page_at(point) > 0 ? MouseCursor::Hand : MouseCursor::Arrow;
}

auto Pagination::semantics_text() const -> std::string { return "分页"; }

auto Pagination::semantics_value() const -> std::string {
  return std::format("{}/{}", page_, page_count_);
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Pagination::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "page" || name == "value") return std::to_string(page_);
  if (name == "page_count" || name == "total") return std::to_string(page_count_);
  if (name == "sibling_count" || name == "siblings") return std::to_string(sibling_count_);
  if (name == "count") {
    // 实际渲染项数（含省略号与首末页）——诊断「折叠算得对不对」用。
    return std::to_string(items().size());
  }
  if (name == "items") {
    std::string out;
    const auto list = items();
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (index > 0) out += '|';
      out += list[index] == kEllipsis ? std::string("...") : std::to_string(list[index]);
    }
    return out;
  }
  if (name == "show_edges") return std::string(show_edges_ ? kFlagTrue : "false");
  return Element::get_property(name);
}

auto Pagination::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "page" || name == "value") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_page(static_cast<int>(*parsed));
    return true;
  }
  if (name == "page_count" || name == "total") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_page_count(static_cast<int>(*parsed));
    return true;
  }
  if (name == "sibling_count" || name == "siblings") {
    const auto parsed = st::parse_i64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_sibling_count(static_cast<int>(*parsed));
    return true;
  }
  if (name == "show_edges") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_show_edges(*parsed);
    return true;
  }
  return Element::set_property(name, value);
}

auto Pagination::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"page", "page_count", "sibling_count", "show_edges"});
}

auto Pagination::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"count", "items"});
}

auto Pagination::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "goto" || action == "set") {
    const auto parsed = st::parse_i64(st::trim(argument));
    if (!parsed.has_value()) return false;
    const int target = static_cast<int>(*parsed);
    if (target < 1 || target > page_count_ || target == page_) return false;
    set_page(target, true);
    return true;
  }
  if (action == "next" || action == "step_forward") {
    if (page_ >= page_count_) return false;
    set_page(page_ + 1, true);
    return true;
  }
  if (action == "prev" || action == "previous" || action == "step_backward") {
    if (page_ <= 1) return false;
    set_page(page_ - 1, true);
    return true;
  }
  if (action == "first") {
    if (page_ == 1) return false;
    set_page(1, true);
    return true;
  }
  if (action == "last") {
    if (page_ == page_count_) return false;
    set_page(page_count_, true);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
//
// 一行一个组件，展开后包含**静态登记**（本 .o 进产物即在 main 前完成注册）
// 与**导入锚点**（`import_components<T>()` 取它的地址，把本 .o 拉进产物）。
// 所以：真的用到组件 → 自动注册；要按名字建 → 另调 import。
ST_UI_COMPONENT_IMPL(::st::ui::Pagination);
