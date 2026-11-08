/// @file ui_form_components_test.cpp
/// @brief 包三「输入与表单」回归：`NumberInput` / `SearchBox` / `FormRow` /
///        `RadioGroup` / `TagInput`。
///
/// 这五个组件的共同点是**状态与显示可能不一致**（数值框的原文 vs 值、单选的值 vs 下标、
/// 标签的草稿 vs 集合），所以用例重点在：
///
/// 1. **不变量**：值永远合法、选中永远唯一、并集永远不丢项；
/// 2. **边界**：夹取、去重、上限、空输入；
/// 3. **几何**：命中区不重叠、标签折行、表单标签列对齐。

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::test::FixedAdvanceTextPort;

namespace {

using st::ui::Event;
using st::ui::EventKind;
using st::ui::FormRow;
using st::ui::NumberInput;
using st::ui::RadioGroup;
using st::ui::RadioOption;
using st::ui::SearchBox;
using st::ui::TagInput;

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width, float height = 400.0f) {
    element.apply_theme(theme);
    st::ui::Constraints constraints{};
    constraints.max_width = width;
    constraints.max_height = height;
    constraints.available_width = width;
    constraints.available_height = height;
    element.measure(context, constraints);
    element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }

  /// 敲一个键。
  void key(st::ui::Element& element, std::string name) {
    Event event{};
    event.kind = EventKind::KeyDown;
    event.key = std::move(name);
    element.on_event(context, event);
  }
  /// 输入一段文本。
  void type(st::ui::Element& element, std::string text) {
    Event event{};
    event.kind = EventKind::TextInput;
    event.text = std::move(text);
    element.on_event(context, event);
  }
  /// 在某个位置点一下。
  void click(st::ui::Element& element, st::math::Point point) {
    Event event{};
    event.kind = EventKind::Click;
    event.position = point;
    element.on_event(context, event);
  }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// NumberInput
// ————————————————————————————————————————————————————————————————————————————

/// 值必须被**区间夹取**：超上限 / 超下限都要落到边界上。
ST_TEST(number_input_clamps_value_to_range) {
  NumberInput input;
  input.set_min(0.0);
  input.set_max(100.0);

  input.set_value(300.0);
  ST_CHECK(std::abs(input.value() - 100.0) < 1e-9);
  input.set_value(-50.0);
  ST_CHECK(std::abs(input.value() - 0.0) < 1e-9);
  input.set_value(42.0);
  ST_CHECK(std::abs(input.value() - 42.0) < 1e-9);
}

/// `precision` 必须同时作用于**值**与**显示文本**（不能只格式化不改值）。
ST_TEST(number_input_precision_rounds_value_and_text) {
  NumberInput input;
  input.set_precision(2);
  input.set_value(1.23456);
  ST_CHECK(std::abs(input.value() - 1.23) < 1e-9);
  ST_CHECK(input.text() == "1.23");

  // 精度 -1 = 自动：整数不带小数点。
  input.set_precision(-1);
  input.set_value(7.0);
  ST_CHECK(input.text() == "7");
  input.set_value(7.5);
  ST_CHECK(input.text() == "7.5");
}

/// 非法输入**不能**把值改坏（用户敲到一半的 `-` 或 `1.` 不该变成 0）。
ST_TEST(number_input_invalid_text_keeps_last_valid_value) {
  NumberInput input(5.0);
  input.set_text("-");
  ST_CHECK(std::abs(input.value() - 5.0) < 1e-9);   // 值保持
  ST_CHECK(input.text() == "-");                    // 原文保留（用户还在敲）

  input.set_text("abc");
  ST_CHECK(std::abs(input.value() - 5.0) < 1e-9);

  input.set_text("12");
  ST_CHECK(std::abs(input.value() - 12.0) < 1e-9);
}

/// 步进按 `step` 走，且在边界处**停住**（不能越过 min/max）。
ST_TEST(number_input_step_respects_step_and_bounds) {
  NumberInput input;
  input.set_min(0.0);
  input.set_max(3.0);
  input.set_step(1.0);
  input.set_value(2.0);

  input.step_up();
  ST_CHECK(std::abs(input.value() - 3.0) < 1e-9);
  input.step_up();   // 已在边界
  ST_CHECK(std::abs(input.value() - 3.0) < 1e-9);
  input.step_down();
  ST_CHECK(std::abs(input.value() - 2.0) < 1e-9);

  input.set_step(0.25);
  input.step_down();
  ST_CHECK(std::abs(input.value() - 1.75) < 1e-9);
}

/// `on_change` **只在值真变了**时派发（否则调用方在回调里 set_value 会无限递归）。
ST_TEST(number_input_change_fires_only_on_real_change) {
  NumberInput input(1.0);
  int calls = 0;
  input.on_change = [&calls](double) { ++calls; };

  input.set_value(1.0);
  ST_CHECK(calls == 0);          // 同值不派发
  input.set_value(1.0 + 1e-12);  // 浮点噪声：也算同值
  ST_CHECK(calls == 0);
  input.set_value(2.0);
  ST_CHECK(calls == 1);
}

/// 步进三角的命中区**不重叠**、都在右缘、且不侵占输入区。
ST_TEST(number_input_stepper_rects_are_disjoint_and_at_right) {
  Fixture fixture;
  NumberInput input(0.0);
  fixture.layout(input, 120.0f, 30.0f);

  const auto up = input.up_rect();
  const auto down = input.down_rect();
  const auto field = input.field_rect();
  ST_CHECK(!up.is_empty());
  ST_CHECK(!down.is_empty());
  // 上下不相交（y 方向严格分离）。
  ST_CHECK(up.bottom() <= down.y + 0.01f);
  // 两者同宽同 x（一列）。
  ST_CHECK(std::abs(up.x - down.x) < 0.01f);
  ST_CHECK(std::abs(up.width - down.width) < 0.01f);
  // 输入区不与步进列重叠。
  ST_CHECK(field.right() <= up.x + 0.01f);
}

/// 点步进三角真的改变值；点输入区不变。
ST_TEST(number_input_click_stepper_changes_value) {
  Fixture fixture;
  NumberInput input(10.0);
  input.set_step(5.0);
  fixture.layout(input, 120.0f, 30.0f);

  fixture.click(input, input.up_rect().center());
  ST_CHECK(std::abs(input.value() - 15.0) < 1e-9);
  fixture.click(input, input.down_rect().center());
  fixture.click(input, input.down_rect().center());
  ST_CHECK(std::abs(input.value() - 5.0) < 1e-9);
  fixture.click(input, input.field_rect().center());
  ST_CHECK(std::abs(input.value() - 5.0) < 1e-9);
}

/// 属性面：`value` 写入即夹取，`min`/`max` 能被读回。
ST_TEST(number_input_property_surface) {
  NumberInput input;
  ST_CHECK(input.set_property("min", "0"));
  ST_CHECK(input.set_property("max", "10"));
  ST_CHECK(input.set_property("value", "99"));
  ST_CHECK(std::abs(input.value() - 10.0) < 1e-9);
  ST_CHECK(input.get_property("value").value_or("") == "10");
  ST_CHECK(input.get_property("max").value_or("") == "10");
  // 非法数值被拒绝（不能静默归零）。
  ST_CHECK(!input.set_property("value", "not-a-number"));
  ST_CHECK(std::abs(input.value() - 10.0) < 1e-9);
}

// ————————————————————————————————————————————————————————————————————————————
// SearchBox
// ————————————————————————————————————————————————————————————————————————————

/// 清除 × **只在有内容时**出现；空内容时它的命中区必须为空。
ST_TEST(search_box_clear_only_present_with_text) {
  Fixture fixture;
  SearchBox box;
  fixture.layout(box, 240.0f, 30.0f);
  ST_CHECK(box.clear_rect().is_empty());   // 空内容 → 无清除位

  box.set_value("abc");
  fixture.layout(box, 240.0f, 30.0f);
  ST_CHECK(!box.clear_rect().is_empty());
}

/// 清除 × 不与输入区重叠（否则点输入区会误清空）。
ST_TEST(search_box_clear_rect_does_not_overlap_field) {
  Fixture fixture;
  SearchBox box;
  box.set_value("hello");
  fixture.layout(box, 240.0f, 30.0f);

  const auto clear = box.clear_rect();
  const auto field = box.field_rect();
  ST_CHECK(!clear.is_empty());
  ST_CHECK(!field.is_empty());
  ST_CHECK(clear.x >= field.right() - 0.01f);
}

/// 点 × 清空并派发 `on_change` / `on_clear`。
ST_TEST(search_box_clear_action_empties_value) {
  Fixture fixture;
  SearchBox box;
  box.set_value("abc");
  fixture.layout(box, 240.0f, 30.0f);
  int changes = 0;
  int clears = 0;
  box.on_change = [&changes](std::string_view) { ++changes; };
  box.on_clear = [&clears] { ++clears; };

  fixture.click(box, box.clear_rect().center());
  ST_CHECK(box.value().empty());
  ST_CHECK(clears == 1);
  ST_CHECK(changes == 1);
}

/// `Esc` 有内容时清空；空内容时**不吃**（应交给上层）。
ST_TEST(search_box_escape_clears_only_when_not_empty) {
  Fixture fixture;
  SearchBox box;
  fixture.layout(box, 240.0f, 30.0f);

  // 空内容：Esc 不处理（`on_event` 返回 false 才是“没吃”）。
  Event escape{};
  escape.kind = EventKind::KeyDown;
  escape.key = "Escape";
  const bool handled_empty = box.on_event(fixture.context, escape);
  ST_CHECK(!handled_empty);

  box.set_value("abc");
  const bool handled = box.on_event(fixture.context, escape);
  ST_CHECK(handled);
  ST_CHECK(box.value().empty());
}

/// 结果计数徽标占右缘，且不与清除位重叠。
ST_TEST(search_box_count_badge_is_rightmost) {
  Fixture fixture;
  SearchBox box;
  box.set_value("x");
  box.set_count(12);
  fixture.layout(box, 300.0f, 30.0f);

  const auto count = box.count_rect();
  const auto clear = box.clear_rect();
  const auto field = box.field_rect();
  ST_CHECK(!count.is_empty());
  ST_CHECK(count.x >= clear.right() - 0.01f);   // 计数在清除右边
  ST_CHECK(count.right() <= 300.0f + 0.01f);     // 不出界
  ST_CHECK(field.right() <= clear.x + 0.01f);    // 输入区在清除左边
}

// ————————————————————————————————————————————————————————————————————————————
// FormRow
// ————————————————————————————————————————————————————————————————————————————

/// 控件落位在标签列**右侧**，且两不重叠。
ST_TEST(form_row_control_sits_right_of_label) {
  Fixture fixture;
  FormRow row("用户名");
  fixture.layout(row, 360.0f, 60.0f);

  const auto label = row.label_rect();
  const auto control = row.control_rect();
  ST_CHECK(!label.is_empty());
  ST_CHECK(!control.is_empty());
  ST_CHECK(control.x >= label.right() + FormRow::kLabelGap - 0.01f);
  ST_CHECK(label.x >= 0.0f);
}

/// 显式给标签宽时，**上下两行**的控件起点必须一致（这是表单对齐的本质）。
ST_TEST(form_row_shared_label_width_aligns_controls) {
  Fixture fixture;
  FormRow first("名");
  first.set_label_width(100.0f);
  fixture.layout(first, 360.0f, 40.0f);

  FormRow second("一个很长很长的标签");
  second.set_label_width(100.0f);
  fixture.layout(second, 360.0f, 40.0f);

  ST_CHECK(std::abs(first.control_rect().x - second.control_rect().x) < 0.01f);
  ST_CHECK(std::abs(first.effective_label_width() - 100.0f) < 0.01f);
  ST_CHECK(std::abs(second.effective_label_width() - 100.0f) < 0.01f);
}

/// 自动标签宽时，长标签撑开列宽、短标签用自身宽——但**同一行内**控件不越界。
ST_TEST(form_row_auto_label_width_fits_text) {
  Fixture fixture;
  FormRow row("标签");
  fixture.layout(row, 360.0f, 40.0f);
  // 两个汉字 × 8px = 16px，自动宽应 ≥ 该值。
  ST_CHECK(row.effective_label_width() >= 16.0f);
  ST_CHECK(row.control_rect().right() <= 360.0f + 0.01f);
}

/// 错误文案会**增加行高**（不能浮在上面盖住下一行）。
ST_TEST(form_row_error_increases_height) {
  Fixture fixture;
  FormRow plain("用户名");
  fixture.layout(plain, 360.0f, 200.0f);
  const float plain_height = plain.measured_size().height;

  FormRow errored("用户名");
  errored.set_error("这个名字已被占用");
  fixture.layout(errored, 360.0f, 200.0f);

  ST_CHECK(errored.measured_size().height > plain_height);
  ST_CHECK(!errored.message_rect().is_empty());
  // 提示区在控件**下方**。
  ST_CHECK(errored.message_rect().y >= errored.control_rect().bottom() - 0.01f);
}

/// 只承载一个控件：挂第二个必须被拒绝（而不是静默替换造成悬垂指针）。
ST_TEST(form_row_accepts_only_one_control) {
  FormRow row("字段");
  auto first = std::make_unique<st::ui::SearchBox>();
  auto second = std::make_unique<st::ui::SearchBox>();
  ST_CHECK(row.add_child(std::move(first)) != nullptr);
  ST_CHECK(row.add_child(std::move(second)) == nullptr);   // 被拒绝
  ST_CHECK(row.child_count() == 1U);
}

// ————————————————————————————————————————————————————————————————————————————
// RadioGroup
// ————————————————————————————————————————————————————————————————————————————

/// 选中永远**唯一**：设第二个必须把第一个取消。
ST_TEST(radio_group_selection_is_exclusive) {
  RadioGroup group;
  group.set_labels({"甲", "乙", "丙"});
  ST_CHECK(group.set_index(0));
  ST_CHECK(group.index() == 0);
  ST_CHECK(group.value() == "甲");

  ST_CHECK(group.set_index(2));
  ST_CHECK(group.index() == 2);
  ST_CHECK(group.value() == "丙");
  // 同一个 group 只有一份 index ——互斥由数据结构保证，不可能出现两项选中。
}

/// 按**值**选中：重排选项后选中跟着值走（不是跟着下标）。
///
/// ⚠ 重排必须让选中项**真的换位置**：最初写成「把丙放到最前」，而乙本来就在下标 1，
/// 重排后仍在 1——于是「按值重定位」与「按下标保留」完全等价，把实现改坏测试仍是绿的
///（逆向验证担到的假绿，见 CONVENTIONS.md §7.1）。这里把乙换到下标 2。
ST_TEST(radio_group_selection_follows_value_across_reorder) {
  RadioGroup group;
  group.set_labels({"甲", "乙", "丙"});
  ST_CHECK(group.set_value("乙"));
  ST_CHECK(group.index() == 1);

  // 重排：乙 被挤到下标 2。
  group.set_labels({"甲", "丙", "乙"});
  ST_CHECK(group.value() == "乙");   // 值不变
  ST_CHECK(group.index() == 2);      // 下标必须跟着值走
}

/// 键盘方向键可移动选中并**跳过禁用项**。
ST_TEST(radio_group_keyboard_skips_disabled) {
  Fixture fixture;
  RadioGroup group;
  group.set_options({RadioOption{"甲", "a", true}, RadioOption{"乙", "b", false},
                     RadioOption{"丙", "c", true}});
  group.set_index(0);
  fixture.layout(group, 200.0f, 100.0f);

  fixture.key(group, "ArrowDown");   // 跳过禁用的乙 → 到丙
  ST_CHECK(group.value() == "c");

  fixture.key(group, "ArrowDown");   // 循环回甲
  ST_CHECK(group.value() == "a");
}

/// 选项矩形**不重叠**且都在组件内（水平折行时也一样）。
ST_TEST(radio_group_option_rects_are_disjoint) {
  Fixture fixture;
  RadioGroup group;
  group.set_labels({"选项一", "选项二", "选项三", "选项四", "选项五"});
  group.set_direction(RadioGroup::Direction::Horizontal);
  fixture.layout(group, 150.0f, 200.0f);

  for (std::size_t i = 0; i < group.count(); ++i) {
    const auto rect = group.option_rect(i);
    ST_CHECK(!rect.is_empty());
    ST_CHECK(rect.x >= 0.0f);
    ST_CHECK(rect.right() <= 150.0f + 0.01f);
    for (std::size_t j = i + 1; j < group.count(); ++j) {
      const auto other = group.option_rect(j);
      const bool overlap = rect.x < other.right() && other.x < rect.right() &&
                           rect.y < other.bottom() && other.y < rect.bottom();
      ST_CHECK(!overlap);
    }
  }
}

/// 点选项按命中区选中；`on_change` 只报值。
ST_TEST(radio_group_click_selects_and_reports_value) {
  Fixture fixture;
  RadioGroup group;
  group.set_options({RadioOption{"甲", "alpha", true}, RadioOption{"乙", "beta", true}});
  fixture.layout(group, 200.0f, 100.0f);
  std::string reported;
  group.on_change = [&reported](std::string_view value) { reported = std::string(value); };

  fixture.click(group, group.option_rect(1).center());
  ST_CHECK(group.value() == "beta");
  ST_CHECK(reported == "beta");
}

// ————————————————————————————————————————————————————————————————————————————
// TagInput
// ————————————————————————————————————————————————————————————————————————————

/// 去重：同值标签只加一次，并派发 `on_duplicate`。
ST_TEST(tag_input_deduplicates) {
  TagInput input;
  int duplicates = 0;
  input.on_duplicate = [&duplicates](std::string_view) { ++duplicates; };

  ST_CHECK(input.add_tag("a"));
  ST_CHECK(!input.add_tag("a"));
  ST_CHECK(input.tags().size() == 1U);
  ST_CHECK(duplicates == 1);
  // 前后空白被裁掉后判重。
  ST_CHECK(!input.add_tag("  a  "));
  ST_CHECK(input.tags().size() == 1U);
}

/// 上限生效：达到 `max` 后不再加。
ST_TEST(tag_input_respects_max) {
  TagInput input;
  input.set_max_tags(2);
  ST_CHECK(input.add_tag("a"));
  ST_CHECK(input.add_tag("b"));
  ST_CHECK(!input.add_tag("c"));
  ST_CHECK(input.tags().size() == 2U);
}

/// 空草稿时退格**删最后一个标签**；有草稿时只删草稿。
ST_TEST(tag_input_backspace_deletes_tag_only_when_draft_empty) {
  Fixture fixture;
  TagInput input;
  input.add_tag("one");
  input.add_tag("two");
  fixture.layout(input, 240.0f, 60.0f);

  // 有草稿 → 只删草稿字符。
  input.set_draft("ab");
  fixture.key(input, "Backspace");
  ST_CHECK(input.draft() == "a");
  ST_CHECK(input.tags().size() == 2U);

  // 清空草稿后再退格 → 删标签。
  input.set_draft("");
  fixture.key(input, "Backspace");
  ST_CHECK(input.tags().size() == 1U);

  // 草稿为空且没有标签 → 不处理（不该吃键）。
  input.clear_tags();
  Event back{};
  back.kind = EventKind::KeyDown;
  back.key = "Backspace";
  ST_CHECK(!input.on_event(fixture.context, back));
}

/// 回车/逗号提交草稿；提交后草稿清空。
ST_TEST(tag_input_enter_commits_draft) {
  Fixture fixture;
  TagInput input;
  fixture.layout(input, 240.0f, 60.0f);

  input.set_draft("alpha");
  fixture.key(input, "Enter");
  ST_CHECK(input.tags().size() == 1U);
  ST_CHECK(input.tags()[0] == "alpha");
  ST_CHECK(input.draft().empty());

  // 逗号同样提交。
  input.set_draft("beta");
  fixture.key(input, ",");
  ST_CHECK(input.tags().size() == 2U);
}

/// 标签会**折行**（窄宽下多行），且各行标签不重叠、都在组件内。
ST_TEST(tag_input_wraps_and_rects_are_disjoint) {
  Fixture fixture;
  TagInput input;
  for (const char* tag : {"alpha", "beta", "gamma", "delta", "epsilon"}) {
    input.add_tag(tag);
  }
  fixture.layout(input, 160.0f, 200.0f);

  bool multiple_rows = false;
  for (std::size_t i = 0; i < input.tags().size(); ++i) {
    const auto rect = input.tag_rect(i);
    ST_CHECK(!rect.is_empty());
    ST_CHECK(rect.right() <= 160.0f + 0.01f);
    for (std::size_t j = i + 1; j < input.tags().size(); ++j) {
      const auto other = input.tag_rect(j);
      if (rect.y != other.y) {
        multiple_rows = true;
        continue;
      }
      const bool overlap = rect.x < other.right() && other.x < rect.right();
      ST_CHECK(!overlap);
    }
  }
  ST_CHECK(multiple_rows);   // 160px 放不下 5 个标签，必须折行
}

/// 关闭 × 的命中区落在自身标签内（不能串到邻居标签上）。
ST_TEST(tag_input_close_rect_inside_own_tag) {
  Fixture fixture;
  TagInput input;
  input.add_tag("one");
  input.add_tag("two");
  fixture.layout(input, 240.0f, 60.0f);

  for (std::size_t i = 0; i < input.tags().size(); ++i) {
    const auto tag = input.tag_rect(i);
    const auto close = input.close_rect(i);
    ST_CHECK(!close.is_empty());
    ST_CHECK(close.x >= tag.x - 0.01f);
    ST_CHECK(close.right() <= tag.right() + 0.01f);
    ST_CHECK(close.y >= tag.y - 0.01f);
    ST_CHECK(close.bottom() <= tag.bottom() + 0.01f);
  }
}

/// 点 × 删除对应标签（按**下标**，不能错删邻居）。
ST_TEST(tag_input_click_close_removes_that_tag) {
  Fixture fixture;
  TagInput input;
  input.add_tag("one");
  input.add_tag("two");
  input.add_tag("three");
  fixture.layout(input, 240.0f, 60.0f);

  fixture.click(input, input.close_rect(1).center());
  ST_CHECK(input.tags().size() == 2U);
  ST_CHECK(input.tags()[0] == "one");
  ST_CHECK(input.tags()[1] == "three");
}

/// `set_tags` 会去空白、去重（整批刷新时也要守住集合语义）。
ST_TEST(tag_input_set_tags_normalizes) {
  TagInput input;
  input.set_tags({"a", " ", "a", " b ", "c"});
  ST_CHECK(input.tags().size() == 3U);
  ST_CHECK(input.tags()[0] == "a");
  ST_CHECK(input.tags()[1] == "b");
  ST_CHECK(input.tags()[2] == "c");
}
