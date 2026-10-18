#include "st/ui/components/tree.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/surface.hpp"
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

void Tree::invalidate_label_widths() const noexcept {
  row_label_widths_.clear();
  label_widths_font_size_ = -1.0F;
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
  // **行宽缓存必须显式失效**：上面那句 `rows_.clear()` 后紧接 push，行数可能与上次相同
  //（常见的“刷新同一目录”）——只靠 `measure` 里“尺寸相同就不重算”的判定会读到**陈旧宽度**。
  // 这是缓存一致性面的核心失效点（护栏：`tree_label_width_cache_follows_sync`）。
  invalidate_label_widths();
  // 选中态跟 key 走：被移除的 key 如实清空（不静默挪到别的行上）
  if (!selected_key_.empty() && !index_of_key(selected_key_).has_value()) {
    selected_key_.clear();
  }
  mark_layout_dirty();
}

void Tree::clear_nodes() {
  rows_.clear();
  invalidate_label_widths();
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

/// 本帧**实际在裁剪区内的行区间** `[先, 后)`（半开）；无交集时 `[0, 0)`。
///
/// 为何需要它（2026-10-14，实测）：`Tree` 是**自绘**组件，行不是子元素，因此
/// `Element::paint` 的视口剔除（只覆盖子元素树）覆盖不到它——`paint_content` 过去
/// 无条件遍历全部行。实测 1000 行树、视口只有 400px（约 15 行可见）时每帧 **5.36 ms**，
/// 2000 行 10.74 ms、5000 行 41.25 ms（完美线性）：**视口里只显示十几行，却为全部行付账**。
/// `DESIGN.md §11` 那条「`tree`（1000 节点）< 5 ms」的目标，缺口正在这里。
///
/// 口径与 `Element::paint` **逐字一致**（读同一份 `canvas.clip_rect()`、同样按
/// `device_scale` 折回逻辑坐标）——不另立一套。
///
/// 为什么可以**假定裁剪区已经扣掉了父级滚动偏移**：宿主把 `Tree` 放进 `ScrollView` 时，
/// `ScrollView::arrange` 把偏移烘进子元素的 `bounds_.y`（`rect.y - offset_`），而 `row_rect`
/// 是相对 `bounds_` 算的——所以 `(clip.top - bounds_.y)` 天然就是「首可见行的内容偏移」。
/// 反过来，脱开 ScrollView 裸用时裁剪区至少含整棵树的包围盒（或整个画布），区间自然退化成全量。
///
/// 竖直方向**不额外加保险外扩**：这里剔除的是一次行填充 + 文本（不投阴影、不发光），
/// 与元素剔除不同——那条路必须按 `paint_margin` 放宽（见 `Element::paint` 的两条安全前提）。
/// 水平方向不参与：行的绘制天然受画布裁剪保护，且全宽行的右侧接缝会随着水平裁剪
/// 出现或消失（若加水平剔除，缩放到某像素宽度时右侧指示条会闪）。
[[nodiscard]] auto visible_row_range(const st::ui::Tree& tree, const st::raster::Surface& canvas)
    -> std::pair<std::size_t, std::size_t> {
  const std::size_t total = tree.node_count();
  if (total == 0U) return {0U, 0U};
  const math::Rect bounds = tree.bounds();
  if (bounds.is_empty() || tree.row_height() <= 0.0f) return {0U, 0U};

  const math::IntRect physical = canvas.clip_rect();
  const float scale = canvas.device_scale();
  const float inv = scale > 0.0f ? 1.0f / scale : 1.0f;
  const float clip_top = static_cast<float>(physical.y) * inv;
  const float clip_bottom = static_cast<float>(physical.y + physical.height) * inv;
  if (clip_bottom <= clip_top) return {0U, 0U};  // 空裁剪区：什么都不该画

  const float row_height = tree.row_height();
  const float local_top = clip_top - bounds.y;
  const float local_bottom = clip_bottom - bounds.y;
  if (local_bottom <= 0.0f || local_top >= static_cast<float>(total) * row_height) {
    return {0U, 0U};
  }

  const float first_f = std::max(0.0f, local_top) / row_height;
  const float last_f = std::min(static_cast<float>(total) * row_height, local_bottom) / row_height;
  const auto first = static_cast<std::size_t>(std::floor(first_f));
  // 上界按 `ceil` 宽出半行，避免「裁剪区底边正落在行中」时漏画那一行的下半截。
  const auto last = static_cast<std::size_t>(std::ceil(last_f)) + 1U;
  return {std::min(first, total), std::min(last, total)};
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
  // 显式排版覆盖最后回放（与 Text/Button 同口径）。字号覆盖直接改写 style_
  //（measure/paint 的行宽缓存与 draw_line 都从 style_.font_size 取，会自动跟随）；
  // 颜色覆盖存起来供 paint_content 的行文本取色用（选中/hover 态色在绘制时才解析）。
  apply_text_overrides();
}

void Tree::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  const float font_size = style_.font_size;

  // **行宽缓存**（见头文件 `row_label_widths_` 的说明——它换来的是“大目录刷新
  // 不再线性重排”）：只在行集或字号变化时重算。
  //
  // 无效判定与填充在同一个 `const` 方法里（缓存字段是 `mutable`，与
  // `CodeEditor` 的几何缓存同一手法）。
  //
  // ⚠ 这里还要**把“最宽行”一起缓存**（2026-10-18，与 `CodeEditor` 同一种缺陷）：
  // 旧实现即使缓存全命中，下面那个“求最大值”的循环仍然**每帧扫全部行**——
  // 于是“缓存生效”只省掉了 `measure_width` 调用，总成本仍是 O(总行数)。
  // 实测参照（`CodeEditor` 的同型缺陷）：5 千行文件单帧 paint 从 4.5ms 涨到 90ms。
  // 行数不变的稳态（滚动/悬停/焦点变化）应该完全 O(1)。
  if (row_label_widths_.size() != rows_.size() || label_widths_font_size_ != font_size ||
      cached_width_metrics_ != &metrics) {
    row_label_widths_.assign(rows_.size(), 0.0F);
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      row_label_widths_[index] = port.measure_width(rows_[index].data.label, font_size);
    }
    label_widths_font_size_ = font_size;
    cached_width_metrics_ = &metrics;
    // 顺手把“最大所需宽”算出来，下一帧就不用再扫（与上面的填充同一趟）。
    cached_max_row_width_ = 0.0F;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      const TreeNode& node = rows_[index].data;
      const float indent = static_cast<float>(node.depth) * kIndentStep;
      const float chevron = node.is_dir ? kChevronSize + metrics.space_xs : 0.0f;
      const float need = indent + chevron + kTextInset + row_label_widths_[index] + kTextInset;
      cached_max_row_width_ = std::max(cached_max_row_width_, need);
    }
  }

  float width = cached_max_row_width_;
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

  // **逐行视口剔除**（见 `visible_row_range` 的说明）：只画裁剪区内那几行——
  // 1000 行的树在 400px 视口里是 15 行而不是 1000 行。
  const auto [first_row, last_row] = visible_row_range(*this, canvas);
  // 记下本帧区间供宿主在帧首取用（`visible_row_range_now()`）——属性面的
  // `first_visible` 也从这里回读真值（与 `CodeEditor` 同一手法）。
  last_visible_first_ = first_row;
  last_visible_last_ = last_row;

  for (std::size_t index = first_row; index < last_row; ++index) {
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

    math::Color label_color = selected ? colors.primary : colors.text;
    // 显式文字色覆盖（DSL `BoxProps::color/hex_color`）盖过选中/常态色：
    // 设了就是"这棵树的字都是这个色"的显式意图（与 Badge 同口径）。
    if (const auto& override_color = text_color_override(); override_color.has_value()) {
      label_color = *override_color;
    }
    const math::Rect text_box{cursor_x, row.y, row.right() - kTextInset - cursor_x, row.height};
    draw_line(context, canvas, node.label, text_box, style_.font_size, label_color);
  }
}

auto Tree::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseDown:
      // **右键**：只报“点了哪一行”，**不改选中**（见 `on_context_menu` 的说明）。
      // 必须在 `MouseDown` 上接而不是 `Click`：右键是否产生 `Click` 取决于后端，
      // 而“按下”在所有后端上都是确定的。
      if (event.button == 2) {
        if (!on_context_menu) return false;
        const auto index = row_index_at(event.position);
        on_context_menu(index.has_value() ? rows_[*index].data.key : std::string_view{});
        event.handled = true;
        return true;
      }
      return false;
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
  if (name == "selected_key") {
    // 程序化选择（写侧）：与键盘/鼠标同链（notify=true 触发 `on_select`），
    // 自动化按键精确驱动而不用坐标猜行序（gbcode e2e 亲历：首版就猜错）。
    select_key(value, true);
    return true;
  }
  // —— 按行号/按 key 的**动作面**（`set` 也能驱动，与 `invoke` 等价）——
  //
  // 为什么 `Tree` 需要这两条：行不是元素（组件自绘、无子 Element），所以
  // `input.mouse{id:...}` 这类“按 id 点”对行无效，调用方只能
  // “拿 bounds + 行高 + 行号自己算坐标”——而**行号会随展开/收起漂移**，
  // 算出来的坐标随时可能落到别的行上（Agent 用坐标驱动文件树时最危险的失效）。
  //
  // 写侧用 `key`（业务身份）而不是行号：`activate_key` 对“点开某个文件”
  // 是稳定入口；行号留给“第几行”这类纯几何操作（仍提供，但只作用于当前快照）。
  if (name == "activate_key") {
    const auto index = index_of_key(value);
    if (!index.has_value()) return false;
    activate_row(*index);
    return true;
  }
  return Element::set_property(name, value);
}

auto Tree::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "rows" || name == "count") return std::to_string(rows_.size());
  // **视口真值来自上一帧绘制**（`visible_row_range_now()`）——不是恒 0。
  //
  // 为何不能"自己算"：`Tree` 自身不滚动，滚动偏移由外部 `ScrollView` 烘进它的
  // `bounds_.y`；而"滚动到哪里了"这件事**只在绘制期知道**（宿主在帧首问属性时没有
  // `RenderContext`）。所以缓存上一帧的区间是唯一同时正确又能脱画读到的答案
  // ——与 `CodeEditor::visible_line_range_now()` 同一立法。
  //
  // 从未绘制过（或区块被剔除）时回退到"首行 0"：那是"没有视口信息时最保守的答案"，
  // 也让"问一个脱画组件"不会得到荒谬的数字。
  if (name == "first_visible") {
    return std::to_string(rows_.empty() ? 0U : last_visible_first_);
  }
  if (name == "visible_rows") return std::to_string(visible_row_count());
  // —— 行内容读面（Agent 的唯一“看得见树里有什么”的通道）——
  //
  // 组件自绘、行不是 Element，所以 `tree`/`find` 拿不到行文本（实测：
  // `#workspace-tree` 的语义文本只有 `"6 行"`，Agent 无法知道树里有哪些
  // 文件、也无法验证“展开对了没有”）。这两条把行集变成可读事实：
  // - `labels`：全部可见行文本，`\n` 分隔（第 i 行 = 第 i 个标签）；
  // - `keys`：  同上，但给的是行的业务 key（点行/激活行用它）。
  // 两个数组同序，调用方按 `index_of` 就能从“我要的文件名”拿到 key。
  if (name == "labels" || name == "keys") {
    std::string out;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      if (index != 0) out.push_back('\n');
      out += name == "labels" ? rows_[index].data.label : rows_[index].data.key;
    }
    return out;
  }
  // 行详情（`rows_detail`）= 每行一条 `label\tkey\tis_dir\tdepth\texpanded`，`\n` 分隔。
  //
  // 为何不叫 `row:N`：属性面是**由 `property_names()` 列举的快照**（`get` 默认只返回
  // 那一份），名字里带参数就进不进快照了（实测：`get row:0` 永远拿不到）。
  // 拼成整表既不破这个契约，也让“批量看结构”只花一次往返。
  if (name == "rows_detail") {
    std::string out;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      if (index != 0) out.push_back('\n');
      const TreeNode& node = rows_[index].data;
      out += std::format("{}\t{}\t{}\t{}\t{}", node.label, node.key,
                         node.is_dir ? "true" : "false", node.depth,
                         node.expanded ? "true" : "false");
    }
    return out;
  }
  if (name == "scroll") {
    // 滚动像素 = 首可见行 × 行高（本组件自身无独立滚动状态，见上）。
    const double offset = rows_.empty() ? 0.0 : static_cast<double>(last_visible_first_) *
                                               static_cast<double>(row_height_);
    return std::format("{:.1f}", offset);
  }
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
  // 行内容读面（见 get_property 里的说明）：`labels` / `keys` 是整表，
  // `row:N` 是单行；`activate_key` 是按业务 key 激活（写侧，与点行同链）。
  names.push_back("labels");
  names.push_back("keys");
  names.push_back("rows_detail");
  names.push_back("activate_key");
  return names;
}

}  // namespace st::ui
