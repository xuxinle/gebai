#include "st/ui/components/table.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口，布局仍可运行）。
[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

/// 对齐名 ↔ 枚举（控制通道属性用）。
[[nodiscard]] auto parse_align(std::string_view text) -> TextAlign {
  if (text == "center") return TextAlign::Center;
  if (text == "end") return TextAlign::End;
  return TextAlign::Start;
}

[[nodiscard]] auto align_name(TextAlign align) noexcept -> std::string_view {
  switch (align) {
    case TextAlign::Start: return "start";
    case TextAlign::Center: return "center";
    case TextAlign::End: return "end";
  }
  return "start";
}

/// 单行单元格文本：按内边距收缩 → 省略 → 按对齐绘制（不修改 `style_`）。
void draw_cell_text(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                    math::Rect box, float padding, TextAlign align, float size,
                    math::Color color) {
  const math::Rect area =
      box.inset(math::Insets{padding, 0.0f, padding, 0.0f});
  if (text.empty() || area.width <= 0.0f || area.height <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, area.width);
  if (clipped.empty()) return;
  const float width = port.measure_width(clipped, size);
  float x = area.x;
  if (align == TextAlign::Center) {
    x = area.x + (area.width - width) * 0.5f;
  } else if (align == TextAlign::End) {
    x = area.right() - width;
  }
  const float line = port.line_height(size);
  const float y = area.y + (area.height - line) * 0.5f;
  port.draw(canvas, clipped, math::Point{x, y}, size, color);
}

/// 覆盖率取向归一（raster 层现况）：`Canvas::blend_coverage_row` 只接受**正绕向**覆盖率
/// （`rasterize_mask` 用 `abs(coverage)`，二者语义不一致），而 `Path` 工厂
/// （`add_rect`/`add_rounded_rect`/`add_circle`）产出的是反向绕向——直接 `fill_path` 会整块
/// 不可见。这里把路径按扁平化折线反转重建，使填充在两个语义下都真实落地；
/// raster 层统一为 `abs` 语义后本函数退化为等价直通（可安全移除）。
[[nodiscard]] auto oriented(const raster::Path& path) -> raster::Path {
  raster::Path out;
  for (const raster::Polyline& polyline : path.flatten(0.25f)) {
    const std::span<const math::Point> points = polyline.points;
    if (points.size() < 2U) continue;
    out.move_to(points.back());
    for (std::size_t index = points.size() - 1U; index > 0U; --index) {
      out.line_to(points[index - 1U]);
    }
    if (polyline.closed) out.close();
  }
  return out;
}

/// 矩形/圆角矩形填充（四角独立半径；经 `oriented` 归一后落盘）。
void fill_round_rect(raster::Surface& canvas, math::Rect rect, float top_left, float top_right,
                     float bottom_right, float bottom_left, const raster::Paint& paint) {
  if (rect.is_empty()) return;
  raster::Path path;
  path.add_rounded_rect(rect, top_left, top_right, bottom_right, bottom_left);
  canvas.fill_path(oriented(path), paint);
}

/// 四角同半径的纯色填充（`radius == 0` 即普通矩形）。
void fill_round_rect(raster::Surface& canvas, math::Rect rect, float radius, math::Color color) {
  fill_round_rect(canvas, rect, radius, radius, radius, radius, raster::Paint::solid(color));
}

}  // namespace

Table::Table(std::vector<TableColumn> columns) {
  columns_ = std::move(columns);
  style_.direction = FlexDirection::Column;
  style_.background = math::Color{0, 0, 0, 0};
  set_focusable(true);
}

auto Table::cell(std::size_t row, std::size_t column) const -> std::string_view {
  if (row >= rows_.size() || column >= rows_[row].size()) return {};
  return rows_[row][column];
}

void Table::set_columns(std::vector<TableColumn> columns) {
  columns_ = std::move(columns);
  recompute_columns(bounds_.width);
  mark_layout_dirty();
}

void Table::add_row(std::vector<std::string> cells) {
  if (!columns_.empty()) cells.resize(columns_.size());
  rows_.push_back(std::move(cells));
  mark_layout_dirty();
}

void Table::clear_rows() {
  if (rows_.empty()) return;
  rows_.clear();
  hovered_row_ = kNoRow;
  mark_layout_dirty();
}

void Table::set_zebra(bool value) {
  if (zebra_ == value) return;
  zebra_ = value;
  mark_dirty();
}

void Table::set_row_height(float height) {
  const float clamped = height > 0.0f ? height : kDefaultRowHeight;
  if (row_height_ == clamped) return;
  row_height_ = clamped;
  mark_layout_dirty();
}

auto Table::max_scroll() const noexcept -> float {
  const float visible = std::max(bounds_.width, 0.0f);
  return std::max(0.0f, total_width_ - visible);
}

void Table::set_scroll_offset(float offset) {
  const float limit = max_scroll();
  const float clamped = std::clamp(offset, 0.0f, limit);
  if (clamped == scroll_offset_) return;
  scroll_offset_ = clamped;
  mark_dirty();
}

void Table::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.radius = 0.0f;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  style_.padding = math::Insets{};
  style_.align_items = Align::Stretch;
}

void Table::recompute_columns(float available) {
  column_widths_.assign(columns_.size(), 0.0f);
  total_width_ = 0.0f;
  if (columns_.empty()) return;

  const float visible = available > 0.0f ? available : 0.0f;
  float fixed_total = 0.0f;
  std::size_t auto_count = 0;
  for (const TableColumn& column : columns_) {
    if (column.width > 0.0f) {
      fixed_total += column.width;
    } else {
      ++auto_count;
    }
  }

  if (auto_count == 0) {
    // 全部为固定宽：不足以填满时按比例拉伸；超出则保持（触发横向滚动）。
    if (fixed_total <= 0.0f) return;
    const float scale = visible > fixed_total ? visible / fixed_total : 1.0f;
    for (std::size_t index = 0; index < columns_.size(); ++index) {
      column_widths_[index] = columns_[index].width * scale;
    }
    total_width_ = fixed_total * scale;
    return;
  }

  const auto auto_columns = static_cast<float>(auto_count);
  float auto_width = (visible - fixed_total) / auto_columns;
  if (auto_width < kMinColumnWidth) auto_width = kMinColumnWidth;
  for (std::size_t index = 0; index < columns_.size(); ++index) {
    column_widths_[index] = columns_[index].width > 0.0f ? columns_[index].width : auto_width;
  }
  total_width_ = fixed_total + auto_width * auto_columns;
  if (total_width_ < visible) {
    const float extra = (visible - total_width_) / auto_columns;
    for (std::size_t index = 0; index < columns_.size(); ++index) {
      if (columns_[index].width <= 0.0f) column_widths_[index] += extra;
    }
    total_width_ = visible;
  }
}

auto Table::column_width(std::size_t column) const noexcept -> float {
  return column < column_widths_.size() ? column_widths_[column] : 0.0f;
}

auto Table::column_x(std::size_t column) const noexcept -> float {
  float x = bounds_.x - scroll_offset_;
  const std::size_t limit = std::min(column, column_widths_.size());
  for (std::size_t index = 0; index < limit; ++index) x += column_widths_[index];
  return x;
}

auto Table::header_rect() const noexcept -> math::Rect {
  return math::Rect{bounds_.x, bounds_.y, bounds_.width, kHeaderHeight};
}

auto Table::row_rect(std::size_t row) const noexcept -> math::Rect {
  return math::Rect{bounds_.x, bounds_.y + kHeaderHeight + row_height_ * static_cast<float>(row),
                    bounds_.width, row_height_};
}

auto Table::cell_rect(std::size_t row, std::size_t column) const noexcept -> math::Rect {
  return math::Rect{column_x(column), row_rect(row).y, column_width(column), row_height_};
}

auto Table::header_cell_rect(std::size_t column) const noexcept -> math::Rect {
  return math::Rect{column_x(column), bounds_.y, column_width(column), kHeaderHeight};
}

void Table::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  float width = 0.0f;
  if (style_.has_explicit_width()) {
    width = style_.width;
  } else if (constraints.max_width < kUnbounded) {
    width = constraints.max_width;
  } else {
    for (const TableColumn& column : columns_) {
      width += column.width > 0.0f ? column.width : 160.0f;
    }
    if (width <= 0.0f) width = 320.0f;
  }
  width = std::clamp(width, style_.min_width, style_.max_width);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);

  float height = kHeaderHeight + row_height_ * static_cast<float>(rows_.size());
  if (style_.has_explicit_height()) height = style_.height;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);

  measured_ = math::Size{width, height};
  recompute_columns(width);
  if (scroll_offset_ > max_scroll()) scroll_offset_ = max_scroll();
}

void Table::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  recompute_columns(bounds_.width);
  if (scroll_offset_ > max_scroll()) scroll_offset_ = max_scroll();
  layout_dirty_ = false;
}

auto Table::row_at(math::Point point) const noexcept -> std::size_t {
  if (!bounds_.contains(point) || row_height_ <= 0.0f) return kNoRow;
  const float top = bounds_.y + kHeaderHeight;
  if (point.y < top) return kNoRow;
  const auto index = static_cast<std::size_t>((point.y - top) / row_height_);
  return index < rows_.size() ? index : kNoRow;
}

void Table::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  canvas.push_clip_rect(bounds_);

  // 行底：斑马纹 + hover 高亮（均取 token 色，不做硬编码）。
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    const math::Rect row = row_rect(index);
    if (row.y >= bounds_.bottom()) break;
    const bool striped = (zebra_ && index % 2U == 1U) || index == hovered_row_;
    if (striped) {
      canvas.fill_rect(row, raster::Paint::solid(colors.surface_alt));
    }
  }

  // 表头（顶部圆角）与底部分隔线。
  const math::Rect header = header_rect();
  if (header.height > 0.0f) {
    fill_round_rect(canvas, header, metrics.radius_md, metrics.radius_md, 0.0f, 0.0f,
                    raster::Paint::solid(colors.surface_alt));
    fill_round_rect(canvas,
                    math::Rect{bounds_.x, bounds_.y + kHeaderHeight - metrics.border_width,
                               bounds_.width, metrics.border_width},
                    0.0f, colors.border);
  }

  // 单元格文本。
  for (std::size_t row = 0; row < rows_.size(); ++row) {
    const math::Rect band = row_rect(row);
    if (band.y >= bounds_.bottom()) break;
    for (std::size_t column = 0; column < columns_.size(); ++column) {
      draw_cell_text(context, canvas, cell(row, column), cell_rect(row, column), kCellPadding,
                     columns_[column].align, metrics.font_base, colors.text);
    }
  }
  // 表头文本。
  for (std::size_t column = 0; column < columns_.size(); ++column) {
    draw_cell_text(context, canvas, columns_[column].name, header_cell_rect(column), kCellPadding,
                   columns_[column].align, metrics.font_xs, colors.text_muted);
  }

  // 横向滚动指示（内容超出容器时）。
  const float limit = max_scroll();
  if (limit > 0.0f && bounds_.height > kHeaderHeight + kScrollBarHeight * 3.0f) {
    const float ratio = std::clamp(bounds_.width / total_width_, 0.0f, 1.0f);
    const float thumb = std::max(24.0f, bounds_.width * ratio);
    const float travel = bounds_.width - thumb;
    const float progress = scroll_offset_ / limit;
    const math::Rect bar{bounds_.x + travel * progress,
                         bounds_.bottom() - kScrollBarHeight - 2.0f, thumb, kScrollBarHeight};
    fill_round_rect(canvas, bar, kScrollBarHeight * 0.5f, colors.border_strong);
  }

  canvas.pop_clip();
}

auto Table::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const std::size_t row = row_at(event.position);
      if (row != hovered_row_) {
        hovered_row_ = row;
        mark_dirty();
      }
      return bounds_.contains(event.position);
    }
    case EventKind::MouseDown:
      return bounds_.contains(event.position);
    case EventKind::Click:
    case EventKind::DoubleClick: {
      const std::size_t row = row_at(event.position);
      if (row == kNoRow) return false;  // 表头 / 空白区不视为行点击
      if (on_row_click_) on_row_click_(row);
      event.handled = true;
      return true;
    }
    case EventKind::Wheel: {
      if (!event.shift || max_scroll() <= 0.0f) return false;
      set_scroll_offset(scroll_offset_ + event.wheel_delta * kScrollStep);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      if (max_scroll() <= 0.0f) return false;
      if (event.key == "ArrowRight") {
        scroll_by(kScrollStep);
      } else if (event.key == "ArrowLeft") {
        scroll_by(-kScrollStep);
      } else if (event.key == "Home") {
        set_scroll_offset(0.0f);
      } else if (event.key == "End") {
        set_scroll_offset(max_scroll());
      } else {
        return false;
      }
      event.handled = true;
      return true;
    }
    default:
      return false;
  }
}

auto Table::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto Table::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < columns_.size(); ++index) {
    if (index > 0) out.append(", ");
    out.append(columns_[index].name);
  }
  return out;
}

auto Table::semantics_value() const -> std::string {
  return std::format("{}x{}", rows_.size(), columns_.size());
}

auto Table::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.scrollable = max_scroll() > 0.0f;
  return flags;
}

auto Table::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "columns") {
    std::string out;
    for (std::size_t index = 0; index < columns_.size(); ++index) {
      if (index > 0) out.push_back(',');
      out.append(std::format("{}:{}:{}", columns_[index].name, columns_[index].width,
                             align_name(columns_[index].align)));
    }
    return out;
  }
  if (name == "rows") return std::format("{}", rows_.size());
  if (name == "row_height") return std::format("{}", row_height_);
  if (name == "zebra") return std::string(zebra_ ? kFlagTrue : "false");
  if (name == "scroll_offset") return std::format("{}", scroll_offset_);
  if (name == "max_scroll") return std::format("{}", max_scroll());
  if (name == "value") return semantics_value();
  if (name == "text") return semantics_text();
  return std::nullopt;
}

auto Table::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "columns") {
    std::vector<TableColumn> columns;
    for (std::string_view item : st::split(value, ',')) {
      item = st::trim(item);
      if (item.empty()) continue;
      const std::vector<std::string_view> parts = st::split(item, ':');
      TableColumn column;
      column.name = std::string(st::trim(parts.empty() ? std::string_view{} : parts[0]));
      if (parts.size() > 1) {
        const auto parsed = st::parse_f64(st::trim(parts[1]));
        if (parsed.has_value() && *parsed > 0.0) column.width = static_cast<float>(*parsed);
      }
      if (parts.size() > 2) column.align = parse_align(st::trim(parts[2]));
      columns.push_back(std::move(column));
    }
    set_columns(std::move(columns));
    return true;
  }
  if (name == "row_height") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_row_height(static_cast<float>(*parsed));
    return true;
  }
  if (name == "zebra") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_zebra(*parsed);
    return true;
  }
  if (name == "scroll_offset" || name == "scroll") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_scroll_offset(static_cast<float>(*parsed));
    return true;
  }
  return false;
}

auto Table::property_names() const -> std::vector<std::string_view> {
  return {"columns", "rows", "row_height", "zebra", "scroll_offset", "max_scroll", "value"};
}

auto Table::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "add_row") {
    std::vector<std::string> cells;
    for (std::string_view part : st::split(argument, '|')) {
      cells.emplace_back(part);
    }
    add_row(std::move(cells));
    return true;
  }
  if (action == "clear_rows") {
    clear_rows();
    return true;
  }
  if (action == "scroll_to") {
    const auto parsed = st::parse_f64(argument);
    if (!parsed.has_value()) return false;
    set_scroll_offset(static_cast<float>(*parsed));
    return true;
  }
  if (action == "scroll_by") {
    const auto parsed = st::parse_f64(argument);
    if (!parsed.has_value()) return false;
    scroll_by(static_cast<float>(*parsed));
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
