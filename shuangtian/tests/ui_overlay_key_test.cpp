/// 浮层里的按键派发：**焦点元素优先于同层其他子元素**。
///
/// 背景（真实缺陷，2026-10-07 gbcode 的“转到行”浮层）：
/// 浮层内容形如 `[文本][输入框][跳转][×]`。`UiRoot::dispatch_key_into` 当时是
/// **逆序问所有子元素**——最后一个子元素先拿键。而尾部那个「×」是 `Button`，
/// 它对 `Enter` 的语义是 `activate()`（点击）⇒ 用户**在行号输入框里按回车**
/// 的结果是浮层被关掉、输入框自己的 `on_submit` 根本没跑（查找条同理：它的尾部
/// 也有「×」，按回车等于关掉查找条）。
///
/// 判定标准：这是框架缺陷而不是组件缺陷——“浮层的按键先给谁”属于**分发顺序**，
/// 任何浮层都会踩到；正确的顺序是「容器自身 → **焦点所在链** → 其余子元素」。
///
/// 两条用例都做过逆向验证（把焦点链那一支去掉即变红）。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Button;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::Input;
using st::ui::Panel;
using st::ui::UiRoot;

/// 浮层夹具：宿主铺满视口，内容是一行 `[输入框][按钮]`（**按钮在后**，
/// 这正是“逆序问子元素”会抢先的那个位置）。
struct OverlayFixture {
  UiRoot root{};
  Panel* host{nullptr};
  Input* input{nullptr};
  Button* button{nullptr};
  int submits{0};
  int button_clicks{0};
  std::vector<std::string> log{};

  OverlayFixture() {
    root.set_viewport(st::math::Size{800.0f, 600.0f});
    root.set_content(std::make_unique<Panel>(st::ui::FlexDirection::Column));

    auto overlay = std::make_unique<Panel>(st::ui::FlexDirection::Row);
    overlay->set_id("overlay-host");
    auto field = std::make_unique<Input>();
    field->set_id("overlay-input");
    input = field.get();
    input->on_submit = [this](std::string_view) {
      ++submits;
      log.push_back("submit");
    };
    overlay->add_child(std::move(field));
    auto action = std::make_unique<Button>("跳转");
    action->set_id("overlay-action");
    button = action.get();
    button->on_click = [this] {
      ++button_clicks;
      log.push_back("button");
    };
    overlay->add_child(std::move(action));
    host = overlay.get();
    root.add_overlay(std::move(overlay), UiRoot::OverlayLayout::FillViewport);
    root.layout(true);
  }

  auto press(std::string key) -> bool {
    Event event;
    event.kind = EventKind::KeyDown;
    event.key = std::move(key);
    return root.dispatch(event);
  }
};

}  // namespace

ST_TEST(overlay_key_goes_to_focused_input_before_sibling_button) {
  OverlayFixture fx;
  ST_REQUIRE(fx.root.set_focus(fx.input));
  fx.input->set_text("12");

  ST_CHECK(fx.press("Enter"));
  // 焦点输入框先拿：`on_submit` 跑一次，按钮**一次都不该**被触发。
  ST_CHECK_EQ(fx.submits, 1);
  ST_CHECK_EQ(fx.button_clicks, 0);
  ST_REQUIRE(fx.log.size() == 1U);
  ST_CHECK_EQ(fx.log[0], std::string("submit"));
  // 输入内容不该被这次按键改掉（提交不是清空；清不清由宿主决定）。
  ST_CHECK_EQ(fx.input->value(), std::string("12"));
}

ST_TEST(overlay_key_falls_through_when_focused_element_ignores_it) {
  OverlayFixture fx;
  ST_REQUIRE(fx.root.set_focus(fx.input));
  // 输入框不认识 F5：应当继续下沉，最终**不消费**（全局快捷键/焦点环的落点靠这个）。
  ST_CHECK(!fx.press("F5"));
  ST_CHECK_EQ(fx.submits, 0);
  ST_CHECK_EQ(fx.button_clicks, 0);
  // 焦点不在浮层子树上时，浮层内的按钮照旧拿得到 Enter（键盘可达性不变）。
  ST_REQUIRE(fx.root.set_focus(nullptr));
  ST_CHECK(fx.press("Enter"));
  ST_CHECK_EQ(fx.button_clicks, 1);
  ST_CHECK_EQ(fx.submits, 0);
}
