#include "st/ui/components/tree.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口）。
[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

/// 单行文本绘制（省略号截断、左对齐垂直居中；不碰 `style_`）。
void draw_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
               math::Rect box, float size, math::Color color) {
  if (text.empty() || box.is_empty() || box.width <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  port.draw(canvas, clipped, math::Point{box.x, box.y + (box.height - line) * 0.5f}, size, color);
}

constexpr float kTextInset{14.0f};  ///< 行文本左边距（与 List 同口径）

/// 无选中哨兵（与 `List` 同值；本组件不依赖 list.hpp）。
constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

}  // namespace

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
  return math::Rect{bounds_.x, bounds_.y + row * kRowHeight, bounds_.width, kRowHeight};
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
  if (local_y < 0.0f || local_y >= static_cast<float>(rows_.size()) * kRowHeight) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(local_y / kRowHeight);
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

  float height = static_cast<float>(rows_.size()) * kRowHeight + style_.padding.vertical();
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

}  // namespace st::ui
