#include "st/ui/components/completion_popup.hpp"

#include <algorithm>
#include <cctype>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::text_port_of;

namespace {

/// 大小写不敏感的子串匹配（ASCII 折叠；非 ASCII 原样比较）。
///
/// 为什么是"子串"而不是"前缀"：LSP 的 `filterText` 常让 `wdt` 匹配 `width`
/// （模糊匹配），而子串能覆盖其中大部分常见情形；真正的模糊匹配留给 server
/// （它返回的候选本身就已是匹配结果）。组件内再做强过滤会**误杀** server 的意图。
[[nodiscard]] auto icontains(std::string_view haystack, std::string_view needle) -> bool {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  const auto fold = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  for (std::size_t at = 0; at + needle.size() <= haystack.size(); ++at) {
    bool matched = true;
    for (std::size_t offset = 0; offset < needle.size(); ++offset) {
      if (fold(haystack[at + offset]) != fold(needle[offset])) {
        matched = false;
        break;
      }
    }
    if (matched) return true;
  }
  return false;
}

}  // namespace

CompletionPopup::CompletionPopup() {
  style_.background = math::Color{0, 0, 0, 0};   // 自绘（不走通用底/边框）
  set_visible(false);
}

auto CompletionPopup::item(std::size_t index) const -> const CompletionItemView* {
  return index < items_.size() ? &items_[index] : nullptr;
}

void CompletionPopup::set_items(std::vector<CompletionItemView> items) {
  items_ = std::move(items);
  rebuild_visible();
  mark_layout_dirty();
  mark_dirty();
}

void CompletionPopup::set_anchor(math::Rect anchor) {
  if (anchor.x == anchor_.x && anchor.y == anchor_.y && anchor.width == anchor_.width) return;
  anchor_ = anchor;
  mark_layout_dirty();
  mark_dirty();
}

void CompletionPopup::set_detail(std::string detail) {
  if (detail == detail_) return;
  detail_ = std::move(detail);
  mark_layout_dirty();
  mark_dirty();
}

void CompletionPopup::rebuild_visible() {
  visible_items_.clear();
  visible_items_.reserve(items_.size());
  for (std::size_t index = 0; index < items_.size(); ++index) {
    const CompletionItemView& view = items_[index];
    const std::string_view key = view.filter_text.empty()
                                     ? std::string_view(view.label)
                                     : std::string_view(view.filter_text);
    if (icontains(key, filter_) || icontains(view.label, filter_)) visible_items_.push_back(index);
  }
  // 过滤后原选中项可能被过滤掉：重选第一个可见项（与"输入时选中项跟着走"的直觉一致）。
  clamp_selection();
}

void CompletionPopup::clamp_selection() {
  if (visible_items_.empty()) {
    selected_ = -1;
    return;
  }
  // ⚠ **不能只看"越界"**：过滤后原选中项常常**仍在范围内但已被滤掉**
  //（实测：候选 6 项、选中 width(0)，过滤 `height` 后 width 不在可见集合里，
  //  只是恰好 index 0 未越界——于是"看不见的项被选中"，按 Enter 插入意外内容）。
  // 正确判据是"选中项在不在**可见集合**里"：不在就选第一个可见项。
  if (selected_ < 0) {
    selected_ = static_cast<std::ptrdiff_t>(visible_items_.front());
    return;
  }
  const auto as_size = static_cast<std::size_t>(selected_);
  if (std::find(visible_items_.begin(), visible_items_.end(), as_size) == visible_items_.end()) {
    selected_ = static_cast<std::ptrdiff_t>(visible_items_.front());
  }
}

void CompletionPopup::set_selected_index(std::ptrdiff_t index) {
  const std::ptrdiff_t before = selected_;
  if (index < 0 || visible_items_.empty()) {
    selected_ = visible_items_.empty() ? -1 : 0;
  } else if (static_cast<std::size_t>(index) >= items_.size()) {
    selected_ = static_cast<std::ptrdiff_t>(items_.size() - 1);
  } else {
    selected_ = index;
  }
  if (selected_ != before) {
    mark_dirty();
    if (on_selection_changed && selected_ >= 0) {
      on_selection_changed(static_cast<std::size_t>(selected_));
    }
  }
}

void CompletionPopup::set_filter(std::string filter) {
  if (filter == filter_) return;
  const std::ptrdiff_t before = selected_;
  filter_ = std::move(filter);
  rebuild_visible();
  mark_dirty();
  // 过滤可能改变了选中项（原项被滤掉 → 归零）——如实通知，别让调用方以为没变。
  if (selected_ != before && on_selection_changed && selected_ >= 0) {
    on_selection_changed(static_cast<std::size_t>(selected_));
  }
}

auto CompletionPopup::selected_row() const -> std::ptrdiff_t {
  if (selected_ < 0) return -1;
  for (std::size_t row = 0; row < visible_items_.size(); ++row) {
    if (static_cast<std::ptrdiff_t>(visible_items_[row]) == selected_) {
      return static_cast<std::ptrdiff_t>(row);
    }
  }
  return -1;
}

void CompletionPopup::move_selection(std::ptrdiff_t delta) {
  if (visible_items_.empty()) return;
  const std::ptrdiff_t row = selected_row();
  if (row < 0) {
    set_selected_index(static_cast<std::ptrdiff_t>(visible_items_.front()));
    return;
  }
  std::ptrdiff_t next = row + delta;
  const auto total = static_cast<std::ptrdiff_t>(visible_items_.size());
  // **不环绕**：列表底部再按 ↓ 停在原地（环绕会让"一路按到底"变成回到顶部，
  // 手比眼快的人会误选）。补全列表短，停住比环绕更符合直觉。
  next = std::clamp(next, std::ptrdiff_t{0}, total - 1);
  set_selected_index(static_cast<std::ptrdiff_t>(visible_items_[static_cast<std::size_t>(next)]));
  // 选中项滚入可视区。
  const float row_top = static_cast<float>(next) * kRowHeight;
  const float view = std::max(0.0f, list_.height);
  if (row_top < scroll_y_) scroll_y_ = row_top;
  else if (row_top + kRowHeight > scroll_y_ + view) scroll_y_ = row_top + kRowHeight - view;
}

void CompletionPopup::accept() {
  if (selected_ < 0 || static_cast<std::size_t>(selected_) >= items_.size()) return;
  const CompletionItemView& view = items_[static_cast<std::size_t>(selected_)];
  const std::string insert = view.insert_text.empty() ? view.label : view.insert_text;
  if (on_accept) on_accept(insert);
  if (on_dismiss) on_dismiss(true);
}

auto CompletionPopup::row_rect(std::size_t row) const noexcept -> math::Rect {
  if (list_.is_empty() || row >= visible_items_.size()) return math::Rect{};
  return math::Rect{list_.x, list_.y + static_cast<float>(row) * kRowHeight - scroll_y_,
                    list_.width, kRowHeight};
}

void CompletionPopup::apply_theme(const Theme& theme) {
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.color = theme.colors().text;
  style_.font_size = theme.metrics().font_base;
}

void CompletionPopup::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  const float row_h = std::max(kRowHeight, port.line_height(metrics.font_sm) + 6.0f);
  float width = kMinWidth;
  for (const auto& view : items_) {
    const float label = port.measure_width(view.label, metrics.font_sm);
    const float detail = port.measure_width(view.detail, metrics.font_xs);
    width = std::max(width, label + detail + 56.0f);
  }
  if (!detail_.empty()) width += kDetailWidth;
  width = std::min(width, kMaxWidth + (!detail_.empty() ? kDetailWidth : 0.0f));
  const float rows = static_cast<float>(std::max<std::size_t>(visible_items_.size(), 1));
  const float height = std::min(rows * row_h + kPadding * 2.0f, kMaxHeight);
  // 受约束夹取（视口很窄时让位）。
  measured_ = math::Size{std::min(width, constraints.max_width), height};
}

void CompletionPopup::arrange(const RenderContext& context, math::Rect rect) {
  // 浮层自定位：忽略传入矩形（它是宿主满屏），按锚点在视口内定位。
  // 视口 = 传入矩形（`FillViewport` 宿主给的就是视口）。
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  const float row_h = std::max(kRowHeight, port.line_height(metrics.font_sm) + 6.0f);
  const float width = measured_.width;
  const float rows_height =
      std::min(static_cast<float>(std::max<std::size_t>(visible_items_.size(), 1)) * row_h + kPadding * 2.0f,
               kMaxHeight);
  const float height = rows_height;

  float x = anchor_.x;
  float y = anchor_.bottom() + 2.0f;
  // 右侧不够：贴右缘（不越界）。
  if (x + width > rect.right() - 4.0f) x = std::max(rect.x + 4.0f, rect.right() - width - 4.0f);
  // 下方不够：翻到锚点上方。
  if (y + height > rect.bottom() - 4.0f) y = std::max(rect.y + 4.0f, anchor_.y - height - 2.0f);
  bounds_ = math::Rect{x, y, width, height};

  const float detail_w = detail_.empty() ? 0.0f : std::min(kDetailWidth, width * 0.5f);
  list_ = math::Rect{bounds_.x + kPadding, bounds_.y + kPadding,
                     std::max(0.0f, bounds_.width - kPadding * 2.0f - detail_w), height - kPadding * 2.0f};
  detail_rect_ = detail_w > 0.0f
                     ? math::Rect{bounds_.right() - kPadding - detail_w, bounds_.y + kPadding,
                                  detail_w, height - kPadding * 2.0f}
                     : math::Rect{};
  // 选中项滚入可视区（候选集可能变过）。
  const std::ptrdiff_t row = selected_row();
  if (row >= 0) {
    const float row_top = static_cast<float>(row) * row_h;
    const float view = std::max(1.0f, list_.height);
    if (row_top < scroll_y_) scroll_y_ = row_top;
    else if (row_top + row_h > scroll_y_ + view) scroll_y_ = row_top + row_h - view;
    const float max_scroll = std::max(0.0f, static_cast<float>(visible_items_.size()) * row_h - view);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
  } else {
    scroll_y_ = 0.0f;
  }
  layout_dirty_ = false;
  (void)metrics;
}

void CompletionPopup::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = text_port_of(context);
  const float row_h = std::max(kRowHeight, port.line_height(metrics.font_sm) + 6.0f);

  // 卡片：阴影 + surface 底 + 描边（与菜单面板同源）。
  const Shadow shadow = shadow_lg(context.theme);
  canvas.draw_shadow(bounds_, metrics.radius_md, shadow.blur, shadow.color,
                     math::Point{shadow.offset_x, shadow.offset_y});
  fill_round_rect(canvas, bounds_, metrics.radius_md, colors.surface);
  raster::Path border;
  border.add_rounded_rect(bounds_, metrics.radius_md);
  canvas.stroke_path(border, raster::Paint::solid(colors.border_strong), metrics.border_width);

  // 候选行。
  const float max_scroll = std::max(0.0f, static_cast<float>(visible_items_.size()) * row_h - list_.height);
  (void)max_scroll;
  for (std::size_t row = 0; row < visible_items_.size(); ++row) {
    const math::Rect rect = row_rect(row);
    if (rect.is_empty()) continue;
    if (rect.bottom() < list_.y || rect.y > list_.bottom()) continue;   // 视口剔除
    const CompletionItemView& view = items_[visible_items_[row]];
    const bool active = static_cast<std::ptrdiff_t>(visible_items_[row]) == selected_;
    const bool hovered = static_cast<int>(row) == hover_row_;
    if (active) {
      fill_round_rect(canvas, rect.inset(math::Insets{2.0f, 2.0f, 2.0f, 2.0f}), metrics.radius_sm,
                      colors.primary_soft);
    } else if (hovered) {
      fill_round_rect(canvas, rect.inset(math::Insets{2.0f, 2.0f, 2.0f, 2.0f}), metrics.radius_sm,
                      colors.surface_alt);
    }
    float text_x = rect.x + 6.0f;
    // 徽标（类型指示：ƒ 函数 / ○ 变量 …）。
    if (!view.badge.empty()) {
      port.draw(canvas, view.badge, math::Point{text_x, rect.y + (rect.height - port.line_height(metrics.font_xs)) * 0.5f},
                metrics.font_xs, active ? colors.primary : colors.text_muted,
                text::FontRole::Monospace);
      text_x += port.measure_width(view.badge, metrics.font_xs) + 6.0f;
    }
    // 右侧 detail（类型/来源）——先算宽度，给主文本留位。
    const float detail_w = view.detail.empty()
                               ? 0.0f
                               : port.measure_width(view.detail, metrics.font_xs) + 8.0f;
    const float label_room = std::max(0.0f, rect.right() - text_x - detail_w - 6.0f);
    const std::string shown = port.ellipsize(view.label, metrics.font_sm, label_room);
    port.draw(canvas, shown,
              math::Point{text_x, rect.y + (rect.height - port.line_height(metrics.font_sm)) * 0.5f},
              metrics.font_sm, active ? colors.primary : colors.text);
    if (detail_w > 0.0f) {
      port.draw(canvas, view.detail,
                math::Point{rect.right() - kPadding - port.measure_width(view.detail, metrics.font_xs),
                            rect.y + (rect.height - port.line_height(metrics.font_xs)) * 0.5f},
                metrics.font_xs, colors.text_muted);
    }
  }

  // 详情面板（右侧，有则画）。
  if (!detail_rect_.is_empty()) {
    canvas.fill_rect(detail_rect_, raster::Paint::solid(colors.surface_alt), metrics.radius_sm);
    float y = detail_rect_.y + 6.0f;
    const float line_h = port.line_height(metrics.font_xs);
    std::size_t at = 0;
    while (at <= detail_.size() && y + line_h <= detail_rect_.bottom() - 2.0f) {
      const std::size_t next = detail_.find('\n', at);
      const std::string line =
          detail_.substr(at, next == std::string::npos ? std::string::npos : next - at);
      const std::string shown =
          port.ellipsize(line, metrics.font_xs, std::max(0.0f, detail_rect_.width - 12.0f));
      port.draw(canvas, shown, math::Point{detail_rect_.x + 6.0f, y}, metrics.font_xs,
                colors.text_muted, text::FontRole::Monospace);
      y += line_h;
      if (next == std::string::npos) break;
      at = next + 1;
    }
  }
}

auto CompletionPopup::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::KeyDown: {
      if (event.key == "ArrowDown") {
        move_selection(1);
        return true;
      }
      if (event.key == "ArrowUp") {
        move_selection(-1);
        return true;
      }
      if (event.key == "PageDown") {
        move_selection(8);
        return true;
      }
      if (event.key == "PageUp") {
        move_selection(-8);
        return true;
      }
      if (event.key == "Enter" || event.key == "Tab") {
        accept();
        return true;
      }
      if (event.key == "Escape" || event.key == "Esc") {
        if (on_dismiss) on_dismiss(false);
        return true;
      }
      return false;
    }
    case EventKind::MouseMove: {
      if (!bounds_.contains(event.position)) {
        if (hover_row_ != -1) {
          hover_row_ = -1;
          mark_dirty();
        }
        return true;   // 浮层吞掉移动（避免穿透到下层的 hover）
      }
      const auto row =
          static_cast<std::ptrdiff_t>((event.position.y - list_.y + scroll_y_) / kRowHeight);
      const int next = (row >= 0 && static_cast<std::size_t>(row) < visible_items_.size())
                           ? static_cast<int>(row)
                           : -1;
      if (next != hover_row_) {
        hover_row_ = next;
        mark_dirty();
      }
      return true;
    }
    case EventKind::MouseDown:
    case EventKind::Click: {
      if (!bounds_.contains(event.position)) {
        // 点弹层外面 = 取消（与菜单面板同一手感）。
        if (event.kind == EventKind::Click && on_dismiss) on_dismiss(false);
        return true;
      }
      if (event.kind != EventKind::Click) return true;
      const auto row =
          static_cast<std::ptrdiff_t>((event.position.y - list_.y + scroll_y_) / kRowHeight);
      if (row >= 0 && static_cast<std::size_t>(row) < visible_items_.size()) {
        set_selected_index(static_cast<std::ptrdiff_t>(visible_items_[static_cast<std::size_t>(row)]));
        accept();
      }
      return true;
    }
    case EventKind::Wheel: {
      const float row_h = kRowHeight;
      const float max_scroll =
          std::max(0.0f, static_cast<float>(visible_items_.size()) * row_h - list_.height);
      scroll_y_ = std::clamp(scroll_y_ + event.wheel_delta * 48.0f, 0.0f, max_scroll);
      mark_dirty();
      return true;
    }
    default:
      return false;
  }
}

auto CompletionPopup::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "items") return std::to_string(items_.size());
  if (name == "visible_items") return std::to_string(visible_items_.size());
  if (name == "selected") return std::to_string(selected_);
  if (name == "selected_label") {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= items_.size()) return std::string{};
    return items_[static_cast<std::size_t>(selected_)].label;
  }
  if (name == "filter") return filter_;
  if (name == "detail") return detail_;
  if (name == "labels") {
    std::string joined;
    for (const auto& view : items_) {
      if (!joined.empty()) joined.push_back('|');
      joined += view.label;
    }
    return joined;
  }
  if (name == "visible_labels") {
    std::string joined;
    for (const std::size_t index : visible_items_) {
      if (!joined.empty()) joined.push_back('|');
      joined += items_[index].label;
    }
    return joined;
  }
  return Element::get_property(name);
}

auto CompletionPopup::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "filter") {
    set_filter(std::string(value));
    return true;
  }
  if (name == "selected") {
    if (const auto parsed = st::parse_i64(value); parsed.has_value()) {
      set_selected_index(static_cast<std::ptrdiff_t>(*parsed));
      return true;
    }
    return false;
  }
  if (name == "detail") {
    set_detail(std::string(value));
    return true;
  }
  return Element::set_property(name, value);
}

auto CompletionPopup::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(), {"items", "visible_items", "selected", "selected_label", "filter",
                             "detail", "labels", "visible_labels"});
  return names;
}

auto CompletionPopup::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "accept") {
    accept();
    return true;
  }
  if (action == "dismiss" || action == "cancel") {
    if (on_dismiss) on_dismiss(false);
    return true;
  }
  if (action == "next") {
    move_selection(1);
    return true;
  }
  if (action == "prev") {
    move_selection(-1);
    return true;
  }
  // `select`：程序化选中第 index 项（**按 items_ 下标**，不是可见行号——
  // 自动化里"选第几项"指的是候选身份，可见行号会随过滤变化）。
  if (action == "select") {
    const auto parsed = st::parse_i64(argument);
    if (!parsed.has_value()) return false;
    set_selected_index(static_cast<std::ptrdiff_t>(*parsed));
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
