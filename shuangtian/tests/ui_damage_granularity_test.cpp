#include <algorithm>
#include <cstddef>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/theme.hpp"

namespace {

[[nodiscard]] auto constraints_for(float width, float height) -> st::ui::Constraints {
  st::ui::Constraints c;
  c.max_width = width;
  c.max_height = height;
  c.available_width = width;
  c.available_height = height;
  return c;
}

/// 造一张 40 行的表。
void fill_rows(st::ui::Table& table, int count) {
  for (int i = 0; i < count; ++i) {
    table.add_row({std::format("row-{}", i), std::format("value-{}", i)});
  }
}

}  // namespace

// 这一组用例只钉一件事：**损坏区是“一行/几行”，不是整块**。
//
// 为什么必须有它：这类退化的表现是“功能对、像素对、只是每次重绘整帧”——
// 任何像素/属性断言都会是绿的，只有量损坏区尺寸才能抓住。
// 实测代价（2026-11）：`Terminal` 占视口 30% 却只报整块，合并后越阈值退化成
// 整帧重绘，paint 从 4.5ms 涨到 28.7ms，而全部既有测试都是绿的。

ST_TEST(table_row_damage_is_smaller_than_bounds) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  st::ui::Table table;
  table.set_columns({{"A", 150.0f, st::ui::TextAlign::Start},
                     {"B", 150.0f, st::ui::TextAlign::Start}});
  fill_rows(table, 40);
  table.apply_theme(theme);
  table.measure(context, constraints_for(600.0f, 700.0f));
  table.arrange(context,
                st::math::Rect{0.0f, 0.0f, table.measured_size().width,
                               table.measured_size().height});
  // 先消耗掉 add_row/set_columns 期间累积的整块损坏（数据全变了，那本来就该整块）。
  (void)table.take_damage();

  table.set_selected_row(std::size_t{5});
  const auto report = table.take_damage();
  ST_CHECK(report.valid);
  const st::math::Rect bounds = table.bounds();
  std::cout << std::format("  [table] damage=({:.0f},{:.0f},{:.0f}x{:.0f}) bounds=({:.0f}x{:.0f})\n",
                           report.rect.x, report.rect.y, report.rect.width, report.rect.height,
                           bounds.width, bounds.height)
            << std::flush;
  // 选中只影响一行：损坏区高度必须远小于控件（40 行）。
  ST_CHECK(report.rect.height < bounds.height * 0.5f);
  ST_CHECK(report.rect.height > 0.0f);
}

ST_TEST(table_hover_damage_is_smaller_than_bounds) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  st::ui::Table table;
  table.set_columns({{"A", 300.0f, st::ui::TextAlign::Start}});
  fill_rows(table, 40);
  table.apply_theme(theme);
  table.measure(context, constraints_for(600.0f, 700.0f));
  table.arrange(context,
                st::math::Rect{0.0f, 0.0f, table.measured_size().width,
                               table.measured_size().height});
  (void)table.take_damage();

  // 悬停到第二行（只该影响新旧两行）。
  st::ui::Event move;
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{table.bounds().width * 0.5f, table.bounds().y + 60.0f};
  (void)table.on_event(context, move);
  const auto report = table.take_damage();
  if (!report.valid) {
    std::cout << "  [table/hover] 未产生损坏上报（指针未命中行），跳过\n" << std::flush;
    return;
  }
  std::cout << std::format("  [table/hover] damage height={:.0f} / bounds {:.0f}\n",
                           report.rect.height, table.bounds().height)
            << std::flush;
  ST_CHECK(report.rect.height < table.bounds().height * 0.5f);
}

ST_TEST(markdown_rows_damage_is_smaller_than_bounds) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  std::string doc;
  for (int i = 0; i < 60; ++i) doc += std::format("第 {} 行正文内容\n\n", i);
  st::ui::MarkdownView view{doc};
  view.apply_theme(theme);
  view.measure(context, constraints_for(500.0f, 400.0f));
  view.arrange(context,
               st::math::Rect{0.0f, 0.0f, view.measured_size().width, view.measured_size().height});
  (void)view.take_damage();

  // 落 caret 的逻辑在 **MouseDown** 分支（`Click` 不落 caret）。
  const float probe_y = view.bounds().y + 80.0f;
  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.button = 1;  // 左键（非左键直接不算，见 MouseDown 分支的首行判据）
  down.position = st::math::Point{view.bounds().x + 40.0f, probe_y};
  (void)view.on_event(context, down);
  const auto report = view.take_damage();
  if (!report.valid) {
    std::cout << "  [md] MouseDown 未产生损坏上报，跳过\n" << std::flush;
    return;
  }
  const st::math::Rect bounds = view.bounds();
  std::cout << std::format("  [md] damage=({:.0f},{:.0f},{:.0f}x{:.0f}) bounds=({:.0f}x{:.0f})\n",
                           report.rect.x, report.rect.y, report.rect.width, report.rect.height,
                           bounds.width, bounds.height)
            << std::flush;
  // 文档内容远高于视口：整块上报会等于 bounds 高度；按行上报应明显更小。
  ST_CHECK(report.rect.height < bounds.height);
}
