/// `Button::on_context_menu`：右键按钮报位置、**不激活**、不改按压态；
/// 以及 `MenuPanel` 的自适应宽度**不截断中文长标签**。
///
/// ## 为什么要专门钉这两条
///
/// 1. **右键不能顺带激活**。`Button::on_event` 的 `MouseDown` 分支在旧实现里
///    只 `mark_dirty()` + `return true`，点击族走 `default` 的 `activate()`。
///    加右键支持时最省事的写法是把 `MouseDown` 也 `activate()`——那样用户
///    “右键看一眼菜单”就已经把操作执行了（分支行上是**检出分支**：会改工作区）。
///    实测代价：这类错只在你右键时触发，肉眼看菜单完全正常。
/// 2. **左键不能被右键分支吃掉**。`MouseDown` 里 `if (button == 2)` 写成
///    `if (button != 1)` 或忘了 `event.button` 默认值，会让左键失效——
///    而“按钮点不动”很难归因到菜单代码。
/// 3. **菜单宽度要按最宽条目自适应**。`kMaxPanelWidth` 曾为 280px，
///    把「强制删除（丢未合并提交）」截成「强制删除（丢未合并提…」。
///    菜单被截断比菜单略宽难受得多，所以上限必须容得下常见中文条目。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/theme.hpp"
#include "tests/support/text_port_fixtures.hpp"

namespace {

[[nodiscard]] auto make_context(const st::ui::TextPort* port = nullptr)
    -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, port, 0.0};
}

[[nodiscard]] auto make_button() -> std::unique_ptr<st::ui::Button> {
  auto button = std::make_unique<st::ui::Button>("分支行");
  button->set_id("b");
  const auto context = make_context();
  st::ui::Constraints constraints;
  constraints.max_width = 200.0f;
  constraints.max_height = 30.0f;
  constraints.available_width = 200.0f;
  constraints.available_height = 30.0f;
  button->measure(context, constraints);
  button->arrange(context, st::math::Rect{0.0f, 0.0f, 200.0f, 30.0f});
  return button;
}

}  // namespace

/// 右键：回调收到**点击位置**，且 `on_click` **一次都没被调用**。
ST_TEST(button_right_click_reports_position_without_activating) {
  auto button_ptr = make_button();
  st::ui::Button& button = *button_ptr;
  const auto context = make_context();

  int clicks = 0;
  std::vector<st::math::Point> reported;
  button.on_click = [&clicks] { ++clicks; };
  button.on_context_menu = [&reported](st::math::Point at) { reported.push_back(at); };

  st::ui::Event down{};
  down.kind = st::ui::EventKind::MouseDown;
  down.button = 2;
  down.position = st::math::Point{37.0f, 11.0f};
  const bool handled = button.on_event(context, down);

  st::log::out("[button] 右键 → handled={} clicks={} reported={}\n", handled, clicks,
               reported.size());
  ST_CHECK(handled);
  ST_CHECK_EQ(clicks, 0);           // **绝不激活**
  ST_REQUIRE(reported.size() == 1);
  ST_CHECK_EQ(reported[0].x, 37.0f);
  ST_CHECK_EQ(reported[0].y, 11.0f);
}

/// 左键仍然走 `Click` 激活（右键分支不许把左键吃掉）。
ST_TEST(button_left_click_still_activates) {
  auto button_ptr = make_button();
  st::ui::Button& button = *button_ptr;
  const auto context = make_context();

  int clicks = 0;
  button.on_click = [&clicks] { ++clicks; };
  button.on_context_menu = [](st::math::Point) {};

  st::ui::Event click{};
  click.kind = st::ui::EventKind::Click;
  click.button = 1;
  click.position = st::math::Point{100.0f, 15.0f};
  (void)button.on_event(context, click);

  st::log::out("[button] 左键 → clicks={}\n", clicks);
  ST_CHECK_EQ(clicks, 1);
}

/// 没挂 `on_context_menu` 时右键**不消费事件**（交回宿主/冒泡链）。
///
/// 理由：`return false` 与“消费了但什么也没做”在交互上完全不同——
/// 前者让上层的通用右键（如容器级菜单）有机会处理。
ST_TEST(button_right_click_without_handler_is_not_consumed) {
  auto button_ptr = make_button();
  st::ui::Button& button = *button_ptr;
  const auto context = make_context();

  st::ui::Event down{};
  down.kind = st::ui::EventKind::MouseDown;
  down.button = 2;
  down.position = st::math::Point{10.0f, 10.0f};
  const bool handled = button.on_event(context, down);

  st::log::out("[button] 无 handler 的右键 → handled={}\n", handled);
  ST_CHECK(!handled);
}

/// 中文长条目**不被截断**。
///
/// 这条守的是一个**实测过的真实缺陷**：`MenuPanel::measure` 算宽度时漏了勾选位，
/// 而 `paint_content` 又把勾选位从文字框里减掉——两侧各写一份魔法数字，
/// 于是 `文字框宽 = 文字宽 − 16px`，**每个条目都必然被截掉一个勾选位**。
/// 症状是「强制删除（会丢提交）」变成「强制删除（会丢提…」，
/// 而且**调高上限没用**（上限管的是最大值，差的是一整个勾选位）。
///
/// 判据用定宽字体桩把“测量”与“绘制请求”固定成可比较的数：
/// 面板宽 —— 减去 `kItemPaddingX × 2` 与 `kCheckSlotWidth` —— 必须 ≥ 文字宽。
/// （不用真实字体：定宽桩下文字宽 = 码点 × 8，与字体版本/DPI 无关。）
ST_TEST(menu_panel_width_leaves_room_for_the_whole_label) {
  // 标签要**长到超过 `kMinPanelWidth`**（否则面板宽恒为最小值，
  // 断言就会“恒绿”——实测：11 字标签文字宽 80px，面板被夹到 160，
  // 差 16px 也看不出来）。13 字以上才会让“测量宽”成为约束。
  const std::string label = "强制删除这个分支（会丢掉未合并的提交）";
  const st::test::FixedAdvanceTextPort port{};
  const float text_width = port.measure_width(label, st::ui::MenuPanel::kItemHeight);

  st::ui::MenuPanel panel({{.id = "x", .label = label}});
  const auto context = make_context(&port);
  panel.measure(context, st::ui::Constraints{.max_width = st::ui::kUnbounded,
                                             .max_height = st::ui::kUnbounded});

  const float width = panel.measured_size().width;
  // 文字框宽 = 面板宽 − 左内边距 − 勾选位 − 右内边距（与绘制同口径）。
  const float text_box = width - st::ui::MenuPanel::kItemPaddingX * 2.0f -
                         st::ui::MenuPanel::kCheckSlotWidth;

  st::log::out("[menu] 标签「{}」文字宽={:.1f} 面板宽={:.1f} 文字框={:.1f}\n", label,
               text_width, width, text_box);
  ST_CHECK(text_box >= text_width);
}

/// 间距常量与“常见中文条目放得下”的底线。
///
/// 只守上限本身没被当“美观参数”调小（实测调小到 280px 时
/// 「强制删除（丢未合并提交）」就会被截断）。
ST_TEST(menu_panel_max_width_can_fit_long_chinese_labels) {
  st::log::out("[menu] kMaxPanelWidth={:.1f}\n", st::ui::MenuPanel::kMaxPanelWidth);
  // 中文 12 字条目在字号 15 下约 300px（含内边距），上限必须高于它。
  ST_CHECK(st::ui::MenuPanel::kMaxPanelWidth >= 360.0f);
}
