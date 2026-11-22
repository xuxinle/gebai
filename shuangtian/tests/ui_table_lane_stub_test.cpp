/// `Table` 行首连线图：**节点-边**模型的几何与颜色契约。
///
/// ## 主约束（用户直接给的判据）
///
/// **圆点是节点，任何线都要在节点之间，不要有单独的线头。**
///
/// 这条约束决定了绘制结构：不能“按行把 `|` 画成竖线”，而必须
/// 先找出节点，再只在**同一道上相邻的两个节点**之间连线。
/// 按行画必然出两类毛病：① 可视区首/末行的线悬空；② 点与线各自独立画出，
/// 观感是“点线拼接”。
///
/// ## 历史缺陷（这条测试当初盯的就是它）
///
/// 节点的绘制原本无条件画「上半行竖线 + 下半行竖线」：
///
/// ```cpp
/// fill_rect({cx-…, row_box.y, w, h*0.5f});   // 上半段
/// fill_rect({cx-…, cy,        w, h*0.5f});   // 下半段
/// ```
///
/// 这两段线的语义是“把本行与**相邻行**连起来”——**首行上面没有行**。
/// 于是第一行上方凭空多出一截悬空线头，而它是画进表体自身矩形里的，
/// 直接从表头裁切边界向下延伸：画面上像“表头里钻出来一条线”。
/// 末行同理（下方没有行）。
///
/// ## 取样框为何要“避开圆点”
///
/// 圆点半径最大 4.2px，会从行中线向上伸入「上半段」区间。若取样框覆盖整段
/// 上半行，量到的墨点里**大头是圆点自己**（实测：首行 26 点，其中圆点占绝大多数），
/// 于是“线头没了”与“线头还在”都会被圆点的墨量盖住——这正是「绿而无效」的
/// 一种形态（尺子量到了别的东西）。所以取样框只取**圆点顶部之上**那一窄条：
/// 那是纯粹的线头区域（有竖线就必然有墨，没竖线就必然干净）。

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
/// 圆点半径夹取上限（与 `Table::kLaneDotRadiusMax` 同一入口，不写死数字）。
constexpr float kDotRadiusMax = Table::kLaneDotRadiusMax;

/// 每行一个节点、全在 0 道，相邻行之间一条同道边——最朴素、也最容易看清线头的数据。
[[nodiscard]] auto make_lane_table(std::size_t rows) -> std::unique_ptr<Table> {
  std::vector<TableColumn> columns{TableColumn{.name = "提交信息", .width = 300.0f}};
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_row_height(20.0f);
  std::vector<Table::GraphNodeSpec> nodes;
  std::vector<Table::GraphEdgeSpec> edges;
  for (std::size_t index = 0; index < rows; ++index) {
    table->add_row({std::format("commit {}", index)});
    nodes.push_back(Table::GraphNodeSpec{.row = index, .lane = 0});
    // 相邻行之间一条同道边：`rows` 个节点恰好 `rows-1` 条边。
    if (index > 0) {
      edges.push_back(Table::GraphEdgeSpec{
          .from_row = index - 1, .from_lane = 0, .to_row = index, .to_lane = 0});
    }
  }
  table->set_graph(std::move(nodes), std::move(edges));
  return table;
}

[[nodiscard]] auto make_backdrop() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kCanvasW, kCanvasH, 1.0f);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

/// 泳道带是**彩色**一系（色相随道号变），与表头的灰底/正文黑都能分开：
/// 判据是“三通道里最大值与最小值差距够大”（灰阶像素的该差值接近 0）。
[[nodiscard]] auto is_lane_ink(const Color& c) -> bool {
  const int maximum = std::max({static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b)});
  const int minimum = std::min({static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b)});
  return maximum - minimum > 40;
}

[[nodiscard]] auto lane_ink_count(const Canvas& canvas, Rect region) -> int {
  int total = 0;
  for (int y = static_cast<int>(region.y); y < static_cast<int>(region.bottom()); ++y) {
    for (int x = static_cast<int>(region.x); x < static_cast<int>(region.right()); ++x) {
      if (is_lane_ink(canvas.pixel_at(x, y))) ++total;
    }
  }
  return total;
}

/// 行上半段里**圆点顶部之上**那一窄条（纯线头区）。
[[nodiscard]] auto stub_band_above(const Table& table, std::size_t row) -> Rect {
  const Rect box = table.row_rect(row);
  const float center = box.y + box.height * 0.5f;
  const float top = center - kDotRadiusMax - 1.0f;   // 圆点上缘再让 1px
  return Rect{box.x, box.y, 24.0f, std::max(1.0f, top - box.y)};
}

/// 行下半段里**圆点底部之下**那一窄条。
[[nodiscard]] auto stub_band_below(const Table& table, std::size_t row) -> Rect {
  const Rect box = table.row_rect(row);
  const float center = box.y + box.height * 0.5f;
  const float start = center + kDotRadiusMax + 1.0f;
  return Rect{box.x, start, 24.0f, std::max(1.0f, box.bottom() - start)};
}

[[nodiscard]] auto render(Table& table, Rect box, const Theme& theme) -> Canvas {
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table.apply_theme(theme);
  table.arrange(context, box);
  Canvas canvas = make_backdrop();
  table.paint(context, canvas);
  return canvas;
}

/// 某一道中心的 x（**与绘制端同源**：读 `graph_metrics()`，不写死偏移）。
///
/// 为何不手算：旧写法把 `band_x + 4 + cell*0.5` 写死，而几何一改（道间距从
/// “分摊”改成固定 24）它就默默指到别处——量到空白，表现为“线悬空”。
[[nodiscard]] auto lane_center_x(const Table& table, std::size_t lane) -> float {
  const Table::GraphMetrics metrics = table.graph_metrics();
  const Rect row = table.row_rect(0);
  return row.x + Table::kLaneBandPadding + metrics.lane_pitch * static_cast<float>(lane);
}

}  // namespace

/// **首行圆点上方不许有墨迹**（表头裁切边界到首行圆点之间）。
ST_TEST(table_first_row_lane_has_no_dangling_stub_above) {
  const Theme theme = Theme::dark();
  auto table = make_lane_table(12);
  const Rect box{0.0f, 0.0f, 340.0f, 200.0f};
  const Canvas canvas = render(*table, box, theme);

  const Rect band = stub_band_above(*table, 0);
  const int ink = lane_ink_count(canvas, band);
  st::log::out("[table] 首行上方线头带 = y[{:.1f}..{:.1f}] 墨量 = {}\n",
               static_cast<double>(band.y), static_cast<double>(band.bottom()), ink);
  ST_CHECK_EQ(ink, 0);
}

/// 反向：**中间行仍然要有连线**（别把“修线头”做成“把线全删了”）。
ST_TEST(table_middle_row_lane_still_connects_upwards) {
  const Theme theme = Theme::dark();
  auto table = make_lane_table(12);
  const Rect box{0.0f, 0.0f, 340.0f, 200.0f};
  const Canvas canvas = render(*table, box, theme);

  const Rect band = stub_band_above(*table, 5);
  const int ink = lane_ink_count(canvas, band);
  st::log::out("[table] 第 5 行上方线头带墨量 = {}\n", ink);
  ST_CHECK(ink > 0);
}

/// 末行圆点下方同样不许悬空（下方没有行可连）。
ST_TEST(table_last_row_lane_has_no_dangling_stub_below) {
  const Theme theme = Theme::dark();
  auto table = make_lane_table(12);
  const Rect box{0.0f, 0.0f, 340.0f, 200.0f};
  const Canvas canvas = render(*table, box, theme);

  const Rect band = stub_band_below(*table, 11);
  const int ink = lane_ink_count(canvas, band);
  st::log::out("[table] 末行下方线头带墨量 = {}\n", ink);
  ST_CHECK_EQ(ink, 0);
}

/// **颜色只错色相**：任意两道在 16 道内都必须可区分，且**饱和度/明度一致**。
///
/// 判据用「RGB 距离」而非“色相不同”：色相算出来的差值好看，但两个色相若落在
/// 同样的明暗区间，画成细线后屏幕上仍是同一团颜色。
///
/// 同时验证“只错色相”这条口径：各道饱和度与明度必须**相等**——
/// 否则又回到了“某些道被置灰”的观感（那正是改掉三维轮转的原因）。
ST_TEST(table_lane_colors_are_generated_without_cap) {
  const Theme theme = Theme::dark();
  // 16 道远超旧的 3 色循环——旧实现下第 0/3/6/9… 道必然同色。
  constexpr std::size_t kLanes = 16;
  std::vector<TableColumn> columns{TableColumn{.name = "提交信息", .width = 300.0f}};
  Table table(std::move(columns));
  table.set_row_height(20.0f);
  table.add_row({"only"});
  // 只要把泳道带宽撑到 16 道（`lane_count_` 由最大道号决定），不必真的有 16 个节点。
  std::vector<Table::GraphNodeSpec> nodes{Table::GraphNodeSpec{.row = 0, .lane = 0}};
  std::vector<Table::GraphEdgeSpec> edges{Table::GraphEdgeSpec{
      .from_row = 0, .from_lane = 0, .to_row = 0, .to_lane = kLanes - 1}};
  table.set_graph(std::move(nodes), std::move(edges));

  const Canvas canvas = render(table, Rect{0.0f, 0.0f, 400.0f, 120.0f}, theme);
  (void)canvas;

  // 直接验生成函数：像素采样只能量到抗锯齿边沿色，会把真实色距会得小。
  const auto& palette = theme.colors();
  std::vector<Color> colors;
  colors.reserve(kLanes);
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    colors.push_back(Table::lane_color(lane, palette));
  }

  // 两两距离：任意两道都不能“看起来同色”。
  int closest = 1 << 20;
  std::size_t closest_pair = 0;
  for (std::size_t i = 0; i < colors.size(); ++i) {
    for (std::size_t j = i + 1; j < colors.size(); ++j) {
      const int dr = static_cast<int>(colors[i].r) - static_cast<int>(colors[j].r);
      const int dg = static_cast<int>(colors[i].g) - static_cast<int>(colors[j].g);
      const int db = static_cast<int>(colors[i].b) - static_cast<int>(colors[j].b);
      const int distance = std::abs(dr) + std::abs(dg) + std::abs(db);
      if (distance < closest) {
        closest = distance;
        closest_pair = i * 100 + j;
      }
    }
  }
  st::log::out("[table] {} 道最接近的一对为 #{} / #{}，RGB 距离 = {}\n", kLanes,
               closest_pair / 100, closest_pair % 100, closest);
  // 60 是“细线在屏幕上仍能一眼分开”的经验下限（同色时距离为 0）。
  ST_CHECK(closest > 60);

  // —— “只错色相”这条口径本身要钉住 ——
  //
  // 若哪天又改成“明度/饱和也轮转”，上面那条色距断言依然能绿（距离够远），
  // 但观感会回到“某些道被置灰”——本条就是那个回归的护栏。
  // 判据：除第 0 道（主题主色，刻意不同）外，其余各道的**饱和度与明度相同的**。
  const auto [h0, s0, l0] = colors[1].to_hsl();
  (void)h0;
  for (std::size_t lane = 2; lane < kLanes; ++lane) {
    const auto [hue, saturation, lightness] = colors[lane].to_hsl();
    (void)hue;
    if (std::abs(saturation - s0) > 0.02F || std::abs(lightness - l0) > 0.02F) {
      st::log::out("[table] 道 #{} 的饱和度/明度与道 #1 不同：{:.2f}/{:.2f} vs {:.2f}/{:.2f}\n",
                   lane, static_cast<double>(saturation), static_cast<double>(lightness),
                   static_cast<double>(s0), static_cast<double>(l0));
      ST_CHECK(false);
    }
  }
  st::log::out("[table] 各道饱和度={:.2f} 明度={:.2f}（统一 ⇒ 只错色相）\n",
               static_cast<double>(s0), static_cast<double>(l0));
}

/// **线不悬空**：可视区里每个节点的上下两侧（除首/末节点外）都必须有线相连。
///
/// 这是用户给的判据“任何线都要在节点之间”的直接反向断言：如果哪个节点上方
/// 或下方是空的，那段线就是孤立的——要么是悬空线头，要么是“点线拼接”。
///
/// 为何用“节点上下两侧取样”而不是“全图墨量”：墨量只能告诉你“有东西”，
/// 而这里要问的是“东西**接在节点上**”——它是个拓扑判断。
ST_TEST(table_lane_edges_touch_their_nodes) {
  const Theme theme = Theme::dark();
  auto table = make_lane_table(12);   // 12 个节点、同一道
  const Rect box{0.0f, 0.0f, 340.0f, 260.0f};
  const Canvas canvas = render(*table, box, theme);

  // 取第 1 道（lane 0）的中心 x：**从 `lane_x` 本身的定义算**，不写死偏移。
  //
  // ⚠ 旧写法是 `row0.x + 4.0f + 7.0f`（那时带宽预算 28、道间距 14）：
  // 改成固定道间距 24 后道中心移到 `x + kLaneBandPadding`，旧写法偏了 3px，
  // 于是拿一个偏离圆心的竖列去量“上方有没有线”——量到的全是空白，假红。
  const int cx = static_cast<int>(std::lround(lane_center_x(*table, 0)));

  /// 某行中线上方/下方 4px 处是否有泳道墨迹。
  const auto has_ink_at = [&](float y) {
    for (int dx = -2; dx <= 2; ++dx) {
      if (is_lane_ink(canvas.pixel_at(cx + dx, static_cast<int>(y)))) return true;
    }
    return false;
  };

  // 第 4 行：**两个方向都必须有线**（它既不是首节点也不是末节点）。
  const Rect row4 = table->row_rect(4);
  const float cy4 = row4.y + row4.height * 0.5f;
  const bool above4 = has_ink_at(cy4 - 8.0f);
  const bool below4 = has_ink_at(cy4 + 8.0f);
  st::log::out("[table] 节点 #4 上方有线={} 下方有线={}\n", above4, below4);
  ST_CHECK(above4);
  ST_CHECK(below4);

  // 首节点的**上方**不许有墨（它是线的起点，不是“悬空线头”）。
  const Rect row0b = table->row_rect(0);
  const float cy0 = row0b.y + row0b.height * 0.5f;
  const bool above0 = has_ink_at(cy0 - 8.0f);
  st::log::out("[table] 首节点上方有线={}\n", above0);
  ST_CHECK(!above0);
}
