/// `DatePicker` / `TimePicker` / `DateTimeField` 的**值与文本同步**。
///
/// 判据选择说明：弹出面板需要宿主（`host_add_overlay`），单测里没有 UiRoot，
/// 所以这里钉的是"不需要弹层就能验、且错了会静默错"的那一半：
///
/// - 手输文本 → 值（含**非法拒绝**：这是最容易被写成"静默兜底"的地方）；
/// - 设值 → 文本（含"同值不重写"，否则每帧回写会冲掉用户正在输入的内容）；
/// - `DateTimeField` 的空态是一等状态（不是哨兵值）。
///
/// 弹出/选日那半由真实应用（dev-tools）端到端验证——那才是有宿主的形态。
#include "st/test/test.hpp"

#include <optional>
#include <string>

#include "st/ui/calendar.hpp"
#include "st/ui/components/calendar_panel.hpp"
#include "st/ui/components/date_picker.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/date_time_field.hpp"
#include "st/ui/components/time_picker.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::test::FixedAdvanceTextPort;
using st::ui::calendar::Clock;
using st::ui::calendar::Date;
using st::ui::DatePicker;
using st::ui::DateTimeField;
using st::ui::DateTimeValue;
using st::ui::TimePicker;

/// 递归找指定 `type()` 的元素（`Element` 自己不带查找，浮层要自己走一遍）。
[[nodiscard]] auto find_type(const st::ui::Element& element, std::string_view type)
    -> const st::ui::Element* {
  if (element.type() == type) return &element;
  for (const auto& child : element.children()) {
    if (const auto* found = find_type(*child, type); found != nullptr) return found;
  }
  return nullptr;
}

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::dark();
  FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width = 320.0f, float height = 32.0f) {
    element.apply_theme(theme);
    st::ui::Constraints constraints{};
    constraints.max_width = width;
    constraints.max_height = height;
    constraints.available_width = width;
    constraints.available_height = height;
    element.measure(context, constraints);
    element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }
};

}  // namespace

/// `DatePicker` 的文本与值双向同步。
ST_TEST(date_picker_text_and_value_stay_in_sync) {
  Fixture fixture;
  DatePicker picker(Date{2026, 11, 15});
  fixture.layout(picker);
  ST_REQUIRE(picker.text_field() != nullptr);
  ST_CHECK_EQ(picker.text_field()->value(), std::string("2026-11-15"));

  // 手输合法 → 值跟着变
  picker.text_field()->set_text("2026-12-01");
  ST_REQUIRE(picker.invoke_action("submit", ""));
  ST_CHECK(picker.value() == (Date{2026, 12, 1}));
  ST_CHECK(!picker.invalid_text());

  // 设值 → 文本跟着变
  picker.set_value(Date{2027, 3, 9}, false);
  ST_CHECK_EQ(picker.text_field()->value(), std::string("2027-03-09"));
}

/// **非法输入如实拒绝并保留原值**（不静默兜底）。
///
/// 为何这条最重要：静默兜底是这类控件最糟的失败方式——用户以为设上了，
/// 实际是另一个日期，中间没有任何提示。
ST_TEST(date_picker_rejects_invalid_text_and_keeps_value) {
  Fixture fixture;
  DatePicker picker(Date{2026, 11, 15});
  fixture.layout(picker);

  for (const char* bad : {"2026-02-30", "2026-13-01", "abc", "2026-1", ""}) {
    picker.text_field()->set_text(bad);
    (void)picker.invoke_action("submit", "");
    ST_CHECK(picker.value() == (Date{2026, 11, 15}));   // 值不变
    ST_CHECK(picker.invalid_text());                    // 但标记为非法
  }
  // 恢复合法输入后标记清除
  picker.text_field()->set_text("2026-11-20");
  (void)picker.invoke_action("submit", "");
  ST_CHECK(!picker.invalid_text());
  ST_CHECK(picker.value() == (Date{2026, 11, 20}));
}

/// `min`/`max` 对**手输**同样生效（不能"输入绕过范围"）。
ST_TEST(date_picker_text_input_respects_range) {
  Fixture fixture;
  DatePicker picker(Date{2026, 11, 15});
  picker.set_min(Date{2026, 11, 10});
  picker.set_max(Date{2026, 11, 20});
  fixture.layout(picker);

  picker.text_field()->set_text("2026-11-05");
  (void)picker.invoke_action("submit", "");
  ST_CHECK(picker.value() == (Date{2026, 11, 15}));
  ST_CHECK(picker.invalid_text());

  picker.text_field()->set_text("2026-11-25");
  (void)picker.invoke_action("submit", "");
  ST_CHECK(picker.value() == (Date{2026, 11, 15}));

  picker.text_field()->set_text("2026-11-10");   // 边界包含
  (void)picker.invoke_action("submit", "");
  ST_CHECK(picker.value() == (Date{2026, 11, 10}));
}

/// `TimePicker`：文本 ↔ 值同步 + 非法拒绝。
ST_TEST(time_picker_text_and_value_stay_in_sync) {
  Fixture fixture;
  TimePicker picker(Clock{14, 30});
  fixture.layout(picker, 160.0f, 32.0f);
  ST_REQUIRE(picker.text_field() != nullptr);
  ST_CHECK_EQ(picker.text_field()->value(), std::string("14:30"));

  picker.text_field()->set_text("09:05");
  (void)picker.invoke_action("submit", "");
  ST_CHECK(picker.value() == (Clock{9, 5}));

  picker.set_value(Clock{23, 59}, false);
  ST_CHECK_EQ(picker.text_field()->value(), std::string("23:59"));
}

ST_TEST(time_picker_rejects_invalid_text) {
  Fixture fixture;
  TimePicker picker(Clock{14, 30});
  fixture.layout(picker, 160.0f, 32.0f);
  for (const char* bad : {"24:00", "09:60", "abc", ""}) {
    picker.text_field()->set_text(bad);
    (void)picker.invoke_action("submit", "");
    ST_CHECK(picker.value() == (Clock{14, 30}));
    ST_CHECK(picker.invalid_text());
  }
}

/// **手输任意分钟必须被接受**——步进只约束列里的候选，不是值的合法性边界。
///
/// 为何钉它：很容易写成"提交时吸附到步进"，那让"我要 14:07 提醒"变成不可能，
/// 而通知类场景恰恰常要具体到分。
ST_TEST(time_picker_text_accepts_any_minute_despite_step) {
  Fixture fixture;
  TimePicker picker(Clock{14, 30});
  picker.set_minute_step(5);
  fixture.layout(picker, 160.0f, 32.0f);
  picker.text_field()->set_text("14:07");
  (void)picker.invoke_action("submit", "");
  ST_CHECK(picker.value() == (Clock{14, 7}));
  ST_CHECK(!picker.invalid_text());
}

/// `DateTimeField` 的**空态是一等状态**（不是 1970/00:00 这类哨兵值）。
ST_TEST(date_time_field_empty_state_is_first_class) {
  Fixture fixture;
  DateTimeField field;
  fixture.layout(field, 320.0f, 32.0f);
  ST_CHECK(!field.has_value());
  // 空态的两个子控件显示**占位符**，而不是某个具体日期——显示具体日期
  // 等于悄悄给了一个值（用户会以为已经设上了）。
  ST_REQUIRE(field.date_picker() != nullptr);
  ST_REQUIRE(field.time_picker() != nullptr);
  ST_CHECK(field.date_picker()->text_field()->value() != "1970-01-01");
  ST_CHECK_EQ(field.semantics_value(), std::string(""));
  // 属性面 value 为空串 = 没有值
  const auto shown = field.get_property("value");
  ST_REQUIRE(shown.has_value());
  ST_CHECK(shown->empty());
  const auto has = field.get_property("has_value");
  ST_REQUIRE(has.has_value());
  ST_CHECK_EQ(*has, std::string("false"));
}

/// 设值/清空会带着两个子控件一起动。
ST_TEST(date_time_field_value_and_clear_propagate) {
  Fixture fixture;
  DateTimeField field;
  fixture.layout(field, 320.0f, 32.0f);

  field.set_value(DateTimeValue{Date{2026, 11, 27}, Clock{17, 45}}, false);
  ST_CHECK(field.has_value());
  ST_CHECK_EQ(field.date_picker()->text_field()->value(), std::string("2026-11-27"));
  ST_CHECK_EQ(field.time_picker()->text_field()->value(), std::string("17:45"));

  field.clear(false);
  ST_CHECK(!field.has_value());
  ST_CHECK(field.date_picker()->text_field()->value() != "2026-11-27");

  // 再设一次（空 → 有 → 空 可反复）
  field.set_value(DateTimeValue{Date{2027, 1, 1}, Clock{8, 0}}, false);
  ST_CHECK(field.has_value());
}

/// 子控件被单独改（模拟用户点了日期面板）时，`DateTimeField` 的**值要跟着变**。
///
/// 这条钉的是"组合件自己的值是否真的由子件驱动"——如果只在 `set_value` 里同步，
/// 用户一点日期，父值就与实际显示脱节（而属性面 `get value` 会继续报旧值）。
ST_TEST(date_time_field_reacts_to_child_pickers) {
  Fixture fixture;
  DateTimeField field;
  fixture.layout(field, 320.0f, 32.0f);

  // 空态下点日期 → 自动补默认时刻 09:00（用户点它就是为了填值）
  (void)field.date_picker()->invoke_action("select", "2026-12-24");
  ST_CHECK(field.has_value());
  ST_REQUIRE(field.value().has_value());
  ST_CHECK(field.value()->date == (Date{2026, 12, 24}));
  ST_CHECK(field.value()->clock == (Clock{9, 0}));

  // 再改时刻 → 日期保持不变
  (void)field.time_picker()->invoke_action("select", "18:30");
  ST_REQUIRE(field.value().has_value());
  ST_CHECK(field.value()->date == (Date{2026, 12, 24}));
  ST_CHECK(field.value()->clock == (Clock{18, 30}));
}

/// 属性面：`YYYY-MM-DD HH:MM` 与 `YYYY-MM-DD`（时刻缺省 09:00）都认。
ST_TEST(date_time_field_property_parses_both_shapes) {
  DateTimeField field;
  ST_CHECK(field.set_property("value", "2026-11-27 17:45"));
  ST_REQUIRE(field.value().has_value());
  ST_CHECK(field.value()->date == (Date{2026, 11, 27}));
  ST_CHECK(field.value()->clock == (Clock{17, 45}));

  ST_CHECK(field.set_property("value", "2027-01-02"));
  ST_REQUIRE(field.value().has_value());
  ST_CHECK(field.value()->date == (Date{2027, 1, 2}));
  ST_CHECK(field.value()->clock == (Clock{9, 0}));

  // 非法拒绝
  ST_CHECK(!field.set_property("value", "2026-02-30 10:00"));
  ST_CHECK(!field.set_property("value", "2026-11-27 25:00"));
  // 空串 = 清空（合法）
  ST_CHECK(field.set_property("value", ""));
  ST_CHECK(!field.has_value());
}

/// **空态下点开日历应看到“今天”，不是 1970-01-01**——
/// 通过真实 `open()` + 叠加层里的 `CalendarPanel` 来验。
///
/// 为何必须走 `open()`：先前写过三条“只验 `CalendarPanel` 自身机制”的用例，
/// 回退修复后**它们依然全绿**（那些机制本来就对，缺陷在 `open()` 的那一行选择上）。
/// 那类“假绿测试”比没有更糟——它让人以为有护栏。
///
/// 为何不直接读 `visible_month()`：`open()` 后面板在叠加层里，要通过
/// `UiRoot::overlays()` 取到真实实例（而不是新建一个）。
ST_TEST(date_picker_empty_state_opens_at_today) {
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto owned = std::make_unique<DatePicker>();
  auto* picker = owned.get();
  root.set_content(std::move(owned));
  // 不传 value（空态），只注入 today
  picker->set_today(Date{2026, 11, 27});
  root.layout(true);

  ST_REQUIRE(picker->open());
  root.layout(true);
  // 弹层在**叠加层**里（不是内容树），所以 `root.find()` 找不到它
  ST_REQUIRE(root.overlay_count() >= 1U);
  const auto* found_cal = find_type(*root.overlay_at(0), "CalendarPanel");
  ST_REQUIRE(found_cal != nullptr);
  const auto* panel = dynamic_cast<const st::ui::CalendarPanel*>(found_cal);
  ST_REQUIRE(panel != nullptr);
  // **判据**：视野在 2026-11（today），不是 1970-01（零值）
  ST_CHECK(panel->visible_month().year == 2026);
  ST_CHECK(panel->visible_month().month == 11);
  picker->close();
}

/// 有值时 `open()` 的视野跟着值（不能被 today 抢走）。
ST_TEST(date_picker_with_value_opens_at_value_month) {
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  auto owned = std::make_unique<DatePicker>(Date{2024, 3, 15});
  auto* picker = owned.get();
  root.set_content(std::move(owned));
  picker->set_today(Date{2026, 11, 27});
  root.layout(true);

  ST_REQUIRE(picker->open());
  root.layout(true);
  ST_REQUIRE(root.overlay_count() >= 1U);
  const auto* found_cal = find_type(*root.overlay_at(0), "CalendarPanel");
  ST_REQUIRE(found_cal != nullptr);
  const auto* panel = dynamic_cast<const st::ui::CalendarPanel*>(found_cal);
  ST_REQUIRE(panel != nullptr);
  ST_CHECK(panel->visible_month().year == 2024);
  ST_CHECK(panel->visible_month().month == 3);
  picker->close();
}

/// `has_explicit_value` 如实反映“用户是否真设过值”。
///
/// 为何需要它：`value_` 的零值 1970-01-01 是**合法日期**，所以
/// “选了 1970-01-01”与“没选”必须能区分（拿零值当哨兵就分不出来）。
ST_TEST(date_picker_reports_whether_value_was_explicitly_set) {
  DatePicker picker(Date{2026, 11, 15});
  const auto fresh = picker.get_property("has_explicit_value");
  ST_REQUIRE(fresh.has_value());
  ST_CHECK_EQ(*fresh, std::string("true"));

  // **默认构造 = 没有值**（这才能让空态定位到 today）
  DatePicker empty;
  const auto none = empty.get_property("has_explicit_value");
  ST_REQUIRE(none.has_value());
  ST_CHECK_EQ(*none, std::string("false"));

  // 真的选 1970-01-01：仍算“有值”
  empty.set_value(Date{1970, 1, 1}, false);
  const auto epoch = empty.get_property("has_explicit_value");
  ST_REQUIRE(epoch.has_value());
  ST_CHECK_EQ(*epoch, std::string("true"));
  ST_CHECK(empty.value() == (Date{1970, 1, 1}));
}
