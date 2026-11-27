/// `CalendarPanel`（月历面板）的几何与交互。
///
/// 判据尽量落在**组件自己暴露的几何**（`cell_rect`/`cell_at`/`prev_button_rect`）上，
/// 而不是测试里重抄一遍网格公式——后者对"公式写错"这件事零分辨力（两边一起错，
/// 断言照样绿）。
#include "st/test/test.hpp"

#include <string>

#include "st/math/geometry.hpp"
#include "st/ui/calendar.hpp"
#include "st/ui/components/calendar_panel.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::test::FixedAdvanceTextPort;
using st::ui::CalendarPanel;
using st::ui::calendar::Date;

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width = 230.0f, float height = 260.0f) {
    element.apply_theme(theme);
    st::ui::Constraints constraints{};
    constraints.max_width = width;
    constraints.max_height = height;
    constraints.available_width = width;
    constraints.available_height = height;
    element.measure(context, constraints);
    element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }

  void click(st::ui::Element& element, st::math::Point point) {
    st::ui::Event event{};
    event.kind = st::ui::EventKind::Click;
    event.position = point;
    element.on_event(context, event);
  }
};

}  // namespace

/// 网格恒为 **6 行 × 7 列 = 42 格**，且第 0 格在左上、第 41 格在右下。
ST_TEST(calendar_panel_exposes_full_6x7_grid) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 11, 27});
  fixture.layout(panel);
  ST_CHECK_EQ(st::ui::calendar::kGridCells, 42);
  const auto first = panel.cell_rect(0);
  const auto last = panel.cell_rect(st::ui::calendar::kGridCells - 1);
  ST_CHECK(!first.is_empty());
  ST_CHECK(!last.is_empty());
  // 左上 → 右下
  ST_CHECK(first.y < last.y);
  ST_CHECK(first.x < last.x);
  // 7 列跨满网格宽
  const auto seventh = panel.cell_rect(6);
  ST_CHECK(seventh.y == first.y);                       // 同一行
  ST_CHECK(std::abs(seventh.right() - first.right() - 6.0f * CalendarPanel::kCellSize) < 0.01f);
}

/// 1 号必须落在**与真实星期一致的那一列**（这是整个日历正确性的地基）。
ST_TEST(calendar_panel_first_day_sits_in_correct_column) {
  Fixture fixture;
  // 2026-11-01 是周日 → 0=周一口径下第 6 列
  CalendarPanel panel(Date{2026, 11, 15});
  fixture.layout(panel);
  const int lead = st::ui::calendar::weekday_of(Date{2026, 11, 1});
  ST_CHECK_EQ(lead, 6);
  ST_CHECK_EQ(panel.cell_day(lead), 1);
  // 之前全是空位
  for (int index = 0; index < lead; ++index) ST_CHECK_EQ(panel.cell_day(index), 0);
  // 点"1 号"所在的格子，选中值必须变成 1 号
  fixture.click(panel, panel.cell_rect(lead).center());
  ST_CHECK(panel.value() == (Date{2026, 11, 1}));
}

/// 点空白格（邻月空位）**不该改值**——否则会出现"点了一下跳到别的月"。
ST_TEST(calendar_panel_click_on_empty_cell_changes_nothing) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 11, 15});
  fixture.layout(panel);
  const Date before = panel.value();
  const int lead = st::ui::calendar::weekday_of(Date{2026, 11, 1});
  ST_REQUIRE(lead > 0);
  fixture.click(panel, panel.cell_rect(0).center());   // 第 0 格必是空位
  ST_CHECK(panel.value() == before);
}

/// `min` / `max` 边界**包含**，且越界格点不动。
ST_TEST(calendar_panel_min_max_are_inclusive_and_block_clicks) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 11, 15});
  panel.set_min(Date{2026, 11, 10});
  panel.set_max(Date{2026, 11, 20});
  fixture.layout(panel);

  const int lead = st::ui::calendar::weekday_of(Date{2026, 11, 1});
  const auto cell_of = [&](int day) { return panel.cell_rect(lead + day - 1).center(); };

  ST_CHECK(panel.is_selectable(Date{2026, 11, 10}));   // 下界包含
  ST_CHECK(panel.is_selectable(Date{2026, 11, 20}));   // 上界包含
  ST_CHECK(!panel.is_selectable(Date{2026, 11, 9}));
  ST_CHECK(!panel.is_selectable(Date{2026, 11, 21}));

  const Date before = panel.value();
  fixture.click(panel, cell_of(5));    // 早于 min
  ST_CHECK(panel.value() == before);
  fixture.click(panel, cell_of(25));   // 晚于 max
  ST_CHECK(panel.value() == before);
  fixture.click(panel, cell_of(12));   // 范围内
  ST_CHECK(panel.value() == (Date{2026, 11, 12}));
}

/// 翻月按钮改**视野**，不改已选值。
ST_TEST(calendar_panel_month_navigation_keeps_selection) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 11, 15});
  fixture.layout(panel);
  fixture.click(panel, panel.next_button_rect().center());
  ST_CHECK_EQ(panel.visible_month().year, 2026);
  ST_CHECK_EQ(panel.visible_month().month, 12);
  ST_CHECK(panel.value() == (Date{2026, 11, 15}));   // 选中值不变
  // 跨年回退
  fixture.click(panel, panel.prev_button_rect().center());
  fixture.click(panel, panel.prev_button_rect().center());
  ST_CHECK_EQ(panel.visible_month().year, 2026);
  ST_CHECK_EQ(panel.visible_month().month, 10);
}

/// 从 31 日的月份翻到只有 28/29 天的月份：**选中日要被夹取**，不能留一个不存在的日期。
ST_TEST(calendar_panel_month_step_clamps_day) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 1, 31});
  fixture.layout(panel);
  fixture.click(panel, panel.next_button_rect().center());   // → 2 月（2026 平年）
  // 视野到 2 月了；选中值仍是 1/31（它属于 1 月，不该被悄悄改掉）
  ST_CHECK_EQ(panel.visible_month().month, 2);
  ST_CHECK(panel.value() == (Date{2026, 1, 31}));
  // 但 `set_value` 传 2/31 时必须夹到 2/28
  panel.set_value(Date{2026, 2, 31});
  ST_CHECK(panel.value() == (Date{2026, 2, 28}));
}

/// 被父容器拉宽时**不跟着变宽**（否则 7 列会散到整窗上，看着像丢了半边）。
///
/// 为何必须钉：面板所在的列容器默认 `align_items = Stretch`，不抗拒就会被拉满。
/// 而缺陷形态**在属性面上看不出来**（`bounds` 就是那个大数，"它本来就该占那么宽"）——
/// 只有看图才看得出七个数字散在 1400px 上。
ST_TEST(calendar_panel_resists_being_stretched) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 11, 15});
  panel.apply_theme(fixture.theme);
  st::ui::Constraints constraints{};
  constraints.max_width = 1400.0f;
  constraints.available_width = 1400.0f;
  constraints.max_height = 700.0f;
  constraints.available_height = 700.0f;
  panel.measure(fixture.context, constraints);
  // 父级给 1400 宽，面板安排的矩形也是 1400
  panel.arrange(fixture.context, st::math::Rect{0.0f, 0.0f, 1400.0f, 600.0f});
  // 但它自己必须收到“网格 + 内边距”那么宽
  const float own = CalendarPanel::kCellSize * 7.0f + CalendarPanel::kPadding * 2.0f;
  ST_CHECK(std::abs(panel.bounds().width - own) < 0.01f);
  // 且仍然居中（左右留白相等）
  ST_CHECK(std::abs(panel.bounds().x - (1400.0f - own) * 0.5f) < 0.01f);
  // 网格不被拉宽：第 0 格与第 6 格的间距仍是 6 格
  ST_CHECK(std::abs(panel.cell_rect(6).x - panel.cell_rect(0).x - 6.0f * CalendarPanel::kCellSize) <
           0.01f);
}

/// 属性面：`value` 非法时**如实拒绝**（不静默兜底成今天）。
ST_TEST(calendar_panel_property_surface_rejects_invalid) {
  CalendarPanel panel(Date{2026, 11, 15});
  ST_CHECK(panel.set_property("value", "2026-11-20"));
  ST_CHECK(panel.value() == (Date{2026, 11, 20}));
  ST_CHECK(!panel.set_property("value", "2026-02-30"));   // 该月没这天
  ST_CHECK(!panel.set_property("value", "abc"));
  ST_CHECK(panel.value() == (Date{2026, 11, 20}));        // 失败不改值

  const auto shown = panel.get_property("value");
  ST_REQUIRE(shown.has_value());
  ST_CHECK_EQ(*shown, std::string("2026-11-20"));
}

/// 动作面：翻月与按日期选中。
ST_TEST(calendar_panel_actions) {
  Fixture fixture;
  CalendarPanel panel(Date{2026, 11, 15});
  fixture.layout(panel);
  ST_CHECK(panel.invoke_action("next-month", ""));
  ST_CHECK_EQ(panel.visible_month().month, 12);
  ST_CHECK(panel.invoke_action("prev-month", ""));
  ST_CHECK_EQ(panel.visible_month().month, 11);
  ST_CHECK(panel.invoke_action("select", "2026-11-18"));
  ST_CHECK(panel.value() == (Date{2026, 11, 18}));
  ST_CHECK(!panel.invoke_action("select", "2026-11-31"));   // 11 月没有 31 日
}
