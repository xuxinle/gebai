// 隔离验证：备用屏进出（不涉及 PTY/组件，纯屏幕模型）。
//
// 目的：把「解析器错」与「接线/时序错」分开。真窗口探针发现终端会**卡在备用屏**
// （`alt_screen` 一直为 true、退不出来），但那条路上 PTY/shell/时序全混在一起；
// 这里只喂字节给屏幕模型，答案就明确了。
//
// 注：`ST_CHECK` **不支持 `<<` 流式消息**（宏体只取表达式）——需要带数值的
// 失败信息时用 `record_failure`（见 `tests/ui_terminal_palette_test.cpp` 同一手法）。

#include "st/test/test.hpp"

#include <cstddef>
#include <cstring>
#include <string>

#include "st/text/ansi_screen.hpp"

using st::text::AnsiScreen;

namespace {

/// 带详情的断言（`ST_CHECK` 的宏消息是源码文本，说不出"哪个序列失败"）。
void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

}  // namespace

ST_TEST(alt_screen_basic_roundtrip) {
  AnsiScreen screen;
  screen.resize(40, 6);
  screen.feed("MAIN-CONTENT");

  ST_CHECK(!screen.in_alt_screen());
  screen.feed("\x1b[?1049h");
  ST_CHECK(screen.in_alt_screen());
  // 备用屏是干净的
  ST_CHECK(screen.plain_text().find("MAIN-CONTENT") == std::string::npos);

  screen.feed("\x1b[?1049l");
  ST_CHECK(!screen.in_alt_screen());
  // 主屏恢复
  ST_CHECK(screen.plain_text().find("MAIN-CONTENT") != std::string::npos);
}

ST_TEST(alt_screen_exit_is_idempotent_and_survives_repeats) {
  AnsiScreen screen;
  screen.resize(40, 6);
  screen.feed("HOST-LINE");
  for (int i = 0; i < 3; ++i) {
    screen.feed("\x1b[?1049h");
    ST_CHECK(screen.in_alt_screen());
    screen.feed("\x1b[?1049l");
    ST_CHECK(!screen.in_alt_screen());
  }
  ST_CHECK(screen.plain_text().find("HOST-LINE") != std::string::npos);
}

ST_TEST(alt_screen_exit_without_enter_is_harmless) {
  AnsiScreen screen;
  screen.resize(40, 6);
  screen.feed("KEEP");
  screen.feed("\x1b[?1049l");   // 没进过就退出
  ST_CHECK(!screen.in_alt_screen());
  ST_CHECK(screen.plain_text().find("KEEP") != std::string::npos);
}

ST_TEST(alt_screen_1047_and_47_variants) {
  // 三种编号（1049 保存光标、1047、47）都要能进能出。
  for (const char* enter : {"\x1b[?1049h", "\x1b[?1047h", "\x1b[?47h"}) {
    AnsiScreen screen;
    screen.resize(40, 6);
    screen.feed("BASE");
    screen.feed(enter);
    expect(screen.in_alt_screen(), std::string("进入失败: ") + enter);
    std::string exit_seq(enter);
    exit_seq.back() = 'l';
    screen.feed(exit_seq);
    expect(!screen.in_alt_screen(), std::string("退出失败: ") + exit_seq);
    expect(screen.plain_text().find("BASE") != std::string::npos,
           std::string("主屏未恢复: ") + enter);
  }
}

ST_TEST(alt_screen_after_resize_still_exits) {
  // 关键怀疑点：`resize` 会 remap 两个缓冲，若 sw­ap 状态没同步，
  // 之后可能"退不出备用屏"（真窗口实测现象）。真窗口里拖动面板就会触发 resize。
  AnsiScreen screen;
  screen.resize(40, 6);
  screen.feed("MAIN-TEXT");
  screen.feed("\x1b[?1049h");
  ST_CHECK(screen.in_alt_screen());
  screen.feed("ALT-TEXT");
  screen.resize(60, 10);   // 备用屏内改变尺寸（真窗口常见）
  ST_CHECK(screen.in_alt_screen());
  screen.feed("\x1b[?1049l");
  expect(!screen.in_alt_screen(), "resize 之后再退出备用屏失败");
  expect(screen.plain_text().find("MAIN-TEXT") != std::string::npos, "resize 后主屏未恢复");
}

ST_TEST(alt_screen_in_pty_mode_resize_keeps_exiting) {
  // 备用屏内 resize 到**更小**再退出：截断路径也要能恢复主屏。
  AnsiScreen screen;
  screen.resize(40, 8);
  screen.feed("MAIN-KEEP");
  screen.feed("\x1b[?1049h");
  screen.resize(20, 4);
  screen.feed("\x1b[?1049l");
  expect(!screen.in_alt_screen(), "缩小 resize 后退出备用屏失败");
  expect(screen.plain_text().find("MAIN-KEEP") != std::string::npos, "缩小 resize 后主屏未恢复");
}

ST_TEST(alt_screen_swallows_scrollback) {
  // 备用屏内滚出的行**不进**回看（vim 的重绘不是历史）。
  AnsiScreen screen;
  screen.resize(20, 3);
  for (int i = 0; i < 10; ++i) screen.feed("main-line\n");
  const std::size_t before = screen.scrollback_count();
  ST_CHECK(before > 0);
  screen.feed("\x1b[?1049h");
  for (int i = 0; i < 10; ++i) screen.feed("alt-line\n");
  expect(screen.scrollback_count() == before, "备用屏内容污染了回看缓冲（进入后）");
  screen.feed("\x1b[?1049l");
  expect(screen.scrollback_count() == before, "备用屏内容污染了回看缓冲（退出后）");
}
