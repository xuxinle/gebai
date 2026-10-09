#include "st/ui/components/tree.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::draw_line;
using components_internal::text_port_of;

namespace {

constexpr float kTextInset{14.0f};  ///< 行文本左边距（与 List 同口径）

}  // namespace

void Tree::set_row_height(float height) noexcept {
  if (height <= 0.0f) return;   // 非正值忽略：行高 0 会让命中/视口计算全失效
  if (height == row_height_) return;
  row_height_ = height;
  mark_layout_dirty();
  mark_dirty();
}

Tree::Tree() {
  style_.radius = 0.0f;
  set_focusable(true);
  set_hover_effect(HoverEffect{.enabled = true,
                               .background = true,
                               .border = false,
                               .lift = false,
                               .glow = false,
                               .cursor = true});
}

void Tree::sync_nodes(const std::vector<TreeNode>& nodes) {
  rows_.clear();
  rows_.reserve(nodes.size());
  for (const auto& node : nodes) rows_.push_back(Row{node});
  // 选中态跟 key 走：被移除的 key 如实清空（不静默挪到别的行上）
  if (!selected_key_.empty() && !index_of_key(selected_key_).has_value()) {
    selected_key_.clear();
  }
  mark_layout_dirty();
}

void Tree::clear_nodes() {
  rows_.clear();
  selected_key_.clear();
  hover_index_ = -1;
  mark_layout_dirty();
}

auto Tree::node_at(std::size_t index) const -> const TreeNode* {
  return index < rows_.size() ? &rows_[index].data : nullptr;
}

auto Tree::index_of_key(std::string_view key) const -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    if (rows_[index].data.key == key) return index;
  }
  return std::nullopt;
}

auto Tree::selected_index() const noexcept -> std::size_t {
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    if (rows_[index].data.key == selected_key_) return index;
  }
  return kNoSelection;
}

void Tree::select_key(std::string_view key, bool notify) {
  const bool valid = index_of_key(key).has_value();
  selected_key_ = valid ? std::string(key) : std::string{};
  mark_dirty();
  if (notify && on_select && valid) on_select(selected_key_);
}

auto Tree::row_rect(std::size_t index) const -> math::Rect {
  if (index >= rows_.size() || bounds_.is_empty()) return {};
  const auto row = static_cast<float>(index);
  return math::Rect{bounds_.x, bounds_.y + row * row_height_, bounds_.width, row_height_};
}

auto Tree::node_rect(std::string_view key) const -> math::Rect {
  const auto index = index_of_key(key);
  if (!index.has_value()) return {};
  return row_rect(*index);
}

auto Tree::node_indent(std::string_view key) const -> float {
  const auto index = index_of_key(key);
  if (!index.has_value()) return 0.0f;
  return static_cast<float>(rows_[*index].data.depth) * kIndentStep;
}

auto Tree::row_index_at(math::Point point) const -> std::optional<std::size_t> {
  if (bounds_.is_empty() || !bounds_.contains(point)) return std::nullopt;
  const float local_y = point.y - bounds_.y;
  if (local_y < 0.0f || local_y >= static_cast<float>(rows_.size()) * row_height_) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(local_y / row_height_);
  if (index >= rows_.size()) return std::nullopt;
  return index;
}

void Tree::activate_row(std::size_t index) {
  const TreeNode* node = node_at(index);
  if (node == nullptr) return;
  if (node->is_dir) {
    toggle_row(index);
    return;
  }
  select_key(node->key, true);
}

void Tree::toggle_row(std::size_t index) {
  const TreeNode* node = node_at(index);
  if (node == nullptr || !node->is_dir) return;
  if (on_toggle) on_toggle(node->key, !node->expanded);
  mark_dirty();
}

void Tree::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = theme.colors().surface;
  style_.radius = metrics.radius_sm;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
}

void Tree::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  float width = 0.0f;
  for (const auto& row : rows_) {
    const float indent = static_cast<float>(row.data.depth) * kIndentStep;
    const float chevron = row.data.is_dir ? kChevronSize + metrics.space_xs : 0.0f;
    const float need = indent + chevron + kTextInset + port.measure_width(row.data.label, style_.font_size)
                       + kTextInset;
    width = std::max(width, need);
  }
  width = std::max(width, 80.0f);
  width = std::clamp(width, style_.min_width, style_.max_width);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);

  float height = static_cast<float>(rows_.size()) * row_height_ + style_.padding.vertical();
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Tree::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  for (std::size_t index = 0; index < rows_.size(); ++index) {
    const TreeNode& node = rows_[index].data;
    const math::Rect row = row_rect(index);
    if (row.is_empty()) continue;
    const bool selected = node.key == selected_key_;

    if (selected) {
      canvas.fill_rect(row, raster::Paint::solid(colors.primary_soft), metrics.radius_sm);
      const float bar_height = std::max(0.0f, row.height - metrics.space_lg);
      const math::Rect indicator{row.x + 1.0f, row.y + (row.height - bar_height) * 0.5f,
                                 kIndicatorWidth, bar_height};
      canvas.fill_rect(indicator, raster::Paint::solid(colors.primary), kIndicatorWidth * 0.5f);
    } else if (hover_index_ == static_cast<int>(index) && enabled()) {
      canvas.fill_rect(row, raster::Paint::solid(colors.surface_alt), metrics.radius_sm);
    }

    const float indent = static_cast<float>(node.depth) * kIndentStep;
    float cursor_x = row.x + indent + kTextInset;

    // 展开指示（目录行）：收起 chevron-right、展开 chevron-down
    if (node.is_dir) {
      const math::Rect chevron{cursor_x, row.y + (row.height - kChevronSize) * 0.5f,
                               kChevronSize, kChevronSize};
      Icon::draw(canvas, node.expanded ? "chevron-down" : "chevron-right", chevron,
                 colors.text_muted);
      cursor_x += kChevronSize + metrics.space_xs;
    }

    const math::Color label_color = selected ? colors.primary : colors.text;
    const math::Rect text_box{cursor_x, row.y, row.right() - kTextInset - cursor_x, row.height};
    draw_line(context, canvas, node.label, text_box, style_.font_size, label_color);
  }
}

auto Tree::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::Click:
    case EventKind::DoubleClick: {
      const auto index = row_index_at(event.position);
      if (!index.has_value()) return false;
      activate_row(*index);
      event.handled = true;
      return true;
    }
    case EventKind::MouseMove: {
      const auto index = row_index_at(event.position);
      const int next = index.has_value() ? static_cast<int>(*index) : -1;
      if (next != hover_index_) {
        hover_index_ = next;
        mark_dirty();
      }
      return false;
    }
    case EventKind::HoverOut:
      if (hover_index_ != -1) {
        hover_index_ = -1;
        mark_dirty();
      }
      return false;
    case EventKind::KeyDown: {
      if (rows_.empty()) return false;
      const std::size_t count = rows_.size();
      const std::size_t current = selected_index() == kNoSelection ? 0 : selected_index();
      const std::string& key = event.key;
      if (key == "ArrowDown") {
        const std::size_t next = current + 1 < count ? current + 1 : count - 1;
        if (!rows_[next].data.is_dir) select_key(rows_[next].data.key);
        else selected_key_ = rows_[next].data.key;
        mark_dirty();
        return true;
      }
      if (key == "ArrowUp") {
        const std::size_t next = current > 0 ? current - 1 : 0;
        if (!rows_[next].data.is_dir) select_key(rows_[next].data.key);
        else selected_key_ = rows_[next].data.key;
        mark_dirty();
        return true;
      }
      if (key == "Home") {
        selected_key_ = rows_.front().data.key;
        mark_dirty();
        return true;
      }
      if (key == "End") {
        selected_key_ = rows_.back().data.key;
        mark_dirty();
        return true;
      }
      if (key == "Enter" || key == " ") {
        activate_row(current);
        return true;
      }
      if (key == "ArrowRight" || key == "ArrowLeft") {
        const TreeNode* node = node_at(current);
        if (node == nullptr) return false;
        if (!node->is_dir) return false;
        const bool expand = key == "ArrowRight";
        if (node->expanded == expand) return true;  // 已在目标状态：消费（无事可做）
        if (on_toggle) on_toggle(node->key, expand);
        mark_dirty();
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

void Tree::activate() {
  if (rows_.empty()) return;
  const std::size_t current = selected_index() == kNoSelection ? 0 : selected_index();
  activate_row(current);
}

auto Tree::semantics_text() const -> std::string {
  return std::format("{} 行", static_cast<unsigned long long>(rows_.size()));
}

auto Tree::semantics_value() const -> std::string {
  return selected_key_.empty() ? "selected=none" : "selected=" + selected_key_;
}

auto Tree::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = !selected_key_.empty();
  flags.scrollable = false;
  return flags;
}

// —— 列表类通用视口契约（详见头文件）——

auto Tree::visible_row_count() const noexcept -> std::size_t {
  if (bounds_.is_empty()) return rows_.empty() ? 1 : rows_.size();
  const auto count = static_cast<std::size_t>(std::max(1.0f, bounds_.height / row_height_));
  return std::max<std::size_t>(1, count);
}

auto Tree::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "row_height") {
    // 行高可写：密度是场景属性，自动化也该能调（视觉验收用）。
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value() || *parsed <= 0.0) return false;
    set_row_height(static_cast<float>(*parsed));
    return true;
  }
  return Element::set_property(name, value);
}

auto Tree::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "rows" || name == "count") return std::to_string(rows_.size());
  // 树自身不滚动（由外部 ScrollView 承载）；`first_visible` 因此恒为 0，
  // 但它在**契约里仍然必须存在**——“这个组件有没有视口概念”是调用方要问的问题。
  if (name == "first_visible") return std::string("0");
  if (name == "visible_rows") return std::to_string(visible_row_count());
  if (name == "scroll") return std::string("0.0");
  if (name == "row_height") return std::format("{:.0f}", row_height_);
  if (name == "selected_key") return std::string(selected_key_);
  if (name == "selected") {
    const auto index = selected_index();
    return index == kNoSelection ? std::string("-1") : std::to_string(index);
  }
  return Element::get_property(name);
}

auto Tree::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.push_back("rows");
  names.push_back("count");
  names.push_back("row_height");
  names.push_back("first_visible");
  names.push_back("visible_rows");
  names.push_back("scroll");
  names.push_back("selected_key");
  names.push_back("selected");
  return names;
}

}  // namespace st::ui
