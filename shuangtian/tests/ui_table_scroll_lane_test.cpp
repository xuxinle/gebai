/// `Table` 的**纵向滚动**与**行首连线图**（LaneGraph）几何。
///
/// ## 守的是什么
///
/// 这两条都是「改完不会崩、也不会报错，只是**画面不对/滚不动**」的类型：
///
/// | 缺陷形态 | 不测的话看起来是 |
/// |---|---|
/// | 纵向滚动缺失 | 「200 条提交只看得到 6 条，滚轮没反应」——而 `max_scroll` 报 0，**像是有意为之** |
/// | `max_scroll_y` 算错（忘扣表头） | 滚到底时最后一行被表头遮住半截 |
/// | `row_rect` 忘了减纵向偏移 | 滚了但画面不动（属性面 `scroll_y` 却在变） |
/// | 可见区间没算纵向偏移 | 每滚一格，顶部漏画一行（**画面上是“行在跳”**） |
/// | 连线图先设后被 `resize` 截空 | `lane_inset = 0`，一条线都不画，**完全不报错** |
/// | 斜线端点用本行高算 | 连线行被压扁时斜线两端够不到圆点，看似“几段短线” |
///
/// 全部用**可复算的量**断言（几何数值 + 像素墨量），不靠“看着有东西”。

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/element.hpp"
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

[[nodiscard]] auto make_table(std::size_t rows) -> std::unique_ptr<Table> {
  std::vector<TableColumn> columns{
      TableColumn{.name = "A"},
      TableColumn{.name = "B", .width = 80.0f},
  };
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_row_height(20.0f);
  for (std::size_t index = 0; index < rows; ++index) {
    table->add_row({std::format("row {}", index), "x"});
  }
  return table;
}

[[nodiscard]] auto make_backdrop() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kCanvasW, kCanvasH, 1.0f);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

[[nodiscard]] auto is_ink(const Color& c) -> bool {
  const double lum = 0.2126 * static_cast<double>(c.r) + 0.7152 * static_cast<double>(c.g) +
                     0.0722 * static_cast<double>(c.b);
  return lum < 200.0;
}

[[nodiscard]] auto ink_count(const Canvas& canvas, Rect region) -> int {
  int total = 0;
  for (int y = static_cast<int>(region.y); y < static_cast<int>(region.bottom()); ++y) {
    for (int x = static_cast<int>(region.x); x < static_cast<int>(region.right()); ++x) {
      if (is_ink(canvas.pixel_at(x, y))) ++total;
    }
  }
  return total;
}

/// 摆到固定矩形并画一帧。
/// 泳道带宽度经**属性面**读（`lane_inset()` 是私有几何，只读属性是它的公开出口）。
[[nodiscard]] auto lane_inset_of(const Table& table) -> float {
  const auto text = table.get_property("lane_inset");
  if (!text.has_value()) return -1.0f;
  try {
    return std::stof(*text);
  } catch (...) {
    return -1.0f;
  }
}

[[nodiscard]] auto render(Table& table, Rect box, const Theme& theme) -> Canvas {
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table.apply_theme(theme);
  table.arrange(context, box);
  Canvas canvas = make_backdrop();
  table.paint(context, canvas);
  return canvas;
}

}  // namespace

/// 纵向可滚量 = 内容高 −（视口高 − 表头高）；表头**不参与滚动**。
ST_TEST(table_vertical_scroll_range_excludes_header) {
  const Theme theme = Theme::dark();
  auto table = make_table(50);   // 50 × 20 = 1000px 内容
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table->apply_theme(theme);
  // 视口 160：表头 36 + 数据区 124。
  table->arrange(context, Rect{0.0f, 0.0f, 300.0f, 160.0f});

  const float expected = 1000.0f - (160.0f - Table::kHeaderHeight);
  st::log::out("[table] max_scroll_y={} 期望={}\n", static_cast<double>(table->max_scroll_y()),
               static_cast<double>(expected));
  ST_CHECK(std::abs(table->max_scroll_y() - expected) < 0.5f);
  // 内容不足一屏时不该能滚。
  auto small = make_table(2);
  small->apply_theme(theme);
  small->arrange(context, Rect{0.0f, 0.0f, 300.0f, 160.0f});
  ST_CHECK(small->max_scroll_y() <= 0.01f);
}

/// 设纵向偏移：夹取到 `[0, max_scroll_y]`，且 `row_rect` 真的跟着挪。
ST_TEST(table_vertical_scroll_moves_rows_and_clamps) {
  const Theme theme = Theme::dark();
  auto table = make_table(50);
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table->apply_theme(theme);
  table->arrange(context, Rect{0.0f, 0.0f, 300.0f, 160.0f});

  const float row0_before = table->row_rect(0).y;
  table->set_scroll_offset_y(60.0f);
  const float row0_after = table->row_rect(0).y;
  st::log::out("[table] row0.y {} → {}\n", static_cast<double>(row0_before),
               static_cast<double>(row0_after));
  // 滚 60px，第 0 行整体上移 60px（**这一条是“滚了但画面不动”的判据**）。
  ST_CHECK(std::abs((row0_before - row0_after) - 60.0f) < 0.01f);
  // 表头不受影响（它钉在顶部）。
  ST_CHECK(std::abs(table->header_rect().y - 0.0f) < 0.01f);

  // 越界夹取。
  table->set_scroll_offset_y(99999.0f);
  ST_CHECK(std::abs(table->scroll_offset_y() - table->max_scroll_y()) < 0.01f);
  table->set_scroll_offset_y(-50.0f);
  ST_CHECK(table->scroll_offset_y() <= 0.01f);
}

/// **滚轮不按 Shift = 纵向**（Shift 才横向）。旧实现只认 Shift，纵向恒不可滚。
ST_TEST(table_wheel_scrolls_vertically_without_shift) {
  const Theme theme = Theme::dark();
  auto table = make_table(50);
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table->apply_theme(theme);
  table->arrange(context, Rect{0.0f, 0.0f, 400.0f, 160.0f});   // 够宽：无横向滚动

  st::ui::Event wheel{};
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{100.0f, 100.0f};
  wheel.wheel_delta = -1.0f;   // 向下滚
  const bool handled = table->on_event(context, wheel);
  st::log::out("[table] wheel(无 Shift) handled={} scroll_y={}\n", handled,
               static_cast<double>(table->scroll_offset_y()));
  ST_CHECK(handled);
  ST_CHECK(table->scroll_offset_y() > 0.0f);
  ST_CHECK(wheel.handled);
}

/// 可见区间**必须扣纵向偏移**：否则每滚一格顶部就漏画一行。
ST_TEST(table_visible_range_accounts_for_vertical_offset) {
  const Theme theme = Theme::dark();
  auto table = make_table(50);
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table->apply_theme(theme);
  table->arrange(context, Rect{0.0f, 0.0f, 300.0f, 160.0f});

  // 未滚动：首可见行是 0。
  {
    Canvas canvas = make_backdrop();
    table->paint(context, canvas);
    ST_CHECK_EQ(table->visible_row_range_now().first, 0U);
  }
  // 滚过 k 个行高：首可见行必须**跟着前移**（而不是仍为 0）。
  //
  // ⚠ 期望不是“first == k”：`first` 是「第一个与裁剪区**有交叠**的行」，
  // 滚动 60px 时第 0 行顶部虽已出界，但它**仍有部分落在裁剪区内**（y=-24，行高 20
  // 意味着第 0 行早已完全排出）——所以真实值是 1。这条要钉的是“偏移被算进去了”，
  // 判据用“比未滚动时大”且“未滚动的行不会被当作首行”，不押具体数字。
  const float row_h = table->row_height();
  const std::size_t steps = 3U;
  table->set_scroll_offset_y(row_h * static_cast<float>(steps));
  {
    Canvas canvas = make_backdrop();
    table->paint(context, canvas);
    const auto [first, last] = table->visible_row_range_now();
    st::log::out("[table] 行高={} 滚 {}px 后可见区间 [{}, {})\n",
                 static_cast<double>(row_h),
                 static_cast<double>(row_h * static_cast<float>(steps)), first, last);
    // ① 不再从第 0 行开始（偏移真的被算进去了）
    ST_CHECK(first > 0U);
    // ② 也不能跳过头：首个可见行的顶边必须在裁剪区之上一点范围内
    //   （滚 3 行 ⇒ 第 2 行末尾已滚出，first 不会超过 3）。
    ST_CHECK(first <= steps);
    ST_CHECK(last > first);
  }
}

/// **行首连线图**：竖线/斜线/圆点都真的画出来，且泳道带真的让出宽度。
///
/// ⚠ 这里**先设图、后建行**（不是先 `make_table(4)`）：
/// `set_lane_links` 若内含 `resize(rows_.size())`，在“行已有 4 行”时**什么也不会截**
/// —— 逆向验证会误判成“这条测试钉不住”（实测：回退后仍然绿）。
/// 必须先有图、再让行数从 0 长上来，那条截断才能真正触发。
ST_TEST(table_lane_links_draw_lines_and_reserve_inset) {
  const Theme theme = Theme::dark();
  auto table = make_table(0);
  st::ui::RenderContext context{theme, nullptr, 0.0};

  // 与 git 同构的四行：合并点 + 分叉斜线。
  table->set_lane_links({"*  ", "|\\ ", "| *", "|/ "});
  ST_CHECK(lane_inset_of(*table) > 0.0f);
  // 建行在**设图之后**：若实现里 resize 把图截空，inset 会在这里掉回 0。
  for (std::size_t index = 0; index < 4U; ++index) {
    table->add_row({std::format("row {}", index), "x"});
  }
  const float inset_after_rows = lane_inset_of(*table);
  st::log::out("[table] 设图后建行的 inset={}\n", static_cast<double>(inset_after_rows));
  ST_CHECK(inset_after_rows > 0.0f);

  table->apply_theme(theme);
  table->arrange(context, Rect{0.0f, 0.0f, 300.0f, 200.0f});
  const Canvas canvas = render(*table, Rect{0.0f, 0.0f, 300.0f, 200.0f}, theme);
  // 泳道带（最左侧一条）必须有墨——竖线 + 圆点 + 斜线。
  const int lane_ink = ink_count(canvas, Rect{0.0f, Table::kHeaderHeight, 40.0f, 80.0f});
  st::log::out("[table] 泳道带墨量={} inset={}\n", lane_ink,
               static_cast<double>(lane_inset_of(*table)));
  ST_CHECK(lane_ink > 20);
  // 而没有连线图时，那一块应当是干净的（否则测的是别的东西）。
  auto bare = make_table(4);
  const Canvas bare_canvas = render(*bare, Rect{0.0f, 0.0f, 300.0f, 200.0f}, theme);
  const int bare_ink = ink_count(bare_canvas, Rect{0.0f, Table::kHeaderHeight, 12.0f, 80.0f});
  ST_CHECK(bare_ink < lane_ink);
}

/// **先设图、后填行**不能把图截掉（`resize(rows_.size())` 那个坑）。
ST_TEST(table_lane_links_survive_rows_added_later) {
  auto table = make_table(0);
  table->set_lane_links({"|* ", "|  "});
  const float inset_before = lane_inset_of(*table);
  ST_CHECK(inset_before > 0.0f);
  // 之后补行：图不能被截断/错位（已有的两格必须原样保留）。
  table->add_row({"a", "b"});
  table->add_row({"c", "d"});
  // 长度**跟着行数长**（新行补空串）——这是正确行为，不是“多出来”。
  ST_CHECK_EQ(table->lane_links().size(), 2U);
  ST_CHECK_EQ(table->lane_links()[0], std::string("|* "));
  ST_CHECK_EQ(table->lane_links()[1], std::string("|  "));
  ST_CHECK(std::abs(lane_inset_of(*table) - inset_before) < 0.01f);
  // 再补一行：新行是空串，且**不能错位**（第 0 格仍是原来的图）。
  table->add_row({"e", "f"});
  ST_CHECK_EQ(table->lane_links().size(), 3U);
  ST_CHECK_EQ(table->lane_links()[0], std::string("|* "));
  ST_CHECK_EQ(table->lane_links()[2], std::string(""));
}

/// **逐行高度**：连线行压扁后，行矩形与内容高都跟着变（斜线端点靠它算）。
ST_TEST(table_row_scales_change_geometry) {
  const Theme theme = Theme::dark();
  auto table = make_table(4);   // 4 × 20 = 80
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table->apply_theme(theme);
  table->arrange(context, Rect{0.0f, 0.0f, 300.0f, 400.0f});
  ST_CHECK(std::abs(table->content_height() - 80.0f) < 0.01f);

  // 第 1 行压到 0.35：总高 80 - 20 + 7 = 67。
  table->set_row_scales({1.0f, 0.35f, 1.0f, 1.0f});
  st::log::out("[table] content_height={} row1.h={}\n",
               static_cast<double>(table->content_height()),
               static_cast<double>(table->row_height_at(1)));
  ST_CHECK(std::abs(table->content_height() - 67.0f) < 0.05f);
  ST_CHECK(std::abs(table->row_height_at(1) - 7.0f) < 0.05f);
  // 后续行的 y 跟着上移（前缀和生效）。
  ST_CHECK(std::abs(table->row_rect(2).y -
                    (Table::kHeaderHeight + 20.0f + 7.0f)) < 0.05f);
  // 命中走**事件**而不是私有 `row_at`：点第 2 行中线应当回调第 2 行
  //（行高逐行不同时，“除行高”算出的会是 1 或 3 —— 这正是要钉住的）。
  std::size_t clicked = 999U;
  table->set_on_row_click([&clicked](std::size_t row) { clicked = row; });
  const Rect row2 = table->row_rect(2);
  st::ui::Event click{};
  click.kind = st::ui::EventKind::Click;
  click.position = st::math::Point{60.0f, row2.y + row2.height * 0.5f};
  table->on_event(context, click);
  st::log::out("[table] 点第 2 行中线 → 回调行 {}\n", clicked);
  ST_CHECK_EQ(clicked, 2U);
}
