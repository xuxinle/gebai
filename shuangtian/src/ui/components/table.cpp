#include "st/ui/components/table.hpp"
#include "st/ui/wheel.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"
#include "st/raster/surface.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::text_port_of;

namespace {

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
  // 色调/泳道表跟着长：否则后调的 `set_cell_tone` 会因行数不够而静默失效，
  // 而“设了不生效”是最难查的一类缺陷（属性面也看不到）。
  cell_tones_.emplace_back(columns_.size(), CellTone::Inherit);
  row_offsets_valid_ = false;   // 多了一行，前缀和必须重建
  mark_layout_dirty();
}

void Table::set_column_tones(std::vector<CellTone> tones) {
  column_tones_ = std::move(tones);
  column_tones_.resize(columns_.size(), CellTone::Inherit);
  mark_dirty();
}

void Table::set_cell_tone(std::size_t row, std::size_t column, CellTone tone) {
  if (row >= rows_.size()) return;
  if (cell_tones_.size() <= row) cell_tones_.resize(row + 1);
  if (cell_tones_[row].size() <= column) {
    cell_tones_[row].resize(std::max(column + 1, rows_[row].size()), CellTone::Inherit);
  }
  cell_tones_[row][column] = tone;
  // 单个单元格的色调变化只影响那一格（按单元格上报，不报整块）。
  record_damage_rect(cell_rect(row, column));
}

auto Table::cell_tone(std::size_t row, std::size_t column) const -> CellTone {
  // 个别覆盖优先，其次是列默认。
  if (row < cell_tones_.size() && column < cell_tones_[row].size() &&
      cell_tones_[row][column] != CellTone::Inherit) {
    return cell_tones_[row][column];
  }
  if (column < column_tones_.size()) return column_tones_[column];
  return CellTone::Inherit;
}

void Table::set_graph(std::vector<GraphNodeSpec> nodes, std::vector<GraphEdgeSpec> edges) {
  graph_nodes_ = std::move(nodes);
  graph_edges_ = std::move(edges);
  // 泳道带占宽 = 道数（`lane_inset` 读它）。
  std::size_t widest = 0;
  for (const GraphNodeSpec& node : graph_nodes_) widest = std::max(widest, node.lane + 1U);
  for (const GraphEdgeSpec& edge : graph_edges_) {
    widest = std::max({widest, edge.from_lane + 1U, edge.to_lane + 1U});
  }
  lane_count_ = widest;
  mark_layout_dirty();
}

void Table::clear_graph() {
  if (graph_nodes_.empty() && graph_edges_.empty()) return;
  graph_nodes_.clear();
  graph_edges_.clear();
  lane_count_ = 0;
  mark_layout_dirty();
}

auto Table::lane_count_used() const noexcept -> std::size_t { return lane_count_; }

/// 道间距：**固定 `kLanePitch`**，只在预算不够放下时才压缩。
///
/// 见头文件 `kLanePitch` 的注释：旧口径把预算当“分摊量”，四道时道间距只剩 8.5px。
/// 这里改用“先给足、放不下再压”的次序（两行间用 clamp 表达）：
///
///   需求带宽 = kLanePitch × 道数 + 2 × kLaneBandPadding
///   实际带宽 = max(预算, 需求带宽)   → 预算只当下限（见下面的注释）
///   实际道间距 = max(kLanePitchMin, (实际带宽 − 留白) / 道数)
///
/// ⚠ 为何预算是**下限**而不是上限：旧默认值 28 是为“单道竖线”时代定的，
/// 它只够一道。若当上限用，四道就会被默默压成 8.5px——而调用方几乎不会
/// 主动去调一个“我不想调”的参数，结果是“多道分支图一律挤成一团”。
/// 带宽变宽的代价是首列往右移，而那正是**应该**的（画图需要地方）。
[[nodiscard]] auto Table::lane_inset() const noexcept -> float {
  const std::size_t lanes = lane_count_used();
  if (lanes == 0U) return 0.0f;
  const float needed = kLanePitch * static_cast<float>(lanes) + kLaneBandPadding * 2.0f;
  return std::max(lane_column_width_, needed);
}

auto Table::graph_metrics() const noexcept -> GraphMetrics {
  GraphMetrics metrics;
  const std::size_t lanes = lane_count_used();
  if (lanes == 0U) return metrics;   // 没开泳道图（全 0 = 无定义）
  const float band = lane_inset();
  // 道间距 = (带宽 − 两侧留白) / 道数，夹到 [kLanePitchMin, kLanePitch]。
  //
  // 夹上限是 `kLanePitch`：预算给得再宽，道也不会被拉得稀稀落落
  //（参照里道间距是固定的，带宽变宽只表现为右侧留白）。
  metrics.lane_pitch = std::clamp((band - kLaneBandPadding * 2.0f) / static_cast<float>(lanes),
                                  kLanePitchMin, kLanePitch);
  metrics.band_width = band;
  const float scale = metrics.lane_pitch / kLanePitch;
  metrics.line_width = std::clamp(kLaneLineWidthMin * std::max(0.7F, scale), kLaneLineWidthMin,
                                  kLaneLineWidthMax);
  // 节点半径：跟随道间距，但夹在参照口径内，并不吃掉行高。
  //
  // 参照实测直径 10~11px（半径 ≈ 5）：取 `lane_pitch × 0.21` = 5.0（道间距 24 时）。
  // 上限 `row_height × 0.2`（行高 28 → 5.6）防大圆点贴住相邻节点。
  const float row_cap = row_height_at(0) * 0.20f;
  metrics.dot_radius =
      std::clamp(metrics.lane_pitch * 0.21F, kLaneDotRadiusMin,
                 std::min({kLaneDotRadiusMax, row_cap}));
  metrics.curve_bend_rows = kLaneCurveBendRows;
  metrics.curve_stub_rows = kLaneCurveStubRows;
  metrics.curve_handle_ratio = kLaneCurveHandleRatio;
  return metrics;
}

void Table::paint_node_edge_graph(raster::Surface& canvas, const Palette& colors,
                                  std::size_t first_row, std::size_t last_row, float clip_left,
                                  float clip_right) const {
  const GraphMetrics metrics = graph_metrics();
  if (metrics.lane_pitch <= 0.0f) return;   // 没开泳道图
  const float band_x = bounds_.x + kLaneBandPadding;
  const float lane_width = metrics.line_width;
  const float dot_radius = metrics.dot_radius;
  // 第 `lane` 道的中心 x：首道距带左缘 `kLaneBandPadding`，之后每道 +`lane_pitch`。
  //
  // ⚠ 旧写法是 `band_x + cell * (lane*2 + 0.5)`（“道 = 两个字符格”时代的残留），
  // 它隐含地假设道间距 = 2×cell；现在道间距是独立参数，再那样算就会
  // 让画出来的道位置与 `lane_inset()` 报出的带宽不一致（首字会被道盖住或留一大块空白）。
  const auto lane_x = [&](std::size_t lane) {
    return band_x + metrics.lane_pitch * static_cast<float>(lane);
  };
  const auto row_center_y = [&](std::size_t row) {
    const math::Rect box = row_rect(row);
    return box.y + box.height * 0.5f;
  };
  // 纵向裁剪边界：表体上下缘（滚动时线被裁到边界，而不是画进表头/表尾外面）。
  const float clip_top = bounds_.y + header_height();
  const float clip_bottom = bounds_.bottom();

  // ── 第一遍：边 ──
  //
  // 三类：同道且相邻行（短竖线）、同道跨多行（一根连到底）、跨道（曲线）。
  // 线**只存在于边里**——没有“某个字符说这里有线”这回事，因此不可能出现悬空线头。
  for (const GraphEdgeSpec& edge : graph_edges_) {
    if (edge.from_row >= rows_.size() || edge.to_row >= rows_.size()) continue;
    const std::size_t color_lane = std::max(edge.from_lane, edge.to_lane);
    const math::Color color = Table::lane_color(color_lane, colors);
    const raster::Paint paint = raster::Paint::solid(color);
    const float from_y = row_center_y(edge.from_row);
    const float to_y = row_center_y(edge.to_row);
    if (from_y >= clip_bottom || to_y <= clip_top) continue;
    if (edge.from_lane == edge.to_lane) {
      const float cx = lane_x(edge.from_lane);
      if (cx >= clip_right || cx <= clip_left) continue;
      const float top = std::min(from_y, to_y);
      const float bottom = std::max(from_y, to_y);
      canvas.fill_rect(math::Rect{cx - lane_width * 0.5f, top, lane_width, bottom - top}, paint);
      continue;
    }
    // 跨道：**弯段长度 = min(纵向跨度, K × 行高)**，两端留短竖直段，
    // 余下全部归终点侧竖直段（见 `kLaneCurveBendRows` 的两种错法）。
    //
    // · 相邻行换道（跨度≈1 行高）：弯段铺满整条边 ⇒ 观感上就是一条贯穿的曲线；
    // · 跨多行换道：弯段只占开头约 0.9 行高，余下全是竖直——
    //   这就是“分叉/合入是曲线、而长距离回边不拖成一条斜线”的统一形状。
    const float from_x = lane_x(edge.from_lane);
    const float to_x = lane_x(edge.to_lane);
    const float span = std::abs(to_y - from_y);
    const float row_height_now = row_height_at(0);
    const float stub = std::min(span * 0.25F, row_height_now * metrics.curve_stub_rows);
    const float bend = std::min(std::max(0.0F, span - stub), row_height_now * metrics.curve_bend_rows);
    const float bend_end = from_y + stub + bend;
    // 对称手柄：不对称会未端出现折角（实测过 0.38/0.04，黄线走水平后突然下折）。
    const float handle = bend * metrics.curve_handle_ratio;
    math::Path line;
    line.move_to(math::Point{from_x, from_y});
    // ① 起点竖直线段（很短，主要让曲线从节点处平滑起弯）。
    line.line_to(math::Point{from_x, from_y + stub});
    // ② 弯曲段。
    if (bend > 0.01F) {
      line.cubic_to(math::Point{from_x, from_y + stub + handle},
                    math::Point{to_x, bend_end - handle}, math::Point{to_x, bend_end});
    }
    // ③ 终点竖直线段（吸收剩余纵向距离）。
    line.line_to(math::Point{to_x, to_y});
    canvas.stroke_path(line, paint, lane_width,
                       raster::StrokeStyle{raster::LineCap::Round, raster::LineJoin::Round});
  }

  // ── 第二遍：节点（盖在边上）──
  //
  // 用 `fill_circle` 而不是圆角方块：参考形态里节点是实心圆，而圆角方块的
  // 圆角是四段贝塞尔的近似（小直径看不出，直径一大就显“圆角方”）。
  for (const GraphNodeSpec& node : graph_nodes_) {
    if (node.row < first_row || node.row >= last_row) continue;
    if (node.row >= rows_.size()) continue;
    const math::Rect box = row_rect(node.row);
    if (box.height <= 0.0f) continue;
    const float cx = lane_x(node.lane);
    if (cx >= clip_right || cx <= clip_left) continue;
    const float cy = box.y + box.height * 0.5f;
    const math::Color color = Table::lane_color(node.lane, colors);
    canvas.fill_circle(math::Point{cx, cy}, dot_radius, raster::Paint::solid(color));
  }
}

void Table::set_lane_column_width(float width) {
  // 夹到合理范围：太窄连一道都放不下、太宽就是在抢内容区。
  lane_column_width_ = std::clamp(width, 8.0f, 400.0f);
  mark_layout_dirty();
}

void Table::clear_rows() {
  if (rows_.empty()) return;
  rows_.clear();
  // ⚠ 色调/泳道表**同样要清**：否则下一次 `add_row` 后旧数据还挂着，
  // 新行会拿到上一批的行位（颜色/泳道错位，而画面看着“有值只是不对”）。
  cell_tones_.clear();
  lane_count_ = 0;   // ⚠ 与 `graph_nodes_` 同步清（否则带宽按旧道数算，首列错位）
  graph_nodes_.clear();
  graph_edges_.clear();
  hovered_row_ = kNoRow;
  selected_row_ = std::nullopt;  // 数据没了：选中索引必然悬空
  // **纵向滚动位置不当场归零**：
  //
  // 旧实现直接 `scroll_offset_y_ = 0`，理由是“行没了，偏移跟着归零”——
  // 但最常见的调用形态恰恰是**clear + 重新填同一批数据的更新版**
  //（声明式列表每帧重建、日志按筛选重算）。那样每点一下就跳回顶部：
  // 实测 gbcode 日志表滚到第 44 行点开提交详情，`scroll_y` 从 1176 瞬间变 0，
  // 用户没法连着看几条提交。
  //
  // 正确做法是“夹取”而不是“清零”：下次 `measure`/`arrange` 会用新的
  // 行数把越界的偏移夹回范围内（行确实变少时照样落到合法位置，
  // 行全空时 `max_scroll_y()==0` 也会自然归零）。这里不动它。
  row_offsets_valid_ = false;
  mark_layout_dirty();
}

void Table::set_zebra(bool value) {
  if (zebra_ == value) return;
  zebra_ = value;
  mark_dirty();
}

void Table::set_row_scales(std::vector<float> scales) {
  row_scales_ = std::move(scales);
  // 夹取：0.2~3.0。低于 0.2 就连一行字都放不下了（行高会被字截切）；
  // 高于 3.0 不是“紧凑/宽松”而是排版事故。
  for (float& scale : row_scales_) {
    scale = std::clamp(scale > 0.0f ? scale : 1.0f, 0.2f, 3.0f);
  }
  row_offsets_valid_ = false;   // 倍率变了，前缀和必须重建
  mark_layout_dirty();
}

auto Table::row_height_at(std::size_t row) const noexcept -> float {
  if (row < row_scales_.size()) return row_height_ * row_scales_[row];
  return row_height_;
}

void Table::ensure_row_offsets() const {
  if (row_offsets_valid_ && row_offsets_.size() == rows_.size() + 1U) return;
  row_offsets_.assign(rows_.size() + 1U, 0.0f);
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    row_offsets_[index + 1U] = row_offsets_[index] + row_height_at(index);
  }
  row_offsets_valid_ = true;
}

auto Table::content_height() const noexcept -> float {
  ensure_row_offsets();
  return row_offsets_.empty() ? 0.0f : row_offsets_.back();
}

void Table::set_row_height(float height) {
  explicit_row_height_ = true;   // 显式设置优先：字号覆盖不再改写行高
  const float clamped = height > 0.0f ? height : kDefaultRowHeight;
  if (row_height_ == clamped) return;
  row_height_ = clamped;
  row_offsets_valid_ = false;   // 行高变了，前缀和失效
  mark_layout_dirty();
}

auto Table::max_scroll() const noexcept -> float {
  const float visible = std::max(bounds_.width, 0.0f);
  return std::max(0.0f, total_width_ - visible);
}

auto Table::max_scroll_y() const noexcept -> float {
  // 可滚动量 = 行数据总高 − （視口高 − 表头高）。
  // 表头**不滚**（它是列名，跟着滚就失去参考意义），所以它占的高度要从視口里扣掉。
  //
  // 放在 `ScrollView` 里时 `bounds_.height` 是**内容全高**，本式自然得 0——
  // 于是纵向滚轮让给外层（见 `on_event`），保持旧行为。
  if (bounds_.is_empty() || row_height_ <= 0.0f) return 0.0f;
  const float content = content_height();
  const float viewport = std::max(0.0f, bounds_.height - header_height());
  return std::max(0.0f, content - viewport);
}

auto Table::viewport_row_count() const noexcept -> std::size_t {
  if (bounds_.is_empty() || row_height_ <= 0.0f) return 0U;
  // 底部有横向滚动条时把它扣掉，否则最后一行的下半截会被指示条遮住。
  const float bar = max_scroll() > 0.0f ? kScrollBarHeight + 2.0f : 0.0f;
  const float body = std::max(0.0f, bounds_.height - header_height() - bar);
  // 按**实际行高**逐行数（行高可能逐行不同，不能除）；只数到装不下为止。
  float used = 0.0f;
  std::size_t count = 0;
  while (count < rows_.size()) {
    const float next = used + row_height_at(count);
    if (next > body + 0.01f) break;
    used = next;
    ++count;
  }
  return count;
}

void Table::set_scroll_offset_y(float offset) {
  const float clamped = std::clamp(offset, 0.0f, max_scroll_y());
  if (clamped == scroll_offset_y_) return;
  scroll_offset_y_ = clamped;
  // 纵向滚动是整个内容区在动（与横向的“列边界动”不同），按整块 bounds 上报。
  record_damage();
  mark_dirty();
}

/// 本帧**实际在裁剪区内的行区间** `[先, 后)`（半开）；无交集时 `[首, 首)`。
///
/// 为何需要它（2026-10-14，实测）：`Table::paint_content` 里有一个看似能救场的提前退出
/// `if (row.y >= bounds_.bottom()) break;`——但宿主把 Table 放进 `ScrollView` 时，
/// `bounds_` 拿到的是**内容全高**（与 `Tree` 同样的情形），于是这个 break
/// 在整个滚动过程中**一次都不触发**，仍然迭代全部行、全部列，每格调
/// `ellipsize` + `measure_width`。
///
/// 实测（`tools/ui_table_rows_bench.cpp`，4 列 / 400px 视口 / 1000 行）：
/// 真实场景（bounds=内容全高）**8.769 ms**，而对照（bounds 恰为视口高）只有 **0.500 ms**
/// ——同一个 break，178× 的差距全在于它**看不到视口**。
///
/// 口径与 `Element::paint` / `Tree::visible_row_range` **一致**（同一份 `clip_rect()`、
/// 同样按 `device_scale` 折回逻辑坐标），不另立一套。
///
/// `canvas` 为空时退化为全量（保持旧行为，且让无画布的测试仍可断言几何）。
/// 之所以是**静态自由函数**：需要非 const 的 `Surface&`（`clip_rect()` 的签名），
/// 而 `paint_content` 是 const。
[[nodiscard]] auto visible_row_range(const Table& table, const st::raster::Surface* canvas,
                                     float row_height, float header_height)
    -> std::pair<std::size_t, std::size_t> {
  const std::size_t total = table.row_count();
  if (total == 0U || row_height <= 0.0f) return {0U, 0U};
  const math::Rect bounds = table.bounds();
  if (bounds.is_empty()) return {0U, 0U};
  if (canvas == nullptr) return {0U, total};

  const math::IntRect physical = canvas->clip_rect();
  const float scale = canvas->device_scale();
  const float inv = scale > 0.0f ? 1.0f / scale : 1.0f;
  const float clip_top = static_cast<float>(physical.y) * inv;
  const float clip_bottom = static_cast<float>(physical.y + physical.height) * inv;
  if (clip_bottom <= clip_top) return {0U, 0U};

  const float rows_top = bounds.y + header_height - table.scroll_offset_y();
  const float content_bottom = rows_top + table.content_height();
  if (clip_bottom <= rows_top || clip_top >= content_bottom) return {0U, total};

  // 行高可逐行不同 ⇒ 不能“除行高”求下标，改用**前缀和二分**（`row_offsets`）。
  // 两处都是 O(log n)：1000 行也不会在滚动时逐行扫。
  const std::vector<float>& offsets = table.row_offsets();
  if (offsets.size() != total + 1U) return {0U, total};
  const float want_top = std::max(0.0f, clip_top - rows_top);
  const float want_bottom = std::min(content_bottom - rows_top, clip_bottom - rows_top);
  const auto first = static_cast<std::size_t>(
      std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(
                                      std::upper_bound(offsets.begin(), offsets.end(), want_top) -
                                      offsets.begin()) - 1));
  // 上界宽出一行：避免“裁剪区底边正落在行中”时漏画那一行。
  const auto last = static_cast<std::size_t>(
      std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(total),
                               static_cast<std::ptrdiff_t>(
                                   std::lower_bound(offsets.begin(), offsets.end(), want_bottom) -
                                   offsets.begin()) +
                                   1));
  return {std::min(first, total), std::min(last, total)};
}

void Table::set_scroll_offset(float offset) {
  const float limit = max_scroll();
  const float clamped = std::clamp(offset, 0.0f, limit);
  if (clamped == scroll_offset_) return;
  scroll_offset_ = clamped;
  mark_dirty();
}

void Table::set_selected_row(std::optional<std::size_t> row) {
  // 越界拒绝（除 nullopt）：静默夹取会掩盖调用方错误（数据刷新后旧索引失效）。 
    if (row.has_value()) {
    if (*row >= rows_.size()) return;
  }
  if (row == selected_row_) return;
  // 选中变化只影响**两行**：旧行（去高亮）与新行（加高亮）——按行上报，
  // 不报整块（大表格整块上报会越过合并阀值退化成整帧重绘）。
  if (selected_row_.has_value()) record_damage_rect(row_rect(*selected_row_));
  selected_row_ = row;
  if (selected_row_.has_value()) record_damage_rect(row_rect(*selected_row_));
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
  // 显式排版覆盖最后回放（与 Text/Button 同口径）。字号覆盖直接改写 style_
  //（paint 的单元格/表头文本都从 style_.font_size 取）；颜色覆盖存起来供
  // paint_content 的单元格取色用（选中态色在绘制时才解析）。
  apply_text_overrides();
  // 行高跟随字号（字号盖过后不让字溢出行盒）：行高 = max(默认行高, 字号×行高系数)。
  // 不跟随的话，size=28 的单元格字会被 32px 行盒上下裁切——"设了但看不全"与
  // "设了但不生效"一样是静默失效。调用方显式 `set_row_height` 过的以它为准
  //（记录在 `explicit_row_height_`；字号覆盖只指底，不指过显式设置）。
  if (!explicit_row_height_ && style_.font_size > 0.0f) {
    row_height_ = std::max(kDefaultRowHeight, style_.font_size * 1.6f);
  }
}

void Table::recompute_columns(float available) {
  column_widths_.assign(columns_.size(), 0.0f);
  total_width_ = 0.0f;
  if (columns_.empty()) return;

  const float visible = available > 0.0f ? available : 0.0f;
  // 自适应列均分的是“泳道带之外”的宽度（见 `lane_inset`）。
  const float content_visible = std::max(0.0f, visible - lane_inset());
  float fixed_total = 0.0f;
  std::size_t auto_count = 0;
  for (const TableColumn& column : columns_) {
    if (column.width > 0.0f) {
      fixed_total += column.width;
    } else {
      ++auto_count;
    }
  }

  // —— 窄容器下先压固定列（按 `shrink` 从小到大，每列不低过 `min_width`）——
  //
  // 为何要先算这一步：下面两种情形（全固定 / 混合）都假设“固定列拿满自己声明
  // 的宽”。不先压，装不下时只能触发横向滚动，而主列同时被压到最小宽——
  // 用户看到的是“背景信息（作者/日期）完整、真正要看的主列被截成省略号”。
  //
  // 让位量只算到“自适应列能拿到 `kMinColumnWidth`”为止：再压就会连主列一起削，
  // 那不如直接横向滚动（内容完整、只是要推一下）。
  std::vector<float> fixed_widths(columns_.size(), 0.0f);
  for (std::size_t index = 0; index < columns_.size(); ++index) {
    fixed_widths[index] = columns_[index].width > 0.0f ? columns_[index].width : 0.0f;
  }
  if (auto_count > 0) {
    const float auto_floor = kMinColumnWidth * static_cast<float>(auto_count);
    float deficit = fixed_total + auto_floor - content_visible;
    if (deficit > 0.0f) {
      // 压缩顺序：`shrink` 升序（>0 的先前），同优先级按列序；`shrink == 0` 最后。
      std::vector<std::size_t> order;
      order.reserve(columns_.size());
      for (std::size_t index = 0; index < columns_.size(); ++index) {
        if (columns_[index].width > 0.0f && columns_[index].min_width > 0.0f) {
          order.push_back(index);
        }
      }
      std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        const int rank_a = columns_[a].shrink > 0 ? columns_[a].shrink : 1'000'000;
        const int rank_b = columns_[b].shrink > 0 ? columns_[b].shrink : 1'000'000;
        return rank_a < rank_b;
      });
      for (const std::size_t index : order) {
        if (deficit <= 0.0f) break;
        const float floor_width = std::max(columns_[index].min_width, 0.0f);
        const float room = std::max(0.0f, fixed_widths[index] - floor_width);
        const float take = std::min(room, deficit);
        fixed_widths[index] -= take;
        deficit -= take;
      }
      // 重新统计（后续分支仍用 `fixed_total`）。
      fixed_total = 0.0f;
      for (std::size_t index = 0; index < columns_.size(); ++index) fixed_total += fixed_widths[index];
    }
  }

  if (auto_count == 0) {
    // 全部为固定宽：不足以填满时按比例拉伸；超出则保持（触发横向滚动）。
    if (fixed_total <= 0.0f) return;
    const float scale = content_visible > fixed_total ? content_visible / fixed_total : 1.0f;
    for (std::size_t index = 0; index < columns_.size(); ++index) {
      column_widths_[index] = fixed_widths[index] * scale;
    }
    total_width_ = fixed_total * scale;
    return;
  }

  const auto auto_columns = static_cast<float>(auto_count);
  float auto_width = (content_visible - fixed_total) / auto_columns;
  if (auto_width < kMinColumnWidth) auto_width = kMinColumnWidth;
  for (std::size_t index = 0; index < columns_.size(); ++index) {
    column_widths_[index] = columns_[index].width > 0.0f ? fixed_widths[index] : auto_width;
  }
  total_width_ = fixed_total + auto_width * auto_columns;
  if (total_width_ < content_visible) {
    const float extra = (content_visible - total_width_) / auto_columns;
    for (std::size_t index = 0; index < columns_.size(); ++index) {
      if (columns_[index].width <= 0.0f) column_widths_[index] += extra;
    }
    total_width_ = content_visible;
  }
}

auto Table::column_width(std::size_t column) const noexcept -> float {
  return column < column_widths_.size() ? column_widths_[column] : 0.0f;
}

auto Table::column_widths_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < column_widths_.size(); ++index) {
    if (index > 0) out.push_back(',');
    out.append(std::format("{}", column_widths_[index]));
  }
  return out;
}

auto Table::column_x(std::size_t column) const noexcept -> float {
  float x = bounds_.x + lane_inset() - scroll_offset_;
  const std::size_t limit = std::min(column, column_widths_.size());
  for (std::size_t index = 0; index < limit; ++index) x += column_widths_[index];
  return x;
}

auto Table::header_rect() const noexcept -> math::Rect {
  // 表头关掉时返**空矩形**（不是“高 0 的矩形”）：调用方与自动化都在用它判
  // “表头在不在”，而高 0 的矩形在多数判据里仍算“存在”。
  if (!show_header_) return {};
  return math::Rect{bounds_.x, bounds_.y, bounds_.width, kHeaderHeight};
}

auto Table::visible_row_count() const noexcept -> std::size_t {
  // **优先用上一帧的实际可见行数**（`last_visible_last_ - first`）。
  //
  // 为何不能只看 `bounds_.height`：`ScrollView` 给子元素的是**内容全高**，
  // 所以 `bounds_.height / row_height_` 会算出“全部行”（实测：200 行的表报 200，
  // 而视口里其实只看得到 11 行）——那样这个属性就答非所问了。
  // 绘制期拿到的可见区间是**唯一可靠**的答案（与 `first_visible` 同源）。
  if (last_visible_last_ > last_visible_first_) return last_visible_last_ - last_visible_first_;
  // 从未绘制过（或空行集）：按 bounds 估（只在 bounds 恰为视口时准确）。
  if (bounds_.is_empty() || row_height_ <= 0.0f) return rows_.empty() ? 1U : rows_.size();
  const float body = std::max(0.0F, bounds_.height - header_height());
  const auto count = static_cast<std::size_t>(std::max(1.0F, body / row_height_));
  return std::max<std::size_t>(1U, count);
}

auto Table::row_rect(std::size_t row) const noexcept -> math::Rect {
  // 纵向偏移在这里减：行矩形是**视口坐标**（绘制/命中/泳道图几何全部经它，
  // 于是“滚了就跟着挪”自动成立）。行高逐行可异，y 取前缀和。
  ensure_row_offsets();
  const float top = row < row_offsets_.size() ? row_offsets_[row]
                                             : row_height_ * static_cast<float>(row);
  return math::Rect{bounds_.x, bounds_.y + header_height() + top - scroll_offset_y_,
                    bounds_.width, row_height_at(row)};
}

auto Table::cell_rect(std::size_t row, std::size_t column) const noexcept -> math::Rect {
  const math::Rect row_box = row_rect(row);
  return math::Rect{column_x(column), row_box.y, column_width(column), row_box.height};
}

auto Table::header_cell_rect(std::size_t column) const noexcept -> math::Rect {
  if (!show_header_) return {};
  return math::Rect{column_x(column), bounds_.y, column_width(column), kHeaderHeight};
}

void Table::set_show_header(bool show) {
  if (show_header_ == show) return;
  show_header_ = show;
  // **布局脏**（不是只重绘）：表头占高变了，行区起点、可见行数、滚动量全要重算。
  mark_layout_dirty();
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

  float height = header_height() + row_height_ * static_cast<float>(rows_.size());
  if (style_.has_explicit_height()) height = style_.height;
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);

  measured_ = math::Size{width, height};
  recompute_columns(width);
  if (scroll_offset_ > max_scroll()) scroll_offset_ = max_scroll();
  if (scroll_offset_y_ > max_scroll_y()) scroll_offset_y_ = max_scroll_y();
}

void Table::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  recompute_columns(bounds_.width);
  if (scroll_offset_ > max_scroll()) scroll_offset_ = max_scroll();
  if (scroll_offset_y_ > max_scroll_y()) scroll_offset_y_ = max_scroll_y();
  layout_dirty_ = false;
}

auto Table::row_at(math::Point point) const noexcept -> std::size_t {
  if (!bounds_.contains(point) || row_height_ <= 0.0f) return kNoRow;
  const float top = bounds_.y + header_height() - scroll_offset_y_;
  if (point.y < top) return kNoRow;
  // 行高逐行可异 ⇒ 用前缀和二分（不能除行高）。
  ensure_row_offsets();
  if (row_offsets_.size() != rows_.size() + 1U) return kNoRow;
  const float local = point.y - top;
  const auto index = static_cast<std::size_t>(
      std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(
                                      std::upper_bound(row_offsets_.begin(), row_offsets_.end(),
                                                       local) -
                                      row_offsets_.begin()) - 1));
  return index < rows_.size() ? index : kNoRow;
}

/// 泳道色：**只错色相**（语义与理由见头文件 `Table::lane_color`）。
[[nodiscard]] auto Table::lane_color(std::size_t lane_index, const Palette& colors) -> math::Color {
  // 第 0 道：主题主色。它是“当前所在分支”，也负责让分支图与界面主色一致。
  if (lane_index == 0U) return colors.primary;

  // —— 只错色相，饱和与明度**全统一** ——
  //
  // 参照口径（VS Code Git Graph 实测）：四道分别是 H203/H320/H122/H36，
  // 而 S 都是 100%、L 都是 42%——即“同一个明度上换色相”。
  //
  // 这比“色相+明度+饱和三维轮转”好在两处：
  //   ① 观感一致：每条道一样鲜亮；三维轮转下第 2 道起就会偏暗/偏灰，
  //      看上去像“某些分支被置灰了”（用户反馈的原话是“不够鲜亮”）。
  //   ② 16 道内辨识度更高：实测最小色距 71 vs 62（RGB 曼哈顿距离）。
  //
  // 代价：色相 20° 步进时 **18 道回到同色**（360/20 = 18）。
  // 这个取舍是用户确认过的：真实仓库极少超过 16 道并行分支
  //（本仓 git log 实测最多 4 道），而这 16 道内的观感与辨识度都要紧得多。
  //
  // 饱和度/明度为何用**常量**而不从 primary 推：
  // 从 primary 推会在浅色主题下得到“接近白色”的淡色（浅底上彻底看不见），
  // 而固定 100%/42% 在深底与浅底上都能立住（深底上是亮色、浅底上是暗色，
  // 两侧都与底色拉开距离）。
  constexpr float kHueStep = 20.0F;
  constexpr float kLaneSaturation = 1.0F;
  constexpr float kLaneLightness = 0.42F;
  const float hue = kHueStep * static_cast<float>(lane_index);
  return math::Color::from_hsl(hue, kLaneSaturation, kLaneLightness);
}

void Table::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  canvas.push_clip_rect(bounds_);

  // 行区裁剪（表头之下、底边之上）：单元格底/文字/连线图三段共用。
  const math::Rect body_clip{bounds_.x, bounds_.y + header_height(), bounds_.width,
                             std::max(0.0f, bounds_.height - header_height())};

  // **逐行视口剔除**（见 `visible_row_range` 的说明）：只画可见那几行——
  // 1000 行的表格在 400px 视口里是 12 行而不是 1000 行。
  // 旧代码靠 `if (row.y >= bounds_.bottom()) break;`——而 ScrollView 给子元素的是
  // **内容全高**，那个 break 在滚动中永不触发（实测 178× 差距）。
  const auto [first_row, last_row] = visible_row_range(*this, &canvas, row_height_, header_height());
  last_visible_first_ = first_row;
  last_visible_last_ = last_row;
  // 横向列区间：横向滚动时同样只需画交叠的那几列（`column_x` 已是滚动后坐标）。
  const float clip_left = static_cast<float>(canvas.clip_rect().x) /
                          std::max(canvas.device_scale(), 1.0f);
  const float clip_right = static_cast<float>(canvas.clip_rect().x + canvas.clip_rect().width) /
                           std::max(canvas.device_scale(), 1.0f);

  // 行底：选中行（primary_soft + 主色左缘条）优先于斑马纹与 hover。
  for (std::size_t index = first_row; index < last_row; ++index) {
    const math::Rect row = row_rect(index);
    const bool is_selected = selected_row_.has_value() && index == *selected_row_;
    const bool striped = !is_selected && ((zebra_ && index % 2U == 1U) || index == hovered_row_);
    if (is_selected) {
      canvas.fill_rect(row, raster::Paint::solid(colors.primary_soft));
      const math::Rect accent{row.x, row.y, kSelectedAccentWidth, row.height};
      canvas.fill_rect(accent, raster::Paint::solid(colors.primary));
    } else if (striped) {
      canvas.fill_rect(row, raster::Paint::solid(colors.surface_alt));
    }
  }

  // —— 表头（必须在行区裁剪**之外**画） ——
  //
  // 表头固定在顶部、不随纵向滚动，而行矩形已减纵向偏移：滚到中间时首行会落到
  // 表头高度以内。不弹裁剪就会把表头切掉一半。
  // 先弹掉 `bounds_`，给表头一个干净的面，画完立即重新压回——**下面所有行内容
  // （文字、连线图）都必须在行区裁剪内**。
  canvas.pop_clip();   // bounds_
  const math::Rect header = header_rect();
  if (header.height > 0.0f) {
    fill_round_rect(canvas, header, metrics.radius_md, metrics.radius_md, 0.0f, 0.0f,
                    raster::Paint::solid(colors.surface_alt));
    fill_round_rect(canvas,
                    math::Rect{bounds_.x, bounds_.y + header_height() - metrics.border_width,
                               bounds_.width, metrics.border_width},
                    0.0f, colors.border);
  }

  // 行区裁剪：**单元格底 / 单元格文字 / 行首连线图三段共用**。
  //
  // ⚠ 这层不能省。踩过的坑：表头弹裁剪后直接画单元格文字，则文字只受最外层
  // `bounds_` 约束——有表头时看着没错（文字落在表头下方），可**一旦关掉表头**，
  // 向上滚动时首行就画到 `bounds_.y` 之上（压在面板工具栏上）。而行的**底色**
  // 一直是受裁的（它在 body_clip 内画），于是只有文字越界，看上去像“字飘出去了”，
  // 比整行越界更难归因。
  canvas.push_clip_rect(bounds_);
  canvas.push_clip_rect(body_clip);

  // 单元格文本（选中行文本用主色，与选中底同语义）。
  for (std::size_t row = first_row; row < last_row; ++row) {
    const bool is_selected = selected_row_.has_value() && row == *selected_row_;
    math::Color text_color = is_selected ? colors.primary : colors.text;
    // 显式文字色覆盖（DSL `BoxProps::color/hex_color`）盖过选中/常态色
    //（与 Tree 行文本同口径）。
    if (const auto& override_color = text_color_override(); override_color.has_value()) {
      text_color = *override_color;
    }
    for (std::size_t column = 0; column < columns_.size(); ++column) {
      const math::Rect box = cell_rect(row, column);
      // 列剔除：与裁剪区无交叠的列不画（也不付 `ellipsize`/`measure_width`）。
      if (box.right() <= clip_left || box.x >= clip_right) continue;
      // 单元格色调：一列一色是“主/次信息分层”的唯一手段——
      // 提交信息主色、作者/日期弱化，否则全表一个颜色只能靠列位置猜重点。
      // 选中行/显式覆盖仍优先（它们是更强的意图）。
      math::Color cell_color = text_color;
      if (!is_selected && !text_color_override().has_value()) {
        switch (cell_tone(row, column)) {
          case CellTone::Inherit:
          case CellTone::Normal: break;
          case CellTone::Muted: cell_color = colors.text_muted; break;
          case CellTone::Accent: cell_color = colors.primary; break;
          case CellTone::Danger: cell_color = colors.danger; break;
        }
      }
      draw_cell_text(context, canvas, cell(row, column), box, kCellPadding,
                     columns_[column].align, style_.font_size, cell_color);
    }
  }
  // 表头文本（同样列剔除）。字号按既有比例跟随覆盖（表头/正文 = font_xs/font_base，
  // 无覆盖时仍是 font_xs）：覆盖盖的是整体字号档位，表头略小的层级关系保留。
  const float header_font =
      style_.font_size * (metrics.font_xs / metrics.font_base);
  // 表头关掉时整段跳过（`header_cell_rect` 会返空矩形，但空矩形仍会走到
  // `draw_cell_text` 里白算一次度量——直接短路更干净）。
  for (std::size_t column = 0; show_header_ && column < columns_.size(); ++column) {
    const math::Rect box = header_cell_rect(column);
    if (box.right() <= clip_left || box.x >= clip_right) continue;
    draw_cell_text(context, canvas, columns_[column].name, box, kCellPadding,
                   columns_[column].align, header_font, colors.text_muted);
  }

  // —— 行首连线图（图形分支线）——
  //
  // 只有一个通道：**节点-边**（`Table::set_graph` 收节点与边，绘制端按 (行, 道) 坐标算）。
  //
  // 为何进框架：泳道是**行属性**，它要和行高/纵向偏移/裁剪同一套几何算；
  // 应用自己画得不到 `row_rect` 的内部知识（现在屏幕上第几像素）。
  //
  // ⚠ 仍在**行区裁剪内**（它已在上面压入）：滚到中间时顶部那根线若不裁，
  // 就会直接穿进表头，看着像“线从表头里长出来”。
  paint_node_edge_graph(canvas, colors, first_row, last_row, clip_left, clip_right);

  // 行区裁剪（文字 + 连线图）到此为止。
  canvas.pop_clip();   // body_clip

  // 横向滚动指示（内容超出容器时）。
  const float limit = max_scroll();
  if (limit > 0.0f && bounds_.height > header_height() + kScrollBarHeight * 3.0f) {
    const float ratio = std::clamp(bounds_.width / total_width_, 0.0f, 1.0f);
    const float thumb = std::max(24.0f, bounds_.width * ratio);
    const float travel = bounds_.width - thumb;
    const float progress = scroll_offset_ / limit;
    const math::Rect bar{bounds_.x + travel * progress,
                         bounds_.bottom() - kScrollBarHeight - 2.0f, thumb, kScrollBarHeight};
    fill_round_rect(canvas, bar, kScrollBarHeight * 0.5f, colors.border_strong);
  }

  // 纵向滚动指示（贴右缘，与横向同一套口径）。
  // 为何必须有：表里有几百行时，“现在看到的是全部还是前几行”只能靠它回答；
  // 缺了就只能观察到“最后一行的日期被截了一半”（那是不对的，但很难归因）。
  const float limit_y = max_scroll_y();
  if (limit_y > 0.0f && bounds_.width > 24.0f) {
    const float content = row_height_ * static_cast<float>(rows_.size());
    const float viewport = std::max(1.0f, bounds_.height - header_height());
    const float ratio = std::clamp(viewport / std::max(content, 1.0f), 0.0f, 1.0f);
    const float thumb = std::max(24.0f, viewport * ratio);
    const float travel = std::max(0.0f, viewport - thumb);
    const float progress = scroll_offset_y_ / limit_y;
    const math::Rect bar{bounds_.right() - kScrollBarHeight - 2.0f,
                         bounds_.y + header_height() + travel * progress, kScrollBarHeight, thumb};
    fill_round_rect(canvas, bar, kScrollBarHeight * 0.5f, colors.border_strong);
  }

  canvas.pop_clip();   // 回到 clip_depth_in（守卫兼底）
}

auto Table::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const std::size_t row = row_at(event.position);
      if (row != hovered_row_) {
        // 悬停只是两行高亮变化：按行上报（旧行去高亮 + 新行加高亮）。
        if (hovered_row_ != kNoRow) record_damage_rect(row_rect(hovered_row_));
        hovered_row_ = row;
        if (hovered_row_ != kNoRow) record_damage_rect(row_rect(hovered_row_));
      }
      return bounds_.contains(event.position);
    }
    case EventKind::HoverOut:
      // 悬浮链离开通知（UiRoot 按链分发，2026-11-02）：出窗/失活/移到别处时
      // 清行高亮。旧实现只靠 MouseMove——但 move 只在本表被命中时才到达，
      // 指针移走后最后 hover 的行高亮永久残留。
      if (hovered_row_ != kNoRow) {
        record_damage_rect(row_rect(hovered_row_));
        hovered_row_ = kNoRow;
      }
      return false;
    case EventKind::MouseDown:
      return bounds_.contains(event.position);
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      const std::size_t row = row_at(event.position);
      if (row == kNoRow) return false;  // 表头 / 空白区不视为行点击
      if (on_row_click_) on_row_click_(row);
      event.handled = true;
      return true;
    }
    case EventKind::Wheel: {
      // 两个轴各有归属：**Shift = 横向**（电子表格/IDE 的通行约定），
      // 不按 Shift = 纵向。两个轴都没内容可滚时返回 false 冒泡给外层
      // （如放进 `ScrollView` 时让外层接管）。
      //
      // 为何不能只做横向（旧行为）：旧注释写“纵向由外层容器管”，但全仓零处把
      // Table 放进 ScrollView——于是纵向恒不可滚且不报错（gbcode 日志表 200 行
      // 只看得到 6 行）。约定没人兑现，就必须自己兜住。
      const float limit = event.shift ? max_scroll() : max_scroll_y();
      if (limit <= 0.0f) return false;
      if (event.shift) {
        const float next = wheel_target(scroll_offset_, event, limit, context.theme);
        if (next < 0.0f) return false;
        set_scroll_offset(next);
      } else {
        const float next = wheel_target(scroll_offset_y_, event, limit, context.theme);
        if (next < 0.0f) return false;
        set_scroll_offset_y(next);
      }
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      // 方向键：上下滚纵向、左右滚横向（各管各的轴，没量的方向就放行）。
      if (event.key == "PageDown" || event.key == "PageUp") {
        if (max_scroll_y() <= 0.0f) return false;
        scroll_by_y((event.key == "PageDown" ? 1.0f : -1.0f) *
                    row_height_ * static_cast<float>(std::max<std::size_t>(1U, viewport_row_count() - 1U)));
        event.handled = true;
        return true;
      }
      if (event.key == "ArrowDown" || event.key == "ArrowUp") {
        if (max_scroll_y() <= 0.0f) return false;
        scroll_by_y((event.key == "ArrowDown" ? 1.0f : -1.0f) * row_height_);
        event.handled = true;
        return true;
      }
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
  // 选中行入语义值：自动化可用 `get`/`tree` 断言"选中了第 N 行"而不必看像素。
  if (selected_row_.has_value()) {
    return std::format("{}x{} sel={}", rows_.size(), columns_.size(), *selected_row_);
  }
  return std::format("{}x{}", rows_.size(), columns_.size());
}

auto Table::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.scrollable = max_scroll() > 0.0f;
  flags.selected = selected_row_.has_value();
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
  // 实际列宽（`arrange` 后有效）：自动化验证“窄容器里到底谁让位了”靠它——
  // 只看截图只能看到“被截了”，看不出是哪一列被压。
  if (name == "column_widths") return column_widths_text();
  if (name == "lane_inset") return std::format("{}", lane_inset());
  if (name == "row_height") return std::format("{}", row_height_);
  if (name == "zebra") return std::string(zebra_ ? kFlagTrue : "false");
  // 表头显隐（可读可写）：自动化验“日志表有没有列头”靠它。
  if (name == "show_header") return std::string(show_header_ ? kFlagTrue : "false");
  if (name == "scroll") return std::format("{}", scroll_offset_);
  if (name == "max_scroll") return std::format("{}", max_scroll());
  if (name == "scroll_y") return std::format("{}", scroll_offset_y_);
  if (name == "max_scroll_y") return std::format("{}", max_scroll_y());
  // 视口契约（与 Tree/List 对齐）：首可见行来自**上一帧绘制**，不是恒 0。
  if (name == "first_visible") return std::format("{}", last_visible_first_);
  if (name == "visible_rows") return std::format("{}", visible_row_count());
  if (name == "selected_row") {
    return selected_row_.has_value() ? std::format("{}", *selected_row_) : std::string("-1");
  }
  // 悬浮行是"只能看像素、看不出状态"的量（行高亮不在元素级 hovered 里）：
  // 自动化验证「出窗/移开后高亮已清」需要直接读到它。-1 = 无悬浮行。
  if (name == "hovered_row") {
    return hovered_row_ == kNoRow ? std::string("-1") : std::format("{}", hovered_row_);
  }
  if (name == "value") return semantics_value();
  if (name == "text") return semantics_text();
  return Element::get_property(name);
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
  if (name == "show_header") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_show_header(*parsed);
    return true;
  }
  if (name == "scroll") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_scroll_offset(static_cast<float>(*parsed));
    return true;
  }
  if (name == "scroll_y") {
    const auto parsed = st::parse_f64(value);
    if (!parsed.has_value()) return false;
    set_scroll_offset_y(static_cast<float>(*parsed));
    return true;
  }
  if (name == "selected_row" || name == "select") {
    // `-1` / 空串 = 清除选中；越界拒绝（静默改写会掩盖调用方错误）。
    if (value == "-1" || value.empty()) {
      set_selected_row(std::nullopt);
      return true;
    }
    const auto parsed = st::parse_u64(value);
    if (!parsed.has_value() || *parsed >= rows_.size()) return false;
    set_selected_row(static_cast<std::size_t>(*parsed));
    return true;
  }
  return Element::set_property(name, value);
}

auto Table::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"columns", "rows", "row_height", "zebra", "show_header",
                                  "scroll", "max_scroll", "scroll_y", "max_scroll_y",
                                  "selected_row", "value", "first_visible", "visible_rows"});
}

/// 只读属性（不进 `property_names()`：那张表是“可读写”契约，`get` 在两处都查）。
auto Table::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"column_widths", "lane_inset"});
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

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
//
// 一行一个组件，展开后包含**静态登记**（本 .o 进产物即在 main 前完成注册）
// 与**导入锚点**（`import_components<T>()` 取它的地址，把本 .o 拉进产物）。
// 所以：真的用到组件 → 自动注册；要按名字建 → 另调 import。
ST_UI_COMPONENT_IMPL(::st::ui::Table);
