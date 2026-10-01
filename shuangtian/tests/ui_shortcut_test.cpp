/// 全局快捷键与事件派发顺序测试（编辑器形态的骨架能力）。
///
/// 覆盖四条派发契约：
/// 1. 快捷键表**先于**焦点链：文本编辑器持有焦点时 Ctrl+S 仍能触发全局保存；
/// 2. 未识别组合键必须放行冒泡（`on_event` 返回值契约：true=消费/false=放行）；
/// 3. Ctrl+Shift+Tab 不被 Tab 焦点遍历截胡：先给焦点元素，未消费才进焦点环
///    （且 `consumes_key` 声明「Tab 自含」的编辑器不参加焦点环）；
/// 4. 不可见/不拦截的浮层不截命中与键盘（关闭的命令面板不再挡住文件树）。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::CodeEditor;
using st::ui::Dialog;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::UiRoot;

[[nodiscard]] auto key_event(const std::string& key, bool ctrl = false, bool shift = false)
    -> Event {
  Event event;
  event.kind = EventKind::KeyDown;
  event.key = key;
  event.ctrl = ctrl;
  event.shift = shift;
  return event;
}

/// 按下并派发一个键（Key 事件是一次性的，辅助函数收敛写法）。
[[nodiscard]] auto press(UiRoot& root, const std::string& key, bool ctrl = false,
                        bool shift = false) -> bool {
  Event event = key_event(key, ctrl, shift);
  return root.dispatch(event);
}

/// 不认识任何键的占位组件（默认 on_event false → 冒泡）。
class Plain : public st::ui::Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Plain"; }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 快捷键表先于焦点链
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(shortcut_fires_before_focused_editor) {
  UiRoot root;
  auto editor = std::make_unique<CodeEditor>();
  editor->set_focusable(true);
  auto* editor_ptr = editor.get();
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->add_child(std::move(editor));
  root.set_content(std::move(page));
  root.layout(true);
  root.set_focus(editor_ptr);
  ST_CHECK(root.focused() == editor_ptr);

  int saved = 0;
  ST_CHECK(root.register_shortcut(
      "s", UiRoot::Shortcut{.ctrl = true}, [&saved]() {
        ++saved;
        return true;
      }));

  // 编辑器持有焦点（还吃下了 Ctrl+A 全选）——Ctrl+S 仍应触发全局保存
  ST_CHECK(press(root, "a", true));
  ST_CHECK_EQ(editor_ptr->text(), std::string(""));
  ST_CHECK(press(root, "s", true));
  ST_CHECK_EQ(saved, 1);

  // 不带修饰的普通 s 不是这条快捷键：裸字符的 KeyDown 也不被编辑器消费
  // （字符输入走 TextInput 通道），事件放行到根
  ST_CHECK(!press(root, "s", false));

  // handler 放弃消费（返回 false）：事件继续下沉给焦点元素
  int passes = 0;
  ST_CHECK(root.register_shortcut(
      "k", UiRoot::Shortcut{.ctrl = true}, [&passes]() {
        ++passes;
        return false;  // 不消费
      }));
  (void)press(root, "k", true);
  ST_CHECK_EQ(passes, 1);
}

ST_TEST(shortcut_unregister_and_re_register) {
  UiRoot root;
  int hits = 0;
  ST_CHECK(root.register_shortcut(
      "w", UiRoot::Shortcut{.ctrl = true}, [&hits]() {
        ++hits;
        return true;
      }));
  ST_CHECK_EQ(root.shortcut_count(), std::size_t{1});
  ST_CHECK(press(root, "w", true));
  ST_CHECK_EQ(hits, 1);

  root.unregister_shortcut("w", UiRoot::Shortcut{.ctrl = true});
  ST_CHECK_EQ(root.shortcut_count(), std::size_t{0});
  (void)press(root, "w", true);  // 已注销：不再触发
  ST_CHECK_EQ(hits, 1);

  // 空键/空 handler 拒绝注册
  ST_CHECK(!root.register_shortcut("", {}, []() { return true; }));
  ST_CHECK(!root.register_shortcut("x", {}, nullptr));
  ST_CHECK_EQ(root.shortcut_count(), std::size_t{0});
}

// ————————————————————————————————————————————————————————————————————————————
// ② 未识别组合键放行冒泡（on_event 返回值契约）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_releases_unknown_combos) {
  CodeEditor editor;
  st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, nullptr, 0.0};

  // 认识的键：消费；未识别的组合/裸键：放行（全局快捷键的落点）
  Event left = key_event("ArrowLeft");
  Event undo = key_event("z", true);
  Event ctrl_w = key_event("w", true);
  Event ctrl_f5 = key_event("F5", true);
  Event f1 = key_event("F1");
  Event shift_tab = key_event("Tab", true, true);
  ST_CHECK(editor.on_event(context, left));
  ST_CHECK(editor.on_event(context, undo));
  ST_CHECK(!editor.on_event(context, ctrl_w));
  ST_CHECK(!editor.on_event(context, ctrl_f5));
  ST_CHECK(!editor.on_event(context, f1));
  ST_CHECK(!editor.on_event(context, shift_tab));
}

// ————————————————————————————————————————————————————————————————————————————
// ③ Ctrl+Shift+Tab 不被焦点遍历截胡；编辑器不参加 Tab 焦点环
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(shift_tab_reaches_focused_element_first) {
  UiRoot root;
  auto panel = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto* first = panel->add_child(std::make_unique<Plain>());
  auto* second = panel->add_child(std::make_unique<Plain>());
  auto* third = panel->add_child(std::make_unique<Plain>());
  first->set_focusable(true);
  second->set_focusable(true);
  third->set_focusable(true);
  root.set_content(std::move(panel));
  root.layout(true);
  root.set_focus(first);

  // Tab 前进（无组件消费 → 焦点环）
  ST_CHECK(press(root, "Tab"));
  ST_CHECK(root.focused() == second);

  // Shift+Tab 后退（同一条路径，不再被截胡）
  ST_CHECK(press(root, "Tab", false, true));
  ST_CHECK(root.focused() == first);
  (void)third;

  // 焦点元素若消费了 Tab（声明自含语义），不再进入焦点环
  class TabEater : public Plain {
   public:
    auto on_event(const st::ui::RenderContext&, Event& event) -> bool override {
      return event.kind == EventKind::KeyDown && event.key == "Tab";
    }
  };
  UiRoot root2;
  auto panel2 = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto* eater = panel2->add_child(std::make_unique<TabEater>());
  auto* other = panel2->add_child(std::make_unique<Plain>());
  eater->set_focusable(true);
  other->set_focusable(true);
  root2.set_content(std::move(panel2));
  root2.layout(true);
  root2.set_focus(eater);
  ST_CHECK(press(root2, "Tab"));
  ST_CHECK(root2.focused() == eater);  // 被组件消费：焦点不动
}

ST_TEST(code_editor_tab_stays_inside_until_read_only) {
  CodeEditor editor;
  // 可编辑：Tab 自含（缩进），焦点环跳过它
  ST_CHECK(!editor.consumes_key("Tab"));
  ST_CHECK(editor.consumes_key("Enter"));
  // 只读：Tab 交回焦点环
  editor.set_read_only(true);
  ST_CHECK(editor.consumes_key("Tab"));
}

// ————————————————————————————————————————————————————————————————————————————
// ④ 不可见/不拦截的浮层不截命中与键盘
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(invisible_overlay_stops_intercepting_input) {
  UiRoot root;

  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto plain = std::make_unique<Plain>();
  plain->set_focusable(true);
  plain->style().width = 200.0f;   // 有尺寸才在命中测试范围内
  plain->style().height = 100.0f;
  st::ui::Element* button = plain.get();
  page->add_child(std::move(plain));
  root.set_content(std::move(page));

  // 模态浮层：可见时拦截指针命中与 Esc；未知键穿透（Ctrl+S 这类全局语义仍可达）
  auto dialog = std::make_unique<Dialog>("标题", "正文");
  Dialog* dialog_ptr = dialog.get();
  root.add_overlay(std::move(dialog));
  root.layout(true);

  st::ui::Event escape = key_event("Escape");
  ST_CHECK(root.dispatch(escape));     // 浮层吃到 Esc（模态语义）
  ST_CHECK(root.hit_test(st::math::Point{10.0f, 10.0f}) != button);  // 可见：命中归浮层
  st::ui::Event move;
  move.kind = EventKind::MouseMove;
  move.position = st::math::Point{10.0f, 10.0f};
  ST_CHECK(root.dispatch(move));       // 指针被模态吞掉

  // 关闭（隐藏）：不再拦截——命中回到内容、Esc 不再被已关闭的对话框吃掉
  dialog_ptr->set_visible(false);
  ST_CHECK(root.hit_test(st::math::Point{10.0f, 10.0f}) == button);

  // 无快捷键、无浮层拦截：键下沉焦点链，占位组件不认识 → 放行（如实返回未处理）
  root.set_focus(button);
  ST_CHECK(!press(root, "s", true));

  // 键盘浮层遍历也跳过不可见浮层：Esc 快捷键可以接管全局语义
  int hits = 0;
  ST_CHECK(root.register_shortcut(
      "Escape", {}, [&hits]() {
        ++hits;
        return true;
      }));
  ST_CHECK(press(root, "Escape"));
  ST_CHECK_EQ(hits, 1);
}

// ————————————————————————————————————————————————————————————————————————————
// ⑤ 行为注入（P2-1）：免子类化的小交互
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(event_handler_injects_behavior) {
  UiRoot root;
  auto plain = std::make_unique<Plain>();
  plain->set_focusable(true);
  Plain* plain_ptr = plain.get();
  root.set_content(std::move(plain));
  root.layout(true);

  int handled_keys = 0;
  plain_ptr->set_event_handler([&handled_keys](Event& event) {
    if (event.kind == EventKind::KeyDown && event.key == "F5") {
      ++handled_keys;
      return true;  // 消费
    }
    return false;   // 其余放行
  });
  ST_CHECK(plain_ptr->has_event_handler());

  root.set_focus(plain_ptr);
  const bool f5 = press(root, "F5");
  ST_CHECK(f5);                                    // handler 消费
  ST_CHECK_EQ(handled_keys, 1);
  const bool f6 = press(root, "F6");
  ST_CHECK(!f6);                                   // 放行 → 全局无处理
  ST_CHECK_EQ(handled_keys, 1);
}
