/// 代码编辑器：翻页 / 滚动条命中 / Tab 列宽 / 只读一致性 / 属性面 —— 回归测试。
///
/// 这一组用例都来自控制通道实测确认的缺陷（每组注释写明「旧行为是什么」），
/// 目的是把它们钉死在**单元层面**，而不是只靠端到端脚本兜住。

#include "st/app/text_port.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/theme.hpp"

#include <cmath>
#include <memory>
#include <string>

namespace {

using st::ui::CodeEditor;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Theme;

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

  void press(std::string key, bool ctrl = false, bool shift = false) {
    Event event;
    event.kind = EventKind::KeyDown;
    event.key = std::move(key);
    event.ctrl = ctrl;
    event.shift = shift;
    (void)editor.on_event(context, event);
  }

  void mouse(EventKind kind, float x, float y, int button = 1) {
    Event event;
    event.kind = kind;
    event.position = st::math::Point{x, y};
    event.button = button;
    (void)editor.on_event(context, event);
  }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 翻页
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_page_down_moves_cursor_and_viewport) {
  // 旧行为：PageDown 只改 `scroll_y_`，随后光标仍在第 1 行 → `ensure_cursor_visible`
  // 又把滚动拉回 0（实测：手工设 `scroll=0,400`，一按 PageDown 立刻回到 0）。
  Fixture fx;
  if (!fx.has_font()) return;
  for (int index = 0; index < 300; ++index) {
    fx.editor.insert_text("line " + std::to_string(index) + "\n");
  }
  fx.editor.set_cursor_index(0);
  const std::size_t rows = fx.editor.visible_line_count(fx.context);
  ST_CHECK(rows > 2);

  fx.press("PageDown");
  ST_CHECK(fx.editor.cursor_line() >= rows - 1);          // 光标真的往下走了一屏
  ST_CHECK(fx.editor.first_visible_line(fx.context) > 1);  // 视口也跟着走
  const std::size_t after_one = fx.editor.cursor_line();

  fx.press("PageDown");
  ST_CHECK(fx.editor.cursor_line() > after_one);

  fx.press("PageUp");
  ST_CHECK_EQ(fx.editor.cursor_line(), after_one);   // 一来一回对得上
}

ST_TEST(code_editor_page_up_at_top_is_a_no_op) {
  Fixture fx;
  if (!fx.has_font()) return;
  for (int index = 0; index < 100; ++index) fx.editor.insert_text("row\n");
  fx.editor.set_cursor_index(0);
  fx.press("PageUp");
  ST_CHECK_EQ(fx.editor.cursor_line(), 0U);
  ST_CHECK(std::abs(fx.editor.scroll_offset().y) < 0.01f);
}

// ————————————————————————————————————————————————————————————————————————————
// 滚动条：拖它不应改光标
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_vertical_scrollbar_drag_does_not_move_cursor) {
  // 旧行为：垂直条命中落在“移光标”分支上——拖一次滚动条，光标从第 1 行跳到第 30 行。
  Fixture fx;
  if (!fx.has_font()) return;
  for (int index = 0; index < 400; ++index) {
    fx.editor.insert_text("row " + std::to_string(index) + "\n");
  }
  fx.editor.set_cursor_index(0);
  const st::math::Rect box = fx.editor.bounds();
  const st::math::Rect track = fx.editor.v_scroll_bar_rect(fx.context);
  ST_CHECK(track.width > 0.0f);   // 有溢出 → 轨道存在
  const float x = track.center().x;
  const float y0 = track.y + 6.0f;

  fx.mouse(EventKind::MouseDown, x, y0);
  fx.mouse(EventKind::MouseMove, x, box.height - 30.0f);
  fx.mouse(EventKind::MouseUp, x, box.height - 30.0f);

  ST_CHECK_EQ(fx.editor.cursor_index(), 0U);            // 光标原地不动
  ST_CHECK(fx.editor.scroll_offset().y > 100.0f);       // 但视口滚下去了
}

ST_TEST(code_editor_scrollbar_hit_uses_the_same_geometry_as_paint) {
  // 命中区与绘制必须同源：轨道矩形给出后，点它的中点必定进拖拽（不落到文本区）。
  Fixture fx;
  if (!fx.has_font()) return;
  for (int index = 0; index < 200; ++index) fx.editor.insert_text("x\n");
  fx.editor.set_cursor_index(0);
  const st::math::Rect track = fx.editor.v_scroll_bar_rect(fx.context);
  ST_CHECK(track.width > 0.0f);
  fx.mouse(EventKind::MouseDown, track.center().x, track.y + 2.0f);
  ST_CHECK_EQ(fx.editor.cursor_index(), 0U);
}

ST_TEST(code_editor_short_document_has_no_scrollbar) {
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_text("one\ntwo\n");
  ST_CHECK(!(fx.editor.v_scroll_bar_rect(fx.context).width > 0.0f));
}

// ————————————————————————————————————————————————————————————————————————————
// 制表符列宽：命中与绘制同源
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_tab_expands_to_tab_stops) {
  // `\t` 不得被当成一个字形：量宽/绘制必须都走「展开到制表位」的同一条路径。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_text("\tabc\n");
  fx.editor.set_tab_width(4);
  // 光标在第 0 个字符（`\t` 之前）与在 `a` 之前（`\t` 之后）的 x 差 = 4 个空格宽
  fx.editor.set_cursor_index(0);
  const float before = fx.editor.caret_offset_x(fx.context);
  fx.editor.set_cursor_index(1);
  const float after = fx.editor.caret_offset_x(fx.context);
  const float space = fx.port->measure_width(" ", fx.editor.font_size(), st::text::FontRole::Monospace);
  ST_CHECK(space > 0.0f);
  ST_CHECK(std::abs((after - before) - space * 4.0f) < 0.5f);
}

ST_TEST(code_editor_click_after_tab_lands_after_it) {
  // 旧行为：点在第 2/3/4 列仍返回同一列（`\t` 按一个字符宽算），鼠标定点与字位错开。
  Fixture fx;
  if (!fx.has_font()) return;
  fx.editor.set_text("\tabc\n");
  fx.editor.set_tab_width(4);
  const float space = fx.port->measure_width(" ", fx.editor.font_size(), st::text::FontRole::Monospace);
  const st::math::Rect box = fx.editor.bounds();
  // 文本原点 → 越过 4 个空格宽后，落点应在 `a`（索引 1）之后
  const float origin_x = box.x + box.width * 0.0f;  // 先算相对量
  (void)origin_x;
  fx.editor.set_cursor_index(0);
  const float x_at_0 = box.x + fx.editor.caret_offset_x(fx.context);
  fx.mouse(EventKind::MouseDown, x_at_0 + space * 4.5f, box.y + 8.0f);
  ST_CHECK(fx.editor.cursor_index() >= 1U);
  ST_CHECK(fx.editor.cursor_index() <= 2U);
}

// ————————————————————————————————————————————————————————————————————————————
// 只读：所有编辑入口一律拒绝
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_read_only_blocks_every_delete_path) {
  // 旧行为：只读下 `select_all` + Delete 把全文清空（`delete_selection` 没查只读）。
  Fixture fx;
  fx.editor.set_text("hello world\nsecond line\n");
  fx.editor.set_read_only(true);
  fx.editor.select_all();
  fx.press("Delete");
  ST_CHECK_EQ(fx.editor.text(), std::string("hello world\nsecond line\n"));
  fx.press("Backspace");
  ST_CHECK_EQ(fx.editor.text(), std::string("hello world\nsecond line\n"));
  (void)fx.editor.invoke_action("cut", {});
  ST_CHECK_EQ(fx.editor.text(), std::string("hello world\nsecond line\n"));
  fx.editor.replace_selection("X");
  ST_CHECK_EQ(fx.editor.text(), std::string("hello world\nsecond line\n"));
  ST_CHECK_EQ(fx.editor.invoke_action("replace_all", "line"), 0U);
  ST_CHECK_EQ(fx.editor.text(), std::string("hello world\nsecond line\n"));
}

ST_TEST(code_editor_read_only_flag_survives_property_roundtrip) {
  Fixture fx;
  ST_CHECK(fx.editor.set_property("read_only", "true"));
  ST_CHECK_EQ(*fx.editor.get_property("read_only"), std::string("true"));
  fx.press("Delete");
  fx.editor.insert_text("X");
  ST_CHECK_EQ(*fx.editor.get_property("read_only"), std::string("true"));
  fx.editor.select_all();
  fx.press("Delete");
  ST_CHECK_EQ(fx.editor.text(), std::string());
}
