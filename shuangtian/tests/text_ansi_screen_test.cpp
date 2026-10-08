/// ANSI 屏幕模型（`st::text::AnsiScreen`）测试。
///
/// 这一层是"真终端"的**渲染核心**：把它测透，终端组件的正确性才有地基。
/// 断言都指向**只有屏幕模型才有**的行为——文字流 / 行列表做不到的那些：
///
/// * 光标定位后覆写（`top`/进度条的重画）
/// * `\r` 回行首覆盖（进度条、`\r` 刷新行）
/// * 清屏/清行/插删行、滚动区域
/// * **备用屏幕缓冲**（`vim` 全屏的前提：切进去画、切回来主屏原样在）
/// * SGR 颜色/样式落在**格子上**（不是整行一个样式）
/// * 宽字符占两列、行尾不放宽字符时先换行
/// * **跨 `feed` 的残片**（半个转义序列、半个 UTF-8 码点）——PTY 分片必然发生

#include "st/test/test.hpp"

#include <string>

#include "st/text/ansi_screen.hpp"

namespace {

using st::text::AnsiColor;
using st::text::AnsiCursorShape;
using st::text::AnsiScreen;
using st::text::AnsiStyle;

/// 屏幕文本（去行尾空白，按行拼）。
[[nodiscard]] auto screen_text(const AnsiScreen& screen) -> std::string { return screen.plain_text(); }

/// 只取第 `row` 行（去行尾空白）。
[[nodiscard]] auto row(const AnsiScreen& screen, int index) -> std::string {
  return screen.row_text(index);
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// 文本与光标
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_writes_text_and_tracks_cursor) {
  AnsiScreen screen(20, 3);
  screen.feed("hello");
  ST_CHECK_EQ(row(screen, 0), std::string("hello"));
  ST_CHECK_EQ(screen.cursor_row(), 0);
  ST_CHECK_EQ(screen.cursor_col(), 5);
  // 终端里换行是 `\r\n`：`\n` 只下移一行、**不回行首**（这是真终端的语义，
  // 也是"%s 前面多空格"这类现象的来源）。
  screen.feed("\r\nworld");
  ST_CHECK_EQ(row(screen, 1), std::string("world"));
  ST_CHECK_EQ(screen.cursor_row(), 1);
}

ST_TEST(ansi_screen_cursor_positioning_overwrites_in_place) {
  // **屏幕模型的核心价值**：定位后覆写。拼字符串做不到这件事——
  // 而这正是 `top` / 进度条 / 状态栏重画的实现方式。
  AnsiScreen screen(20, 3);
  screen.feed("0123456789");
  // 光标移到第 1 行第 3 列（1 基）再写：覆盖前两个字符。
  screen.feed("\x1b[1;3HAB");
  ST_CHECK_EQ(row(screen, 0), std::string("01AB456789"));
  // 绝对定位到末行
  screen.feed("\x1b[3;5Hxy");
  ST_CHECK_EQ(row(screen, 2), std::string("    xy"));
}

ST_TEST(ansi_screen_carriage_return_overwrites_line) {
  // `\r` 回行首（进度条的标准做法：回到行首重写整行）。
  AnsiScreen screen(20, 2);
  screen.feed("progress: 10%");
  screen.feed("\rprogress: 90%");
  ST_CHECK_EQ(row(screen, 0), std::string("progress: 90%"));
  // 新内容**更短**时旧尾部会留下——这是真终端的语义（`\r` 只移光标、**不清行**），
  // 所以进度条要么等宽填充、要么配 `\x1b[K`。把这条语义钉住，免得以后有人
  // 把 `\r` 实现成"清行"（那会破坏用 `\r` 逐字符刷新的路径）。
  screen.feed("\rOK");
  ST_CHECK_EQ(row(screen, 0), std::string("OKogress: 90%"));
  // 配上清行就是干净的一行（进度条的实际写法）。
  screen.feed("\r\x1b[KOK");
  ST_CHECK_EQ(row(screen, 0), std::string("OK"));
}

ST_TEST(ansi_screen_backspace_and_tab) {
  AnsiScreen screen(30, 2);
  screen.feed("abc\b\bX");
  ST_CHECK_EQ(row(screen, 0), std::string("aXc"));
  AnsiScreen tabs(30, 2);
  tabs.feed("a\tb");
  // 制表位 8 列：`a` 在第 0 列，`\t` 到第 8 列。
  ST_CHECK_EQ(tabs.cursor_col(), 9);
  ST_CHECK(tabs.cell(0, 8).ch == U'b');
}

ST_TEST(ansi_screen_text_wraps_at_right_edge) {
  AnsiScreen screen(5, 3);
  screen.feed("abcdefg");
  ST_CHECK_EQ(row(screen, 0), std::string("abcde"));
  ST_CHECK_EQ(row(screen, 1), std::string("fg"));
  ST_CHECK_EQ(screen.cursor_row(), 1);
}

ST_TEST(ansi_screen_pending_wrap_keeps_cursor_on_last_cell) {
  // 写满一行后**不立刻**换行（等下一个字符）——否则每写满一行就多一个空行。
  // 渲染时要知道这个挂起态（光标停在最后一格，不是下一行首）。
  AnsiScreen screen(5, 3);
  screen.feed("abcde");
  ST_CHECK(screen.pending_wrap());
  ST_CHECK_EQ(screen.cursor_row(), 0);
  ST_CHECK_EQ(screen.cursor_col(), 4);
  screen.feed("f");
  ST_CHECK(!screen.pending_wrap());
  ST_CHECK_EQ(row(screen, 1), std::string("f"));
}

// ════════════════════════════════════════════════════════════════════════════
// 清屏 / 滚动 / 滚动区域
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_clear_screen_and_line) {
  AnsiScreen screen(10, 3);
  screen.feed("aaa\r\nbbb\r\nccc");
  screen.feed("\x1b[2J");          // 清整屏
  ST_CHECK_EQ(screen_text(screen), std::string("\n\n"));
  screen.feed("\x1b[1;1Hhello");
  screen.feed("\x1b[1;1H\x1b[K");  // 清到行尾
  ST_CHECK_EQ(row(screen, 0), std::string(""));
  // 清到行首（**含光标那一格**）：`abcd` 的光标在第 3 列（1 基）后清，
  // 第 1..3 列被清掉 ⇒ 只剩从第 4 列起的 `d`。这是 `CSI 1K` 的规范语义，
  // 也是"清行"与"清到行首"容易混的地方。
  screen.feed("\x1b[1;1Habcd");
  screen.feed("\x1b[1;3H\x1b[1K");
  ST_CHECK_EQ(row(screen, 0), std::string("   d"));
}

ST_TEST(ansi_screen_scrolling_pushes_lines_to_scrollback) {
  // 底部换行 = 滚屏；滚出的行进**回看缓冲**（用户 `↑` 能翻回去）。
  AnsiScreen screen(10, 2);
  screen.feed("one\r\ntwo\r\nthree");
  ST_CHECK_EQ(row(screen, 0), std::string("two"));
  ST_CHECK_EQ(row(screen, 1), std::string("three"));
  ST_CHECK_EQ(screen.scrollback_count(), std::size_t{1});
  ST_CHECK_EQ(screen.scrollback_line(0), std::string("one"));
}

ST_TEST(ansi_screen_scroll_region_restricts_scrolling) {
  // `DECSTBM` 设滚动区域：区域外的行**不动**（`vim`/`less` 用它在顶部留状态栏）。
  AnsiScreen screen(10, 4);
  screen.feed("HEADER");
  screen.feed("\x1b[2;4r");        // 滚动区 = 第 2..4 行
  screen.feed("\x1b[4;1Ha\nb");    // 在区域内滚
  // 第 1 行（区域外）原样保留
  ST_CHECK_EQ(row(screen, 0), std::string("HEADER"));
}

ST_TEST(ansi_screen_insert_and_delete_lines) {
  AnsiScreen screen(10, 4);
  screen.feed("one\r\ntwo\r\nthree\r\nfour");
  screen.feed("\x1b[2;1H");        // 光标到第 2 行
  screen.feed("\x1b[L");           // 插入一行
  ST_CHECK_EQ(row(screen, 0), std::string("one"));
  ST_CHECK_EQ(row(screen, 1), std::string(""));       // 插入的空行
  ST_CHECK_EQ(row(screen, 2), std::string("two"));
  screen.feed("\x1b[M");           // 删掉它
  ST_CHECK_EQ(row(screen, 1), std::string("two"));
}

// ════════════════════════════════════════════════════════════════════════════
// 备用屏幕缓冲（全屏程序）
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_alt_screen_switches_and_restores) {
  // **全屏程序的前提**：切到备用屏画，退出时主屏原样回来。
  // 没有这一条，`vim`/`htop` 退出后主屏会被它的绘制内容覆盖掉。
  AnsiScreen screen(20, 3);
  screen.feed("main content line 1\r\nmain line 2");
  const std::string main_before = screen_text(screen);

  screen.feed("\x1b[?1049h");      // 进备用屏
  ST_CHECK(screen.in_alt_screen());
  ST_CHECK_EQ(screen_text(screen), std::string("\n\n"));   // 备用屏是空白
  screen.feed("\x1b[1;1HFULL SCREEN APP");
  ST_CHECK_EQ(row(screen, 0), std::string("FULL SCREEN APP"));

  screen.feed("\x1b[?1049l");      // 回主屏
  ST_CHECK(!screen.in_alt_screen());
  ST_CHECK_EQ(screen_text(screen), main_before);   // 主屏**原样恢复**
}

ST_TEST(ansi_screen_alt_screen_does_not_pollute_scrollback) {
  // 全屏程序的重绘不是"历史"——它滚出的行不该进回看缓冲（否则用户 `↑`
  // 翻到的全是 vim 的重绘碎片）。
  AnsiScreen screen(10, 2);
  screen.feed("history-1\r\nhistory-2");
  const std::size_t before = screen.scrollback_count();
  screen.feed("\x1b[?1049h");
  for (int i = 0; i < 20; ++i) screen.feed("redraw\r\n");
  screen.feed("\x1b[?1049l");
  ST_CHECK_EQ(screen.scrollback_count(), before);
}

// ════════════════════════════════════════════════════════════════════════════
// 样式
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_sgr_basic_styles_land_on_cells) {
  AnsiScreen screen(20, 2);
  screen.feed("\x1b[1;31mRED\x1b[0mplain");
  ST_CHECK(screen.cell(0, 0).style.bold);
  ST_CHECK(screen.cell(0, 0).style.fg.kind == AnsiColor::Kind::Indexed);
  ST_CHECK_EQ(static_cast<int>(screen.cell(0, 0).style.fg.index), 1);
  // `\x1b[0m` 之后回到默认——**样式是按格子的**，不是整行一个。
  ST_CHECK(!screen.cell(0, 3).style.bold);
  ST_CHECK(screen.cell(0, 3).style.fg.kind == AnsiColor::Kind::Default);
}

ST_TEST(ansi_screen_sgr_extended_colors) {
  AnsiScreen screen(20, 2);
  screen.feed("\x1b[38;5;208mX");
  ST_CHECK_EQ(static_cast<int>(screen.cell(0, 0).style.fg.index), 208);
  screen.feed("\x1b[38;2;10;20;30mY");
  const auto& color = screen.cell(0, 1).style.fg;
  ST_CHECK(color.kind == AnsiColor::Kind::Rgb);
  ST_CHECK_EQ(static_cast<int>(color.r), 10);
  ST_CHECK_EQ(static_cast<int>(color.b), 30);
  screen.feed("\x1b[48;2;1;2;3mZ");
  ST_CHECK(screen.cell(0, 2).style.bg.kind == AnsiColor::Kind::Rgb);
}

ST_TEST(ansi_screen_sgr_reverse_and_reset) {
  AnsiScreen screen(20, 2);
  screen.feed("\x1b[7mREV\x1b[27mNORM");
  ST_CHECK(screen.cell(0, 0).style.reverse);
  ST_CHECK(!screen.cell(0, 3).style.reverse);
}

// ════════════════════════════════════════════════════════════════════════════
// 宽字符
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_wide_characters_take_two_cells) {
  AnsiScreen screen(20, 2);
  screen.feed("中文abc");
  // 两个汉字占 4 列
  ST_CHECK_EQ(screen.cursor_col(), 7);
  ST_CHECK(screen.cell(0, 0).ch == U'中');
  // 右半格是 continuation 标记（不持有字符、渲染时跳过）
  ST_CHECK(screen.cell(0, 1).continuation);
  ST_CHECK(screen.cell(0, 2).ch == U'文');
  // 取文本时不重复输出右半格
  ST_CHECK_EQ(row(screen, 0), std::string("中文abc"));
}

ST_TEST(ansi_screen_wide_char_at_line_edge_wraps_first) {
  // 行尾放不下宽字符时**先换行**（否则两格被劈开，整行错位）。
  AnsiScreen screen(4, 3);
  screen.feed("abc中");
  ST_CHECK_EQ(row(screen, 0), std::string("abc"));
  ST_CHECK_EQ(row(screen, 1), std::string("中"));
}

// ════════════════════════════════════════════════════════════════════════════
// 分片（PTY 必然发生）
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_handles_split_escape_sequences) {
  // **PTY 分片边界与序列边界无关**：一次读 4096 字节经常把序列从中间切开。
  // 跨 `feed` 的残片必须被正确接续（否则屏幕上会出现 `[31m` 这种垃圾）。
  AnsiScreen screen(20, 2);
  screen.feed("\x1b[3");
  screen.feed("1mRED");
  ST_CHECK_EQ(row(screen, 0), std::string("RED"));
  ST_CHECK_EQ(static_cast<int>(screen.cell(0, 0).style.fg.index), 1);
}

ST_TEST(ansi_screen_handles_split_utf8_codepoints) {
  // 半个 UTF-8 码点：`中` 是 3 字节，切成 1+2。
  AnsiScreen screen(20, 2);
  const std::string chinese = "中";
  screen.feed(std::string_view(chinese).substr(0, 1));
  screen.feed(std::string_view(chinese).substr(1));
  ST_CHECK(screen.cell(0, 0).ch == U'中');
}

ST_TEST(ansi_screen_handles_split_osc_title) {
  AnsiScreen screen(20, 2);
  screen.feed("\x1b]0;my ti");
  screen.feed("tle\x07");
  ST_CHECK_EQ(screen.title(), std::string("my title"));
}

// ════════════════════════════════════════════════════════════════════════════
// 尺寸 / 增量 / 杂项
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(ansi_screen_resize_keeps_content_and_clamps_cursor) {
  AnsiScreen screen(10, 3);
  screen.feed("hello\r\nworld");
  screen.feed("\x1b[3;10H");      // 光标放到右下角
  screen.resize(5, 2);
  ST_CHECK_EQ(screen.cols(), 5);
  ST_CHECK_EQ(screen.rows(), 2);
  ST_CHECK_EQ(row(screen, 0), std::string("hello"));
  ST_CHECK(screen.cursor_row() <= 1);
  ST_CHECK(screen.cursor_col() <= 4);
}

ST_TEST(ansi_screen_dirty_rows_track_changes) {
  // 增量重绘的地基：只有**变过的行**该被重画（终端一帧往往只改几行）。
  AnsiScreen screen(10, 4);
  (void)screen.take_dirty_rows();          // 清掉初始全脏
  screen.feed("\x1b[2;1Hx");
  const auto dirty = screen.take_dirty_rows();
  ST_CHECK_EQ(dirty.size(), std::size_t{1});
  ST_CHECK_EQ(dirty[0], 1);
  // 取过一次就清了
  ST_CHECK(screen.take_dirty_rows().empty());
}

ST_TEST(ansi_screen_ignores_unknown_sequences_without_printing_them) {
  // 未知序列要**安静忽略**——当文本画出来会污染屏幕（这是"看到 [1;2x 乱码"
  // 这类现象的来源）。这里用鼠标上报这类本层刻意不支持的开关。
  AnsiScreen screen(20, 2);
  screen.feed("\x1b[?1000h");
  screen.feed("OK");
  ST_CHECK_EQ(row(screen, 0), std::string("OK"));
  ST_CHECK_EQ(screen.cursor_col(), 2);
}

ST_TEST(ansi_screen_bell_counts_and_cursor_shape) {
  AnsiScreen screen(20, 2);
  screen.feed("\a\a");
  ST_CHECK_EQ(screen.bell_count(), std::size_t{2});
  screen.feed("\x1b[5 q");
  // 枚举不能直接 `std::format`（本仓的断言宏要把值格式化）——转成字符串比。
  ST_CHECK(screen.cursor_shape() == AnsiCursorShape::Bar);
  ST_CHECK(screen.cursor_shape() != AnsiCursorShape::Block);
  screen.feed("\x1b[?25l");
  ST_CHECK(!screen.cursor_visible());
  screen.feed("\x1b[?25h");
  ST_CHECK(screen.cursor_visible());
}

ST_TEST(ansi_screen_delete_and_insert_characters) {
  AnsiScreen screen(20, 2);
  screen.feed("abcdef");
  screen.feed("\x1b[1;3H\x1b[2P");   // 删两个字符
  ST_CHECK_EQ(row(screen, 0), std::string("abef"));
  screen.feed("\x1b[1;3H\x1b[2@");   // 插两个空位
  ST_CHECK_EQ(row(screen, 0), std::string("ab  ef"));
}

ST_TEST(ansi_screen_rep_repeats_last_graphic) {
  // `CSI b`（REP）：重复上个字符 n 次。PowerShell 表格线、字符图表常用；
  // 旧实现直接忽略——一条横线 `\x1b[20b` 画不出来。
  AnsiScreen screen(30, 2);
  screen.feed("-\x1b[5b");   // `-` 后重复 5 次 → 共 6 个
  ST_CHECK_EQ(row(screen, 0), std::string("------"));
  ST_CHECK_EQ(screen.cursor_col(), 6);
}

ST_TEST(ansi_screen_osc_escape_only_terminates_on_st) {
  // OSC 结束符 `ESC \`（ST）才是正式收尾；裸 `ESC` 后跟**其它字节**时不能把那个
  // 字节无端吞掉（旧实现：`ESC[` 的 `[` 被吃、后续 CSI 整个错位）。
  AnsiScreen screen(20, 2);
  screen.feed("\x1b]0;title\x1b\\OK");   // ST 结束后正常接文本
  ST_CHECK_EQ(screen.title(), std::string("title"));
  ST_CHECK_EQ(row(screen, 0), std::string("OK"));
  // 裸 ESC 后跟非 `\`：OSC 继续（不会吞字节）——这里用 BEL 结束验证。
  AnsiScreen odd(20, 2);
  odd.feed("\x1b]0;t2\x1bX!");
  odd.feed(std::string(1, char(0x07)) + "after");
  ST_CHECK_EQ(odd.title(), std::string("t2X!"));
  ST_CHECK_EQ(row(odd, 0), std::string("after"));
}

ST_TEST(ansi_screen_delete_leaves_no_orphan_continuation) {
  // DCH 的宽字符语义（xterm/kitty 同款）：删除点在宽字**主格**时，
  // 整个宽字（两格）算**一个**删除单位——不会剩下半张脸；删除点落在
  // **右半**（continuation）时回退到主格删。两部都验证：
  AnsiScreen screen(10, 2);
  screen.feed("ab中cd");
  // ① 定位到 `中` 的主格（第 3 列，1 基）删 1 个「字符」= 整个宽字。
  screen.feed("\x1b[1;3H\x1b[1P");
  ST_CHECK_EQ(row(screen, 0), std::string("abcd"));
  // ② 删除点落在宽字**右半**（continuation，第 2 列删完后 `中` 在 2-3、
  //    右半是 3）：回退到主格删，不剩半张脸。
  AnsiScreen half(10, 2);
  half.feed("x中y");
  half.feed("\x1b[1;3H\x1b[1P");   // 第 3 列 = `中` 的右半
  ST_CHECK_EQ(row(half, 0), std::string("xy"));
}

ST_TEST(ansi_screen_resize_truncation_leaves_no_orphan) {
  // 缩窄正好切开宽字符：留下的左半要补空，不能留半个汉字占一格。
  AnsiScreen screen(8, 2);
  screen.feed("abc中d");
  screen.resize(4, 2);   // 截断点在第 4 列，切开 `中`（占 3-4 列）
  ST_CHECK_EQ(screen.cols(), 4);
  const std::string text = row(screen, 0);
  // 要么 `abc` 要么 `ab`（补空后截尾），但不能有半个 `中` 的字符在末尾。
  ST_CHECK(text.find("中") == std::string::npos || text == std::string("abc"));
}
