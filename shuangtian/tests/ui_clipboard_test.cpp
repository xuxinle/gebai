/// 剪贴板写侧（`HostFocus::host_set_clipboard_text`）与各文本组件 copy 动作面的测试。
///
/// 覆盖：UiRoot 读写 provider 的转发与未装配时如实返回 false、
/// Text/Input/TextArea/CodeEditor/MarkdownView 的 copy 动作面、
/// Input/TextArea 的 Ctrl+C 键盘路径、headless 后端的读写闭环。
///
/// 姿态：组件测试装**假 provider**（std::string 捕获）断言写入内容与返回值；
/// headless 后端闭环在 shell 层测（backend 测试文件）。

#include <string>
#include <utility>

#include "st/test/test.hpp"

#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 假剪贴板宿主：记录最后一次写入（成功/失败两态可配）。
/// 组件测试不依赖真后端——「写没写、写了什么」才是这里的断言对象。
class FakeClipboardHost : public st::ui::UiRoot {
 public:
  std::string last_written{};
  bool accept_writes{true};

  auto host_set_clipboard_text(std::string_view text) -> bool override {
    if (!accept_writes) return false;
    last_written = std::string(text);
    return true;
  }

  [[nodiscard]] auto host_clipboard_text() -> std::string override { return last_written; }
};

/// 造一个带修饰键的键盘事件（组件 on_event 直驱）。
[[nodiscard]] auto ctrl_key_event(const std::string& key) -> st::ui::Event {
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = key;
  event.ctrl = true;
  return event;
}

/// KeyDown 路径不取字体几何，但 RenderContext 需要 theme 引用。
[[nodiscard]] auto idle_context() -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, nullptr, 0.0};
}

}  // namespace

ST_TEST(clipboard_write_side_defaults_to_false_without_provider) {
  // 未装配写 provider：如实返回 false（组件据此上报“复制失败”，不假装成功）。
  st::ui::UiRoot root;
  ST_CHECK(!root.host_set_clipboard_text("x"));
  ST_CHECK(root.host_clipboard_text().empty());
}

ST_TEST(clipboard_write_provider_roundtrip) {
  FakeClipboardHost root;
  ST_CHECK(root.host_set_clipboard_text("你好 clipboard"));
  ST_CHECK_EQ(root.last_written, std::string("你好 clipboard"));
  ST_CHECK_EQ(root.host_clipboard_text(), std::string("你好 clipboard"));
  // 写失败透传。
  root.accept_writes = false;
  ST_CHECK(!root.host_set_clipboard_text("again"));
}

ST_TEST(text_copy_action_writes_content_to_clipboard) {
  FakeClipboardHost root;
  st::ui::Text label("状态：运行中");
  // 挂上宿主（host_ 由树挂载赋值；这里直接 set_host 模拟）。
  label.set_host(&root);
  ST_CHECK(label.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("状态：运行中"));
  // Heading 继承同一条路径。
  st::ui::Heading heading("标题文本", 2);
  heading.set_host(&root);
  ST_CHECK(heading.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("标题文本"));
  // 空内容 → false。
  st::ui::Text empty;
  empty.set_host(&root);
  ST_CHECK(!empty.invoke_action("copy", {}));
  // 无宿主 → false。
  st::ui::Text orphan("有内容但没宿主");
  ST_CHECK(!orphan.invoke_action("copy", {}));
}

ST_TEST(input_copy_action_and_ctrl_c) {
  FakeClipboardHost root;
  st::ui::Input field;
  field.set_host(&root);
  field.set_text("hello 单行");
  // 动作面。
  ST_CHECK(field.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("hello 单行"));
  // 键盘 Ctrl+C（组件级事件，不经过 root 分发——直接喂 on_event）。
  auto event = ctrl_key_event("c");
  ST_CHECK(field.on_event(idle_context(), event));
  ST_CHECK_EQ(root.last_written, std::string("hello 单行"));
  // 密码框与其他文本框同口径：掩码只影响**画成什么**，剪贴板拿到明文。
  // （与浏览器 `<input type=password>` 一致：能选就能拷。）
  field.set_password(true);
  ST_CHECK(field.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("hello 单行"));
  // 空文本 → false。
  field.set_password(false);
  field.set_text({});
  ST_CHECK(!field.invoke_action("copy", {}));
}

ST_TEST(input_ctrl_v_pastes_from_clipboard) {
  // 粘贴：框架之前只有 `Ctrl+C`/`Ctrl+X`/`Ctrl+A`，**没有 `Ctrl+V`**——
  // 而 `CodeEditor` 与 `Terminal` 都有。用户报“输入框粘贴不了”就是这条。
  FakeClipboardHost root;
  st::ui::Input field;
  field.set_host(&root);
  field.set_text("ab");
  field.set_cursor_index(2);
  ST_CHECK(root.host_set_clipboard_text("CD"));

  auto event = ctrl_key_event("v");
  ST_CHECK(field.on_event(idle_context(), event));
  ST_CHECK_EQ(field.value(), std::string("abCD"));

  // 有选区时粘贴应当**替换**选区（通行行为，走 `replace_selection_with`）。
  field.set_selection(0, 2);
  ST_REQUIRE(field.has_selection());
  ST_CHECK(root.host_set_clipboard_text("XY"));
  auto replace_event = ctrl_key_event("v");
  ST_CHECK(field.on_event(idle_context(), replace_event));
  ST_CHECK_EQ(field.value(), std::string("XYCD"));

  // 单行框：换行一律降成空格（直接插 `\n` 会得到一个看不见但存在的换行）。
  ST_CHECK(root.host_set_clipboard_text("一\r\n二"));
  field.set_selection(0, field.value().size());
  auto multiline_event = ctrl_key_event("v");
  ST_CHECK(field.on_event(idle_context(), multiline_event));
  ST_CHECK_EQ(field.value(), std::string("一  二"));
}

ST_TEST(textarea_ctrl_v_pastes_and_keeps_newlines) {
  // 多行框反过来：**保留换行**，并把 `\r\n`/裸 `\r` 归成 `\n`
  //（`layout_lines` 只认 `\n`，留着 `\r` 会变成不可见的多余字符）。
  FakeClipboardHost root;
  st::ui::TextArea area;
  area.set_host(&root);
  ST_CHECK(root.host_set_clipboard_text("第一行\r\n第二行\r第三行"));

  auto event = ctrl_key_event("v");
  ST_CHECK(area.on_event(idle_context(), event));
  ST_CHECK_EQ(area.value(), std::string("第一行\n第二行\n第三行"));

  // 只读：粘贴被拦（不能改内容）。
  area.set_read_only(true);
  const std::string before = area.value();
  ST_CHECK(root.host_set_clipboard_text("不该进来"));
  auto blocked = ctrl_key_event("v");
  ST_CHECK(area.on_event(idle_context(), blocked));
  ST_CHECK_EQ(area.value(), before);
}

ST_TEST(textarea_copy_action_and_ctrl_c_readonly_too) {
  FakeClipboardHost root;
  st::ui::TextArea area;
  area.set_host(&root);
  area.set_text("多行\n文本域");
  ST_CHECK(area.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("多行\n文本域"));
  // 只读下仍可复制（只读展示正是复制的高频场景）。
  area.set_read_only(true);
  ST_CHECK(area.invoke_action("copy", {}));
  // Ctrl+C 键盘路径。
  auto event = ctrl_key_event("c");
  ST_CHECK(area.on_event(idle_context(), event));
  ST_CHECK_EQ(root.last_written, std::string("多行\n文本域"));
}

ST_TEST(code_editor_copy_is_dual_track_and_paste_prefers_platform) {
  FakeClipboardHost root;
  st::ui::CodeEditor editor;
  editor.set_host(&root);
  editor.set_text("alpha beta");
  editor.set_selection(0, 5);
  // copy：进程内 + 平台双写。
  ST_CHECK(editor.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("alpha"));
  // paste：优先平台剪贴板（外部内容进得来）。
  root.last_written = "外部文本";
  editor.set_selection(0, 0);
  editor.insert_text(editor.paste_text_from_clipboard());
  ST_CHECK_EQ(editor.text(), std::string("外部文本alpha beta"));
  // 平台剪贴板为空时回退进程内那轨。
  root.last_written.clear();
  ST_CHECK_EQ(editor.paste_text_from_clipboard(), std::string("alpha"));
}

ST_TEST(markdown_view_copy_action_writes_source) {
  FakeClipboardHost root;
  st::ui::MarkdownView view;
  view.set_host(&root);
  view.set_markdown("# 标题\n\n正文 `code`");
  ST_CHECK(view.invoke_action("copy", {}));
  ST_CHECK_EQ(root.last_written, std::string("# 标题\n\n正文 `code`"));
}
