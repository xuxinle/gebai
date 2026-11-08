#include "st/ui/components/breadcrumb.hpp"

#include <algorithm>
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

using components_internal::draw_line;
using components_internal::fill_round_rect;
using components_internal::text_port_of;

Breadcrumb::Breadcrumb() {
  // 自绘内容全在 paint_content（外壳透明，靠分段胶囊与分隔符构成形状）。
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.direction = FlexDirection::Row;
  style_.align_items = Align::Center;
  set_focusable(true);
}

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void Breadcrumb::set_segments(std::vector<Segment> segments) {
  segments_ = std::move(segments);
  for (Segment& segment : segments_) {
    if (segment.value.empty()) segment.value = segment.label;
  }
  widths_valid_ = false;
  placed_.clear();
  ellipsis_ = math::Rect{};
  collapsed_ = false;
  hover_index_ = -1;
  mark_layout_dirty();
}

void Breadcrumb::add_segment(std::string label, std::string value) {
  Segment segment{};
  segment.label = std::move(label);
  segment.value = value.empty() ? segment.label : std::move(value);
  segments_.push_back(std::move(segment));
  widths_valid_ = false;
  mark_layout_dirty();
}

void Breadcrumb::clear_segments() {
  if (segments_.empty()) return;
  segments_.clear();
  text_widths_.clear();
  segment_widths_.clear();
  placed_.clear();
  ellipsis_ = math::Rect{};
  collapsed_ = false;
  hover_index_ = -1;
  widths_valid_ = false;
  mark_layout_dirty();
}

auto Breadcrumb::segment_label(std::size_t index) const -> std::string_view {
  return index < segments_.size() ? std::string_view(segments_[index].label) : std::string_view{};
}

auto Breadcrumb::current() const -> std::string_view {
  return segments_.empty() ? std::string_view{} : std::string_view(segments_.back().label);
}

void Breadcrumb::set_last_clickable(bool value) {
  if (last_clickable_ == value) return;
  last_clickable_ = value;
  mark_dirty();
}

void Breadcrumb::set_separator(std::string icon_name) {
  if (separator_ == icon_name) return;
  separator_ = std::move(icon_name);
  // 图标名换了不影响占位宽（分隔符槽是固定宽），只影响绘制。
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void Breadcrumb::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = metrics.radius_sm;
  style_.font_size = metrics.font_sm;
  style_.color = theme.colors().text;
  apply_text_overrides();
  // 字号可能被显式覆盖 → 文本宽度缓存作废（缓存键必须覆盖所有会让结果变化的输入）。
  widths_valid_ = false;
}

void Breadcrumb::rebuild_widths(const RenderContext& context) const {
  if (widths_valid_) return;
  const TextPort& port = text_port_of(context);
  const float size = style_.font_size;
  text_widths_.assign(segments_.size(), 0.0f);
  segment_widths_.assign(segments_.size(), 0.0f);
  for (std::size_t index = 0; index < segments_.size(); ++index) {
    const Segment& segment = segments_[index];
    float width = port.measure_width(segment.label, size);
    text_widths_[index] = width;
    width += kSegmentPaddingX * 2.0f;
    if (!segment.icon.empty()) width += kIconSize + kIconGap;
    segment_widths_[index] = width;
  }
  widths_valid_ = true;
  placed_.clear();   // 宽度变了 → 落位作废
}

void Breadcrumb::measure(const RenderContext& context, const Constraints& constraints) {
  rebuild_widths(context);
  float natural = 0.0f;
  for (std::size_t index = 0; index < segment_widths_.size(); ++index) {
    natural += segment_widths_[index];
    if (index + 1 < segment_widths_.size()) natural += separator_slot();
  }

  float width = style_.has_explicit_width() ? style_.width : natural;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : kHeight;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);

  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位（折叠几何的唯一来源）
// ————————————————————————————————————————————————————————————————————————————

void Breadcrumb::relayout() const {
  placed_.clear();
  ellipsis_ = math::Rect{};
  collapsed_ = false;
  if (segments_.empty() || segment_widths_.size() != segments_.size() || bounds_.is_empty()) {
    return;
  }

  const std::size_t count = segments_.size();
  float natural = 0.0f;
  for (std::size_t index = 0; index < count; ++index) {
    natural += segment_widths_[index];
    if (index + 1 < count) natural += separator_slot();
  }

  // 能全放就全放——**绝不为了“好看”而提前折叠**。
  std::vector<std::size_t> shown;
  if (natural <= bounds_.width) {
    shown.reserve(count);
    for (std::size_t index = 0; index < count; ++index) shown.push_back(index);
  } else if (count == 1U) {
    shown.push_back(0U);   // 单段再折叠就没内容可看了
  } else {
    // 折叠：保留首段 + 从末尾往前装，中间插 `…`。
    // 从末尾往前装的理由：路径导航里「当前页的父级」比「路径中段」好点得多。
    collapsed_ = true;
    const float sep = separator_slot();
    // 已占用：首段 + 它后面的分隔符 + `…` + `…` 后面的分隔符。
    float used = segment_widths_[0] + sep + kEllipsisWidth + sep;
    // **末段（当前页）永远在场**：它是导航的锚点，折叠掉等于控件只说“你在某处”。
    // 宽度实在不够时宁可把它截短，也不能让它消失。
    std::vector<std::size_t> tail{count - 1U};
    used += segment_widths_[count - 1U];
    for (std::size_t index = count - 1U; index-- > 1U;) {
      // 非末段后面要跟一个分隔符。
      const float cost = segment_widths_[index] + sep;
      if (used + cost > bounds_.width) break;
      used += cost;
      tail.push_back(index);
    }
    std::reverse(tail.begin(), tail.end());
    shown.push_back(0U);
    shown.insert(shown.end(), tail.begin(), tail.end());
  }

  // 从首段开始依次落位；折叠时首段之后的第一个空位留给 `…`。
  const float center_y = bounds_.center().y;
  float x = bounds_.x;
  bool ellipsis_done = !collapsed_;
  for (std::size_t slot = 0; slot < shown.size(); ++slot) {
    const std::size_t index = shown[slot];
    if (!ellipsis_done && index != 0U) {
      ellipsis_ = math::Rect{x, center_y - kSeparatorGlyph * 0.5f, kEllipsisWidth, kSeparatorGlyph};
      x += kEllipsisWidth + separator_slot();
      ellipsis_done = true;
    }
    float width = segment_widths_[index];
    // 最后一段吃掉剩余宽（避免浮点累加导致贴不满右缘而留一道缝）。
    if (slot + 1 == shown.size()) width = std::max(0.0f, bounds_.right() - x);
    const math::Rect rect{x, bounds_.y + (bounds_.height - kHeight) * 0.5f, width,
                          std::min(kHeight, bounds_.height)};
    placed_.push_back(Placed{index, rect});
    x += segment_widths_[index];
    if (index + 1 < count) x += separator_slot();
  }

  // 折叠时 `…` 若没机会落位（宽度极端不足），把它压到首段后面，保证可见——
  // 静默不画 `…` 会让「明明还有更多层」这件事从画面上消失。
  if (!ellipsis_done && !placed_.empty()) {
    const math::Rect first = placed_.front().rect;
    const float left = std::min(first.right(), bounds_.right() - kEllipsisWidth);
    ellipsis_ = math::Rect{std::max(left, bounds_.x), center_y - kSeparatorGlyph * 0.5f,
                           kEllipsisWidth, kSeparatorGlyph};
  }
}

void Breadcrumb::arrange(const RenderContext& context, math::Rect rect) {
  rebuild_widths(context);
  Element::arrange(context, rect);
  relayout();
}

auto Breadcrumb::segment_rect(std::size_t index) const -> math::Rect {
  relayout();
  for (const Placed& entry : placed_) {
    if (entry.index == index) return entry.rect;
  }
  return math::Rect{};
}

auto Breadcrumb::visible_indices() const -> std::vector<std::size_t> {
  relayout();
  std::vector<std::size_t> out;
  out.reserve(placed_.size());
  for (const Placed& entry : placed_) out.push_back(entry.index);
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void Breadcrumb::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout();
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float size = style_.font_size;

  // 分隔符按**声明顺序**画在两个可见段之间（用 placed_ 的相邻关系，不按序号推——
  // 折叠后相邻的两段序号并不连续）。
  for (std::size_t slot = 0; slot < placed_.size(); ++slot) {
    const Placed& entry = placed_[slot];
    const std::size_t index = entry.index;
    const bool is_last = index + 1 == segments_.size();
    const bool hovered = static_cast<int>(index) == hover_index_ && !is_last;
    // 悬停胶囊：只在可点的非末段上画（末段是「当前位置」，视觉上不该像按钮）。
    if (hovered) {
      fill_round_rect(canvas, entry.rect, style_.radius, colors.surface_hover);
    }

    float text_x = entry.rect.x + kSegmentPaddingX;
    const Segment& segment = segments_[index];
    if (!segment.icon.empty()) {
      const math::Rect icon_box{text_x, entry.rect.center().y - kIconSize * 0.5f, kIconSize,
                                kIconSize};
      const math::Color ink = is_last ? colors.text : (hovered ? colors.text : colors.text_muted);
      Icon::draw(canvas, segment.icon, icon_box, ink, Icon::effective_stroke(segment.icon, kIconSize));
      text_x += kIconSize + kIconGap;
    }
    const math::Rect text_box{text_x, entry.rect.y,
                              std::max(0.0f, entry.rect.right() - kSegmentPaddingX - text_x),
                              entry.rect.height};
    const math::Color ink = is_last ? colors.text : (hovered ? colors.text : colors.text_muted);
    draw_line(context, canvas, segment.label, text_box, size, ink);

    // 分隔符：画在本段右侧的槽里（末段右侧没有）。
    if (!is_last) {
      const float gap_start = entry.rect.right();
      const math::Rect glyph{gap_start + kSeparatorGap,
                             entry.rect.center().y - kSeparatorGlyph * 0.5f, kSeparatorGlyph,
                             kSeparatorGlyph};
      Icon::draw(canvas, separator_, glyph, colors.text_faint,
                 Icon::effective_stroke(separator_, kSeparatorGlyph));
    }
  }

  // 折叠占位 `…`：用三个小圆点自绘（文本 `…` 的字面宽与占位宽对不齐，
  // 而这里的几何要与命中一致）。
  if (!ellipsis_.is_empty()) {
    const float radius = 1.5f;
    const float gap = 5.0f;
    const float start_x = ellipsis_.center().x - gap;
    for (int dot = 0; dot < 3; ++dot) {
      canvas.fill_circle(math::Point{start_x + static_cast<float>(dot) * gap,
                                     ellipsis_.center().y},
                         radius, raster::Paint::solid(colors.text_faint));
    }
  }

  (void)metrics;
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto Breadcrumb::segment_at(math::Point point) const -> std::optional<std::size_t> {
  for (const Placed& entry : placed_) {
    if (entry.rect.contains(point)) return entry.index;
  }
  return std::nullopt;
}

void Breadcrumb::activate_segment(std::size_t index) {
  if (index >= segments_.size()) return;
  if (!activatable(index)) return;   // 末段默认是「当前位置」
  if (on_click) on_click(index);
  if (on_navigate) on_navigate(segments_[index].value);
}

auto Breadcrumb::activatable(std::size_t index) const noexcept -> bool {
  if (index >= segments_.size()) return false;
  const bool is_last = index + 1 == segments_.size();
  return !is_last || last_clickable_;
}

auto Breadcrumb::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const auto hit = segment_at(event.position);
      const int next = hit.has_value() ? static_cast<int>(*hit) : -1;
      if (next != hover_index_) {
        hover_index_ = next;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      // 一次按放只来一条点击族事件 → 三种**同等**处理（只认 Click 会让连点丢事件）。
      const auto hit = segment_at(event.position);
      if (!hit.has_value()) return false;
      // 不可激活的段（末段默认）**不处理事件**：返回 true 会让冒泡链断在这里，
      // 外层可点容器的点击就永远到不了（用户看到的是“点了壳子没反应”）。
      if (!activatable(*hit)) return false;
      activate_segment(*hit);
      event.handled = true;
      return true;
    }
    default: return false;
  }
}

auto Breadcrumb::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto Breadcrumb::cursor_at(math::Point point) const noexcept -> MouseCursor {
  const auto hit = segment_at(point);
  if (!hit.has_value()) return MouseCursor::Arrow;
  return activatable(*hit) ? MouseCursor::Hand : MouseCursor::Arrow;
}

auto Breadcrumb::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < segments_.size(); ++index) {
    if (index > 0) out += " > ";
    out += segments_[index].label;
  }
  return out;
}

auto Breadcrumb::semantics_value() const -> std::string { return std::string(current()); }

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto Breadcrumb::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "segments" || name == "options" || name == "items") {
    std::string out;
    for (std::size_t index = 0; index < segments_.size(); ++index) {
      if (index > 0) out += '|';
      out += segments_[index].label;
    }
    return out;
  }
  if (name == "current" || name == "text") return std::string(current());
  if (name == "count") return std::to_string(segments_.size());
  if (name == "separator") return separator_;
  if (name == "last_clickable") return std::string(last_clickable_ ? kFlagTrue : "false");
  return std::nullopt;
}

auto Breadcrumb::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "segments" || name == "options" || name == "items") {
    std::vector<Segment> next;
    for (const std::string_view part : st::split(value, '|')) {
      Segment segment{};
      segment.label = std::string(st::trim(part));
      segment.value = segment.label;
      next.push_back(std::move(segment));
    }
    set_segments(std::move(next));
    return true;
  }
  if (name == "current" || name == "text") {
    // 写「当前位置」= 替换末段标签（不增不减段）。
    if (segments_.empty()) {
      add_segment(std::string(value));
      return true;
    }
    segments_.back().label = std::string(value);
    segments_.back().value = segments_.back().label;
    widths_valid_ = false;
    mark_layout_dirty();
    return true;
  }
  if (name == "separator") {
    set_separator(std::string(value));
    return true;
  }
  if (name == "last_clickable") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_last_clickable(*parsed);
    return true;
  }
  return false;
}

auto Breadcrumb::property_names() const -> std::vector<std::string_view> {
  return {"segments", "current", "separator", "last_clickable"};
}

auto Breadcrumb::readonly_property_names() const -> std::vector<std::string_view> {
  return {"count"};
}

auto Breadcrumb::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "goto_crumb" || action == "goto" || action == "crumb") {
    // 参数可以是段序号，也可以是业务值/标签（按名找更稳：序号会随路径漂移）。
    if (const auto index = st::parse_u64(st::trim(argument)); index.has_value()) {
      if (*index >= segments_.size()) return false;
      activate_segment(static_cast<std::size_t>(*index));
      return true;
    }
    const std::string_view wanted = st::trim(argument);
    for (std::size_t index = 0; index < segments_.size(); ++index) {
      if (segments_[index].value == wanted || segments_[index].label == wanted) {
        activate_segment(index);
        return true;
      }
    }
    return false;
  }
  if (action == "back" || action == "up") {
    // 上溯一层：激活倒数第二段（末段是当前位置）。
    if (segments_.size() < 2U) return false;
    activate_segment(segments_.size() - 2U);
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
ST_UI_COMPONENT_IMPL(::st::ui::Breadcrumb);
