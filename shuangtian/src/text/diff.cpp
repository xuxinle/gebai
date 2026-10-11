#include "st/text/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::text {

namespace {

/// 行索引：切好的行 + 「末尾有没有换行」。
///
/// 末尾换行单独记，是因为它**只在线行集合上体现不出来**：
/// `"a\n"` 与 `"a"` 切出来的行都是 `{"a"}`，而它们在 git 里是两个不同的 blob
/// （`\ No newline at end of file`）。差异视图若只比行内容，这类改动会**整篇消失**。
struct TextLines {
  std::vector<std::string_view> lines{};
  bool trailing_newline{true};
};

[[nodiscard]] auto index_lines(std::string_view text) -> TextLines {
  TextLines out;
  out.trailing_newline = text.empty() || text.back() == '\n';
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::size_t end = eol == std::string_view::npos ? text.size() : eol;
    std::string_view line = text.substr(begin, end - begin);
    // 行尾 `\r` 不算内容：CRLF 工作区与 LF 仓库比出来的「整篇都改了」是假的。
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    out.lines.push_back(line);
    if (eol == std::string_view::npos) break;
    begin = eol + 1;
  }
  return out;
}

/// 差异操作（LCS 回溯的产物）。
enum class Op : std::uint8_t { Equal, Delete, Insert };

struct OpItem {
  Op op{Op::Equal};
  std::size_t old_index{0};
  std::size_t new_index{0};
};

/// LCS 回溯：把两段行序列变成「相等 / 删除 / 插入」的操作序列。
///
/// 贪心策略（相等则取、否则取 LCS 值大的那一侧）保证结果**稳定且确定**：
/// 同一对输入永远给同一份差异，不随容器实现或遍历顺序变化——
/// 这是「并列视图与导出的补丁必须一致」的前提。
[[nodiscard]] auto backtrack(const std::vector<std::string_view>& old_lines,
                             const std::vector<std::string_view>& new_lines) -> std::vector<OpItem> {
  const std::size_t n = old_lines.size();
  const std::size_t m = new_lines.size();
  std::vector<OpItem> ops;
  ops.reserve(n + m);
  const std::size_t cols = m + 1;
  std::vector<std::uint32_t> dp((n + 1) * cols, 0);
  const auto cell = [&dp, cols](std::size_t i, std::size_t j) -> std::uint32_t& {
    return dp[i * cols + j];
  };
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      if (old_lines[i] == new_lines[j]) {
        cell(i, j) = cell(i + 1, j + 1) + 1;
      } else {
        cell(i, j) = std::max(cell(i + 1, j), cell(i, j + 1));
      }
    }
  }
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n && j < m) {
    if (old_lines[i] == new_lines[j]) {
      ops.push_back(OpItem{.op = Op::Equal, .old_index = i, .new_index = j});
      ++i;
      ++j;
    } else if (cell(i + 1, j) >= cell(i, j + 1)) {
      ops.push_back(OpItem{.op = Op::Delete, .old_index = i, .new_index = j});
      ++i;
    } else {
      ops.push_back(OpItem{.op = Op::Insert, .old_index = i, .new_index = j});
      ++j;
    }
  }
  while (i < n) ops.push_back(OpItem{.op = Op::Delete, .old_index = i++, .new_index = j});
  while (j < m) ops.push_back(OpItem{.op = Op::Insert, .old_index = i, .new_index = j++});
  return ops;
}

/// 给「本侧最后一行且本侧无末尾换行」的行打上标记。
void mark_no_newline(DiffHunk& hunk, std::size_t old_total, bool old_trailing,
                     std::size_t new_total, bool new_trailing) {
  for (DiffLine& line : hunk.lines) {
    if (!old_trailing && line.old_line != 0 && line.old_line == old_total &&
        line.kind != DiffLineKind::Added) {
      line.no_newline = true;
    }
    if (!new_trailing && line.new_line != 0 && line.new_line == new_total &&
        line.kind == DiffLineKind::Added) {
      line.no_newline = true;
    }
  }
}

/// 把一行文本切成**码位级**的 `string_view` 序列（每个元素是一个 UTF-8 码位）。
///
/// 为何按码位而不是字节：字节级 LCS 会把一个汉字切碎，区间落到半个字符上，
/// 编辑器的高亮就会错位（三字节的汉字被切成 2+1，看着就是“高亮偏了”）。
/// 切出来的 `string_view` 仍然指向原串，所以后续能用首尾指针算出**字节偏移**
/// （编辑器要的是字节偏移，不是码位下标）。
[[nodiscard]] auto split_codepoints(std::string_view text)
    -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  out.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if ((lead & 0x80U) == 0) {
      length = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
    }
    // 畸形/截断的尾巴按单字节吃掉（不能让 `length` 越界把后面一起吞了）。
    if (index + length > text.size()) length = 1;
    out.push_back(text.substr(index, length));
    index += length;
  }
  return out;
}

/// 把「连续的不相等」归并成区间（相邻/重叠的合一，零长的丢掉）。
///
/// 为何要归并：`backtrack` 给出的是一条条独立的 Delete/Insert 操作，而编辑器要的是
/// 「哪几段被改了」——每个字符一个区间会让绘制时逐字符填色（慢且会出现缝隙）。
[[nodiscard]] auto merge_ranges(std::vector<std::pair<std::size_t, std::size_t>> ranges)
    -> std::vector<std::pair<std::size_t, std::size_t>> {
  std::vector<std::pair<std::size_t, std::size_t>> out;
  for (const auto& range : ranges) {
    if (range.second <= range.first) continue;
    if (!out.empty() && range.first <= out.back().second) {
      out.back().second = std::max(out.back().second, range.second);
    } else {
      out.push_back(range);
    }
  }
  return out;
}

}  // namespace

auto split_lines(std::string_view text) -> std::vector<std::string_view> {
  return index_lines(text).lines;
}

auto count_changes(const std::vector<DiffHunk>& hunks)
    -> std::pair<std::size_t, std::size_t> {
  std::size_t additions = 0;
  std::size_t deletions = 0;
  for (const DiffHunk& hunk : hunks) {
    for (const DiffLine& line : hunk.lines) {
      if (line.kind == DiffLineKind::Added) ++additions;
      if (line.kind == DiffLineKind::Removed) ++deletions;
    }
  }
  return {additions, deletions};
}

auto build_rows(const DiffHunk& hunk, std::size_t hunk_index) -> std::vector<DiffRow> {
  std::vector<DiffRow> rows;
  rows.reserve(hunk.lines.size());
  std::size_t index = 0;
  while (index < hunk.lines.size()) {
    if (hunk.lines[index].kind == DiffLineKind::Context) {
      rows.push_back(DiffRow{.hunk = hunk_index,
                             .left = static_cast<std::ptrdiff_t>(index),
                             .right = static_cast<std::ptrdiff_t>(index)});
      ++index;
      continue;
    }
    // 一段连续的非上下文行 = 一次「改动」。里面的删除与新增**分别成列**再逐行对齐：
    // 删 3 行 / 加 1 行时右侧补两行填充，否则两侧行号会越错越远。
    const std::size_t run_begin = index;
    while (index < hunk.lines.size() && hunk.lines[index].kind != DiffLineKind::Context) ++index;
    std::vector<std::ptrdiff_t> removed;
    std::vector<std::ptrdiff_t> added;
    for (std::size_t k = run_begin; k < index; ++k) {
      if (hunk.lines[k].kind == DiffLineKind::Removed) {
        removed.push_back(static_cast<std::ptrdiff_t>(k));
      } else {
        added.push_back(static_cast<std::ptrdiff_t>(k));
      }
    }
    const std::size_t height = std::max(removed.size(), added.size());
    for (std::size_t k = 0; k < height; ++k) {
      rows.push_back(DiffRow{.hunk = hunk_index,
                            .left = k < removed.size() ? removed[k] : -1,
                            .right = k < added.size() ? added[k] : -1});
    }
  }
  return rows;
}

auto diff_hunks(std::string_view old_text, std::string_view new_text, std::size_t context)
    -> std::vector<DiffHunk> {
  const TextLines old_side = index_lines(old_text);
  const TextLines new_side = index_lines(new_text);
  const auto& old_lines = old_side.lines;
  const auto& new_lines = new_side.lines;

  // 规模上限：两侧乘积超过阈值就不再求 LCS。**不做静默截断**——
  // 退化成「整文件替换」是如实反映「这两份差得太远」，而截断会显示出一个
  // 「看起来正常但少了几行」的假差异。
  const bool too_large = (old_lines.size() + 1) * (new_lines.size() + 1) > kMaxLcsCells;
  std::vector<DiffHunk> hunks;
  if (too_large) {
    DiffHunk hunk;
    hunk.old_start = old_lines.empty() ? 0 : 1;
    hunk.old_count = old_lines.size();
    hunk.new_start = new_lines.empty() ? 0 : 1;
    hunk.new_count = new_lines.size();
    hunk.lines.reserve(old_lines.size() + new_lines.size());
    for (std::size_t i = 0; i < old_lines.size(); ++i) {
      hunk.lines.push_back(DiffLine{.kind = DiffLineKind::Removed,
                                    .text = std::string(old_lines[i]),
                                    .old_line = i + 1,
                                    .new_line = 0});
    }
    for (std::size_t j = 0; j < new_lines.size(); ++j) {
      hunk.lines.push_back(DiffLine{.kind = DiffLineKind::Added,
                                    .text = std::string(new_lines[j]),
                                    .old_line = 0,
                                    .new_line = j + 1});
    }
    mark_no_newline(hunk, old_lines.size(), old_side.trailing_newline, new_lines.size(),
                    new_side.trailing_newline);
    hunks.push_back(std::move(hunk));
    return hunks;
  }

  const std::vector<OpItem> ops = backtrack(old_lines, new_lines);
  std::vector<std::size_t> changed;
  changed.reserve(ops.size());
  for (std::size_t k = 0; k < ops.size(); ++k) {
    if (ops[k].op != Op::Equal) changed.push_back(k);
  }
  if (changed.empty()) {
    // 行内容全等：仍要看末尾换行。`"a\n"` 与 `"a"` 的行都是 `{"a"}`，
    // 而它们是不同的 blob——这类改动必须能被看见（git 也照样出 hunk）。
    if (old_side.trailing_newline == new_side.trailing_newline || old_lines.empty()) return {};
    DiffHunk hunk;
    hunk.old_start = 1;
    hunk.old_count = old_lines.size();
    hunk.new_start = 1;
    hunk.new_count = new_lines.size();
    hunk.lines.push_back(DiffLine{.kind = DiffLineKind::Removed,
                                  .text = std::string(old_lines.back()),
                                  .old_line = old_lines.size(),
                                  .new_line = 0,
                                  .no_newline = !old_side.trailing_newline});
    hunk.lines.push_back(DiffLine{.kind = DiffLineKind::Added,
                                  .text = std::string(new_lines.back()),
                                  .old_line = 0,
                                  .new_line = new_lines.size(),
                                  .no_newline = !new_side.trailing_newline});
    return {std::move(hunk)};
  }

  // —— 切 hunk ——
  //
  // 两条改动之间隔着**超过 2×context** 行相等内容时就断开（git 同口径）：
  // 上下文各留 `context` 行，两段之间不会重叠。距边界不足 `context` 时
  // 夹取到文档首/尾（所以前导上下文的条数就是 `group.first` 本身）。
  //
  // 行号直接来自**前缀和**：`before_op[k]` = 第 k 条 op 之前已经走掉的旧侧/新侧行数，
  // 于是「op k 落在哪一行」是 `before_op[k] + 1`，不必维护任何滚动游标
  // （前导上下文要回退时，滚动游标那种写法正是最容易出错的地方）。
  std::vector<std::size_t> old_before(ops.size() + 1, 0);
  std::vector<std::size_t> new_before(ops.size() + 1, 0);
  for (std::size_t k = 0; k < ops.size(); ++k) {
    old_before[k + 1] = old_before[k] + (ops[k].op != Op::Insert ? 1 : 0);
    new_before[k + 1] = new_before[k] + (ops[k].op != Op::Delete ? 1 : 0);
  }

  struct Group {
    std::size_t first{0};
    std::size_t last{0};
  };
  std::vector<Group> groups;
  {
    Group current{.first = changed.front(), .last = changed.front()};
    for (std::size_t t = 1; t < changed.size(); ++t) {
      if (changed[t] - current.last > 2 * context + 1) {
        groups.push_back(current);
        current = Group{.first = changed[t], .last = changed[t]};
      } else {
        current.last = changed[t];
      }
    }
    groups.push_back(current);
  }

  hunks.reserve(groups.size());
  for (const Group& group : groups) {
    const std::size_t first = group.first;
    // 前导上下文：从 `first` 往回取 `context` 行（不足则夹取到文档开头）。
    // 这一段里的 op **必然**全是 Equal——`first` 是分组后的第一条改动
    //（分组规则保证了同一组内不会有空洞，而组前的改动早已被上一组吃掉）。
    const std::size_t begin = first > context ? first - context : 0;
    const std::size_t last = std::min(group.last + context, ops.size() - 1);
    DiffHunk hunk;
    bool has_line = false;
    for (std::size_t k = begin; k <= last; ++k) {
      const OpItem& item = ops[k];
      DiffLine line;
      switch (item.op) {
        case Op::Equal:
          line.kind = DiffLineKind::Context;
          line.text = std::string(old_lines[item.old_index]);
          line.old_line = old_before[k] + 1;
          line.new_line = new_before[k] + 1;
          break;
        case Op::Delete:
          line.kind = DiffLineKind::Removed;
          line.text = std::string(old_lines[item.old_index]);
          line.old_line = old_before[k] + 1;
          line.new_line = 0;
          break;
        case Op::Insert:
          line.kind = DiffLineKind::Added;
          line.text = std::string(new_lines[item.new_index]);
          line.old_line = 0;
          line.new_line = new_before[k] + 1;
          break;
      }
      if (!has_line) {
        has_line = true;
        // 空侧的起始行号按 git 惯例：取**前一行**（删除块的 `+c,0` 里 c 是它左邻行）。
        const auto start_of = [](std::size_t line_no, std::size_t before) -> std::size_t {
          if (line_no != 0) return line_no;
          return before == 0 ? 0 : before - 1;
        };
        hunk.old_start = start_of(line.old_line, old_before[k]);
        hunk.new_start = start_of(line.new_line, new_before[k]);
      }
      hunk.lines.push_back(std::move(line));
    }
    if (!has_line) continue;
    for (const DiffLine& line : hunk.lines) {
      if (line.old_line != 0) ++hunk.old_count;
      if (line.new_line != 0) ++hunk.new_count;
    }
    mark_no_newline(hunk, old_lines.size(), old_side.trailing_newline, new_lines.size(),
                    new_side.trailing_newline);
    hunks.push_back(std::move(hunk));
  }
  return hunks;
}

auto make_file_diff(std::string_view old_text, std::string_view new_text, std::string old_path,
                    std::string new_path) -> FileDiff {
  FileDiff out;
  out.old_path = std::move(old_path);
  out.new_path = std::move(new_path);
  out.added = old_text.empty() && !new_text.empty();
  out.deleted = new_text.empty() && !old_text.empty();
  out.hunks = diff_hunks(old_text, new_text);
  const auto [additions, deletions] = count_changes(out.hunks);
  out.additions = additions;
  out.deletions = deletions;
  // 行内区间一并算好（默认就给）：它是“改了什么”的一部分，而不是可选项——
  // 让调用方记得单独调一次「精化」迟早会漏（而漏了只能从“视图看着粗”看出来）。
  refine_inline_diffs(out.hunks);
  return out;
}

auto diff_inline(std::string_view old_line, std::string_view new_line)
    -> std::pair<std::vector<std::pair<std::size_t, std::size_t>>,
                 std::vector<std::pair<std::size_t, std::size_t>>> {
  // 规模上限：字符级是 O(n·m) 的 DP，两行各上千字符（长行 JSON/CSS）时会很贵，
  // 而那种情况下字符级高亮本来也没什么信息量。超限就**返回空区间**（调用方退化为
  // 整行高亮）——而不是静默给一个看着对、实际错的区间。
  constexpr std::size_t kMaxInlineCells = 200'000;
  const auto old_points = split_codepoints(old_line);
  const auto new_points = split_codepoints(new_line);
  if ((old_points.size() + 1) * (new_points.size() + 1) > kMaxInlineCells) return {};
  if (old_points == new_points) return {};

  const std::vector<OpItem> ops = backtrack(old_points, new_points);
  // 区间用**字节偏移**表达（编辑器与霜天 DiagnosticMark 都是字节口径，
  // 字符列 ↔ 字节的换算责任在调用方——保持同一套口径比在这里换一次更少出错）。
  const auto base = old_line.data();
  const auto new_base = new_line.data();
  std::vector<std::pair<std::size_t, std::size_t>> old_ranges;
  std::vector<std::pair<std::size_t, std::size_t>> new_ranges;
  for (const OpItem& op : ops) {
    if (op.op == Op::Equal) continue;
    if (op.op == Op::Delete && op.old_index < old_points.size()) {
      const std::size_t begin = static_cast<std::size_t>(old_points[op.old_index].data() - base);
      old_ranges.emplace_back(begin, begin + old_points[op.old_index].size());
    } else if (op.op == Op::Insert && op.new_index < new_points.size()) {
      const std::size_t begin =
          static_cast<std::size_t>(new_points[op.new_index].data() - new_base);
      new_ranges.emplace_back(begin, begin + new_points[op.new_index].size());
    }
  }
  return {merge_ranges(std::move(old_ranges)), merge_ranges(std::move(new_ranges))};
}

void refine_inline_diffs(std::vector<DiffHunk>& hunks) {
  for (DiffHunk& hunk : hunks) {
    // 在**同一个连续改动段**内配对：段 = 一段连续的非 Context 行
    //（与 `build_rows` 的“改动段”同一口径，否则并列视图的两栏与行内区间会对不上）。
    std::size_t index = 0;
    while (index < hunk.lines.size()) {
      if (hunk.lines[index].kind == DiffLineKind::Context) {
        ++index;
        continue;
      }
      const std::size_t run_begin = index;
      while (index < hunk.lines.size() && hunk.lines[index].kind != DiffLineKind::Context) {
        ++index;
      }
      std::vector<std::size_t> removed;
      std::vector<std::size_t> added;
      for (std::size_t k = run_begin; k < index; ++k) {
        if (hunk.lines[k].kind == DiffLineKind::Removed) {
          removed.push_back(k);
        } else {
          added.push_back(k);
        }
      }
      // ⚠ 只在**等量**时逐对精化：两侧数量不等说明是“多了/少了若干行”，
      // 按顺序硬配会把「新增 3 行」误读成「这 3 行都是别行的改动」。
      if (removed.size() != added.size()) continue;
      for (std::size_t k = 0; k < removed.size(); ++k) {
        auto [old_ranges, new_ranges] =
            diff_inline(hunk.lines[removed[k]].text, hunk.lines[added[k]].text);
        hunk.lines[removed[k]].inline_old = std::move(old_ranges);
        hunk.lines[removed[k]].inline_new = {};
        hunk.lines[added[k]].inline_new = std::move(new_ranges);
        hunk.lines[added[k]].inline_old = {};
      }
    }
  }
}

}  // namespace st::text
