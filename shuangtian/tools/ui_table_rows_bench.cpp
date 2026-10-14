/// `Table` 大行数量尺 —— 与 `ui_tree_rows_bench`/`ui_list_items_bench` 同一套口径。
///
/// ## 为什么 Table 要单独量（三种形态里的第三种）
///
/// | 组件 | 每行是 | measure 缺口 | paint 缺口 |
/// |---|---|---|---|
/// | `Tree` | 数据行（`std::vector<Row>`） | 手写循环量每行文本 | 手写循环画每行 |
/// | `List` | 真子元素 | 基类递归 × N 项 | **有**元素级剔除兜底 |
/// | `Table` | 数据行（自绘，无子节点） | **只按列定义算宽**（不量行内容） | 手写循环，**每格**调 `ellipsize` + `measure_width` |
///
/// `Table::paint_content` 里有一个看似能救场的提前退出：
/// `if (row.y >= bounds_.bottom()) break;`。但宿主把 Table 放进 `ScrollView` 时，
/// `bounds_` 拿到的是**内容全高**（与 `Tree` 完全一样的情形）——于是这个 break
/// 在整个滚动过程中**一次都不触发**，仍然迭代全部行、全部列。
///
/// ## 量什么
///
/// | 尺子 | 对应真实场景 |
/// |---|---|
/// | `measure` | 刷新后重排（只算列宽，预期极便宜） |
/// | `paint(400px 视口 / 全高 bounds)` | **每帧真实成本**——滚动容器的实际形态 |
/// | `paint(400px 视口 / bounds 也 400px)` | 对照：bounds 恰为视口高时提前退出生效 |
///
/// 第三条是**对照实验**：它证明那个 `break` 的效力完全取决于 `bounds_` 恰好等于视口高，
/// 而真实 ScrollView 场景不满足——两个数放一起才说明"为什么它救不了"。
///
/// ## 用法
///
/// ```bash
/// tools/build_ui_probe.sh ui_table_rows_bench
/// build/probe/ui_table_rows_bench [行数] [列数]
/// ```

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

auto elapsed_ms(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Fn>
[[nodiscard]] auto best_ms(int rounds, Fn&& fn) -> double {
  double best = 1e9;
  for (int round = 0; round < rounds; ++round) {
    const auto start = Clock::now();
    fn();
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

[[nodiscard]] auto make_columns(std::size_t count) -> std::vector<st::ui::TableColumn> {
  std::vector<st::ui::TableColumn> columns;
  columns.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    columns.push_back(st::ui::TableColumn{.name = "column_" + std::to_string(index),
                                          .width = 140.0F,
                                          .align = st::ui::TextAlign::Start});
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
      cells.push_back("src/ui/components/file_" + std::to_string(row) + "_" +
                      std::to_string(column) + ".cpp");
    }
    rows.push_back(std::move(cells));
  }
  return rows;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::size_t rows_wanted = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 1000U;
  const std::size_t columns_wanted = argc > 2 ? static_cast<std::size_t>(std::atoll(argv[2])) : 4U;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value()) {
    st::print("SKIP: 无系统字体，量尺不成立（measure_width 恒 0 会量出假结果）\n");
    return 0;
  }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, port.get(), 0.0};

  st::ui::Table table{make_columns(columns_wanted)};
  for (auto& row : make_rows(rows_wanted, columns_wanted)) table.add_row(std::move(row));

  const float content_height = static_cast<float>(rows_wanted) * table.row_height() +
                               st::ui::Table::kHeaderHeight + 8.0F;
  constexpr int kCanvasWidth = 600;
  constexpr int kViewportHeight = 400;

  // —— 尺子①：measure（只算列宽，预期极便宜）——
  table.measure(context, st::ui::Constraints{});
  const double measure_ms = best_ms(20, [&] { table.measure(context, st::ui::Constraints{}); });

  // —— 尺子②：paint —— 滚动容器的真实形态：bounds = **内容全高** ——
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kViewportHeight, 1.0f);
  table.arrange(context,
                st::math::Rect{0.0F, 0.0F, static_cast<float>(kCanvasWidth), content_height});
  const double paint_fullbounds_ms = best_ms(10, [&] {
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    table.paint(context, canvas);
  });

  // —— 尺子③：对照——bounds 恰为视口高（提前退出生效的情形）——
  table.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kCanvasWidth),
                                        static_cast<float>(kViewportHeight)});
  const double paint_viewportbounds_ms = best_ms(10, [&] {
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    table.paint(context, canvas);
  });

  st::print("rows={}  cols={}  行高={:.0f}  视口={}px(约 {} 行)\n", rows_wanted, columns_wanted,
            table.row_height(), kViewportHeight,
            static_cast<std::size_t>(static_cast<float>(kViewportHeight) / table.row_height()));
  st::print("  measure                 {:8.3f} ms   <- 只算列宽，与行数无关\n", measure_ms);
  st::print("  paint(bounds=内容全高)   {:8.3f} ms   <- **真实场景**（ScrollView 给内容全高）\n",
            paint_fullbounds_ms);
  st::print("  paint(bounds=视口高)     {:8.3f} ms   <- 对照：提前退出生效时\n",
            paint_viewportbounds_ms);
  return 0;
}
