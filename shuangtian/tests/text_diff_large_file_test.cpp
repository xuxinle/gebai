// 大文件的差异不能退化成「整篇替换」。
//
// 缺陷实录（2026-11-05）：`gbcode` 点开一个 2000 行的 `page.hpp` 的变更，
// 差异视图**整篇一片红**、右栏几乎空白、看不到任何真实改动。
//
// 根因：`diff_hunks` 用「双侧行数乘积」判规模上限（`kMaxLcsCells = 4'000'000`），
// 而 2000 行文件两侧相乘恰好是 4.004M —— **一个中等规模的头文件就会越过上限**，
// 于是退化为“整篇删除 + 整篇新增”。这不是“大文件才有的性能妥协”，而是
// 尺寸阈值用了**未剪枝**的行数：把公共前后缀剪掉之后，真正要算 LCS 的往往只剩几行。
//
// 这组用例钉住三件事：结果正确（改动数对）、公共前后缀照旧出现（视图有完整上下文）、
// 以及“真的大到离谱”时仍然如实退化（不静默给出假差异）。

#include "st/test/test.hpp"
#include "st/text/diff.hpp"

#include <string>
#include <vector>

namespace {

auto number_lines(const std::vector<std::string>& lines) -> std::string {
  std::string text;
  for (const std::string& line : lines) {
    text += line;
    text += '\n';
  }
  return text;
}

}  // namespace

ST_TEST(diff_large_file_with_few_changes_is_not_whole_file_rewrite) {
  // 2000 行，只改 3 行 —— 这是编辑器里最常见的场景（改了几处、提交前看差异）。
  std::vector<std::string> before;
  before.reserve(2000);
  for (int index = 1; index <= 2000; ++index) {
    before.push_back("// line " + std::to_string(index) + " body");
  }
  std::vector<std::string> after = before;
  after[9] = "// line 10 CHANGED";
  after[999] = "// line 1000 CHANGED";
  after[1999] = "// line 2000 CHANGED";

  const auto hunks = st::text::diff_hunks(number_lines(before), number_lines(after), 3);
  const auto [added, removed] = st::text::count_changes(hunks);

  // 旧行为：3 处改动被报成「+2000 −2000」、1 个巨型块。
  ST_CHECK_EQ(added, 3);
  ST_CHECK_EQ(removed, 3);
  ST_CHECK(hunks.size() >= 3);   // 三处相隔很远，必须是三块（不是一整块）

  // 每一块都要带上下文（否则视图里改动之间是完全空白的，看不出位置）。
  bool any_context = false;
  for (const auto& hunk : hunks) {
    for (const auto& line : hunk.lines) {
      if (line.kind == st::text::DiffLineKind::Context) any_context = true;
    }
  }
  ST_CHECK(any_context);
}

ST_TEST(diff_large_file_identical_prefix_and_suffix_are_context) {
  // 剪枝后行号与内容必须仍对得上：第一块的前导上下文要从**文件开头**算起，
  // 末块的后缀要接到**文件末尾**（拼接 ops 时最容易在这里错一位）。
  std::vector<std::string> before;
  before.reserve(1200);
  for (int index = 1; index <= 1200; ++index) {
    before.push_back("row " + std::to_string(index));
  }
  std::vector<std::string> after = before;
  after[0] = "row 1 EDITED";       // 第一行就改（无前导上下文）
  after[1199] = "row 1200 EDITED"; // 最后一行也改（无后续上下文）

  const auto hunks = st::text::diff_hunks(number_lines(before), number_lines(after), 2);
  ST_REQUIRE(hunks.size() == 2);
  // 第一块的第一条就是删除（第 1 行），后面紧跟它的新增。
  ST_CHECK(hunks[0].lines.front().kind == st::text::DiffLineKind::Removed);
  ST_CHECK_EQ(hunks[0].lines.front().old_line, std::size_t{1});
  // 最后一块的最后两条是「改末行」的那一对。
  const auto& tail = hunks.back();
  ST_REQUIRE(tail.lines.size() >= 2);
  ST_CHECK(tail.lines.back().kind == st::text::DiffLineKind::Added);
  ST_CHECK_EQ(tail.lines.back().new_line, std::size_t{1200});
}

ST_TEST(diff_common_span_counts_prefix_and_suffix_without_overlap) {
  // 全相同：前缀取满、后缀为 0（不能两边都算，否则拼 ops 会重复行）。
  const auto same = st::text::diff_hunks("a\nb\nc\n", "a\nb\nc\n", 3);
  ST_CHECK(same.empty());

  // 只有中间一行不同：前缀 1、后缀 1。
  const auto middle = st::text::diff_hunks("a\nb\nc\n", "a\nB\nc\n", 3);
  ST_REQUIRE(middle.size() == 1);
  ST_CHECK_EQ(middle.front().old_start, std::size_t{1});
  ST_CHECK_EQ(middle.front().old_count, std::size_t{3});
}

ST_TEST(diff_genuinely_huge_files_still_degrade_honestly) {
  // 两侧**完全无关**的 3000 行（中间段无公共前后缀可剪）：
  // 仍会越过上限并如实退化为「整篇替换」——这是刻意的（不静默截断）。
  // 本用例存在的意义是钉住“退化仍在”，免得为了修上面的缺陷把上限整个拿掉。
  std::vector<std::string> left;
  std::vector<std::string> right;
  left.reserve(3000);
  right.reserve(3000);
  for (int index = 0; index < 3000; ++index) {
    left.push_back("left unique " + std::to_string(index));
    right.push_back("right unique " + std::to_string(index));
  }
  const auto hunks = st::text::diff_hunks(number_lines(left), number_lines(right), 3);
  ST_REQUIRE(hunks.size() == 1);
  const auto [added, removed] = st::text::count_changes(hunks);
  ST_CHECK_EQ(added, 3000);
  ST_CHECK_EQ(removed, 3000);
}
