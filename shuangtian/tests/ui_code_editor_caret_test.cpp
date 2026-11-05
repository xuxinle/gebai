// 差异视图里“编辑后重排文本”的光标保持（`CodeEditor::replace_text_keep_state`）。
//
// 这一组用例的存在理由（实测缺陷链）：
//  ① 旧实现用 `set_text` 推重排后的文本 ⇒ 光标归 0、滚动回顶、撤销栈清空
//     （现象：每敲一个字光标跳回行首）；
//  ② 改成“按行号+列号重新定位”后 ⇒ 填充行数量一变，同一显示行号指向别的内容
//     （现象：光标跑到别处）；
//  ③ 改用**逻辑行**（真实内容行）后 ⇒ 仍在原行，但列被“刚打的字”吃掉
//     （现象：光标卡在刚打的字**前面**不动）。
// 三条都不会让文本内容或差异结果出错——只有正在打字的人能发现，所以必须钉死。
//
// 口径：逻辑行 = 真实内容行；对齐填充行与 `@@` 分隔行不进逻辑行计数。

#include "st/test/test.hpp"
#include "st/ui/components/code_editor.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

using st::ui::CodeEditor;

/// 把一段文本切成行（用于断言“光标在第几行第几列”）。
auto line_and_column(const std::string& text, std::size_t index) -> std::pair<std::size_t, std::size_t> {
  std::size_t line = 0;
  std::size_t line_start = 0;
  const std::size_t upto = std::min(index, text.size());
  for (std::size_t at = 0; at < upto; ++at) {
    if (text[at] == '\n') {
      ++line;
      line_start = at + 1;
    }
  }
  return {line, upto - line_start};
}

/// 旧文本里第 `logical_row` 个真实行、第 `column` 列的字节偏移。
auto offset_of_logical(const std::string& text, const std::vector<char>& real,
                       std::size_t logical_row, std::size_t column) -> std::size_t {
  std::size_t begin = 0;
  std::size_t seen = 0;
  std::size_t row = 0;
  std::size_t at = 0;
  while (at <= text.size()) {
    if (at == text.size() || text[at] == '\n') {
      const bool is_real = row >= real.size() ? true : real[row] != 0;
      if (is_real) {
        if (seen == logical_row) {
          const std::size_t end = at;
          return begin + std::min(column, end - begin);
        }
        ++seen;
      }
      ++row;
      begin = at + 1;
    }
    ++at;
  }
  return text.size();
}

// ————————————————————————————————————————————————————————————————————————————
// ① 基础：光标与滚动不能丢
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(replace_text_keep_state_keeps_caret_not_reset_to_zero) {
  CodeEditor editor;
  editor.set_text("aaa\nbbb\nccc\nddd\n");
  editor.set_cursor_index(6);   // = "bbb" 的第 2 列（aaa\n + b|b）
  editor.set_scroll_offset(0.0f, 12.0f);

  // 重排后的文本多了一行（模拟差异视图插了一条对齐填充行）
  editor.replace_text_keep_state("aaa\nbbb\n\nccc\nddd\n");
  // 光标仍在 "bbb" 这一行、同一列，而不是被重置到 0
  const auto [line, column] = line_and_column(editor.text(), editor.cursor_index());
  ST_CHECK_EQ(line, std::size_t{1});   // 0 起：aaa 是 0、bbb 是 1
  ST_CHECK_EQ(column, std::size_t{2});
  // 滚动位置原样保留（不是回顶）
  ST_CHECK(std::abs(editor.scroll_offset().y - 12.0f) < 0.01f);
}

ST_TEST(replace_text_keep_state_keeps_undo_history) {
  CodeEditor editor;
  editor.set_text("base\n");
  editor.insert_text("EDIT\n");
  ST_CHECK(editor.can_undo());
  editor.replace_text_keep_state("base\nEDIT\n");   // 同一份内容重新合成
  // 撤销栈不能被清掉：否则用户打字时撤销键静默失灵
  ST_CHECK(editor.can_undo());
  ST_CHECK(editor.undo());
  ST_CHECK_EQ(editor.text(), std::string("base\n"));
}

// ————————————————————————————————————————————————————————————————————————————
// ② 填充行变化时按**逻辑行**定位（不是显示行）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(replace_text_keep_state_maps_by_logical_row_when_fillers_change) {
  CodeEditor editor;
  // 旧：第 1 行真实；第 2 行是填充；第 3 行真实 "target"
  const std::string old_text = "first\n\ntarget\ntail\n";
  const std::vector<char> old_real{1, 0, 1, 1};
  editor.set_text(old_text);
  // 光标放到 "target" 的第 3 个字符（t-a-r|get）
  editor.set_cursor_index(offset_of_logical(old_text, old_real, /*logical_row=*/1, /*column=*/3));

  // 新：填充行跑到 "target" 之前多了一条（这正是“重算后填充行数量变了”）
  const std::string new_text = "first\n\n\ntarget\ntail\n";
  const std::vector<char> new_real{1, 0, 0, 1, 1};

  editor.set_text(old_text);   // 复位（set_text 会重置光标，随后再设）
  editor.set_cursor_index(offset_of_logical(old_text, old_real, 1, 3));
  editor.replace_text_keep_state(new_text, old_real, new_real);

  const auto [display_line, column] = line_and_column(editor.text(), editor.cursor_index());
  // 新文本里 "target" 是第 4 显示行（0 起 3）
  ST_CHECK_EQ(display_line, std::size_t{3});
  ST_CHECK_EQ(column, std::size_t{3});   // 列保持：t-a-r|get
}

// ————————————————————————————————————————————————————————————————————————————
// ③ 实打实“编辑后重排”：光标必须跟着刚打的字，而不是卡在它前面
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(replace_text_keep_state_caret_follows_the_typed_char) {
  CodeEditor editor;
  const std::string text = "line one\nline two\nline three\n";
  editor.set_text(text);
  // 光标放到第 2 行第 5 列（"line |two"）
  const std::size_t start = text.find("two");
  editor.set_cursor_index(start);
  ST_CHECK_EQ(line_and_column(editor.text(), editor.cursor_index()).second, std::size_t{5});

  // 模拟“用户打了一个 X”：文本插入 + 光标前进（`insert_text` 的真实顺序）
  editor.insert_text("X");
  const std::size_t caret_after_typing = editor.cursor_index();
  // 此时 on_change 已触发过（真实链路里宿主就在这一刻重排）
  // 宿主重排：文本带着对齐填充行重新合成，光标位置按“打完字之后”交代回去
  const std::string rebuilt = "line one\n\nline two\nextra\nline three\n";
  const std::vector<char> new_real{1, 0, 1, 1, 1};
  std::vector<char> old_real(text.size(), 1);   // 旧文本没有填充行

  editor.replace_text_keep_state(rebuilt, old_real, new_real, caret_after_typing);

  const auto [line, column] = line_and_column(editor.text(), editor.cursor_index());
  // 仍在 "line Xtwo" 那一行（新文本第 3 显示行 = 0 起 2），列 6（打过 X 之后）
  ST_CHECK_EQ(line, std::size_t{2});
  ST_CHECK_EQ(column, std::size_t{6});
}

ST_TEST(replace_text_keep_state_without_anchor_uses_current_caret) {
  // `kNoCaretAnchor`（默认）：用当前 `cursor_` 当锚点。
  // 这条是给“不是编辑触发的重排”（例如切换布局重建了两栏）用的。
  CodeEditor editor;
  editor.set_text("alpha\nbeta\n");
  editor.set_cursor_index(9);   // = "beta" 第 3 列（b/e/t | a）
  // 新文本里的空行是**填充行**（`real` 里标 0）——不标的话它会被当成真实内容，
  // 光标就落到那条空行上（实测过的同一类错：填与内容的身份必须显式给）。
  editor.replace_text_keep_state("alpha\n\nbeta\n", {}, {1, 0, 1});
  const auto [line, column] = line_and_column(editor.text(), editor.cursor_index());
  ST_CHECK_EQ(line, std::size_t{2});   // 依然在 "beta"（新文本第 3 显示行）
  ST_CHECK_EQ(column, std::size_t{3});
}

}  // namespace
