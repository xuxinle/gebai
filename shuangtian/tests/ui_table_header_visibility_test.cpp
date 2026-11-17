/// `Table::set_show_header(false)`：表头让出那 36px，**行区顶到边**。
///
/// ## 为什么这条要测“几何位移”而不只是“表头看不见”
///
/// 关表头最容易做成“不画了、但高度还留着”——那会留下一道 36px 的空白色带，
/// 用户看到的是“上面莫名其妙空了”，而“表头已经没了”确实也是事实
/// （看起来像改对了）。真正的判据是**行区起点上移了一个表头高**。
///
/// 三件事必须同时成立：
/// 1. `header_rect()` 是**空矩形**（不是高 0 的矩形——后者在多数判据里仍算存在）；
/// 2. 第 0 行的 `row_rect().y` 恰好在表格顶边（而不是顶边 + 36）；
/// 3. 视口能多放一行（`viewport_row_count` 变大）——高度真的还给了行区。

#include "st/test/test.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Rect;
using st::ui::Table;
using st::ui::TableColumn;

[[nodiscard]] auto make_table(bool show_header, std::size_t rows)
    -> std::unique_ptr<Table> {
  std::vector<TableColumn> columns{TableColumn{.name = "提交信息", .width = 240.0f},
                                   TableColumn{.name = "作者", .width = 100.0f}};
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_row_height(24.0f);
  table->set_show_header(show_header);
  for (std::size_t index = 0; index < rows; ++index) {
    table->add_row({"commit " + std::to_string(index), "author"});
  }
  return table;
}

/// 布局到固定矩形（`Table` 不可拷贝，按引用传入）。
template <class T>
void layout(T& table, const st::ui::Theme& theme, Rect box) {
  st::ui::RenderContext context{theme, nullptr, 0.0};
  table.apply_theme(theme);
  table.arrange(context, box);
}

}  // namespace

/// 关表头：`header_rect()` 为空，且第 0 行贴到表格顶边。
ST_TEST(table_without_header_starts_rows_at_top_edge) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const Rect box{0.0f, 0.0f, 400.0f, 300.0f};

  // 行数要**超出视口**：10 行 × 24 = 240 < 300，两种情形都能全装下，
  // 于是 `viewport_row_count` 被行数封顶、两边都是 10——量不出“高度还给了行区”。
  auto with_header = make_table(true, 40);
  layout(*with_header, theme, box);
  const auto header_rect = with_header->header_rect();
  const float row0_with_header = with_header->row_rect(0).y;
  ST_CHECK(!header_rect.is_empty());
  ST_CHECK_NEAR(header_rect.height, Table::kHeaderHeight, 0.01f);
  // 有表头时第 0 行在表头**之下**。
  ST_CHECK_NEAR(row0_with_header, box.y + Table::kHeaderHeight, 0.5f);

  auto no_header = make_table(false, 40);
  layout(*no_header, theme, box);
  const auto empty_header = no_header->header_rect();
  const float row0_no_header = no_header->row_rect(0).y;

  st::log::out("[table] 有表头 row0.y={} / 无表头 row0.y={}；header_rect 空={}\n",
               static_cast<double>(row0_with_header), static_cast<double>(row0_no_header),
               empty_header.is_empty());

  // ① 表头矩形为空（判据不是“高 0”）。
  ST_CHECK(empty_header.is_empty());
  // ② 行区顶到边：位移恰好是一个表头高。
  ST_CHECK_NEAR(row0_no_header, box.y, 0.5f);
  ST_CHECK_NEAR(row0_with_header - row0_no_header, Table::kHeaderHeight, 0.5f);
  // ③ 高度真的还给了行区（300px / 24 = 12 行；有表头时 (300-36)/24 = 11 行）。
  st::log::out("[table] viewport_row_count: 有表头 {} / 无表头 {}\n",
               with_header->viewport_row_count(), no_header->viewport_row_count());
  ST_CHECK(no_header->viewport_row_count() > with_header->viewport_row_count());
}

/// 默认**有表头**（这是长期既有行为，不能因为新增开关就变）。
///
/// 反向护栏：把默认值写成 `false` 会让所有既有表格静默失去列头，
/// 而“列头没了”在测试里没有任何断言会失败——必须显式钉住默认。
ST_TEST(table_header_is_visible_by_default) {
  std::vector<TableColumn> columns{TableColumn{.name = "列", .width = 100.0f}};
  auto table_ptr = std::make_unique<Table>(std::move(columns));
  Table& table = *table_ptr;
  table.add_row({"x"});
  const st::ui::Theme theme = st::ui::Theme::dark();
  layout(table, theme, Rect{0.0f, 0.0f, 200.0f, 200.0f});

  ST_CHECK(table.show_header());
  ST_CHECK(!table.header_rect().is_empty());
  ST_CHECK_NEAR(table.row_rect(0).y, Table::kHeaderHeight, 0.5f);
}

/// 切换会**重算布局**：先用有表头布局，再关掉，行矩形立刻上移。
ST_TEST(table_toggling_header_relayouts_rows) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const Rect box{0.0f, 0.0f, 400.0f, 300.0f};
  auto table = make_table(true, 8);
  layout(*table, theme, box);
  const float before = table->row_rect(0).y;

  table->set_show_header(false);
  layout(*table, theme, box);
  const float after = table->row_rect(0).y;

  st::log::out("[table] 切表头前后 row0.y: {} → {}\n", static_cast<double>(before),
               static_cast<double>(after));
  ST_CHECK_NEAR(before - after, Table::kHeaderHeight, 0.5f);
  // 内容总高也少掉一个表头高。
  ST_CHECK_NEAR(table->content_height(),
                24.0f * 8.0f, 0.5f);
}
