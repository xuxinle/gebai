/// `Input` 内嵌下拉按钮的**鼠标指针**：箭头区是 Hand，其余是 Text。
///
/// ## 缺陷现场
///
/// 旧实现只在构造里 `set_cursor(MouseCursor::Text)` —— 那是**元素级**的静态值，
/// 于是整个输入框（包括框内那个下拉按钮）都是 I-beam 光标：
/// 鼠标移到箭头上看着像“可以选中文字”，实际点下去却是展开下拉。两者不一致。
///
/// ## 修法
///
/// 覆写 `cursor_at(point)`：命中 `dropdown_rect()` 时返回 `Hand`，其余返回 `Text`。
/// 这是框架给“同一元素里不同区域有不同交互”的既有机制（`Link`/`CodeEditor` 等都这样）。
#include <string>
#include <vector>

#include "st/test/test.hpp"

#include "st/ui/components/input_box.hpp"
#include "st/ui/components/select_panel.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Point;
using st::math::Rect;
using st::ui::Constraints;
using st::ui::Element;
using st::ui::Input;
using st::ui::MouseCursor;
using st::ui::RenderContext;
using st::ui::SelectOption;
using st::ui::Theme;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

/// 摆好一个带下拉的输入框，返回它的矩形。
[[nodiscard]] auto layout_input(Fixture& fixture, Input& input) -> Rect {
  input.set_dropdown(true);
  std::vector<SelectOption> options;
  options.push_back(SelectOption{"a", "模型 A"});
  options.push_back(SelectOption{"b", "模型 B"});
  input.set_dropdown_options(std::move(options));

  Constraints constraints;
  constraints.max_width = 400.0f;
  constraints.max_height = 200.0f;
  constraints.available_width = 400.0f;
  constraints.available_height = 200.0f;
  input.measure(fixture.context(), constraints);
  input.arrange(fixture.context(), Rect{20.0f, 40.0f, 400.0f, 34.0f});
  return input.bounds();
}

}  // namespace

ST_TEST(input_cursor_is_hand_on_dropdown_arrow) {
  // **核心回归**：箭头区必须是 Hand（“可点”），而不是 I-beam。
  Fixture fixture;
  Input input;
  ST_CHECK(!layout_input(fixture, input).is_empty());

  const Rect arrow = input.dropdown_rect();
  ST_CHECK(!arrow.is_empty());
  const Point arrow_center{arrow.x + arrow.width * 0.5f, arrow.y + arrow.height * 0.5f};
  ST_CHECK(input.cursor_at(arrow_center) == MouseCursor::Hand);
}

ST_TEST(input_cursor_is_text_on_editing_area) {
  // 正文区仍是 I-beam（“能选字/能输入”）。
  Fixture fixture;
  Input input;
  (void)layout_input(fixture, input);

  const Rect arrow = input.dropdown_rect();
  // 取左边一截，远离箭头。
  const Rect bounds = input.bounds();
  const Point editing{bounds.x + 10.0f, arrow.y + arrow.height * 0.5f};
  ST_CHECK(input.cursor_at(editing) == MouseCursor::Text);
}

ST_TEST(input_cursor_is_arrow_outside) {
  // 框外不属于它：交回 Arrow（让上层决定）。
  Fixture fixture;
  Input input;
  (void)layout_input(fixture, input);
  ST_CHECK(input.cursor_at(Point{-50.0f, -50.0f}) == MouseCursor::Arrow);
}

ST_TEST(input_without_dropdown_is_all_text) {
  // 没开下拉的输入框：整框都是 I-beam（没有按钮区可言）。
  Fixture fixture;
  Input input;
  Constraints constraints;
  constraints.max_width = 400.0f;
  constraints.max_height = 200.0f;
  constraints.available_width = 400.0f;
  constraints.available_height = 200.0f;
  input.measure(fixture.context(), constraints);
  input.arrange(fixture.context(), Rect{20.0f, 40.0f, 400.0f, 34.0f});

  ST_CHECK(input.dropdown_rect().is_empty());
  const Rect bounds = input.bounds();
  ST_CHECK(input.cursor_at(Point{bounds.right() - 6.0f, bounds.y + bounds.height * 0.5f}) ==
           MouseCursor::Text);
}
