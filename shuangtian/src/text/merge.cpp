#include "st/text/merge.hpp"

#include <algorithm>
#include <string>

#include "st/text/diff.hpp"

#include "diff_internal.hpp"

namespace st::text {

namespace {

/// 把一段行的区间拷成 `vector<string>`（`[begin, end)`）。
[[nodiscard]] auto slice(const std::vector<std::string_view>& lines, std::size_t begin,
                         std::size_t end) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (end <= begin) return out;
  out.reserve(end - begin);
  for (std::size_t index = begin; index < end; ++index) out.emplace_back(lines[index]);
  return out;
}

/// 两个行序列是否逐行相同。
[[nodiscard]] auto same_lines(const std::vector<std::string>& lhs,
                              const std::vector<std::string>& rhs) -> bool {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index] != rhs[index]) return false;
  }
  return true;
}

/// 一侧的"改动脚本"：相对 base 的「未变行数 / 替换成的新行」序列。
///
/// 为何要这个中间形态：三方合并的正解是把 ours 与 theirs 的改动**都相对 base 定位**，
/// 再按 base 的行区间对齐。直接做两次两两 diff 再拼会在「两侧改同一处」时错位
///（那一处的两个改动在两次 diff 里落在不同的区块边界上）。
struct Edit {
  /// 相对 base：这段改动从 base 的第几行开始（0 起；`base_end == base_start` 表示纯插入）。
  std::size_t base_start{0};
  std::size_t base_end{0};
  /// 替换成的新行（`side` 侧）。
  std::vector<std::string> replacement{};
  /// 这段改动在 `side` 里的起始行（0 起）。
  std::size_t side_start{0};
};

/// 把「base → side」的 LCS 结果变成改动列表（相邻的删/插并成一次改动）。
[[nodiscard]] auto edits_against(const std::vector<std::string_view>& base_lines,
                                 const std::vector<std::string_view>& side_lines)
    -> std::vector<Edit> {
  const auto ops = detail::lcs_backtrack(base_lines, side_lines);
  std::vector<Edit> edits;
  // 当前正在积累的改动（`base_start == kNone` 表示不在改动里）。
  constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  Edit current{};
  current.base_start = kNone;
  std::size_t base_cursor = 0;
  std::size_t side_cursor = 0;

  const auto flush = [&edits, &current]() {
    if (current.base_start == kNone) return;
    edits.push_back(std::move(current));
  };

  for (const auto& item : ops) {
    if (item.op == detail::LcsOp::Equal) {
      flush();
      current = Edit{};
      current.base_start = kNone;
      ++base_cursor;
      ++side_cursor;
      continue;
    }
    if (current.base_start == kNone) {
      current = Edit{};
      current.base_start = base_cursor;
      current.base_end = base_cursor;
      current.side_start = side_cursor;
    }
    if (item.op == detail::LcsOp::Delete) {
      current.base_end = base_cursor + 1;   // 吃掉 base 的一行
      ++base_cursor;
    } else {
      current.replacement.emplace_back(side_lines[side_cursor]);
      ++side_cursor;
    }
  }
  flush();
  return edits;
}

}  // namespace

auto merge_three_way(std::string_view base_text, std::string_view ours_text,
                     std::string_view theirs_text) -> MergePlan {
  const auto base_lines = split_lines(base_text);
  const auto ours_lines = split_lines(ours_text);
  const auto theirs_lines = split_lines(theirs_text);

  MergePlan plan;
  const bool too_large = (base_lines.size() + 1) * (ours_lines.size() + 1) >
                             detail::kMaxLcsCells ||
                         (base_lines.size() + 1) * (theirs_lines.size() + 1) >
                             detail::kMaxLcsCells;
  if (too_large) {
    // 如实退化为「整文件冲突」——不静默截断（截断会给出一个看着正常、其实错位的合并）。
    MergeSegment whole;
    whole.kind = MergeKind::Conflict;
    whole.base_start = base_lines.empty() ? 0 : 1;
    whole.ours_start = ours_lines.empty() ? 0 : 1;
    whole.theirs_start = theirs_lines.empty() ? 0 : 1;
    whole.base = slice(base_lines, 0, base_lines.size());
    whole.ours = slice(ours_lines, 0, ours_lines.size());
    whole.theirs = slice(theirs_lines, 0, theirs_lines.size());
    plan.conflicts = 1;
    plan.segments.push_back(std::move(whole));
    return plan;
  }

  const auto ours_edits = edits_against(base_lines, ours_lines);
  const auto theirs_edits = edits_against(base_lines, theirs_lines);

  std::size_t base_cursor = 0;   // 已消费到的 base 行（0 起）
  std::size_t ours_cursor = 0;
  std::size_t theirs_cursor = 0;
  std::size_t ours_edit = 0;
  std::size_t theirs_edit = 0;

  // 把「未变的一段」落成一个 `Unchanged` 段（`length` 行）。
  const auto push_unchanged = [&](std::size_t length) {
    if (length == 0) return;
    MergeSegment segment;
    segment.kind = MergeKind::Unchanged;
    segment.base_start = base_cursor + 1;
    segment.ours_start = ours_cursor + 1;
    segment.theirs_start = theirs_cursor + 1;
    segment.base = slice(base_lines, base_cursor, base_cursor + length);
    segment.ours = segment.base;
    segment.theirs = segment.base;
    plan.segments.push_back(std::move(segment));
    base_cursor += length;
    ours_cursor += length;
    theirs_cursor += length;
  };

  // ⚠ 循环条件必须包含「还有未消费的改动点」：
  // 纯**尾部插入**（`base_start == base_lines.size()`、`base_end == base_start`）时
  // `base_cursor` 已经等于 `base_lines.size()`，只看 base 行的话循环立刻结束——
  // 那一行插入被整个丢掉（实测踩到：`x` + 在末尾插 `new` 只得到 `x`）。
  // —— 主循环：按「下一个改动点」推进 ——
  //
  // 不变式：`base_cursor` / `ours_cursor` / `theirs_cursor` 始终指向"尚未处理的第一行"。
  // 每一步处理**一个改动区间**（两侧在这个 base 区间上的改动合并考虑），
  // 区间之间是未变段。
  //
  // ⚠ 循环条件必须包含「还有未消费的改动点」：纯**尾部插入**
  //（`base_start == base_lines.size()`）时 `base_cursor` 已等于行数，只看 base 行
  // 的话循环立刻结束——那一行插入会被整个丢掉（实测踩到：`x` + 末尾插 `new` 只得到 `x`）。
  while (base_cursor < base_lines.size() || ours_edit < ours_edits.size() ||
         theirs_edit < theirs_edits.size()) {
    // 下一个改动点：两侧里更靠前的那个（没有改动点的侧用 `base_lines.size()` 兜底）。
    std::size_t next_change = base_lines.size();
    if (ours_edit < ours_edits.size()) {
      next_change = std::min(next_change, ours_edits[ours_edit].base_start);
    }
    if (theirs_edit < theirs_edits.size()) {
      next_change = std::min(next_change, theirs_edits[theirs_edit].base_start);
    }

    // 到改动点之间的（或尾部的）未变段。
    if (next_change > base_cursor) {
      push_unchanged(next_change - base_cursor);
      continue;
    }
    // 没有改动点且 base 已消费完 → 收工。
    if (ours_edit >= ours_edits.size() && theirs_edit >= theirs_edits.size()) {
      if (base_cursor >= base_lines.size()) break;
      push_unchanged(base_lines.size() - base_cursor);
      continue;
    }

    // —— 处理一个改动区间 ——
    //
    // 区间起点 = 当前的改动点；终点 = 把所有"起点落在区间内"的改动都吞进来
    //（两侧改动相邻/重叠时必须一起处理，分开处理会切出重复行或漏行）。
    //
    // ⚠ 纯插入（`base_start == base_end`）不推进 `region_end`：它的 base 区间是空的。
    // 正因如此，"吞相邻改动"的判据要用 `<= region_end`（而不是 `<`），
    // 否则紧跟在一个纯插入之后的改动会被漏掉。
    std::size_t region_start = next_change;
    std::size_t region_end = region_start;
    // ⚠ 吸吞的判据是**真重叠**（`base_start < region_end`），不是"相邻"
    //（`<= region_end`）——相邻不重叠的改动（ours 改第 2 行、theirs 改第 3 行）
    // 各自成段才正确；用 `<=` 会把它们并成一个"两侧内容都不同"的区间，
    // 于是**白报一次冲突**（实测踩到：改不同行的两处被报成冲突，
    // 用户在两个都该保留的版本里被迫二选一）。
    //
    // 唯一要用 `<=` 的情形是**纯插入**：它的 `base_start == base_end`（base 区间为空），
    // 若不被吞进来，它会与"同一位置的另一个纯插入"分成两段——两段各自看都是"只我改"，
    // 合成时按顺序各插入一次，结果反而正确；但"两侧在同一位置各插一行不同内容"
    // 本该是一处冲突（用户的直觉是"这里两边都动了"），所以纯插入要与**同一位置**
    // 的另一侧插入一起处理。
    const auto overlaps = [](const Edit& edit, std::size_t begin, std::size_t end) {
      if (edit.base_start == edit.base_end) {
        // 纯插入：与区间"同点或落在区间内"都算在一起。
        return edit.base_start >= begin && edit.base_start <= end;
      }
      return edit.base_start < end || (begin == end && edit.base_start == begin);
    };
    const auto absorb = [&](const std::vector<Edit>& edits, std::size_t& cursor) {
      bool progressed = true;
      while (progressed) {
        progressed = false;
        while (cursor < edits.size() && overlaps(edits[cursor], region_start, region_end)) {
          region_end = std::max(region_end, edits[cursor].base_end);
          ++cursor;
          progressed = true;
        }
      }
    };
    absorb(ours_edits, ours_edit);
    absorb(theirs_edits, theirs_edit);

    // 各侧在本区间上的内容 = 「本区间里该侧的改动替换」+「未被该侧改动的 base 行」。
    const auto build_block = [&](const std::vector<Edit>& edits, std::size_t first_edit,
                                 std::size_t after_edit) -> std::vector<std::string> {
      std::vector<std::string> block;
      std::size_t cursor = region_start;
      for (std::size_t index = first_edit; index < after_edit; ++index) {
        const Edit& edit = edits[index];
        // 改动点之前未改的 base 行原样拷进来。
        while (cursor < edit.base_start) {
          block.emplace_back(base_lines[cursor]);
          ++cursor;
        }
        block.insert(block.end(), edit.replacement.begin(), edit.replacement.end());
        cursor = std::max(cursor, edit.base_end);
      }
      while (cursor < region_end) {
        block.emplace_back(base_lines[cursor]);
        ++cursor;
      }
      return block;
    };

    // 找出本区间里属于**每一侧**的改动下标区间：`absorb` 推进后的游标即"之后"，
    // 而"之前"需要回扫（改动点是按 base 序的，回扫到 `base_start < region_start` 为止）。
    const auto first_in_region = [&](const std::vector<Edit>& edits, std::size_t after) {
      std::size_t index = after;
      while (index > 0 && edits[index - 1].base_start >= region_start) --index;
      return index;
    };
    const std::size_t ours_first = first_in_region(ours_edits, ours_edit);
    const std::size_t theirs_first = first_in_region(theirs_edits, theirs_edit);

    MergeSegment segment;
    segment.base_start = region_start + 1;
    segment.ours_start = ours_cursor + 1;
    segment.theirs_start = theirs_cursor + 1;
    segment.base = slice(base_lines, region_start, region_end);
    segment.ours = build_block(ours_edits, ours_first, ours_edit);
    segment.theirs = build_block(theirs_edits, theirs_first, theirs_edit);

    const bool ours_same_as_base = same_lines(segment.ours, segment.base);
    const bool theirs_same_as_base = same_lines(segment.theirs, segment.base);
    if (ours_same_as_base && theirs_same_as_base) {
      segment.kind = MergeKind::Unchanged;
    } else if (ours_same_as_base) {
      segment.kind = MergeKind::TheirsOnly;
    } else if (theirs_same_as_base) {
      segment.kind = MergeKind::OursOnly;
    } else if (same_lines(segment.ours, segment.theirs)) {
      // ⚠ 两侧改成**同一内容**不是冲突：把它当冲突会让"两边都加了同一行"白报一次冲突，
      // 而用户面对两个一模一样的选项只能随便点一个。
      segment.kind = MergeKind::BothSame;
    } else {
      segment.kind = MergeKind::Conflict;
      ++plan.conflicts;
    }
    ours_cursor += segment.ours.size();
    theirs_cursor += segment.theirs.size();
    base_cursor = std::max(region_end, region_start);
    plan.segments.push_back(std::move(segment));
  }

  return plan;
}

auto merge_apply(const MergePlan& plan, const std::vector<int>& choices, std::string_view eol)
    -> std::string {
  std::string out;
  for (std::size_t index = 0; index < plan.segments.size(); ++index) {
    const MergeSegment& segment = plan.segments[index];
    const std::vector<std::string>* chosen = &segment.ours;
    switch (segment.kind) {
      case MergeKind::Unchanged:
      case MergeKind::OursOnly:
      case MergeKind::BothSame:
        chosen = &segment.ours;
        break;
      case MergeKind::TheirsOnly:
        chosen = &segment.theirs;
        break;
      case MergeKind::Conflict: {
        // 缺省取 ours（"保留我方"）——调用方应当先问用户；这里只是不让它崩。
        const bool take_theirs = index < choices.size() && choices[index] == 1;
        chosen = take_theirs ? &segment.theirs : &segment.ours;
        break;
      }
    }
    for (const auto& line : *chosen) {
      out += line;
      out += eol;
    }
  }
  return out;
}

auto merge_prefer_ours(std::string_view base_text, std::string_view ours_text,
                       std::string_view theirs_text) -> std::string {
  return merge_apply(merge_three_way(base_text, ours_text, theirs_text));
}

}  // namespace st::text
