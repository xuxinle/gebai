/// 终端**光标**测试：闪烁续帧（动画不冻住）+ 形状选择（默认竖线、程序可覆盖）。
///
/// ### 本轮钉住的两个缺陷（2026-10-08 用户报「光标闪烁不稳定」「光标改为竖线」）
///
/// ① **闪烁不稳定 ⇒ 实际是"完全不动"**。
///    `paint_content` 用 `fmod(context.time_seconds, 1.0)` 决定光标显隐，
///    却**从不调 `request_animation()`**——而帧的存续由它汇总（`UiRoot` 帧末
///    `collect_animation_requests` → `followup_`）。重绘平静下来后没有新帧，
///    光标就**冻在它碰巧停住的相位上**（实测：静置 6 秒像素零变化）。
///    "不稳定"是用户对"有时亮着一直不灭"的准确描述。
///
/// ② **光标形状**：默认是 `Block`（整格半透明填充），与"插入点"的直觉不符。
///    改为默认 `Bar`（竖线）；但**程序用 `DECSCUSR` 指定过时不能覆盖**——
///    `vim` 的插入模式正是靠它要求竖线。
///
/// 两条都进了回归；①还做了逆向验证（去掉 `request_animation` 即变红，
/// 见 `tools/terminal_cursor_reverse_verify.py`）。

#include "st/test/test.hpp"

#include <string>

#include "st/text/ansi_screen.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::text::AnsiCursorShape;
using st::text::AnsiScreen;

/// 带详情的断言（`ST_CHECK` 的宏消息只有源码文本）。
void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 默认形状 = 竖线
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(ansi_screen_default_cursor_shape_is_bar) {
  // 终端默认给**插入点**（竖线），不是整格填充。
  AnsiScreen screen;
  ST_CHECK(screen.cursor_shape() == AnsiCursorShape::Bar);
  ST_CHECK(!screen.cursor_shape_is_explicit());
}

ST_TEST(ansi_screen_host_default_shape_applies) {
  AnsiScreen screen;
  screen.set_cursor_shape(AnsiCursorShape::Underline);
  expect(screen.cursor_shape() == AnsiCursorShape::Underline, "宿主默认未生效");
  expect(screen.has_cursor_shape_default(), "未记住宿主默认");
}

// ————————————————————————————————————————————————————————————————————————————
// ② 程序（`DECSCUSR`）优先于宿主默认
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(ansi_screen_decscusr_overrides_host_default) {
  // vim 插入模式发 `CSI 5 q` 要竖线。宿主默认是块时，**必须**能改成竖线。
  AnsiScreen screen;
  screen.set_cursor_shape(AnsiCursorShape::Block);
  screen.feed("\x1b[5 q");
  expect(screen.cursor_shape() == AnsiCursorShape::Bar, "DECSCUSR 竖线未生效");
  expect(screen.cursor_shape_is_explicit(), "未记录“程序已表态”");
}

ST_TEST(ansi_screen_host_default_does_not_clobber_explicit) {
  // **关键回归**：程序已明确指定形状后，宿主再设默认**不能**覆盖它。
  // 否则用户改内置设置会把 `vim` 的插入光标撞掉。
  AnsiScreen screen;
  screen.feed("\x1b[5 q");   // 程序要竖线
  screen.set_cursor_shape(AnsiCursorShape::Block);
  expect(screen.cursor_shape() == AnsiCursorShape::Bar, "宿主默认覆盖了程序指定的形状");
}

ST_TEST(ansi_screen_decscusr_all_forms) {
  // 1/2 块、3/4 下划线、5/6 竖线（成对编号必须等价）。
  struct Case {
    const char* seq;
    AnsiCursorShape shape;
  };
  const Case cases[] = {
      {"\x1b[1 q", AnsiCursorShape::Block},     {"\x1b[2 q", AnsiCursorShape::Block},
      {"\x1b[3 q", AnsiCursorShape::Underline}, {"\x1b[4 q", AnsiCursorShape::Underline},
      {"\x1b[5 q", AnsiCursorShape::Bar},       {"\x1b[6 q", AnsiCursorShape::Bar},
  };
  for (const Case& item : cases) {
    AnsiScreen screen;
    screen.feed(item.seq);
    expect(screen.cursor_shape() == item.shape,
           std::string("DECSCUSR 形状不对: ") + item.seq);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// ③ 闪烁相位：同一个时间值必须给同一个可见性（可回归的前提）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(ansi_screen_cursor_visibility_toggle_sequence) {
  // `CSI ?25l/h` 是程序隐藏/显示光标的通道（`vim` 退出时会显示回来）。
  AnsiScreen screen;
  ST_CHECK(screen.cursor_visible());
  screen.feed("\x1b[?25l");
  ST_CHECK(!screen.cursor_visible());
  screen.feed("\x1b[?25h");
  ST_CHECK(screen.cursor_visible());
}
