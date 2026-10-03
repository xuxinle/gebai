/// 代码编辑器组件测试：编辑操作、光标/选择、撤销重做、缩进/注释、
/// 语法高亮（内置 + 自定义语言）、属性面与动作（控制通道驱动面）。
///
/// 测试用真实 `TextRenderer`（系统字体）作为文本端口；无系统字体的环境会退化为
/// `NullTextPort`（宽度为 0），此时与几何/命中相关的用例自动跳过，其余仍照常验证。

#include "st/app/text_port.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/theme.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace {

using st::ui::CodeEditor;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Theme;

/// 测试环境：主题 + 文本端口 + 已排布的编辑器。
struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  CodeEditor editor{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    editor.arrange(context, st::math::Rect{0.0f, 0.0f, 800.0f, 400.0f});
  }

  [[nodiscard]] auto has_font() const noexcept -> bool { return port != nullptr; }

  void type(std::string_view text) { editor.insert_text(text); }

  void press(std::string key, bool ctrl = false, bool shift = false) {
    Event event;
    event.kind = EventKind::KeyDown;
    event.key = std::move(key);
    event.ctrl = ctrl;
    event.shift = shift;
    (void)editor.on_event(context, event);
  }

  void type_text(std::string_view text) {
    Event event;
    event.kind = EventKind::TextInput;
    event.text = std::string(text);
    (void)editor.on_event(context, event);
  }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 基础编辑
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_starts_empty_and_accepts_text) {
  CodeEditor editor;
  ST_CHECK_EQ(editor.text(), std::string());
  ST_CHECK_EQ(editor.cursor_index(), 0U);
  ST_CHECK_EQ(editor.line_count(), 1U);

  editor.insert_text("hello");
  ST_CHECK_EQ(editor.text(), std::string("hello"));
  ST_CHECK_EQ(editor.cursor_index(), 5U);
}

ST_TEST(code_editor_set_text_resets_cursor_and_history) {
  CodeEditor editor;
  editor.insert_text("draft");
  ST_CHECK(editor.can_undo());
  editor.set_text("final");
  ST_CHECK_EQ(editor.text(), std::string("final"));
  ST_CHECK_EQ(editor.cursor_index(), 0U);
  ST_CHECK(!editor.can_undo());  // 载入新文档 → 历史清空
}

ST_TEST(code_editor_insert_delete_and_cursor_movement) {
  Fixture fx;
  fx.type("abc");
  fx.press("ArrowLeft");
  ST_CHECK_EQ(fx.editor.cursor_index(), 2U);
  fx.type("X");
  ST_CHECK_EQ(fx.editor.text(), std::string("abXc"));
  fx.press("Backspace");
  ST_CHECK_EQ(fx.editor.text(), std::string("abc"));
  fx.press("Delete");
  ST_CHECK_EQ(fx.editor.text(), std::string("ab"));
  fx.press("End");
  ST_CHECK_EQ(fx.editor.cursor_index(), 2U);
  fx.press("Home");
  ST_CHECK_EQ(fx.editor.cursor_index(), 0U);
}

ST_TEST(code_editor_handles_utf8_boundaries) {
  Fixture fx;
  fx.type("霜天");
  ST_CHECK_EQ(fx.editor.text(), std::string("霜天"));
  fx.press("ArrowLeft");
  ST_CHECK_EQ(fx.editor.cursor_index(), 3U);  // 一个汉字 3 字节
  fx.press("Backspace");
  ST_CHECK_EQ(fx.editor.text(), std::string("天"));
  fx.press("ArrowRight");
  fx.type("空");
  ST_CHECK_EQ(fx.editor.text(), std::string("天空"));
}

ST_TEST(code_editor_newline_inherits_indent) {
  Fixture fx;
  fx.type("    let x = 1;");
  fx.press("Enter");
  ST_CHECK_EQ(fx.editor.text(), std::string("    let x = 1;\n    "));
}

ST_TEST(code_editor_newline_after_open_brace_indents_and_closes) {
  Fixture fx;
  fx.type("fn main() {");
  fx.type("}");
  fx.press("ArrowLeft");  // 光标落在 } 之前
  fx.press("Enter");
  // `{|}` 展开为三行，光标停在中间行
  ST_CHECK_EQ(fx.editor.text(), std::string("fn main() {\n    \n}"));
  ST_CHECK_EQ(fx.editor.cursor_line(), 1U);
  ST_CHECK_EQ(fx.editor.cursor_column(), 4U);
}

ST_TEST(code_editor_vertical_movement_keeps_column) {
  Fixture fx;
  fx.type("longer line");
  fx.press("Enter");
  fx.type("short");
  fx.press("Home");
  fx.press("ArrowRight");
  fx.press("ArrowRight");
  fx.press("ArrowRight");
  ST_CHECK_EQ(fx.editor.cursor_line(), 1U);
  fx.press("ArrowUp");
  ST_CHECK_EQ(fx.editor.cursor_line(), 0U);
  ST_CHECK_EQ(fx.editor.cursor_column(), 3U);
  fx.press("ArrowDown");
  ST_CHECK_EQ(fx.editor.cursor_line(), 1U);
  ST_CHECK_EQ(fx.editor.cursor_column(), 3U);  // short 够长，列保持
}

// ————————————————————————————————————————————————————————————————————————————
// 选择
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_selection_and_replace) {
  Fixture fx;
  fx.type("hello world");
  fx.editor.set_selection(0, 5);
  ST_CHECK(fx.editor.has_selection());
  ST_CHECK_EQ(fx.editor.selected_text(), std::string("hello"));
  fx.type("bye");
  ST_CHECK_EQ(fx.editor.text(), std::string("bye world"));
  ST_CHECK(!fx.editor.has_selection());
}

ST_TEST(code_editor_shift_arrows_extend_selection) {
  Fixture fx;
  fx.type("abcdef");
  fx.press("Home");
  fx.press("ArrowRight", false, true);
  fx.press("ArrowRight", false, true);
  ST_CHECK_EQ(fx.editor.selected_text(), std::string("ab"));
  ST_CHECK_EQ(fx.editor.cursor_index(), 2U);
  // 反向扩展：Shift+左 收回选择
  fx.press("ArrowLeft", false, true);
  ST_CHECK_EQ(fx.editor.selected_text(), std::string("a"));
}

ST_TEST(code_editor_select_all_and_delete) {
  Fixture fx;
  fx.type("first\nsecond\nthird");
  fx.press("a", true);
  ST_CHECK_EQ(fx.editor.selected_text(), std::string("first\nsecond\nthird"));
  fx.press("Backspace");
  ST_CHECK_EQ(fx.editor.text(), std::string());
}

ST_TEST(code_editor_copy_paste_within_process) {
  Fixture fx;
  fx.type("payload");
  fx.press("a", true);
  fx.press("c", true);
  fx.press("End");
  fx.press("v", true);
  ST_CHECK_EQ(fx.editor.text(), std::string("payloadpayload"));
  // 剪切会移除源文本
  fx.editor.set_selection(0, 7);
  fx.press("x", true);
  ST_CHECK_EQ(fx.editor.text(), std::string("payload"));
}

ST_TEST(code_editor_word_movement_with_ctrl) {
  Fixture fx;
  fx.type("alpha beta gamma");
  fx.press("Home");
  fx.press("ArrowRight", true);  // 跳到 "beta" 之后
  fx.press("ArrowRight", true);
  ST_CHECK_EQ(fx.editor.cursor_index(), 10U);
  fx.press("ArrowLeft", true);  // 回退到 "beta" 词首（不是空白处）
  ST_CHECK_EQ(fx.editor.cursor_index(), 6U);
  fx.press("ArrowLeft", true);  // 再退到 "alpha" 词首
  ST_CHECK_EQ(fx.editor.cursor_index(), 0U);
}

// ————————————————————————————————————————————————————————————————————————————
// 撤销 / 重做
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_undo_redo_roundtrip) {
  Fixture fx;
  fx.type("one");
  fx.press("Enter");
  fx.type("two");
  fx.press("z", true);
  ST_CHECK(fx.editor.text() != std::string("one\ntwo"));
  fx.press("z", true);
  fx.press("z", true);
  fx.press("z", true);
  fx.press("z", true);
  ST_CHECK_EQ(fx.editor.text(), std::string());
  // 重做回到最后状态
  for (int step = 0; step < 8; ++step) {
    if (!fx.editor.can_redo()) break;
    fx.press("z", true, true);
  }
  ST_CHECK_EQ(fx.editor.text(), std::string("one\ntwo"));
}

ST_TEST(code_editor_redo_cleared_after_new_edit) {
  Fixture fx;
  fx.type("a");
  fx.editor.undo();
  ST_CHECK(fx.editor.can_redo());
  fx.type("b");
  ST_CHECK(!fx.editor.can_redo());
}

// ————————————————————————————————————————————————————————————————————————————
// 缩进与注释
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_indent_and_dedent_selection) {
  Fixture fx;
  fx.editor.set_text("one\ntwo\nthree");
  fx.editor.set_selection(0, fx.editor.text().size());
  fx.editor.indent_selection(false);
  ST_CHECK_EQ(fx.editor.text(), std::string("    one\n    two\n    three"));
  fx.editor.set_selection(0, fx.editor.text().size());
  fx.editor.indent_selection(true);
  ST_CHECK_EQ(fx.editor.text(), std::string("one\ntwo\nthree"));
}

ST_TEST(code_editor_toggle_comment_uses_language_marker) {
  Fixture fx;
  fx.editor.set_text("int a = 1;\nint b = 2;");
  fx.editor.set_language("cpp");
  fx.editor.set_selection(0, fx.editor.text().size());
  ST_CHECK(fx.editor.toggle_comment());
  ST_CHECK_EQ(fx.editor.text(), std::string("// int a = 1;\n// int b = 2;"));
  fx.editor.set_selection(0, fx.editor.text().size());
  ST_CHECK(fx.editor.toggle_comment());
  ST_CHECK_EQ(fx.editor.text(), std::string("int a = 1;\nint b = 2;"));

  // 无行注释标记的语言（JSON）无法注释
  fx.editor.set_language("json");
  ST_CHECK(!fx.editor.toggle_comment());
}

ST_TEST(code_editor_tab_inserts_spaces_or_indents_selection) {
  Fixture fx;
  fx.editor.set_tab_width(2);
  fx.press("Tab");
  ST_CHECK_EQ(fx.editor.text(), std::string("  "));
  // 有选择时 Tab 缩进行
  fx.editor.set_text("a\nb");
  fx.editor.set_selection(0, 3);
  fx.press("Tab");
  ST_CHECK_EQ(fx.editor.text(), std::string("  a\n  b"));
}

// ————————————————————————————————————————————————————————————————————————————
// 只读模式
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_read_only_blocks_edits_but_allows_selection) {
  Fixture fx;
  fx.editor.set_text("const char* s = \"readonly\";");
  fx.editor.set_read_only(true);
  fx.type("X");
  ST_CHECK_EQ(fx.editor.text(), std::string("const char* s = \"readonly\";"));
  fx.press("Backspace");
  fx.press("Enter");
  ST_CHECK_EQ(fx.editor.text(), std::string("const char* s = \"readonly\";"));
  fx.editor.select_all();
  ST_CHECK_EQ(fx.editor.selected_text(), std::string("const char* s = \"readonly\";"));
  ST_CHECK(fx.editor.get_property("read_only").value() == "true");
}

// ————————————————————————————————————————————————————————————————————————————
// 语法高亮
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_language_detection_and_switch) {
  CodeEditor editor;
  editor.set_language_from_path("src/main.rs");
  ST_CHECK_EQ(editor.language(), std::string("rust"));
  editor.set_language_from_path("unknown.weirdext");
  ST_CHECK_EQ(editor.language(), std::string("rust"));  // 无法识别则保持
  editor.set_language("python");
  ST_CHECK_EQ(editor.language(), std::string("python"));
}

ST_TEST(code_editor_highlight_renders_tokens) {
  Fixture fx;
  if (!fx.has_font()) return;  // 无字体环境跳过像素断言
  fx.editor.set_font_size(14.0f);
  fx.editor.set_language("cpp");
  fx.editor.set_text("// 注释\nint value = 42;");
  // 组件级单测（不挂根）：这里直接置位即可。**焦点相关的回归不能这么写**——
  // 直接 set_focused 会让「组件遮蔽了基类成员」这类缺陷读写落在同一侧而被掩盖，
  // 必须经 UiRoot::set_focus（见 tests/ui_focus_semantics_test.cpp）。
  fx.editor.set_focused(true);

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(800, 400, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  auto& mutable_editor = fx.editor;
  mutable_editor.apply_theme(fx.theme);
  mutable_editor.measure(fx.context, st::ui::Constraints{800.0f, 400.0f});
  mutable_editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 800.0f, 400.0f});
  mutable_editor.paint(fx.context, canvas);

  // 画面上出现了非背景像素（确实绘制了文字/行号槽）
  std::size_t ink = 0;
  for (const auto pixel : canvas.pixels()) {
    if ((pixel & 0xFFU) != 0U) ++ink;
  }
  ST_CHECK(ink > 200U);
}

ST_TEST(code_editor_accepts_custom_language_spec) {
  Fixture fx;
  st::text::LanguageSpec spec = st::text::make_language("mylog");
  st::text::with_aliases(spec, {"mlog"});
  st::text::with_keywords(spec, {"TRACE", "FATAL"});
  st::text::with_line_comment(spec, "#");
  fx.editor.set_language_spec(spec);
  ST_CHECK_EQ(fx.editor.language(), std::string("mylog"));
  fx.editor.set_text("TRACE ok\nFATAL bad  # 注释");
  // 自定义语言同样参与注释切换（用其行注释标记）
  fx.editor.set_selection(0, fx.editor.text().size());
  ST_CHECK(fx.editor.toggle_comment());
  ST_CHECK(fx.editor.text().starts_with("# TRACE ok"));
}

ST_TEST(code_editor_available_languages_includes_builtins) {
  const auto names = CodeEditor::available_languages();
  ST_CHECK(names.size() > 20U);
  bool has_cpp = false;
  bool has_python = false;
  for (const auto& name : names) {
    if (name == "cpp") has_cpp = true;
    if (name == "python") has_python = true;
  }
  ST_CHECK(has_cpp);
  ST_CHECK(has_python);
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面 / 动作（控制通道驱动）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_properties_roundtrip) {
  CodeEditor editor;
  editor.set_text("line one\nline two\nline three");
  editor.set_cursor_index(9);  // 第二行内
  ST_CHECK_EQ(*editor.get_property("line"), std::string("2"));
  ST_CHECK_EQ(*editor.get_property("lines"), std::string("3"));

  ST_CHECK(editor.set_property("language", "python"));
  ST_CHECK_EQ(editor.language(), std::string("python"));
  ST_CHECK(editor.set_property("read_only", "true"));
  ST_CHECK(editor.read_only());
  ST_CHECK(editor.set_property("read_only", "false"));
  ST_CHECK(!editor.read_only());
  ST_CHECK(editor.set_property("tab_width", "2"));
  ST_CHECK_EQ(editor.tab_width(), 2);

  // 选择属性：`begin:end`
  ST_CHECK(editor.set_property("selection", "0:4"));
  ST_CHECK_EQ(editor.selected_text(), std::string("line"));
  ST_CHECK_EQ(*editor.get_property("selection"), std::string("0:4"));

  // 跳行属性
  ST_CHECK(editor.set_property("goto_line", "3"));
  ST_CHECK_EQ(editor.cursor_line(), 2U);
  ST_CHECK(!editor.set_property("nope", "1"));
  ST_CHECK(!editor.get_property("nope").has_value());
}

ST_TEST(code_editor_actions_cover_agent_workflow) {
  CodeEditor editor;
  ST_CHECK(editor.invoke_action("insert", "hello world"));
  ST_CHECK_EQ(editor.text(), std::string("hello world"));
  ST_CHECK(editor.invoke_action("select_all", ""));
  ST_CHECK(editor.invoke_action("copy", ""));
  ST_CHECK(editor.invoke_action("goto_line", "1"));
  ST_CHECK(editor.invoke_action("undo", ""));
  ST_CHECK_EQ(editor.text(), std::string());
  ST_CHECK(editor.invoke_action("redo", ""));
  ST_CHECK_EQ(editor.text(), std::string("hello world"));
  ST_CHECK(editor.invoke_action("paste", ""));
  ST_CHECK_EQ(editor.text(), std::string("hello worldhello world"));
  ST_CHECK(editor.invoke_action("clear_selection", ""));
  ST_CHECK(!editor.has_selection());
  ST_CHECK(!editor.invoke_action("no-such-action", ""));
  ST_CHECK(editor.invoke_action("goto_line", "not-a-number") == false);
}

ST_TEST(code_editor_semantics_expose_state) {
  CodeEditor editor;
  editor.set_id("editor");
  editor.set_language("cpp");
  editor.set_text("int x = 1;\nint y = 2;");
  editor.set_cursor_index(3);  // 第一行内
  ST_CHECK_EQ(editor.semantics_text(), std::string("cpp"));
  ST_CHECK_EQ(editor.semantics_value(), std::string("int x = 1;"));
  const auto flags = editor.semantics_flags();
  ST_CHECK(flags.editable);
  ST_CHECK(flags.scrollable);
  ST_CHECK_EQ(editor.type(), std::string_view("CodeEditor"));
  ST_CHECK(editor.role() == st::ui::Role::Code);
}

// ————————————————————————————————————————————————————————————————————————————
// 几何与滚动
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_scroll_clamps_within_content) {
  Fixture fx;
  fx.editor.set_text("a\nb\nc\nd\ne\nf\ng\nh");
  fx.editor.set_scroll_offset(0.0f, 10000.0f);
  // 滚动上限由内容高度决定（这里只验证不崩且能读回）
  ST_CHECK(fx.editor.scroll_offset().y >= 0.0f);
  fx.editor.scroll_to_line(3);
  ST_CHECK(fx.editor.scroll_offset().y >= 0.0f);
}

ST_TEST(code_editor_point_mapping_roundtrip) {
  Fixture fx;
  if (!fx.has_font()) return;  // 无字体时宽度为 0，命中无意义
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("void run() {\n  return;\n}");
  fx.editor.measure(fx.context, st::ui::Constraints{800.0f, 400.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 800.0f, 400.0f});
  st::ui::Event click;
  click.kind = EventKind::MouseDown;
  click.position = st::math::Point{60.0f, 8.0f};  // 第一行某处
  ST_CHECK(fx.editor.on_event(fx.context, click));
  ST_CHECK(fx.editor.cursor_index() <= 12U);  // 落在第一行内
  ST_CHECK_EQ(fx.editor.cursor_line(), 0U);
}

ST_TEST(code_editor_double_click_selects_word) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_font_size(14.0f);
  fx.editor.set_text("alpha beta");
  fx.editor.measure(fx.context, st::ui::Constraints{800.0f, 400.0f});
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 800.0f, 400.0f});
  st::ui::Event event;
  event.kind = EventKind::DoubleClick;
  event.position = st::math::Point{12.0f, 8.0f};
  ST_CHECK(fx.editor.on_event(fx.context, event));
  const std::string picked = fx.editor.selected_text();
  ST_CHECK(picked == "alpha" || picked == "lpha" || picked == "alph");
}

// ————————————————————————————————————————————————————————————————————————————
// 查找与替换
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_find_counts_and_highlights_matches) {
  Fixture fx;
  fx.type("foo bar foo baz foo");
  const std::size_t count = fx.editor.set_find("foo");
  ST_CHECK_EQ(count, static_cast<std::size_t>(3));
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(3));
  // 大小写不敏感
  ST_CHECK_EQ(fx.editor.set_find("FOO"), static_cast<std::size_t>(3));
  // 大小写敏感：只剩小写
  ST_CHECK_EQ(fx.editor.set_find("FOO", true), static_cast<std::size_t>(0));
  // 中文
  ST_CHECK_EQ(fx.editor.set_find("\u4e2d\u6587"), fx.editor.find_match_count());  // 无中文命中=0
  fx.editor.clear_find();
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(0));
}

ST_TEST(code_editor_find_next_selects_and_wraps) {
  Fixture fx;
  fx.type("aa bb aa cc aa");
  ST_CHECK_EQ(fx.editor.set_find("aa"), static_cast<std::size_t>(3));
  // 光标在 0：第一个命中 [0,2)
  ST_CHECK_EQ(fx.editor.find_next(false), static_cast<std::size_t>(0));
  const auto [b1, e1] = fx.editor.selection();
  ST_CHECK_EQ(b1, static_cast<std::size_t>(0));
  ST_CHECK_EQ(e1, static_cast<std::size_t>(2));
  ST_CHECK_EQ(fx.editor.find_next(false), static_cast<std::size_t>(1));
  ST_CHECK_EQ(fx.editor.find_next(false), static_cast<std::size_t>(2));
  // 环绕
  ST_CHECK_EQ(fx.editor.find_next(false), static_cast<std::size_t>(0));
  // 反向
  ST_CHECK_EQ(fx.editor.find_next(true), static_cast<std::size_t>(2));
  ST_CHECK_EQ(fx.editor.find_active_index(), static_cast<std::size_t>(2));
}

ST_TEST(code_editor_replace_current_and_advance) {
  Fixture fx;
  fx.type("one two one two one");
  fx.editor.set_find("two");
  fx.editor.find_next(false);  // 选中第一个 two
  ST_CHECK(fx.editor.replace_current("XX"));
  ST_CHECK_EQ(fx.editor.text(), "one XX one two one");
  // 替换后跳到下一个命中（第二个 two）
  ST_CHECK_EQ(fx.editor.find_active_index(), static_cast<std::size_t>(0));
  const auto [b, e] = fx.editor.selection();
  ST_CHECK_EQ(fx.editor.text().substr(b, e - b), "two");
}

ST_TEST(code_editor_replace_all_and_undo) {
  Fixture fx;
  fx.type("k1 v k1 v k1");
  fx.editor.set_find("k1");
  ST_CHECK_EQ(fx.editor.replace_all("KEY"), static_cast<std::size_t>(3));
  ST_CHECK_EQ(fx.editor.text(), "KEY v KEY v KEY");
  // 全部替换可一次撤销
  ST_CHECK(fx.editor.undo());
  ST_CHECK_EQ(fx.editor.text(), "k1 v k1 v k1");
}

ST_TEST(code_editor_edit_refreshes_matches) {
  Fixture fx;
  fx.type("abc abc");
  fx.editor.set_find("abc");
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(2));
  fx.type("abc ");  // 光标在文末追加 → 3 个命中
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(3));
}

ST_TEST(code_editor_find_actions_via_invoke) {
  Fixture fx;
  fx.type("lorem ipsum lorem");
  // find 动作（argument 语法 "needle" / "needle|1"）
  ST_CHECK(fx.editor.invoke_action("find", "lorem"));
  ST_CHECK_EQ(fx.editor.get_property("find_matches").value(), "2");
  ST_CHECK(fx.editor.invoke_action("find_next", ""));
  ST_CHECK_EQ(fx.editor.get_property("find_active").value(), "0");
  ST_CHECK(fx.editor.invoke_action("replace", "X"));
  ST_CHECK_EQ(fx.editor.text(), "X ipsum lorem");
  ST_CHECK(fx.editor.invoke_action("replace_all", "Y"));
  ST_CHECK_EQ(fx.editor.text(), "X ipsum Y");
  ST_CHECK(fx.editor.invoke_action("clear_find", ""));
  ST_CHECK_EQ(fx.editor.get_property("find_matches").value(), "0");
}

ST_TEST(code_editor_read_only_blocks_replace) {
  Fixture fx;
  fx.type("data data");
  fx.editor.set_read_only(true);
  fx.editor.set_find("data");
  fx.editor.find_next(false);
  ST_CHECK(!fx.editor.replace_current("x"));
  ST_CHECK_EQ(fx.editor.replace_all("x"), static_cast<std::size_t>(0));
  ST_CHECK_EQ(fx.editor.text(), "data data");
}
