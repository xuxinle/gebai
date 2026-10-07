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

#include <cstdio>
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

  // 字号：两个入口各写各的输入，互不覆盖
  ST_CHECK(editor.set_property("font_scale", "0.8"));
  ST_CHECK(std::abs(editor.font_scale() - 0.8F) < 0.001F);
  ST_CHECK(editor.set_property("font_size", "20"));
  ST_CHECK(std::abs(editor.font_size_override() - 20.0F) < 0.001F);   // 输入侧
  ST_CHECK(std::abs(editor.font_size() - 20.0F) < 0.001F);            // 显式值优先
  ST_CHECK(editor.set_property("font_size", "-1"));                  // 复位 = 跟随主题
  ST_CHECK(editor.font_size_override() < 0.0F);                       // 输入侧已复位
  ST_CHECK(editor.font_size() > 0.0F);                                // 实际字号恒为正

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

/// 全词匹配是**真选项**，不是把词改写成 `\b词\b` 的把戏。
///
/// 为何要这条用例：gbcode 的查找条里曾用“改写查找词”来假装支持全词，
/// 而 `set_find` 只做字面量匹配——`\b` 被当成两个真字符，于是**一处也匹配不到**，
/// 而调用方（与用户）看到的是“勾了全词、命中数变成 0”。
ST_TEST(code_editor_find_whole_word_is_a_real_option) {
  Fixture fx;
  fx.type("cat category cat concat cat");
  // 不带全词：5 处都含 "cat"
  ST_CHECK_EQ(fx.editor.set_find("cat"), static_cast<std::size_t>(5));
  // 全词：只剩两处独立单词 cat
  const CodeEditor::FindOptions whole{.whole_word = true};
  ST_CHECK_EQ(fx.editor.set_find("cat", whole), static_cast<std::size_t>(3));
  ST_CHECK(fx.editor.find_options().whole_word);
  // 字面量里的 `\b` 不当边界（口径不能靠改字符串实现）
  ST_CHECK_EQ(fx.editor.set_find("\\bcat\\b"), static_cast<std::size_t>(0));
  // 选项回到默认后命中数也要回去（两个选项都要能关）
  ST_CHECK_EQ(fx.editor.set_find("cat", CodeEditor::FindOptions{}), static_cast<std::size_t>(5));
  ST_CHECK(!fx.editor.find_options().whole_word);
  // 大小写 + 全词可共存
  fx.editor.set_text("Foo foo fooBar");
  ST_CHECK_EQ(fx.editor.set_find("foo", CodeEditor::FindOptions{.case_sensitive = true,
                                                                .whole_word = true}),
              static_cast<std::size_t>(1));
}

/// 属性面改查找选项必须**当场重建命中表**：
/// 否则“勾了全词、命中数还是旧的”，而高亮画的是旧命中——选项与界面静默分岔。
ST_TEST(code_editor_find_option_properties_rebuild_matches) {
  Fixture fx;
  fx.type("cat category cat");
  fx.editor.set_find("cat");
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(3));
  ST_CHECK(fx.editor.set_property("find_word", "true"));
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(2));
  ST_CHECK_EQ(fx.editor.get_property("find_word").value(), "true");
  ST_CHECK(fx.editor.set_property("find_word", "false"));
  ST_CHECK_EQ(fx.editor.find_match_count(), static_cast<std::size_t>(3));
}

// ————————————————————————————————————————————————————————————————————————————
// Ctrl+D（选中下一处同词）与 Alt+↑/↓（上下移行）
// ————————————————————————————————————————————————————————————————————————————
//
// 这两条的背景：gbcode 的帮助卡列着 “Ctrl+D 选中下一处同词”与
// “Alt+↑/↓ 上/下移当前行”，而**实现里一个字都没写**——键按下去毫无反应，
// 用户看到一个恒真的“功能列表”。这里把它们钉在组件层。

ST_TEST(code_editor_select_next_occurrence_takes_word_then_walks) {
  Fixture fx;
  fx.editor.set_text("alpha beta alpha gamma alpha");
  fx.editor.set_cursor_index(2);  // 落在第一个 alpha 里
  // ① 首次调用：把光标处的词选上
  ST_CHECK(fx.editor.select_next_occurrence());
  ST_CHECK_EQ(fx.editor.selected_text(), "alpha");
  const auto [b1, e1] = fx.editor.selection();
  ST_CHECK_EQ(b1, static_cast<std::size_t>(0));
  ST_CHECK_EQ(e1, static_cast<std::size_t>(5));
  // ② 再次调用：走到第二处
  ST_CHECK(fx.editor.select_next_occurrence());
  const auto [b2, e2] = fx.editor.selection();
  ST_CHECK_EQ(b2, static_cast<std::size_t>(11));
  ST_CHECK_EQ(fx.editor.selected_text(), "alpha");
  // ③ 第三处
  ST_CHECK(fx.editor.select_next_occurrence());
  ST_CHECK_EQ(fx.editor.selection().first, static_cast<std::size_t>(23));
  // ④ 已到最后一处：环绕回第一处（而不是“无反应”）
  ST_CHECK(fx.editor.select_next_occurrence());
  ST_CHECK_EQ(fx.editor.selection().first, static_cast<std::size_t>(0));
  // ⑤ 全文只此一处：返回 false 且**不改动**选区
  fx.editor.set_text("only one token");
  fx.editor.set_cursor_index(1);
  ST_CHECK(fx.editor.select_next_occurrence());
  ST_CHECK_EQ(fx.editor.selected_text(), "only");
  ST_CHECK(!fx.editor.select_next_occurrence());
  ST_CHECK_EQ(fx.editor.selected_text(), "only");
}

ST_TEST(code_editor_ctrl_d_is_wired_to_handle_key) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_text("value value value");
  fx.editor.set_cursor_index(1);
  fx.press("d", /*ctrl=*/true);
  ST_CHECK_EQ(fx.editor.selected_text(), "value");
  fx.press("d", /*ctrl=*/true);
  ST_CHECK_EQ(fx.editor.selection().first, static_cast<std::size_t>(6));
  // 动作面与键盘同一条路（避免“快捷键能用、控制通道不能用”）
  ST_CHECK(fx.editor.invoke_action("select_next_occurrence", ""));
  ST_CHECK_EQ(fx.editor.selection().first, static_cast<std::size_t>(12));
}

ST_TEST(code_editor_move_lines_up_and_down) {
  Fixture fx;
  fx.editor.set_text("one\ntwo\nthree");
  fx.editor.set_cursor_index(0);
  // 首行还往上：不越界，如实返回 false
  ST_CHECK(!fx.editor.move_lines(-1));
  ST_CHECK_EQ(fx.editor.text(), "one\ntwo\nthree");
  // 下移一行（光标随行）
  ST_CHECK(fx.editor.move_lines(1));
  ST_CHECK_EQ(fx.editor.text(), "two\none\nthree");
  ST_CHECK_EQ(fx.editor.cursor_line(), 1U);
  // 再上移回到原位
  ST_CHECK(fx.editor.move_lines(-1));
  ST_CHECK_EQ(fx.editor.text(), "one\ntwo\nthree");
  ST_CHECK_EQ(fx.editor.cursor_line(), 0U);
  // 末行还往下：不越界
  fx.editor.set_cursor_index(fx.editor.text().size());
  ST_CHECK(!fx.editor.move_lines(1));
  // 可撤销（与其它编辑动作同一个撤销栈）
  ST_CHECK(fx.editor.move_lines(-1));
  ST_CHECK_EQ(fx.editor.text(), "one\nthree\ntwo");
  ST_CHECK(fx.editor.undo());
  ST_CHECK_EQ(fx.editor.text(), "one\ntwo\nthree");
  // 只读下不动
  fx.editor.set_read_only(true);
  ST_CHECK(!fx.editor.move_lines(1));
}

ST_TEST(code_editor_alt_arrows_move_lines_via_handle_key) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_text("aaa\nbbb\nccc");
  fx.editor.set_cursor_index(0);
  Event down;
  down.kind = EventKind::KeyDown;
  down.key = "ArrowDown";
  down.alt = true;
  ST_CHECK(fx.editor.on_event(fx.context, down));
  ST_CHECK_EQ(fx.editor.text(), "bbb\naaa\nccc");
  Event up = down;
  up.key = "ArrowUp";
  ST_CHECK(fx.editor.on_event(fx.context, up));
  ST_CHECK_EQ(fx.editor.text(), "aaa\nbbb\nccc");
}

/// 选中多行时整块搬家（列不变、块内顺序不变）。
ST_TEST(code_editor_move_lines_moves_a_selected_block) {
  Fixture fx;
  fx.editor.set_text("a1\nb1\nb2\nc1");
  fx.editor.set_selection(3, 8);  // 选中 b1/b2 两行
  ST_CHECK(fx.editor.move_lines(1));
  ST_CHECK_EQ(fx.editor.text(), "a1\nc1\nb1\nb2");
  ST_CHECK(fx.editor.move_lines(-1));
  ST_CHECK_EQ(fx.editor.text(), "a1\nb1\nb2\nc1");
}

// ————————————————————————————————————————————————————————————————————————————
// 光标位置不变式（防止量宽/绘制口径再度分家）
// ————————————————————————————————————————————————————————————————————————————
//
// 这三条用例的**共同口径**：文字是按 Monospace 画的，所以一切 x 坐标
// （光标、选择、查找高亮、缩进线）必须也用 Monospace 量。逐一列断言既抓
// “漏传 role”，也抓“前缀量宽与逐字累加混用”两类分家。

ST_TEST(code_editor_caret_advance_uses_the_drawn_font_role) {
  // 回归用例：`x_for_index` 曾经漏传 Monospace，掉到接口默认的 Proportional——
  // 量宽用比例字体、绘字用等宽字体，x 误差随列号线性累积（实测 45 列 7.7px）。
  Fixture fx;
  if (!fx.has_font()) return;
  const std::string row = "AVATAR = Wave.Offset * Total;";
  fx.editor.set_text(row);
  fx.editor.set_cursor_index(0);
  const float origin = fx.editor.caret_offset_x(fx.context);
  const auto advance = [&](std::size_t column) {
    return fx.port->measure_width(row.substr(column, 1), fx.editor.font_size(),
                                  st::text::FontRole::Monospace);
  };
  // 逐列推进：每前进一列，光标 x 的增量必须等于该字符的**等宽**步进
  for (std::size_t column = 0; column < row.size(); ++column) {
    const float before = fx.editor.caret_offset_x(fx.context);
    fx.editor.set_cursor_index(column + 1);
    const float after = fx.editor.caret_offset_x(fx.context);
    ST_CHECK(std::abs((after - before) - advance(column)) < 0.5f);
  }
  // 行尾总宽：比例字体与等宽在这句上差 ~5px，口径一致时误差在亚像素级
  fx.editor.set_cursor_index(row.size());
  ST_CHECK(std::abs((fx.editor.caret_offset_x(fx.context) - origin) -
                    fx.port->measure_width(row, fx.editor.font_size(),
                                           st::text::FontRole::Monospace)) < 0.5f);
}

ST_TEST(code_editor_caret_is_monotonic_and_consistent_with_hit_test) {
  // 回环不变式：把光标放在 x，再用同一个 x 反查落点，必须回到同一列。
  // 抓的是“画的地方”与“点的地方”不同源（两类分家的合集症状）。
  Fixture fx;
  if (!fx.has_font()) return;
  const std::string row = "foo(bar, baz) + qux;";
  fx.editor.set_text(row);
  fx.editor.set_cursor_index(0);
  float previous = fx.editor.caret_offset_x(fx.context);
  for (std::size_t column = 1; column <= row.size(); ++column) {
    fx.editor.set_cursor_index(column);
    const float caret = fx.editor.caret_offset_x(fx.context);
    ST_CHECK(caret >= previous - 0.01f);  // 单调不减
    previous = caret;
    Event event;
    event.kind = EventKind::MouseDown;
    event.button = 1;
    event.position = st::math::Point{fx.editor.bounds().x + caret,
                                     fx.editor.bounds().y + 4.0f};
    (void)fx.editor.on_event(fx.context, event);
    // 允许 ±1 列的边界歧义（格点半个字宽归属），但不得漂到别的字符上
    const std::size_t landed = fx.editor.cursor_index();
    ST_CHECK(landed + 1 >= column && landed <= column + 1);
  }
}

ST_TEST(code_editor_caret_is_independent_of_device_scale) {
  // DPI 是用户报的线索。量宽口径与 `device_scale` 无关（字形按物理像素栅格化，
  // 布局保持逻辑坐标）；本用例把这条不变式钉死：换 scale 后 caret x 逐字段不变。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.type("scale invariant");
  const float base = fx.editor.caret_offset_x(fx.context);
  Theme scaled{Theme::light()};
  RenderContext context{scaled, fx.port.get(), 0.0};
  fx.editor.arrange(context, st::math::Rect{0.0f, 0.0f, 800.0f, 400.0f});
  const float again = fx.editor.caret_offset_x(context);
  ST_CHECK(std::abs(base - again) < 0.01f);
}

// ————————————————————————————————————————————————————————————————————————————
// 编辑体验：自动配对 / 智能 Home / 按词删除 / 滚动
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_auto_pairs_close_brackets_and_quotes) {
  Fixture fx;
  fx.type_text("(");
  ST_CHECK_EQ(fx.editor.text(), std::string("()"));
  ST_CHECK_EQ(fx.editor.cursor_index(), 1U);  // 光标留在中间，接着敲内容
  fx.type_text(")");                         // 已是闭符：**跳过**而不是再插一个
  ST_CHECK_EQ(fx.editor.text(), std::string("()"));
  ST_CHECK_EQ(fx.editor.cursor_index(), 2U);

  fx.editor.set_text("");
  fx.type_text("{");
  ST_CHECK_EQ(fx.editor.text(), std::string("{}"));
  fx.editor.set_text("");
  fx.type_text("\"");
  ST_CHECK_EQ(fx.editor.text(), std::string("\"\""));
  // 词中的引号不配对（`isn't` 这类文本不被越修越乱）
  fx.editor.set_text("isn");
  fx.editor.set_cursor_index(3);
  fx.type_text("'");
  ST_CHECK_EQ(fx.editor.text(), std::string("isn'"));
}

ST_TEST(code_editor_auto_pairs_wrap_selection) {
  Fixture fx;
  fx.type("value");
  fx.editor.select_all();
  fx.type_text("(");
  ST_CHECK_EQ(fx.editor.text(), std::string("(value)"));
  // 包裹后选中内部文本仍保留（方便继续改写）
  ST_CHECK(fx.editor.has_selection());
  ST_CHECK_EQ(fx.editor.selected_text(), std::string("value"));
}

ST_TEST(code_editor_auto_pairs_delete_empty_pair_atomically) {
  Fixture fx;
  fx.type_text("(");
  fx.press("Backspace");
  // 一次退格把空对一起吃掉（否则要按两下，且第二下看着像“什么都没删”）
  ST_CHECK_EQ(fx.editor.text(), std::string());
  ST_CHECK_EQ(fx.editor.cursor_index(), 0U);
}

ST_TEST(code_editor_auto_pairs_can_be_disabled) {
  Fixture fx;
  fx.editor.set_auto_pairs(false);
  fx.type_text("(");
  ST_CHECK_EQ(fx.editor.text(), std::string("("));
  ST_CHECK(!fx.editor.auto_pairs());
  fx.editor.set_auto_pairs(true);
  ST_CHECK(fx.editor.auto_pairs());
}

ST_TEST(code_editor_smart_home_toggles_between_indent_and_column_zero) {
  // 两段式 Home：第一次落在行首非空列，第二次落到列 0。
  Fixture fx;
  fx.editor.set_text("    indented code");
  fx.editor.set_cursor_index(17);  // 行尾
  fx.press("Home");
  ST_CHECK_EQ(fx.editor.cursor_index(), 4U);   // 缩进后（代码开始处）
  fx.press("Home");
  ST_CHECK_EQ(fx.editor.cursor_index(), 0U);   // 再按一次：真行首
  fx.press("Home");
  ST_CHECK_EQ(fx.editor.cursor_index(), 4U);   // 再按：回到代码开始处
}

ST_TEST(code_editor_ctrl_backspace_deletes_by_word) {
  Fixture fx;
  fx.type("alpha beta gamma");
  fx.press("Backspace", /*ctrl=*/true);
  ST_CHECK_EQ(fx.editor.text(), std::string("alpha beta "));
  fx.press("Backspace", true);
  ST_CHECK_EQ(fx.editor.text(), std::string("alpha "));
  // Ctrl+Delete 在词首：吃掉整词，**尾随空白保留**（VSCode 同款——
  // 空白属于下一段的“空缺”，不由上一个词的删除吃掉）
  fx.editor.set_cursor_index(0);
  fx.press("Delete", true);
  ST_CHECK_EQ(fx.editor.text(), std::string(" "));
}

ST_TEST(code_editor_ctrl_delete_clears_whitespace_only_tail) {
  // 光标之后只剩下空白：Ctrl+Delete 应当把剩下的空白清掉，而不是“什么也不删”
  // （不能无反应——用户会以为按键坏了）。
  Fixture fx;
  fx.type("alpha   ");
  fx.editor.set_cursor_index(5);
  fx.press("Delete", true);
  ST_CHECK_EQ(fx.editor.text(), std::string("alpha"));
  fx.editor.set_cursor_index(5);
  fx.press("Delete", true);  // 已在文末：无动作（不越界）
  ST_CHECK_EQ(fx.editor.text(), std::string("alpha"));
}

ST_TEST(code_editor_ctrl_delete_removes_to_next_word_end) {
  Fixture fx;
  fx.type("one two three");
  fx.editor.set_cursor_index(4);  // 位于 "two" 的词首
  fx.press("Delete", true);
  ST_CHECK_EQ(fx.editor.text(), std::string("one  three"));
}

ST_TEST(code_editor_scroll_to_line_clamps_and_reports_viewport) {
  Fixture fx;
  if (!fx.has_font()) return;
  for (int index = 0; index < 200; ++index) {
    fx.editor.insert_text("line " + std::to_string(index) + "\n");
  }
  // 超出内容末尾的行号：不得把视口推到空白屏（旧实现直接相乘，滚过去什么都不显示）
  fx.editor.scroll_to_line(100000);
  ST_CHECK(fx.editor.last_visible_line(fx.context) + 1 >= fx.editor.line_count());
  const std::size_t first = fx.editor.first_visible_line(fx.context);
  const std::size_t last = first + fx.editor.visible_line_count(fx.context) - 1;
  ST_CHECK(last + 1 >= fx.editor.line_count());  // 末尾仍在视口内

  fx.editor.scroll_to_line(10);
  ST_CHECK_EQ(fx.editor.first_visible_line(fx.context), static_cast<std::size_t>(10));
  ST_CHECK(fx.editor.visible_line_count(fx.context) >= static_cast<std::size_t>(1));

  // 负方向也不越界
  fx.editor.scroll_by(0.0f, -100000.0f);
  ST_CHECK_EQ(fx.editor.scroll_offset().y, 0.0f);
  ST_CHECK_EQ(fx.editor.first_visible_line(fx.context), static_cast<std::size_t>(1));
}

ST_TEST(code_editor_scroll_and_indent_guides_are_drivable) {
  Fixture fx;
  // 属性面（智能体据此断言视口/行为）
  ST_CHECK(fx.editor.set_property("indent_guides", "false"));
  ST_CHECK(!fx.editor.indent_guides());
  ST_CHECK(fx.editor.set_property("indent_guides", "true"));
  ST_CHECK(fx.editor.indent_guides());
  ST_CHECK(fx.editor.set_property("scroll", "12,34"));
  const auto point = fx.editor.scroll_offset();
  ST_CHECK(std::abs(point.x - 12.0f) < 0.01f);
  ST_CHECK(std::abs(point.y - 34.0f) < 0.01f);
  ST_CHECK(fx.editor.invoke_action("scroll_to_line", "3"));
  ST_CHECK(!fx.editor.invoke_action("scroll_to_line", "not-a-number"));
  fx.editor.set_text("one\ntwo\nthree\nfour");
  ST_CHECK(fx.editor.invoke_action("reveal_line", "3"));
  const auto [begin, end] = fx.editor.selection();
  ST_CHECK_EQ(begin, end);  // reveal 只移光标不选中
  ST_CHECK_EQ(fx.editor.cursor_line(), static_cast<std::size_t>(2));
}

ST_TEST(code_editor_reveal_line_uses_the_clamped_scroll_path) {
  // `reveal_line` = goto_line + scroll_to_line；两条路径都走同一夹取。
  // 旧实现里 `scroll_to_line` 不夹取，`reveal_line` 会把视口推到内容之外的空白。
  Fixture fx;
  if (!fx.has_font()) return;
  for (int index = 0; index < 60; ++index) fx.editor.insert_text("row " + std::to_string(index) + "\n");
  ST_CHECK(fx.editor.invoke_action("reveal_line", "99999"));
  ST_CHECK(fx.editor.first_visible_line(fx.context) <= fx.editor.line_count());
  ST_CHECK(fx.editor.last_visible_line(fx.context) + 1 >= fx.editor.line_count());
  ST_CHECK_EQ(fx.editor.cursor_line() + 1, fx.editor.line_count());
}


// ————————————————————————————————————————————————————————————————————————————
// 绘制细节（像素级）：缩进参考线 / 当前行高亮 / 滚动条
// ————————————————————————————————————————————————————————————————————————————
//
// 口径与 `ui_toggle_test.cpp` 同族：**渲染两帧做差**，而不是断言绝对颜色——
// 绝对颜色断言会把主题 token 的改变误判成渲染缺陷。

ST_TEST(code_editor_indent_guides_paint_pixels_where_expected) {
  // 回归用例：缩进参考线是"看不见就等于没有"的那类功能（无子元素、无语义，
  // 只能靠像素断言）。曾经把线的颜色取成 `colors.border`——在浅色主题下几乎与
  // 编辑区底色同值，等于白画。本用例把"参考线确实出现在缩进列上"钉死。
  Fixture fx;
  if (!fx.has_font()) return;
  const std::string body = "    indented\n        deeper\n";
  fx.editor.set_text(body);
  fx.editor.arrange(fx.context, st::math::Rect{0.0f, 0.0f, 400.0f, 120.0f});

  const auto render = [&](bool guides) {
    fx.editor.set_indent_guides(guides);
    st::raster::Canvas canvas(400, 120);
    canvas.clear(fx.theme.colors().surface);
    fx.editor.paint_content(fx.context, canvas);
    return canvas;
  };
  const st::raster::Canvas with_guides = render(true);
  const st::raster::Canvas without = render(false);
  fx.editor.set_indent_guides(true);

  // 逐列统计"有参考线 vs 无参考线"的差异像素：至少要从 1 级、2 级缩进列上各取到一条竖线。
  const st::math::Rect box = fx.editor.bounds();
  int diff_pixels = 0;
  std::vector<int> diff_columns;
  for (int x = static_cast<int>(box.x); x < static_cast<int>(box.right()); ++x) {
    int column_diff = 0;
    for (int y = static_cast<int>(box.y); y < static_cast<int>(box.bottom()); ++y) {
      const st::math::Point p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
      if (with_guides.pixel_at_point(p) != without.pixel_at_point(p)) ++column_diff;
    }
    if (column_diff > 0) {
      diff_pixels += column_diff;
      diff_columns.push_back(x);
    }
  }
  ST_CHECK(diff_pixels > 0);           // 画了（不是"设了开关却什么也没变"）
  {
    // 参考线必须**看得见**：与相邻背景的亮度差要有意义（不是 1/255 的不可辨差）
    // 取 max 而不是 min：只有**有缩进的那几行**会画线，没有缩进的行本来就该是 0。
    int best_delta = 0;
    for (const int column : diff_columns) {
      for (int y = static_cast<int>(box.y) + 4; y < static_cast<int>(box.bottom()) - 4; ++y) {
        const st::math::Point on{static_cast<float>(column) + 0.5f, static_cast<float>(y) + 0.5f};
        const st::math::Point off{static_cast<float>(column) + 3.5f, static_cast<float>(y) + 0.5f};
        const st::math::Color a = with_guides.pixel_at_point(on);
        const st::math::Color b = with_guides.pixel_at_point(off);
        const int delta = std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)) +
                          std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)) +
                          std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
        best_delta = std::max(best_delta, delta);
      }
    }
    // 可见性阈值（sum|Δ| 三通道）：
    //   · `border`（旧值）实测 ~57 —— 那就是“画了但看不见”；
    //   · `border_strong`（现值）实测 ~150。
    // 取 120 卡在中间：再淡回去就红。
    ST_CHECK(best_delta >= 120);
  }
  ST_CHECK(diff_columns.size() >= 2);  // 两级缩进 → 至少两条竖线所在列
  // 参考线必须落在文本区的缩进列上，而不是贴着行号槽（坐标也要对）
  const float gutter = fx.editor.bounds().x + 30.0f;
  for (const int column : diff_columns) {
    ST_CHECK(static_cast<float>(column) >= gutter);
  }
  // 关掉开关后，与"从头就没开"逐像素一致（开关语义干净）
  const st::raster::Canvas again = render(false);
  std::size_t same = 0;
  std::size_t total = 0;
  for (int y = 0; y < 120; ++y) {
    for (int x = 0; x < 400; ++x) {
      ++total;
      if (again.pixel_at(x, y) == without.pixel_at(x, y)) ++same;
    }
  }
  ST_CHECK_EQ(same, total);
}

ST_TEST(code_editor_current_line_is_highlighted_in_gutter_and_row) {
  // 当前行高亮：行底色 + 行号槽都要变（行号槽高亮是"我在哪一行"的锚点）。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_text("one\ntwo\nthree\nfour\nfive\n");
  fx.editor.set_cursor_index(0);       // 第 1 行
  const auto render = [&]() {
    st::raster::Canvas canvas(400, 120);
    canvas.clear(fx.theme.colors().surface);
    fx.editor.paint_content(fx.context, canvas);
    return canvas;
  };
  const st::raster::Canvas first_row = render();
  fx.editor.set_cursor_index(9);       // 第 3 行
  const st::raster::Canvas third_row = render();

  // 两帧应当在"光标所在行"的区域上有差异（行号槽也在内）
  int diff = 0;
  for (int y = 0; y < 120; ++y) {
    for (int x = 0; x < 100; ++x) {  // 只看行号槽一带：这里最干净（没有文字抗锯齿）
      if (first_row.pixel_at(x, y) != third_row.pixel_at(x, y)) ++diff;
    }
  }
  ST_CHECK(diff > 0);
}

ST_TEST(code_editor_scrollbars_appear_only_when_content_overflows) {
  // 画布必须覆盖元素 bounds——否则滚动条（贴在 bounds 右/下缘）落在画布外，
  // 断言会以“没画”的假象失败（写这条测试时先踩了一次：画布 400x120 而元素 800x400）。
  Fixture fx;
  if (!fx.has_font()) return;
  const st::math::Rect box = fx.editor.bounds();
  const int width = static_cast<int>(box.right());
  const int height = static_cast<int>(box.bottom());
  const auto render = [&]() {
    st::raster::Canvas canvas(width, height);
    canvas.clear(fx.theme.colors().surface);
    fx.editor.paint_content(fx.context, canvas);
    return canvas;
  };

  // 判据用「滑动条带里最长的一段连续非底色」而不是「有多少非底色像素」：
  // 当前行底色会横贯到最右侧（那是另一条正确的行为），纯计数会把两者混在一起。
  // 滑块有最小长度（`kMinThumbLength` 语义 ≥ 24px），当前行底色只有一行高（≈16px）。
  const int bar_x = width - 6;
  const auto longest_run = [&](const st::raster::Canvas& canvas) {
    int best = 0;
    int run = 0;
    for (int y = 0; y < height; ++y) {
      if (canvas.pixel_at(bar_x, y) == fx.theme.colors().surface) {
        run = 0;
      } else {
        ++run;
        best = std::max(best, run);
      }
    }
    return best;
  };

  fx.editor.set_text("short\n");
  const st::raster::Canvas short_doc = render();
  const int short_run = longest_run(short_doc);
  ST_CHECK(short_run < 24);  // 没有滑块（顶多是一行当前行底色）

  // 长内容：出现滑块；滚到中段后滑块位置必须跟着动
  std::string long_doc;
  for (int index = 0; index < 400; ++index) long_doc += "line\n";
  fx.editor.set_text(long_doc);
  const st::raster::Canvas long_top = render();
  const int top_bar_pixels = longest_run(long_top);
  ST_CHECK(top_bar_pixels >= 24);

  fx.editor.set_scroll_offset(0.0f, 3000.0f);
  const st::raster::Canvas long_mid = render();
  int moved = 0;
  for (int y = 0; y < height; ++y) {
    if (long_top.pixel_at(bar_x, y) != long_mid.pixel_at(bar_x, y)) ++moved;
  }
  ST_CHECK(moved > 0);
}

ST_TEST(code_editor_horizontal_scrollbar_tracks_long_lines) {
  Fixture fx;
  if (!fx.has_font()) return;
  const st::math::Rect box = fx.editor.bounds();
  const int width = static_cast<int>(box.right());
  const int height = static_cast<int>(box.bottom());
  const auto render = [&]() {
    st::raster::Canvas canvas(width, height);
    canvas.clear(fx.theme.colors().surface);
    fx.editor.paint_content(fx.context, canvas);
    return canvas;
  };
  fx.editor.set_text(std::string(400, 'x'));   // 超长单行 → 必须出现水平条
  const st::raster::Canvas at_left = render();
  fx.editor.set_scroll_offset(600.0f, 0.0f);
  const st::raster::Canvas at_right = render();

  // 水平条在文本区底部：滑块位置变了
  int bar_diff = 0;
  for (int y = height - 12; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (at_left.pixel_at(x, y) != at_right.pixel_at(x, y)) ++bar_diff;
    }
  }
  ST_CHECK(bar_diff > 0);
  // 水平偏移也真的作用到了正文（文字区域跟着横移）
  int text_diff = 0;
  for (int y = 0; y < 40; ++y) {
    for (int x = 0; x < width; ++x) {
      if (at_left.pixel_at(x, y) != at_right.pixel_at(x, y)) ++text_diff;
    }
  }
  ST_CHECK(text_diff > 0);
}
