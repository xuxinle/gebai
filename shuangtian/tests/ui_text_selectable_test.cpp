/// `Element::set_text_selectable`：**展示型文本组件的统一“可选中复制”能力**。
///
/// ## 守的是什么
///
/// 这个能力是为一类**静默失败**加的：`Chip` / `Link` / `Badge` / `EmptyState` /
/// `KeyValueRow` / `Breadcrumb` / `StatusBar` 等十几个展示型文本组件原本都
/// **不能选中复制**——而“选不中”不会报错，用户只会以为「这个应用不支持复制」。
///
/// 三个坑，每个都单独钉一条：
///
/// | 坑 | 不测的话看起来是 |
/// |---|---|
/// | 开关没接上（永远是 `false`） | 按 `Ctrl+A` 毫无反应 |
/// | **可选中但不可聚焦** | 鼠标点得中、`Ctrl+C` 没反应（键盘事件先走焦点链，收不到） |
/// | `Ctrl+C` 不写剪贴板 | 选中高亮有了，粘出来是空的 |
///
/// 第三条在单测里用**注入的 `HostFocus`** 捕获写入内容（真剪贴板是平台能力，
/// 单测不该碰）；前两条靠属性面与 `focused()` 断言。

#include "st/test/test.hpp"

#include <string>

#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::Element;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Theme;

/// 只实现本用例要用的两个宿主契约：给焦点、收剪贴板。
class RecordingHost final : public st::ui::HostFocus {
 public:
  auto set_keyboard_focus(Element* element) -> bool override {
    focused_ = element;
    return element != nullptr;
  }
  auto host_set_clipboard_text(std::string_view text) -> bool override {
    clipboard = std::string(text);
    return true;
  }
  [[nodiscard]] auto focus_ring_visible() const -> bool override { return false; }

  Element* focused_{nullptr};
  std::string clipboard{};
};

[[nodiscard]] auto make_context() -> RenderContext {
  static const Theme theme = Theme::light();
  return RenderContext{theme, nullptr, 0.0};
}

/// 把一个元素摆到固定矩形上（命中测试依赖 `bounds_`）。
void layout(Element& element, float width = 200.0f, float height = 30.0f) {
  const auto context = make_context();
  st::ui::Constraints constraints;
  constraints.max_width = width;
  constraints.max_height = height;
  constraints.available_width = width;
  constraints.available_height = height;
  element.measure(context, constraints);
  element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
}

[[nodiscard]] auto key_event(std::string key, bool ctrl = false) -> Event {
  Event event{};
  event.kind = EventKind::KeyDown;
  event.key = std::move(key);
  event.ctrl = ctrl;
  return event;
}

}  // namespace

/// 开关 → 自动变为可聚焦（否则键盘事件永远到不了它）。
ST_TEST(text_selectable_switch_makes_element_focusable) {
  st::ui::Chip chip("选中我");
  st::log::out("[selectable] chip 默认 selectable={} focusable={}\n", chip.text_selectable(),
               chip.focusable());
  ST_CHECK(chip.text_selectable());

  st::ui::Button button("按钮");
  ST_CHECK(!button.text_selectable());   // 交互控件默认不抢 Ctrl+C
  button.set_text_selectable(true);
  ST_CHECK(button.text_selectable());
  ST_CHECK(button.focusable());
}

/// 点击 → 拿焦点 + 整块选中（有可见高亮的那个状态位）。
ST_TEST(text_selectable_click_focuses_and_selects) {
  st::ui::KeyValueRow row("分支", "master");
  layout(row);
  RecordingHost host;
  row.set_host(&host);
  const auto context = make_context();

  ST_REQUIRE(row.text_selectable());
  ST_CHECK(!row.has_text_selection());

  Event down{};
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = st::math::Point{10.0f, 10.0f};
  const bool handled = row.on_event(context, down);

  st::log::out("[selectable] 点击 → handled={} focused={} selected={}\n", handled,
               host.focused_ == &row, row.has_text_selection());
  ST_CHECK(handled);
  ST_CHECK(host.focused_ == &row);    // **必须拿到焦点**
  ST_CHECK(row.has_text_selection());
}

/// `Ctrl+A` 置选中、`Ctrl+C` 把 `semantics_text()` 写进剪贴板、`Esc` 清选区。
ST_TEST(text_selectable_ctrl_a_ctrl_c_and_escape) {
  st::ui::KeyValueRow row("分支", "master");
  layout(row);
  RecordingHost host;
  row.set_host(&host);
  const auto context = make_context();

  auto select_all = key_event("a", true);
  ST_CHECK(row.on_event(context, select_all));
  ST_CHECK(row.has_text_selection());

  auto copy = key_event("c", true);
  ST_CHECK(row.on_event(context, copy));
  st::log::out("[selectable] 剪贴板收到：\"{}\"（期望含 \"{}\"）\n", host.clipboard,
               row.semantics_text());
  // 内容来自 `semantics_text()`——这是组件**已经有的**语义文本，不必再维护第二份。
  ST_CHECK(host.clipboard == row.semantics_text());
  ST_CHECK(!host.clipboard.empty());

  auto escape = key_event("Escape");
  ST_CHECK(row.on_event(context, escape));
  ST_CHECK(!row.has_text_selection());
}

/// 未开启时**一个键都不消费**（静态标签不能抢全局快捷键）。
///
/// 这条是防火墙：把开关默认改成 `true` 会让整个界面的 `Ctrl+A` / `Ctrl+C` 集体失灵。
ST_TEST(text_selectable_off_consumes_nothing) {
  st::ui::Button button("按钮");
  layout(button);
  RecordingHost host;
  button.set_host(&host);
  const auto context = make_context();

  auto select_all = key_event("a", true);
  st::log::out("[selectable] 未开启时 Ctrl+A handled={}\n", button.on_event(context, select_all));
  ST_CHECK(!button.on_event(context, select_all));
  ST_CHECK(!button.has_text_selection());
}
