/// 终端文本选择的**网格换算**测试（列 ↔ 字节偏移、宽字符、行尾裁空白）。
///
/// ### 为什么不测 `Terminal` 本体
///
/// `Terminal` 的 PTY 模式要真 shell 才能出屏幕（见 `ui_terminal_pty_test.cpp`
/// 的 `Harness`），那类用例**环境敏感、默认跳过**。而本层的缺陷（选中字与高亮
/// 格不对应）全部来自**换算规则**——把它单独钉住，比在 PTY 夹具里测更稳、更快。
///
/// 端到端（真鼠标拖选 + 精确字符串判据）在 gbcode 上做，见 e2e 脚本。
///
/// ### 本轮钉住的事（2026-10-14）
///
/// 用户要求框架所有文本组件都要有好的选中复制；终端此前**只能整屏复制**。
/// 选择建在**等宽网格**上（`{绝对行号, 列}`），于是换算成了唯一的风险点：
/// ① 列 → 字节：CJK 占两列，按 1 列/字推会越选越偏；
/// ② 行尾空格：终端行用空格填满，不裁会把大量空白拷进剪贴板；
/// ③ 行号空间：回看（scrollback）与当前屏必须是**一根连续行号**，
///    否则跨「回看 + 屏幕」的选择会错行。

#include <string>
#include <string_view>

#include "st/core/string.hpp"
#include "st/test/test.hpp"
#include "st/text/ansi_screen.hpp"

namespace {

using st::text::AnsiScreen;

/// 把一行**网格文本**按「列 → 字节偏移」切片（与 `Terminal::selected_text`
/// 同一算法；此处独立复刻是为了能在没有 Terminal 的情况下测规则本身）。
[[nodiscard]] auto slice_columns(std::string_view text, int from_column, int to_column)
    -> std::string {
  std::size_t byte_begin = text.size();
  std::size_t byte_end = text.size();
  int column = 0;
  const std::size_t count = st::utf8_length(text);
  for (std::size_t i = 0; i <= count; ++i) {
    if (column <= from_column) byte_begin = st::utf8_offset(text, i);
    if (column <= to_column) byte_end = st::utf8_offset(text, i);
    if (i == count) break;
    const char32_t ch = st::utf8_decode(st::utf8_slice(text, i, 1)).front();
    column += st::text::cell_width(ch);
  }
  if (byte_end <= byte_begin) return {};
  return std::string(text.substr(byte_begin, byte_end - byte_begin));
}

/// 屏幕行 → 纯文本（与 `Terminal::grid_line_text` 同一算法）。
[[nodiscard]] auto screen_line_text(const AnsiScreen& screen, int row) -> std::string {
  std::string text;
  for (int col = 0; col < screen.cols();) {
    const st::text::AnsiCell& cell = screen.cell(row, col);
    if (cell.continuation) {
      ++col;
      continue;
    }
    if (cell.ch != 0) {
      // 就地把码点编成 UTF-8（不引 core/string 的编码器，保持本测试自包含）。
      const char32_t ch = cell.ch;
      if (ch < 0x80U) {
        text.push_back(static_cast<char>(ch));
      } else if (ch < 0x800U) {
        text.push_back(static_cast<char>(0xC0U | (ch >> 6U)));
        text.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
      } else if (ch < 0x10000U) {
        text.push_back(static_cast<char>(0xE0U | (ch >> 12U)));
        text.push_back(static_cast<char>(0x80U | ((ch >> 6U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
      } else {
        text.push_back(static_cast<char>(0xF0U | (ch >> 18U)));
        text.push_back(static_cast<char>(0x80U | ((ch >> 12U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | ((ch >> 6U) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
      }
    }
    col += st::text::cell_width(cell.ch);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
  return text;
}

}  // namespace

ST_TEST(terminal_selection_column_to_byte_ascii) {
  const std::string line = "hello world";
  // 列区间 → 字节（ASCII 一列一字节）
  ST_CHECK_EQ(slice_columns(line, 0, 5), std::string("hello"));
  ST_CHECK_EQ(slice_columns(line, 6, 11), std::string("world"));
  ST_CHECK_EQ(slice_columns(line, 3, 8), std::string("lo wo"));
  // 区间为空
  ST_CHECK_EQ(slice_columns(line, 5, 5), std::string{});
  // 越过行尾安全（不崩、不越界）
  ST_CHECK_EQ(slice_columns(line, 6, 999), std::string("world"));
}

ST_TEST(terminal_selection_column_to_byte_wide_chars) {
  // **关键**：CJK 占两列。若按“一列一字节”推，第二列起就会错位。
  const std::string line = "汉字abc";
  // "汉" 列 0-1、"字" 列 2-3、a 列 4、b 列 5、c 列 6
  ST_CHECK_EQ(slice_columns(line, 0, 2), std::string("汉"));
  ST_CHECK_EQ(slice_columns(line, 0, 4), std::string("汉字"));
  ST_CHECK_EQ(slice_columns(line, 2, 4), std::string("字"));
  ST_CHECK_EQ(slice_columns(line, 4, 7), std::string("abc"));
  ST_CHECK_EQ(slice_columns(line, 0, 7), line);
  // 落在宽字符**中间那一列**（列 1）：语义上归到该字符起点（不切出半个汉字）。
  const std::string half = slice_columns(line, 1, 4);
  ST_CHECK(half == "汉字" || half == "字");
  ST_CHECK(st::utf8_is_valid(half));   // 绝不产生半个码点
}

ST_TEST(terminal_selection_screen_text_trims_trailing_blanks) {
  // 屏幕行用空格填满：行尾空白必须裁掉，否则复制会带一大串空格。
  AnsiScreen screen(20, 3);
  screen.feed("hi");
  const std::string text = screen_line_text(screen, 0);
  ST_CHECK_EQ(text, std::string("hi"));
  // 整行未写内容 → 空串（而非 20 个空格）
  ST_CHECK_EQ(screen_line_text(screen, 1), std::string{});
}

ST_TEST(terminal_selection_screen_text_keeps_wide_cells_consistent) {
  AnsiScreen screen(20, 3);
  screen.feed("你好世界");
  const std::string text = screen_line_text(screen, 0);
  ST_CHECK_EQ(text, std::string("你好世界"));
  // 每个汉字占两列：`cell(col)` 存字形、`cell(col+1).continuation` 标记右半格。
  ST_CHECK(screen.cell(0, 0).ch == U'你');
  ST_CHECK(screen.cell(0, 1).continuation);   // “你” 的右半格是占位
  ST_CHECK(screen.cell(0, 2).ch == U'好');
  ST_CHECK(screen.cell(0, 3).continuation);
}
