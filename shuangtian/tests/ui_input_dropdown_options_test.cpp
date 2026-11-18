/// `Input` 内嵌下拉：**选项尚未就绪时点箭头也要有反应**。
///
/// ## 缺陷现场（首次填模型配置时下拉是空的）
///
/// 小白的模型字段用 `Input` 内嵌下拉，选项靠异步拉 `/v1/models`。
/// 首次填配置时列表还没拉过，用户点箭头——旧实现有两个地方把它吞掉：
///
/// 1. `MouseDown` 分支里写的是 `if (!dropdown_options_.empty()) set_dropdown_open(...)`：
///    选项为空时**整个什么都不做**，连 `open_dropdown_panel()` 都进不去；
/// 2. `open_dropdown_panel()` 遇到空表直接 `return`，没有任何“请给我选项”的信号。
///
/// 结果是“点了箭头完全没反应”，看起来就是“这个下拉没有列表”。
///
/// ## 修法
///
/// - 点箭头一律走 `set_dropdown_open(!open)`；
/// - `open_dropdown_panel()` 在空表时调 `on_dropdown_need_options()` 并记下
///   `dropdown_pending_open_`；
/// - `set_dropdown_options()` 看到这个待展开标记就自动展开——用户不必再点一次。
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
using st::ui::RenderContext;
using st::ui::SelectOption;
using st::ui::Theme;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  /// 记录被 `overlay_host` 交出去的面板（模拟宿主挂载）。
  std::vector<std::unique_ptr<Element>> mounted{};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto option_list(std::size_t count) -> std::vector<SelectOption> {
  std::vector<SelectOption> options;
  for (std::size_t index = 0; index < count; ++index) {
    options.push_back(SelectOption{std::to_string(index), "模型 " + std::to_string(index)});
  }
  return options;
}

/// 建一个带下拉的输入框并摆好；`overlay_host` 把面板收进 `mounted`。
void layout_input(Fixture& fixture, Input& input) {
  input.set_dropdown(true);
  input.overlay_host = [&fixture](std::unique_ptr<Element> panel) {
    fixture.mounted.push_back(std::move(panel));
  };
  Constraints constraints;
  constraints.max_width = 400.0f;
  constraints.max_height = 200.0f;
  constraints.available_width = 400.0f;
  constraints.available_height = 200.0f;
  input.measure(fixture.context(), constraints);
  input.arrange(fixture.context(), Rect{20.0f, 40.0f, 400.0f, 34.0f});
}

/// 在箭头上投一次 MouseDown。
void click_arrow(Fixture& fixture, Input& input) {
  const Rect arrow = input.dropdown_rect();
  st::ui::Event event;
  event.kind = st::ui::EventKind::MouseDown;
  event.button = 1;
  event.position = Point{arrow.x + arrow.width * 0.5f, arrow.y + arrow.height * 0.5f};
  input.measure(fixture.context(), [&] {
    Constraints constraints;
    constraints.max_width = 400.0f;
    constraints.max_height = 200.0f;
    constraints.available_width = 400.0f;
    constraints.available_height = 200.0f;
    return constraints;
  }());
  (void)input.on_event(fixture.context(), event);
}

}  // namespace

ST_TEST(input_dropdown_requests_options_when_empty) {
  // **核心回归**：选项为空时点箭头，必须回调 `on_dropdown_need_options`。
  Fixture fixture;
  Input input;
  layout_input(fixture, input);   // 注意：**没有** set_dropdown_options

  int requests = 0;
  input.on_dropdown_need_options = [&requests] { ++requests; };

  click_arrow(fixture, input);
  ST_CHECK_EQ(requests, 1);
}

ST_TEST(input_dropdown_auto_opens_after_options_arrive) {
  // 用户点过箭头之后再拿到选项 ⇒ 自动展开（不必再点一次）。
  Fixture fixture;
  Input input;
  layout_input(fixture, input);

  input.on_dropdown_need_options = [] {};
  click_arrow(fixture, input);
  ST_CHECK(!input.dropdown_open());

  // 选项到位（模拟异步拉取完成）。
  input.set_dropdown_options(option_list(5));
  ST_CHECK(input.dropdown_open());
  ST_CHECK_EQ(fixture.mounted.size(), 1U);
}

ST_TEST(input_dropdown_with_options_opens_directly) {
  // 已有选项时：点箭头直接开，**不该**走“请给我选项”回调。
  Fixture fixture;
  Input input;
  layout_input(fixture, input);
  input.set_dropdown_options(option_list(3));

  int requests = 0;
  input.on_dropdown_need_options = [&requests] { ++requests; };

  click_arrow(fixture, input);
  ST_CHECK_EQ(requests, 0);
  ST_CHECK(input.dropdown_open());
}

ST_TEST(input_dropdown_second_click_closes) {
  // 第二次点箭头应收起（展开/收起是同一个手势）。
  Fixture fixture;
  Input input;
  layout_input(fixture, input);
  input.set_dropdown_options(option_list(3));

  click_arrow(fixture, input);
  ST_CHECK(input.dropdown_open());
  click_arrow(fixture, input);
  ST_CHECK(!input.dropdown_open());
}
