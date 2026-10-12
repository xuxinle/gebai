/// 结构化差异（`st::text::diff`）测试：两段文本生成 hunk / unified patch 解析 / 两栏行 /
/// 往返一致性。
///
/// 本层是「对比能力」的地基（并列视图、hunk 导航、将来的逐块暂存与 gutter 都读它），
/// 因此用例覆盖的是**语义**而不是实现：行号三种坐标、末尾换行、二进制、重命名、
/// `\ No newline`、上下文分组边界——这些做错的表现都是"看着正常但差了几行"。

#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/text/diff.hpp"

namespace {

using st::text::DiffDocument;
using st::text::DiffHunk;
using st::text::DiffLineKind;
using st::text::FileDiff;

/// 取 hunk 正文的「前缀 + 文本」形态（与 patch 文本同形，断言更贴近人读的东西）。
[[nodiscard]] auto shape(const DiffHunk& hunk) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& line : hunk.lines) {
    switch (line.kind) {
      case DiffLineKind::Context: out.push_back(" " + line.text); break;
      case DiffLineKind::Removed: out.push_back("-" + line.text); break;
      case DiffLineKind::Added: out.push_back("+" + line.text); break;
    }
  }
  return out;
}

}  // namespace

ST_TEST(diff_single_line_change_middle) {
  const std::vector<DiffHunk> hunks = st::text::diff_hunks("a\nb\nc\n", "a\nB\nc\n");
  ST_REQUIRE(hunks.size() == 1);
  const DiffHunk& hunk = hunks.front();
  // 行号：两侧都是第 2 行；上下文各 1 行（文档只有 3 行）。
  ST_CHECK_EQ(hunk.old_start, std::size_t{1});
  ST_CHECK_EQ(hunk.old_count, std::size_t{3});
  ST_CHECK_EQ(hunk.new_start, std::size_t{1});
  ST_CHECK_EQ(hunk.new_count, std::size_t{3});
  const auto lines = shape(hunk);
  ST_REQUIRE(lines.size() == 4);
  ST_CHECK_EQ(lines[0], std::string(" a"));
  ST_CHECK_EQ(lines[1], std::string("-b"));
  ST_CHECK_EQ(lines[2], std::string("+B"));
  ST_CHECK_EQ(lines[3], std::string(" c"));
  // 行号映射：删除行只在旧侧、新增行只在新侧。
  ST_CHECK_EQ(hunk.lines[1].old_line, std::size_t{2});
  ST_CHECK_EQ(hunk.lines[1].new_line, std::size_t{0});
  ST_CHECK_EQ(hunk.lines[2].old_line, std::size_t{0});
  ST_CHECK_EQ(hunk.lines[2].new_line, std::size_t{2});
  ST_CHECK_EQ(hunk.lines[3].old_line, std::size_t{3});
  ST_CHECK_EQ(hunk.lines[3].new_line, std::size_t{3});
}

ST_TEST(diff_insertion_only_touches_new_side) {
  const std::vector<DiffHunk> hunks = st::text::diff_hunks("a\nc\n", "a\nb\nc\n");
  ST_REQUIRE(hunks.size() == 1);
  const auto lines = shape(hunks.front());
  ST_REQUIRE(lines.size() == 3);
  ST_CHECK_EQ(lines[0], std::string(" a"));
  ST_CHECK_EQ(lines[1], std::string("+b"));
  ST_CHECK_EQ(lines[2], std::string(" c"));
  // 新增行只在新侧有行号，旧侧为 0（"本行在那一侧不存在"）。
  ST_CHECK_EQ(hunks.front().lines[1].old_line, std::size_t{0});
  ST_CHECK_EQ(hunks.front().lines[1].new_line, std::size_t{2});
  ST_CHECK_EQ(hunks.front().lines[0].old_line, hunks.front().lines[0].new_line);
}

ST_TEST(diff_lcs_not_naive_replace) {
  // 中间插一行：只应产出 1 个新增行，而不是"后面全改"。
  const std::vector<DiffHunk> hunks = st::text::diff_hunks("a\nc\n", "a\nb\nc\n");
  const auto [additions, deletions] = st::text::count_changes(hunks);
  ST_CHECK_EQ(additions, std::size_t{1});
  ST_CHECK_EQ(deletions, std::size_t{0});
}

ST_TEST(diff_multiple_hunks_split_by_context) {
  std::string old_text;
  std::string new_text;
  for (int i = 0; i < 40; ++i) {
    const std::string line = "line" + std::to_string(i) + "\n";
    old_text += line;
    new_text += (i == 2 || i == 30) ? "line" + std::to_string(i) + "x\n" : line;
  }
  const std::vector<DiffHunk> hunks = st::text::diff_hunks(old_text, new_text);
  // 两处改动相隔 28 行 > 2×3，必须是两个 hunk。
  ST_REQUIRE(hunks.size() == 2);
  ST_CHECK_EQ(hunks[0].old_start, std::size_t{1});    // 0..2 行 + 前后文
  ST_CHECK_EQ(hunks[1].old_start, std::size_t{28});   // 30 行前后各 3 行
  // 两个 hunk 的上下文不重叠。
  ST_CHECK(hunks[0].old_start + hunks[0].old_count <= hunks[1].old_start);
}

ST_TEST(diff_nearby_changes_merge_into_one_hunk) {
  std::string old_text;
  std::string new_text;
  for (int i = 0; i < 12; ++i) {
    const std::string line = "l" + std::to_string(i) + "\n";
    old_text += line;
    new_text += (i == 3 || i == 6) ? "l" + std::to_string(i) + "z\n" : line;
  }
  const std::vector<DiffHunk> hunks = st::text::diff_hunks(old_text, new_text);
  // 相隔 3 行 ≤ 2×3，合成一个 hunk（git 同口径）。
  ST_CHECK_EQ(hunks.size(), std::size_t{1});
}

ST_TEST(diff_empty_sides_start_at_zero) {
  // 新文件：旧侧 0 行 → `@@ -0,0 +1,N @@`。
  const std::vector<DiffHunk> added = st::text::diff_hunks("", "a\nb\n");
  ST_REQUIRE(added.size() == 1);
  ST_CHECK_EQ(added.front().old_start, std::size_t{0});
  ST_CHECK_EQ(added.front().old_count, std::size_t{0});
  ST_CHECK_EQ(added.front().new_start, std::size_t{1});
  ST_CHECK_EQ(added.front().new_count, std::size_t{2});
  // 删空文件：新侧 0 行 → `@@ -1,N +0,0 @@`。
  const std::vector<DiffHunk> removed = st::text::diff_hunks("a\nb\n", "");
  ST_REQUIRE(removed.size() == 1);
  ST_CHECK_EQ(removed.front().old_start, std::size_t{1});
  ST_CHECK_EQ(removed.front().new_start, std::size_t{0});
  ST_CHECK_EQ(removed.front().new_count, std::size_t{0});
}

ST_TEST(diff_trailing_newline_only_change_is_visible) {
  // 行内容完全相同、只有一个缺末行换行：这**是**一个真实的改动（不同的 blob），
  // 必须能被看见——只比行内容的实现会在这里给出"没有差异"。
  const std::vector<DiffHunk> hunks = st::text::diff_hunks("a\nb\n", "a\nb");
  ST_REQUIRE(hunks.size() == 1);
  const auto lines = shape(hunks.front());
  ST_REQUIRE(lines.size() == 2);
  ST_CHECK_EQ(lines[0], std::string("-b"));
  ST_CHECK_EQ(lines[1], std::string("+b"));
  ST_CHECK(!hunks.front().lines[0].no_newline);  // 旧侧有换行
  ST_CHECK(hunks.front().lines[1].no_newline);   // 新侧末行无换行（标记只在缺换行的那一侧）
  // 反方向同理。
  const std::vector<DiffHunk> back = st::text::diff_hunks("a\nb", "a\nb\n");
  ST_REQUIRE(back.size() == 1);
  // 反方向同理：back 的**旧**侧末行无换行（`a\nb`）。
  ST_CHECK(back.front().lines[0].no_newline);
  ST_CHECK(!back.front().lines[1].no_newline);
}

ST_TEST(diff_identical_text_has_no_hunks) {
  ST_CHECK(st::text::diff_hunks("a\nb\n", "a\nb\n").empty());
  ST_CHECK(st::text::diff_hunks("", "").empty());
}

ST_TEST(diff_crlf_is_not_a_content_change) {
  // Windows 工作区读到的 CRLF 与仓库里的 LF 不该被报成"整篇都改了"。
  ST_CHECK(st::text::diff_hunks("a\r\nb\r\n", "a\nb\n").empty());
}

ST_TEST(diff_parse_unified_basic) {
  const std::string patch =
      "diff --git a/src/a.cpp b/src/a.cpp\n"
      "index 1111111..2222222 100644\n"
      "--- a/src/a.cpp\n"
      "+++ b/src/a.cpp\n"
      "@@ -1,3 +1,4 @@ void f() {\n"
      " int main() {\n"
      "-  return 1;\n"
      "+  return 2;\n"
      "+  (void)0;\n"
      " }\n";
  const DiffDocument document = st::text::parse_unified(patch);
  ST_REQUIRE(document.files.size() == 1);
  const FileDiff& file = document.files.front();
  ST_CHECK_EQ(file.old_path, std::string("src/a.cpp"));
  ST_CHECK_EQ(file.new_path, std::string("src/a.cpp"));
  ST_REQUIRE(file.hunks.size() == 1);
  const DiffHunk& hunk = file.hunks.front();
  ST_CHECK_EQ(hunk.old_start, std::size_t{1});
  ST_CHECK_EQ(hunk.old_count, std::size_t{3});
  ST_CHECK_EQ(hunk.new_start, std::size_t{1});
  ST_CHECK_EQ(hunk.new_count, std::size_t{4});
  ST_CHECK_EQ(hunk.section, std::string("void f() {"));
  const auto lines = shape(hunk);
  ST_REQUIRE(lines.size() == 5);
  ST_CHECK_EQ(lines[1], std::string("-  return 1;"));
  ST_CHECK_EQ(lines[2], std::string("+  return 2;"));
  ST_CHECK_EQ(lines[3], std::string("+  (void)0;"));
  ST_CHECK_EQ(file.additions, std::size_t{2});
  ST_CHECK_EQ(file.deletions, std::size_t{1});
  ST_CHECK(document.warnings.empty());
}

ST_TEST(diff_parse_unified_keeps_content_that_looks_like_a_header) {
  // hunk 内容里的一行 `-- 注释` 在 patch 文本里就是 `--- 注释`：
  // 按行首前缀无条件判头部会把正文吃掉（这是最容易做错的一条）。
  const std::string patch =
      "--- a/x.txt\n"
      "+++ b/x.txt\n"
      "@@ -1,2 +1,1 @@\n"
      "--- 这是内容，不是文件头\n"
      "-old\n"
      "+new\n";
  const DiffDocument document = st::text::parse_unified(patch);
  ST_REQUIRE(document.files.size() == 1);
  const FileDiff& file = document.files.front();
  ST_CHECK_EQ(file.old_path, std::string("x.txt"));
  ST_REQUIRE(file.hunks.size() == 1);
  const auto lines = shape(file.hunks.front());
  ST_REQUIRE(lines.size() == 3);
  // 删除行的内容是 `-- 这是内容…`，在 patch 文本里就是 `--- 这是内容…`——
  // 按行首前缀无条件判头部会把这一行吃掉。
  ST_CHECK_EQ(lines[0], std::string("--- 这是内容，不是文件头"));
}

ST_TEST(diff_parse_unified_binary_and_rename) {
  const std::string patch =
      "diff --git a/old.png b/new.png\n"
      "similarity index 100%\n"
      "rename from old.png\n"
      "rename to new.png\n"
      "Binary files a/old.png and b/new.png differ\n"
      "diff --git a/gone.txt b/gone.txt\n"
      "deleted file mode 100644\n"
      "--- a/gone.txt\n"
      "+++ /dev/null\n"
      "@@ -1,1 +0,0 @@\n"
      "-bye\n";
  const DiffDocument document = st::text::parse_unified(patch);
  ST_REQUIRE(document.files.size() == 2);
  ST_CHECK(document.files[0].renamed);
  ST_CHECK(document.files[0].binary);
  ST_CHECK_EQ(document.files[0].old_path, std::string("old.png"));
  ST_CHECK_EQ(document.files[0].new_path, std::string("new.png"));
  ST_CHECK(document.files[1].deleted);
  ST_CHECK(document.files[1].new_path.empty());
  ST_REQUIRE(document.files[1].hunks.size() == 1);
  ST_CHECK_EQ(document.files[1].hunks.front().new_start, std::size_t{0});
}

ST_TEST(diff_parse_unified_no_newline_marker) {
  const std::string patch =
      "--- a/x\n"
      "+++ b/x\n"
      "@@ -1,1 +1,1 @@\n"
      "-old\n"
      "\\ No newline at end of file\n"
      "+new\n"
      "\\ No newline at end of file\n";
  const DiffDocument document = st::text::parse_unified(patch);
  ST_REQUIRE(document.files.size() == 1);
  ST_REQUIRE(document.files[0].hunks.size() == 1);
  const DiffHunk& hunk = document.files[0].hunks.front();
  ST_REQUIRE(hunk.lines.size() == 2);
  ST_CHECK(hunk.lines[0].no_newline);
  ST_CHECK(hunk.lines[1].no_newline);
}

ST_TEST(diff_parse_unified_warns_on_mismatched_counts) {
  // hunk 头声明 4 行，实际只给 2 行（模拟被截断的 patch）：**如实告警**，
  // 并且按实际行收录——静默忽略会显示出一个"看着正常但少几行"的差异。
  const std::string patch =
      "--- a/x\n"
      "+++ b/x\n"
      "@@ -1,4 +1,4 @@\n"
      " one\n"
      " two\n";
  const DiffDocument document = st::text::parse_unified(patch);
  ST_REQUIRE(document.files.size() == 1);
  ST_CHECK_EQ(document.warnings.size(), std::size_t{1});
  ST_REQUIRE(document.files[0].hunks.size() == 1);
  ST_CHECK_EQ(document.files[0].hunks[0].lines.size(), std::size_t{2});
}

ST_TEST(diff_unified_round_trip) {
  // 结构 → patch → 结构：行号、类型、内容、末尾换行标记都必须一致。
  const FileDiff original =
      st::text::make_file_diff("a\nb\nc\nd\ne\nf\ng\nh\n", "a\nB\nc\nd\ne\nf\ng\nH\n",
                               "src/a.txt", "src/a.txt");
  const std::string patch = st::text::to_unified(original);
  ST_CHECK(patch.starts_with("diff --git a/src/a.txt b/src/a.txt\n"));
  const DiffDocument parsed = st::text::parse_unified(patch);
  ST_REQUIRE(parsed.files.size() == 1);
  const FileDiff& file = parsed.files.front();
  ST_REQUIRE(file.hunks.size() == original.hunks.size());
  ST_CHECK_EQ(file.additions, original.additions);
  ST_CHECK_EQ(file.deletions, original.deletions);
  for (std::size_t h = 0; h < file.hunks.size(); ++h) {
    ST_CHECK_EQ(file.hunks[h].old_start, original.hunks[h].old_start);
    ST_CHECK_EQ(file.hunks[h].old_count, original.hunks[h].old_count);
    ST_CHECK_EQ(file.hunks[h].new_start, original.hunks[h].new_start);
    ST_CHECK_EQ(file.hunks[h].new_count, original.hunks[h].new_count);
    const auto left = shape(file.hunks[h]);
    const auto right = shape(original.hunks[h]);
    ST_CHECK_EQ(left.size(), right.size());
    for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
      ST_CHECK_EQ(left[i], right[i]);
    }
  }
  ST_CHECK(parsed.warnings.empty());
}

ST_TEST(diff_build_rows_pads_the_shorter_side) {
  // 删 2 行、加 1 行：两栏必须是 2 行高（右侧补一行填充），否则两栏会错位。
  const std::vector<DiffHunk> hunks = st::text::diff_hunks("a\nb\nc\nz\n", "a\nc!z\n");
  ST_REQUIRE(!hunks.empty());
  const auto rows = st::text::build_rows(hunks.front(), 0);
  std::size_t left_count = 0;
  std::size_t right_count = 0;
  bool saw_pad = false;
  for (const auto& row : rows) {
    if (row.left >= 0) ++left_count;
    if (row.right >= 0) ++right_count;
    if (row.left < 0 || row.right < 0) saw_pad = true;
    ST_CHECK_EQ(row.hunk, std::size_t{0});
  }
  ST_CHECK_EQ(left_count, hunks.front().lines.size() - 1);   // 除新增行外
  ST_CHECK(left_count != right_count || saw_pad);
  ST_CHECK(saw_pad);
}

ST_TEST(diff_split_lines_semantics) {
  const auto lines = st::text::split_lines("a\n\nb\n");
  ST_REQUIRE(lines.size() == 3);
  ST_CHECK_EQ(std::string(lines[1]), std::string(""));   // 空行保留
  ST_CHECK(st::text::split_lines("").empty());
  ST_CHECK_EQ(st::text::split_lines("\n").size(), std::size_t{1});
}

// ————————————————————————————————————————————————————————————————————————————
// 行内字符级差异（`diff_inline` / `refine_inline_diffs`）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(diff_inline_marks_only_the_changed_word) {
  // 「改一个词」：改动区间必须**落在那个词里**，而不是盖满整行。
  // 这是行内差异存在的全部理由——盖整行的话视图上跟纯行级没区别。
  const auto [old_ranges, new_ranges] =
      st::text::diff_inline("int count = 1;", "int total = 1;");
  ST_REQUIRE(!old_ranges.empty());
  ST_REQUIRE(!new_ranges.empty());
  // ⚠ 区间**可能不止一段**（这里是两段）：`count` → `total` 之间有共同的 `t`/`o`，
  // 字符级 LCS 会把它们认成「没变」，于是改动被切成几段。这是正确行为
  //（VSCode / IDEA 同样如此）——硬要求“一段”会掩盖真实语义，
  // 所以判据改成「所有区间都落在被改的那个词内（不碰 `int ` 与 ` = 1;`）」。
  for (const auto& range : old_ranges) {
    ST_CHECK(range.first >= 4 && range.second <= 9);
  }
  for (const auto& range : new_ranges) {
    ST_CHECK(range.first >= 4 && range.second <= 9);
  }
  // 且改动确实覆盖了那个词的关键部分（不是空手而归）。
  ST_CHECK_EQ(old_ranges.front().first, std::size_t{4});
}

ST_TEST(diff_inline_ranges_are_byte_offsets_for_cjk) {
  // 中文：区间必须是**字节偏移**且**不切碎汉字**。
  // 字节级 LCS 会把一个汉字切成 2+1，高亮看起来就“偏了”——本用例正是钉这个。
  const auto [old_ranges, new_ranges] = st::text::diff_inline("你好世界", "你好霜天");
  ST_REQUIRE(old_ranges.size() == 1);
  ST_REQUIRE(new_ranges.size() == 1);
  // 「世界」在 UTF-8 里各 3 字节：从第 6 字节开始、到第 12 字节结束。
  ST_CHECK_EQ(old_ranges[0].first, std::size_t{6});
  ST_CHECK_EQ(old_ranges[0].second, std::size_t{12});
  ST_CHECK_EQ(new_ranges[0].first, std::size_t{6});
  ST_CHECK_EQ(new_ranges[0].second, std::size_t{12});
  // 区间边界必须落在字符边界上（不与任何码位相交）：
  // 判据是“按这个偏移切片，切出来的仍是合法 UTF-8 的开头”。
  const std::string_view text = "你好世界";
  for (const auto& range : old_ranges) {
    // ⚠ 区间是**半开区间** `[first, second)`：`second` 就是区间后第一个字节的位置，
    // 它可以等于 `size()`（本用例里恰好就是：`[6,12)` 而文本正好 12 字节）。
    // 那时它指向 `end()`——用 `operator[]` 读是越界（`_GLIBCXX_ASSERTIONS` 下当场
    // `abort`：**整个分片进程消失**，后面所有用例都不跑，而汇总里只报“分片未能启动”；
    // release 下则是静默 UB）。所以右端要单独判“到末尾也算合法”。
    const auto lead = static_cast<unsigned char>(text[range.first]);
    ST_CHECK((lead & 0xC0U) != 0x80U);   // 不是续字节 = 落在码位开头
    if (range.second >= text.size()) continue;   // 末尾是合法边界，无可读字节
    const auto tail = static_cast<unsigned char>(text[range.second]);
    ST_CHECK((tail & 0xC0U) != 0x80U);
  }
}

ST_TEST(diff_inline_equal_lines_have_no_ranges) {
  const auto [old_ranges, new_ranges] = st::text::diff_inline("same();", "same();");
  ST_CHECK(old_ranges.empty());
  ST_CHECK(new_ranges.empty());
}

ST_TEST(diff_inline_merges_adjacent_edits) {
  // 连续改动应归并成**一段**而不是每字符一段：逐字符填色不但慢，
  // 还会因四舍五入出现「缝」（实测观感就是“断成一截一截”）。
  const auto [old_ranges, new_ranges] = st::text::diff_inline("abcdef", "abXYef");
  ST_REQUIRE(old_ranges.size() == 1);
  ST_REQUIRE(new_ranges.size() == 1);
  ST_CHECK_EQ(old_ranges[0].first, std::size_t{2});
  ST_CHECK_EQ(old_ranges[0].second, std::size_t{4});
  ST_CHECK_EQ(new_ranges[0].first, std::size_t{2});
  ST_CHECK_EQ(new_ranges[0].second, std::size_t{4});
}

ST_TEST(make_file_diff_fills_inline_ranges) {
  // `make_file_diff` 默认就把行内区间算好（不用调用方额外记一次“精化”）。
  const FileDiff file = st::text::make_file_diff("int count = 1;\n", "int total = 1;\n");
  ST_REQUIRE(!file.hunks.empty());
  bool saw_inline = false;
  for (const auto& line : file.hunks.front().lines) {
    if (!line.inline_old.empty() || !line.inline_new.empty()) saw_inline = true;
  }
  ST_CHECK(saw_inline);
}

ST_TEST(refine_inline_diffs_skips_unequal_run_sizes) {
  // 两侧行数不等（这里是「纯新增一行」）时**不配对**：按顺序硬配会把
  // 「新增了 1 行」误读成「某一行被改了」。
  auto hunks = st::text::diff_hunks("a\nb\n", "a\nb\nc\n");
  st::text::refine_inline_diffs(hunks);
  bool any_inline = false;
  for (const auto& hunk : hunks) {
    for (const auto& line : hunk.lines) {
      if (!line.inline_old.empty() || !line.inline_new.empty()) any_inline = true;
    }
  }
  ST_CHECK(!any_inline);
}

// ————————————————————————————————————————————————————————————————————————————
// 部分暂存（`build_partial_patch`）——「只把这几个改动提交了」的地基
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(partial_patch_keeps_only_selected_added_lines) {
  // 一行改三处：只选中间那处新增行 → 拼出来的补丁里只该有它。
  const FileDiff file = st::text::make_file_diff("a\nX\nc\n", "a\nX\nc\nNEW\n");
  ST_REQUIRE(!file.hunks.empty());
  const auto& hunk = file.hunks.front();
  // 找到那条新增行的下标（不硬编码位置：hunk 的行序由 LCS 决定）。
  std::optional<std::size_t> added_index;
  for (std::size_t i = 0; i < hunk.lines.size(); ++i) {
    if (hunk.lines[i].kind == st::text::DiffLineKind::Added && hunk.lines[i].text == "NEW") {
      added_index = i;
    }
  }
  ST_REQUIRE(added_index.has_value());
  const std::string patch = st::text::build_partial_patch(hunk, {*added_index});
  ST_CHECK(patch.find("+NEW") != std::string::npos);
  // 没选中的上下文行仍在（补丁要能贴到正确位置，上下文不能丢）。
  ST_CHECK(patch.find(" a") != std::string::npos);
  // 不得出现文件头之外的 `diff --git`（只给 hunk 片段时不应自己造头）。
  ST_CHECK(patch.find("diff --git") == std::string::npos);
}

ST_TEST(partial_patch_downgrades_unselected_removal_to_context) {
  // 关键语义：未选中的**删除行**必须降级成上下文，不能丢掉——
  // 否则新旧两侧行数对不上，`git apply` 会以「补丁不一致」拒绝。
  const FileDiff file = st::text::make_file_diff("keep\nDROP\ntail\n", "keep\ntail\nADD\n");
  ST_REQUIRE(!file.hunks.empty());
  const auto& hunk = file.hunks.front();
  std::optional<std::size_t> added_index;
  for (std::size_t i = 0; i < hunk.lines.size(); ++i) {
    if (hunk.lines[i].kind == st::text::DiffLineKind::Added) added_index = i;
  }
  ST_REQUIRE(added_index.has_value());
  const std::string patch = st::text::build_partial_patch(hunk, {*added_index});
  // `DROP` 必须作为**上下文**（前缀是空格）出现，而不是 `-DROP`。
  ST_CHECK(patch.find("\n DROP\n") != std::string::npos);
  ST_CHECK(patch.find("\n-DROP\n") == std::string::npos);
}

ST_TEST(partial_patch_empty_selection_produces_nothing) {
  const FileDiff file = st::text::make_file_diff("a\n", "b\n");
  ST_REQUIRE(!file.hunks.empty());
  ST_CHECK(st::text::build_partial_patch(file.hunks.front(), {}).empty());
  // 只选上下文行也拼不出有效补丁（没有任何改动可应用）。
  const FileDiff same = st::text::make_file_diff("a\nb\n", "a\nB\n");
  ST_REQUIRE(!same.hunks.empty());
  std::vector<std::size_t> context_only;
  for (std::size_t i = 0; i < same.hunks.front().lines.size(); ++i) {
    if (same.hunks.front().lines[i].kind == st::text::DiffLineKind::Context) {
      context_only.push_back(i);
    }
  }
  ST_CHECK(!context_only.empty());
  ST_CHECK(st::text::build_partial_patch(same.hunks.front(), context_only).empty());
}

ST_TEST(partial_patch_whole_file_equals_full_patch) {
  // 全选时应当与 `to_unified` 的 hunk 片段**等价**（同样的头与正文）——
  // 这条判据挡住“部分补丁的头上行号算错了”这类错（全选时两者必须一致）。
  const FileDiff file = st::text::make_file_diff("a\nb\nc\n", "a\nB\nc\n");
  ST_REQUIRE(!file.hunks.empty());
  const auto& hunk = file.hunks.front();
  std::vector<std::size_t> all;
  for (std::size_t i = 0; i < hunk.lines.size(); ++i) all.push_back(i);
  const std::string partial = st::text::build_partial_patch(hunk, all);
  const std::string full = st::text::to_unified(hunk);
  ST_CHECK_EQ(partial, full);
}
