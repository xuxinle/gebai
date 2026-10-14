/// `Table` 视口剔除与视口契约的回归护栏。
///
/// ## 这组用例守的是什么
///
/// `Table::paint_content` 里有一个**看似**能救场的提前退出：
/// `if (row.y >= bounds_.bottom()) break;`——但宿主把 `Table` 放进 `ScrollView` 时，
/// `bounds_` 拿到的是**内容全高**，于是这个 break 在整个滚动过程中**一次都不触发**，
/// 仍然迭代全部行 × 全部列，每格调 `ellipsize` + `measure_width`。
///
/// 实测（`tools/ui_table_rows_bench.cpp`，4 列 / 400px 视口）：
///
/// | 行数 | 改前（bounds=内容全高） | 改后 |
/// |---|---|---|
/// | 1000 | 8.769 ms | 0.505 ms |
/// | 5000 | 113.274 ms | 0.502 ms |
///
/// 而对照（bounds 恰为视口高）一直是 0.496 ms——**同一个 break，178× 的差距**
/// 全在于它看不到视口。这条差距是本组用例的存在理由。
///
/// ## 为什么用"画了哪些单元格"而不是耗时来断言
///
/// 与 `ui_tree_viewport_test.cpp` 同：耗时机器相关；剔除错了的表现是"某块表没画出来"，
/// 而本项目跨运行像素不确定（逐像素 A/B 不可用，见 `tests/ui_culling_test.cpp` 开头）。
/// 所以这里数 `draw` 调用（画了几格文本）——可复现、且直接对应"付了几次
/// `ellipsize`/`measure_width`"。

#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::raster::Canvas;
using st::test::FixedAdvanceTextPort;
using st::ui::Constraints;
using st::ui::RenderContext;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::TextAlign;
using st::ui::Theme;

inline constexpr int kCanvasWidth = 600;
inline constexpr int kViewportHeight = 400;

[[nodiscard]] auto make_columns(std::size_t count) -> std::vector<TableColumn> {
  std::vector<TableColumn> columns;
  columns.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    columns.push_back(
        TableColumn{.name = "c" + std::to_string(index), .width = 140.0F, .align = TextAlign::Start});
  }
  return columns;
}

[[nodiscard]] auto make_rows(std::size_t row_count, std::size_t column_count)
    -> std::vector<std::vector<std::string>> {
  std::vector<std::vector<std::string>> rows;
  rows.reserve(row_count);
  for (std::size_t row = 0; row < row_count; ++row) {
    std::vector<std::string> cells;
    cells.reserve(column_count);
    for (std::size_t column = 0; column < column_count; ++column) {
      cells.push_back("r" + std::to_string(row) + "c" + std::to_string(column));
    }
    rows.push_back(std::move(cells));
  }
  return rows;
}

/// 绘制夹具：真实 `Canvas` + 定宽文本桩（记录画过的文本）。
struct PaintFixture {
  Theme theme{Theme::light()};
  FixedAdvanceTextPort port{};
  Canvas canvas{kCanvasWidth, kViewportHeight};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }

  /// 布局到 `bounds`（真实 ScrollView 给的是**内容全高**），绘制一次，
  /// 返回本次新画过的文本条数。
  [[nodiscard]] auto paint_at(Table& table, float bounds_height) -> std::size_t {
    table.measure(context(), Constraints{});
    table.arrange(context(),
                  Rect{0.0F, 0.0F, static_cast<float>(kCanvasWidth), bounds_height});
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const std::size_t before = port.drawn().size();
    table.paint(context(), canvas);
    return port.drawn().size() - before;
  }
};

[[nodiscard]] auto content_height(std::size_t rows, const Table& table) -> float {
  return static_cast<float>(rows) * table.row_height() + Table::kHeaderHeight + 8.0F;
}

}  // namespace

/// **真实场景**：bounds = 内容全高（ScrollView 给的）时，只画视口内那几行的单元格。
///
/// 这条是本组用例的核心——它直接对应 `ui_table_rows_bench` 量出的 113 ms → 0.50 ms。
ST_TEST(table_paint_draws_only_rows_inside_viewport) {
  PaintFixture fixture;
  Table table{make_columns(4)};
  for (auto& row : make_rows(1000, 4)) table.add_row(std::move(row));

  const std::size_t painted = fixture.paint_at(table, content_height(1000, table));

  // 视口 400px / 行高 32px ≈ 12 行；4 列 + 表头 4 格。远小于 1000×4 = 4000。
  ST_CHECK(painted <= 80U);
  ST_CHECK(painted >= 40U);  // 且不能少画到"表格中间有洞"
}

/// 行数从 100 涨到 4000，**每帧绘制量不跟着涨**——剔除的结构性证据。
ST_TEST(table_paint_cost_is_independent_of_total_row_count) {
  const auto painted_for = [](std::size_t rows) {
    PaintFixture fixture;
    Table table{make_columns(4)};
    for (auto& row : make_rows(rows, 4)) table.add_row(std::move(row));
    return fixture.paint_at(table, content_height(rows, table));
  };
  const std::size_t small = painted_for(100U);
  const std::size_t large = painted_for(4000U);
  // 40 倍行数差，绘制量必须完全一致。
  ST_CHECK_EQ(small, large);
}

/// 上下滚动后只画可见的那一段（bounds 被 ScrollView 挪成负 y）。
ST_TEST(table_paint_follows_scrolled_bounds) {
  PaintFixture fixture;
  Table table{make_columns(4)};
  for (auto& row : make_rows(500, 4)) table.add_row(std::move(row));

  table.measure(fixture.context(), Constraints{});
  const float height = content_height(500, table);
  // 滚到第 100 行：行顶 = bounds.y + kHeaderHeight + 100×32，要让它落在视口上沿。
  const float scrolled_y =
      -static_cast<float>(100) * table.row_height() - Table::kHeaderHeight;
  table.arrange(fixture.context(),
                Rect{0.0F, scrolled_y, static_cast<float>(kCanvasWidth), height});
  fixture.canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
  const std::size_t before = fixture.port.drawn().size();
  table.paint(fixture.context(), fixture.canvas);
  const std::size_t painted = fixture.port.drawn().size() - before;

  ST_CHECK(painted <= 80U);  // 仍然只有视口那几行
  // 首可见行应当报到 100 附近（这才是"契约通电"）。
  ST_CHECK(table.visible_row_range_now().first >= 99U);
  ST_CHECK(table.visible_row_range_now().first <= 101U);
}

/// `first_visible` 是真值（上一帧首可见行），`visible_rows` 是视口容量。
ST_TEST(table_viewport_contract_is_wired) {
  PaintFixture fixture;
  Table table{make_columns(4)};
  for (auto& row : make_rows(200, 4)) table.add_row(std::move(row));

  // 从未绘制过：保守回退 0（不报荒谬数字）。
  ST_CHECK_EQ(table.get_property("first_visible").value_or("?"), std::string("0"));

  (void)fixture.paint_at(table, content_height(200, table));
  const auto [first, last] = table.visible_row_range_now();
  ST_CHECK_EQ(first, std::size_t{0});
  // 400px 视口 - 36px 表头 = 364px → 11 行；区间宽出量不超过 +2。
  ST_CHECK(last >= 11U);
  ST_CHECK(last <= 15U);
  // `visible_rows` 与绘制区间**同源**（不能用 bounds.height 算——那是内容全高）。
  ST_CHECK_EQ(table.get_property("visible_rows").value_or("?"), std::to_string(last - first));
  // 且这个数必须远小于总行数（这才是"用户现在看得到几行"的答案）。
  ST_CHECK((last - first) < 20U);
}

/// 横向滚动时**只画交叠的列**（列剔除；`column_x` 已是滚动后坐标）。
ST_TEST(table_paint_skips_columns_outside_clip) {
  PaintFixture fixture;
  // 20 列 × 140px = 2800px 内容宽，容器 600px → 大量列在视口外。
  Table table{make_columns(20)};
  for (auto& row : make_rows(20, 20)) table.add_row(std::move(row));

  const std::size_t painted = fixture.paint_at(table, content_height(20, table));
  // 可见约 5 列（600/140）+ 表头，绝不是 20×20 = 400。
  ST_CHECK(painted <= 120U);
  ST_CHECK(painted >= 20U);
}

/// 空表不崩、区间安全（只有表头文本会被画）。
ST_TEST(table_visible_range_is_safe_when_empty) {
  PaintFixture fixture;
  Table table{make_columns(3)};
  const std::size_t painted = fixture.paint_at(table, 400.0F);
  ST_CHECK_EQ(painted, 3U);  // 仅表头的 3 格（无数据行）
  const auto [first, last] = table.visible_row_range_now();
  ST_CHECK_EQ(first, std::size_t{0});
  ST_CHECK_EQ(last, std::size_t{0});
  ST_CHECK_EQ(table.get_property("first_visible").value_or("?"), std::string("0"));
}
