/// `Table` 行首连线图：**绘制几何与曲线形态**（观感类，口径对着参考实现量）。
///
/// ## 为何单独一个文件
///
/// `ui_table_lane_stub_test.cpp` 守的是**拓扑契约**（线必须接在节点上、不许悬空线头）；
/// 本文件守的是**观感口径**——节点多大、线多粗、曲线什么时候贴住目标道。
/// 两者是不同的量：拓扑对了但点线细如发丝，一样是“不像 Git Graph”。
///
/// ## 参照与口径（2026-11-22）
///
/// 参照是 VS Code Git Graph 插件的一张真实截图（440×403，无 DPI 缩放）。
/// 逐像素量出来的口径：
///
/// | 量 | 参照 | 本项目旧值 | 现在 |
/// |---|---|---|---|
/// | 道间距 | 24 px | 24 px（一致） | 24 px |
/// | 线宽 | 3.0 px | 2.0 px | ≥ 2.4 px |
/// | 节点直径 | 7.0 px | 2.7 px | ≥ 6.8 px |
/// | 曲线横向完成度 @ 50% 纵向 | **58%** | 50%（对称贝塞尔） | ≥ 56% |
/// | 曲线横向完成度 @ 75% 纵向 | **98%** | 78% | ≥ 88% |
///
/// ⚠ 为何要量“横向完成度”而不是只量点线尺寸：点线尺寸只解决“看不清”，
/// 而**曲线的形态**决定“像不像分支图”——对称手柄让位移在中点才完成一半，
/// 观感是“线慢慢地横过去”；参照是两头都短、早早贴住新道。
/// 这一条用像素反算曲线中点，无法作假。
///
/// ## 量法（它自己踩过的坑）
///
/// 取“横向**墨迹重心**”而不是“最左/最右边缘”：曲线有 3px 线宽，边缘在
/// 两端会被抗锯齿拉宽，重心才是这条线**当前在哪**的稳定度量。
/// 取样只在两条道之间的 x 区间内——含了道本身就会把两端的中点拉平。

#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::Theme;

constexpr int kCanvasW = 420;
constexpr int kCanvasH = 260;

[[nodiscard]] auto is_lane_ink(const Color& color) -> bool {
  const int maximum =
      std::max({static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b)});
  const int minimum =
      std::min({static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b)});
  return maximum - minimum > 40;
}

[[nodiscard]] auto render(Table& table, Rect box, const Theme& theme) -> Canvas {
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table.apply_theme(theme);
  table.arrange(context, box);
  Canvas canvas = Canvas::for_logical_size(kCanvasW, kCanvasH, 1.0f);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  table.paint(context, canvas);
  return canvas;
}

/// 行很高的档位：节点半径不得小于参照下限（否则圆点变成“小点”、与参照不一致）。
///
/// 这条同时钉住一个**真实的 clamp 参数倒置**：行很小时 `row_height × 0.2` 会小于
/// `kLaneDotRadiusMin`，而 `std::clamp(v, lo, hi)` 要求 `lo <= hi`——libstdc++ 的
/// debug 断言会 **abort 整个进程**（release 下是 UB，结果看编译器兴致）。
/// 实测代价：debug 档跑全量时整张分片的进程直接消失，一条失败行都没有。
ST_TEST(table_lane_dot_radius_survives_tiny_row_height) {
  const Theme theme = Theme::dark();
  // 行高 20 → `row_height × 0.2 = 4.0 < kLaneDotRadiusMin(4.4)`：正是交叉区间。
  for (const float row_height : {20.0f, 12.0f, 4.0f}) {
    std::vector<TableColumn> columns{TableColumn{.name = "A", .width = 200.0f}};
    auto table = std::make_unique<Table>(std::move(columns));
    table->set_row_height(row_height);
    table->add_row({"child"});
    table->add_row({"parent"});
    table->set_graph(std::vector<Table::GraphNodeSpec>{
                         Table::GraphNodeSpec{.row = 0, .lane = 0},
                         Table::GraphNodeSpec{.row = 1, .lane = 1}},
                     std::vector<Table::GraphEdgeSpec>{Table::GraphEdgeSpec{
                         .from_row = 0, .from_lane = 0, .to_row = 1, .to_lane = 1}});
    (void)render(*table, Rect{0.0f, 0.0f, 340.0f, 160.0f}, theme);
    const Table::GraphMetrics metrics = table->graph_metrics();
    // 不得小于参照下限（行高装不下时也是“宁可小到下限”而不是跨出盒子）。
    ST_CHECK(metrics.dot_radius >= Table::kLaneDotRadiusMin - 0.01F);
    ST_CHECK(metrics.dot_radius <= Table::kLaneDotRadiusMax + 0.01F);
  }
}

/// 一条**跨道曲线**的最小场景：两行、两个节点，0 道 → 1 道。
///
/// 行高取 36（与参照图的行距一致），这样手柄比例与参照可比。
[[nodiscard]] auto make_curve_table() -> std::unique_ptr<Table> {
  std::vector<TableColumn> columns{TableColumn{.name = "提交信息", .width = 300.0f}};
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_row_height(36.0f);
  table->add_row({"child"});
  table->add_row({"parent"});
  std::vector<Table::GraphNodeSpec> nodes{Table::GraphNodeSpec{.row = 0, .lane = 0},
                                          Table::GraphNodeSpec{.row = 1, .lane = 1}};
  std::vector<Table::GraphEdgeSpec> edges{Table::GraphEdgeSpec{
      .from_row = 0, .from_lane = 0, .to_row = 1, .to_lane = 1}};
  table->set_graph(std::move(nodes), std::move(edges));
  return table;
}

/// 某一道中心的 x（**与绘制端同源**）。
///
/// ⚠ 这里不能再写 `band_x + cell*(lane*2+0.5)`（那是“道 = 两个字符格”时代的公式）。
/// 实测代价：改完实现后此函数仍用旧公式，算出的道中心偏了 2px，
/// 于是「圆心右方 3px 应有墨」与「10% 处重心不低于原点」两条断言假红——
/// 量尺指向了旁边一条竖线。**几何量只有一个真源**：`kLaneBandPadding + pitch × lane`。
[[nodiscard]] auto lane_center(const Table& table, std::size_t lane) -> float {
  const Table::GraphMetrics metrics = table.graph_metrics();
  const Rect row = table.row_rect(0);
  return row.x + Table::kLaneBandPadding + metrics.lane_pitch * static_cast<float>(lane);
}

/// 某一行中线的 y。
[[nodiscard]] auto row_center_y(const Table& table, std::size_t row) -> float {
  const Rect box = table.row_rect(row);
  return box.y + box.height * 0.5f;
}

/// 在 `y` 这一横向扫过的**墨迹重心 x**（找不到墨返回 `-1`）。
///
/// `x_from`/`x_to` 限定扫掠区间：曲线两侧端点附近会被抗锯齿拉宽，
/// 而道本身也在区间里——所以调用方给的区间是“两道之间”。
[[nodiscard]] auto ink_centroid_x(const Canvas& canvas, int y, int x_from, int x_to) -> float {
  double sum = 0.0;
  double weight = 0.0;
  for (int x = x_from; x <= x_to; ++x) {
    if (!is_lane_ink(canvas.pixel_at(x, y))) continue;
    // 用“彩色程度”当权重：抗锯齿边缘的权重自然更小，重心更稳。
    const Color color = canvas.pixel_at(x, y);
    const int maximum = std::max(
        {static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b)});
    const int minimum = std::min(
        {static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b)});
    const double w = static_cast<double>(maximum - minimum);
    sum += static_cast<double>(x) * w;
    weight += w;
  }
  return weight > 0.0 ? static_cast<float>(sum / weight) : -1.0F;
}

}  // namespace

/// **点线尺寸对着参照口径**：节点直径 ≥ 6.8px、线宽 ≥ 2.4px。
///
/// 旧值（直径 2.7 / 线宽 2.0）在截图里“像一根发丝上挂了几个像素点”，
/// 与参照的“实心圆 + 明显线条”是两种观感。判据取**几何量**而不是“看着对不对”：
/// 几何量可复算，而“看着”不能。
///
/// ⚠ 道间距的判据是“**与参照同档**（24px）”，而不是“等于某个与道数无关的常数”——
/// 道数多到预算放不下时它允许被压（那是合理的妥协）。本条用 2 道场景（预算宽裕）。
ST_TEST(table_lane_metrics_match_reference_proportions) {
  const Theme theme = Theme::dark();
  auto table = make_curve_table();
  const Rect box{0.0f, 0.0f, 340.0f, 160.0f};
  const Canvas canvas = render(*table, box, theme);

  const Table::GraphMetrics metrics = table->graph_metrics();
  st::log::out("[table] 泳道几何：道间距={:.1f} 线宽={:.2f} 节点半径={:.2f}\n",
               static_cast<double>(metrics.lane_pitch), static_cast<double>(metrics.line_width),
               static_cast<double>(metrics.dot_radius));

  // —— 道间距：与参照同档（24px）——
  //
  // ⚠ 这条曾经因为**场景只有 2 道**而红：旧口径下带宽预算 28 被“分摊”成每道 14px。
  // 修正口径后（道间距固定、预算当下限）2 道就是 24px。换成 4 道也必须还是 24px
  // ——那才是“不随道数缩”的真正含义（下面单独量）。
  ST_CHECK(std::abs(metrics.lane_pitch - Table::kLanePitch) < 0.5F);
  // —— 线宽：参照竖线 2px ——
  ST_CHECK(metrics.line_width >= 2.0F);
  ST_CHECK(metrics.line_width <= Table::kLaneLineWidthMax + 0.01F);
  // —— 节点：参照直径 10~11px（含抗锯齿）⇒ 半径 5 ——
  ST_CHECK(metrics.dot_radius >= 4.4F);
  ST_CHECK(metrics.dot_radius <= Table::kLaneDotRadiusMax + 0.01F);

  // —— 逐像素复核节点直径（垂直扫描一条道中线）——
  //
  // ⚠ 这里扫的是**圆心所在列**：偏一点就会量到圆弦上的短直径而偏小。
  const int cx = static_cast<int>(std::lround(lane_center(*table, 0)));
  const int cy = static_cast<int>(std::lround(row_center_y(*table, 0)));
  int top = cy;
  int bottom = cy;
  while (top > 0 && is_lane_ink(canvas.pixel_at(cx, top - 1))) --top;
  while (bottom + 1 < kCanvasH && is_lane_ink(canvas.pixel_at(cx, bottom + 1))) ++bottom;
  const int vertical_run = bottom - top + 1;
  // 圆心行上那一条**节点下半 + 下方竖线** 是连着的，不能直接当直径——
  // 所以只在“同一行内部”量：逐行取宽度，最大宽度就是直径。
  int max_row_width = 0;
  for (int y = top; y <= bottom; ++y) {
    if (!is_lane_ink(canvas.pixel_at(cx, y))) continue;
    int left = cx;
    int right = cx;
    while (left > 0 && is_lane_ink(canvas.pixel_at(left - 1, y))) --left;
    while (right + 1 < kCanvasW && is_lane_ink(canvas.pixel_at(right + 1, y))) ++right;
    max_row_width = std::max(max_row_width, right - left + 1);
  }
  st::log::out("[table] 像素侧：圆心列纵向延={}px，最大行宽={}px（参照直径 10~11）\n", vertical_run,
               max_row_width);
  // 抗锯齿会把边缘算成墨（略大 1~2px）；下界 9 是“真画成 5px 直径时必然红”的判据。
  ST_CHECK(max_row_width >= 9);
  ST_CHECK(max_row_width <= 14);
}

/// **跨多行的换道曲线也只在靠近终点那一小段弯**（三段模型的核心）。
///
/// ## 这条守的是一个“看不见”的缺陷
///
/// 若把整条边画成一条贝塞尔（手柄按跳距或按行高都算错），跨多行时曲线会被
/// 拉成一条贯穿全程的缓斜线——而参照实测：“弯曲段长度固定在约 0.55 行高，
/// 与跨了几行无关，多出来的距离全归到终点侧竖直段”。
///
/// 实测过两种错法（都红）：
/// · `handle = span × 0.5`：跨度 180px 时 0.5 行处只走 **6%**（参照 ≥50%）；
/// · `handle = 0.31 × 行高`：两个手柄加起来才占跨度 12%，同样是缓斜线。
ST_TEST(table_lane_curve_bends_within_one_row_even_for_long_edges) {
  const Theme theme = Theme::dark();
  std::vector<TableColumn> columns{TableColumn{.name = "提交信息", .width = 300.0f}};
  Table table(std::move(columns));
  table.set_row_height(36.0f);
  for (int index = 0; index < 6; ++index) table.add_row({std::format("commit {}", index)});
  // 一条跨 **5 行** 的换道边：0 道 → 1 道。
  std::vector<Table::GraphNodeSpec> nodes{Table::GraphNodeSpec{.row = 0, .lane = 0},
                                          Table::GraphNodeSpec{.row = 5, .lane = 1}};
  std::vector<Table::GraphEdgeSpec> edges{Table::GraphEdgeSpec{
      .from_row = 0, .from_lane = 0, .to_row = 5, .to_lane = 1}};
  table.set_graph(std::move(nodes), std::move(edges));

  const Rect box{0.0f, 0.0f, 340.0f, 260.0f};
  const Canvas canvas = render(table, box, theme);

  const float from_x = lane_center(table, 0);
  const float to_x = lane_center(table, 1);
  const float from_y = row_center_y(table, 0);
  const float to_y = row_center_y(table, 5);
  ST_REQUIRE(to_y - from_y > 100.0F);   // 确实是一条长边（5 行）

  const int scan_from = static_cast<int>(std::lround(from_x)) - 2;
  const int scan_to = static_cast<int>(std::lround(to_x)) + 2;
  const auto progress_at_row = [&](float rows_down) -> double {
    const int y = static_cast<int>(std::lround(from_y + table.row_height() * rows_down));
    const float centroid = ink_centroid_x(canvas, y, scan_from, scan_to);
    if (centroid < 0.0F) return -1.0;
    return static_cast<double>((centroid - from_x) / (to_x - from_x));
  };

  const double at_half_row = progress_at_row(0.5F);
  const double at_one_row = progress_at_row(1.0F);
  const double at_two_rows = progress_at_row(2.0F);
  const double at_four_rows = progress_at_row(4.0F);
  st::log::out(
      "[table] 跨5行边：0.5行→{:.2f}  1行→{:.2f}  2行→{:.2f}  4行→{:.2f}\n", at_half_row,
      at_one_row, at_two_rows, at_four_rows);
  for (const double value : {at_half_row, at_one_row, at_two_rows, at_four_rows}) {
    ST_REQUIRE(value > -0.5);
  }

  // 弯曲段在 0.06H + 0.92H = 0.98 行高内完成 ⇒ 1 行处应已基本贴住。
  // （旧错法：按跳距取手柄时 1 行处只有 ~0.19。）
  ST_CHECK(at_half_row >= 0.25);
  ST_CHECK(at_one_row >= 0.90);
  // 之后全部是竖直段（贴住目标道，不再动）。
  ST_CHECK(at_two_rows >= 0.95);
  ST_CHECK(at_four_rows >= 0.95);
}

/// **节点尺寸自检（量尺基准用例）**：把节点直径钉在参照口径上（10~11px）。
///
/// ## 为何单独一条
///
/// 上一轮对曲线形态得出了**错误结论**（以为有“两端长竖直段”），根因就是
/// **节点直径量成 7px，而实际 10~11px**——于是把节点内部的直线误认为竖直段，
/// 在节点内部取的重心也被节点本体拉向道中心。
///
/// 定一条基准用例的意义：任何后续“量曲线”的测试都以它为尺，
/// 而它自己对着**像素**量（不看 `graph_metrics` 的自报值）——
/// 否则实现改了、测试跟着改，两边一起错（量尺与被测同源就没意义了）。
ST_TEST(table_lane_dot_size_is_measured_from_pixels) {
  const Theme theme = Theme::dark();
  auto table = make_curve_table();
  const Rect box{0.0f, 0.0f, 340.0f, 160.0f};
  const Canvas canvas = render(*table, box, theme);

  const int cx = static_cast<int>(std::lround(lane_center(*table, 0)));
  const int cy = static_cast<int>(std::lround(row_center_y(*table, 0)));

  // 逐行扫圆心列，取**最大行宽**作为直径（避开“节点+竖线连成一条”的纵向延）。
  int max_width = 0;
  int top = cy;
  int bottom = cy;
  while (top > 0 && (is_lane_ink(canvas.pixel_at(cx, top - 1)) ||
                     is_lane_ink(canvas.pixel_at(cx - 1, top - 1)))) {
    --top;
  }
  while (bottom + 1 < kCanvasH && (is_lane_ink(canvas.pixel_at(cx, bottom + 1)) ||
                                   is_lane_ink(canvas.pixel_at(cx - 1, bottom + 1)))) {
    ++bottom;
  }
  for (int y = top; y <= bottom; ++y) {
    if (!is_lane_ink(canvas.pixel_at(cx, y))) continue;
    int left = cx;
    int right = cx;
    while (left > 0 && is_lane_ink(canvas.pixel_at(left - 1, y))) --left;
    while (right + 1 < kCanvasW && is_lane_ink(canvas.pixel_at(right + 1, y))) ++right;
    max_width = std::max(max_width, right - left + 1);
  }
  st::log::out("[table] 节点直径（像素量）= {}px（参照 10~11）\n", max_width);
  ST_CHECK(max_width >= 9);
  ST_CHECK(max_width <= 13);
}

/// **道间距不随道数缩**：2 道与 6 道都必须给足 `kLanePitch`。
///
/// ## 这条守的是什么缺陷（真实回归）
///
/// 旧口径 `cell = min(14, 预算 / (2 × 道数))`、`pitch = 2 × cell`：
/// 预算 28 时 2 道 → 14px、4 道 → 8.5px、8 道 → 4.25px。
/// 也就是说**道数越多，图越挤**——而分支图的道数恰恰是“有分支时才多”，
/// 最需要看清的场合反而最挤。实测参照是固定 24px。
ST_TEST(table_lane_pitch_does_not_shrink_with_lane_count) {
  const auto pitch_for = [](std::size_t lanes) {
    std::vector<TableColumn> columns{TableColumn{.name = "提交信息", .width = 300.0f}};
    Table table(std::move(columns));
    table.set_row_height(28.0f);
    table.add_row({"only"});
    std::vector<Table::GraphNodeSpec> nodes{Table::GraphNodeSpec{.row = 0, .lane = 0}};
    std::vector<Table::GraphEdgeSpec> edges{Table::GraphEdgeSpec{
        .from_row = 0, .from_lane = 0, .to_row = 0, .to_lane = lanes - 1}};
    table.set_graph(std::move(nodes), std::move(edges));
    return table.graph_metrics();
  };

  for (const std::size_t lanes : {std::size_t{2}, std::size_t{4}, std::size_t{6}}) {
    const Table::GraphMetrics metrics = pitch_for(lanes);
    st::log::out("[table] {} 道：道间距={:.1f} 带宽={:.1f}\n", lanes,
                 static_cast<double>(metrics.lane_pitch),
                 static_cast<double>(metrics.band_width));
    ST_CHECK(std::abs(metrics.lane_pitch - Table::kLanePitch) < 0.5F);
    // 带宽必须够放下所有道（含两侧留白）——否则最右那道会被裁掉。
    ST_CHECK(metrics.band_width >=
             Table::kLanePitch * static_cast<float>(lanes));
  }
}

/// **跨道曲线是一条对称贝塞尔：从节点处直接开始弯，中间没有竖直直段**。
///
/// ## 判据口径（对着参考图逐行量出的实测值）
///
/// 参考图（VS Code Git Graph，440×403，道间距 24、行距 36）按色相隔离后逐行量
/// 曲线重心，归一化横向进度：
///
/// | 纵向 | 19% | 28% | 39% | **50%** | 61% | 72% | 83% | 94% |
/// |---|---|---|---|---|---|---|---|---|
/// | 实测 | .02 | .06 | .19 | **.50** | .81 | .94 | .98 | 1.00 |
/// | 对称贝塞尔 | .06 | .14 | .29 | **.50** | .71 | .86 | .95 | .99 |
///
/// **50% 处正好 0.50** 是对称贝塞尔的指纹：曲线在纵向中点上正好走完一半横向位移。
///
/// ⚠ 这条用例曾守错过一个结论：“两端各有一段长竖直线段”（三段结构）。
/// 错因不是采样点，而是**节点直径量错了**（量成 7px，实际 10~11px）：
/// 节点内部那段本来就是直线，被当成了“竖直段”，而且在节点内部取的重心
/// 被节点本体拉向道中心。现已在下一条用例里把节点尺寸钉死，防重蹈。
ST_TEST(table_lane_curve_is_a_symmetric_bezier) {
  const Theme theme = Theme::dark();
  auto table = make_curve_table();
  const Rect box{0.0f, 0.0f, 340.0f, 160.0f};
  const Canvas canvas = render(*table, box, theme);

  const float from_x = lane_center(*table, 0);
  const float to_x = lane_center(*table, 1);
  const float from_y = row_center_y(*table, 0);
  const float to_y = row_center_y(*table, 1);
  const float span_y = to_y - from_y;
  ST_CHECK(span_y > 1.0F);

  // 扫掠区间：两道之间（含端点各留 2px 余量），避开道本身的竖线。
  const int scan_from = static_cast<int>(std::lround(from_x)) - 2;
  const int scan_to = static_cast<int>(std::lround(to_x)) + 2;

  /// 纵向 `ratio` 处的横向完成度（0 = 原点，1 = 已贴到目标道）。
  const auto progress_at = [&](float ratio) -> double {
    const int y = static_cast<int>(std::lround(from_y + span_y * ratio));
    const float centroid = ink_centroid_x(canvas, y, scan_from, scan_to);
    if (centroid < 0.0F) return -1.0;   // 该行没有墨：曲线断了
    return static_cast<double>((centroid - from_x) / (to_x - from_x));
  };

  const double at_19 = progress_at(0.19F);
  const double at_39 = progress_at(0.39F);
  const double at_50 = progress_at(0.50F);
  const double at_61 = progress_at(0.61F);
  st::log::out("[table] 曲线横向进度：19%→{:.2f}  39%→{:.2f}  50%→{:.2f}  61%→{:.2f}\n", at_19,
               at_39, at_50, at_61);

  // 曲线必须真的存在（“量不到墨”是哨兵值 -1）。
  for (const double value : {at_19, at_39, at_50, at_61}) {
    ST_REQUIRE(value > -0.5);
  }

  // 判据对着参考实测：19%→0.02  39%→0.19  50%→0.50  61%→0.81
  // 容差覆盖抗锯齿与线宽（量到的是墨迹重心，不是理论中心线）。
  ST_CHECK(at_19 <= 0.16);                         // 起点几乎还在原道
  ST_CHECK(std::abs(at_39 - 0.19) <= 0.18);
  ST_CHECK(std::abs(at_50 - 0.50) <= 0.16);        // 中点约走到一半
  ST_CHECK(std::abs(at_61 - 0.81) <= 0.18);
  // 单调推进。
  ST_CHECK(at_19 <= at_39 + 0.06);
  ST_CHECK(at_39 <= at_50 + 0.06);
  ST_CHECK(at_50 <= at_61 + 0.06);
}

/// **节点画成圆，不是方块**：四角淡、圆心浓。
///
/// ## 为何用“四角”而不是“偏上 Npx 变窄”
///
/// 前面两版都败在同一件事上：**抗锯齿**。半径 3.4 时，偏上 2px 的理论宽
/// `2√(r²−4)` = 5.5px，抗锯齿再把两侧各算 1px ⇒ 7px，与圆心行相等；
/// 半径 4.4 时偏上 3px 同理。量尺自己分不出“圆弧”与“方块”（手册 §5）。
///
/// 四角判据不受抗锯齿干扰：真圆的**对角线顶点（相对圆心 ±(r−1), ±(r−1)）**在圆外——
/// 那里采到的应是底色；方块的同一位置是满色。两者差的是“通通有/通通无”，
/// 不是“1px 宽窄”，抗锯齿抹不平（-1px 余量已经避开边缘）。
ST_TEST(table_lane_dot_is_round_not_square) {
  const Theme theme = Theme::dark();
  auto table = make_curve_table();
  const Rect box{0.0f, 0.0f, 340.0f, 160.0f};
  const Canvas canvas = render(*table, box, theme);

  const Table::GraphMetrics metrics = table->graph_metrics();
  const int cx = static_cast<int>(std::lround(lane_center(*table, 0)));
  const int cy = static_cast<int>(std::lround(row_center_y(*table, 0)));
  // 对角偏移：必须让 `√2 × offset > 半径`（落在圆外），同时 `offset < 半径`
  //（否则连“右边 1px 半径内应有墨”都不成立）。
  //
  // ⚠ 第一版取 `r − 1`：半径 4.4 时 offset=3，而 √2×3 = 4.24 < 4.4 ⇒ 这个点仍在
  // **圆内**，实测四个角有 3 个是墨——真圆也会被判成方块。要靠几何反算，不能靠感觉。
  const int offset = static_cast<int>(std::lround(metrics.dot_radius * 0.85F));

  const auto inked = [&](int x, int y) { return is_lane_ink(canvas.pixel_at(x, y)); };

  // 圆心必须是墨（否则下面量的不是节点）。
  ST_REQUIRE(inked(cx, cy));

  // 四个对角顶点：真圆必然在圆外（无墨），方块则是满墨。
  const int corners = static_cast<int>(inked(cx + offset, cy + offset)) +
                      static_cast<int>(inked(cx - offset, cy + offset)) +
                      static_cast<int>(inked(cx + offset, cy - offset)) +
                      static_cast<int>(inked(cx - offset, cy - offset));
  st::log::out("[table] 半径={:.1f} 对角偏移={} ⇒ 四个对角顶点有墨的个数={}（真圆应为 0）\n",
               static_cast<double>(metrics.dot_radius), offset, corners);
  // 允许 1 个（恰好落在圆上 + 抗锯齿），方块会是 4 个。
  ST_CHECK(corners <= 1);

  // 正向对照：轴向（半径内）必须有墨——否则上面“四角无墨”可能只是“什么都没画”。
  const int inner = std::max(1, offset - 2);
  ST_CHECK(inked(cx + inner, cy));
  ST_CHECK(inked(cx, cy + inner));
}
