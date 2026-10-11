/// `st::text` 三方合并测试。
///
/// 三方合并最容易错的**不是**"能不能合"，而是四类"看着对、其实错"的情形：
/// ① 两侧改**不同文件位置**（不该报冲突）；② 两侧改**同一处但内容相同**
/// （不该报冲突——报了让人在两份一样的内容里选）；③ 一侧改、另一侧没动
/// （该自动取改动方）；④ **相邻**改动（两侧各改一行且紧挨着，最容易切错区间）。

#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/text/merge.hpp"

namespace {

using st::text::MergeKind;
using st::text::MergePlan;
using st::text::MergeSegment;

/// 找第一段指定类型的区段（`nullptr` = 没有）。
[[nodiscard]] auto first_of(const MergePlan& plan, MergeKind kind) -> const MergeSegment* {
  for (const auto& segment : plan.segments) {
    if (segment.kind == kind) return &segment;
  }
  return nullptr;
}

/// 各类型区段数。
[[nodiscard]] auto count_of(const MergePlan& plan, MergeKind kind) -> std::size_t {
  std::size_t total = 0;
  for (const auto& segment : plan.segments) {
    if (segment.kind == kind) ++total;
  }
  return total;
}

}  // namespace

ST_TEST(merge_three_way_unchanged_when_all_equal) {
  const MergePlan plan = st::text::merge_three_way("a\nb\nc\n", "a\nb\nc\n", "a\nb\nc\n");
  ST_CHECK(!plan.has_conflicts());
  ST_CHECK_EQ(plan.conflicts, std::size_t{0});
  ST_CHECK_EQ(count_of(plan, MergeKind::Unchanged), std::size_t{1});
  ST_CHECK_EQ(st::text::merge_apply(plan), std::string("a\nb\nc\n"));
  // 行号 1 起，三侧一致。
  const auto* segment = first_of(plan, MergeKind::Unchanged);
  ST_REQUIRE(segment != nullptr);
  ST_CHECK_EQ(segment->base_start, std::size_t{1});
  ST_CHECK_EQ(segment->ours_start, std::size_t{1});
  ST_CHECK_EQ(segment->theirs_start, std::size_t{1});
}

ST_TEST(merge_three_way_sides_change_different_places) {
  // ① 两侧改不同位置：**不该**报冲突，且合成结果两边都要有。
  const std::string base = "one\ntwo\nthree\nfour\nfive\n";
  const std::string ours = "one\nTWO\nthree\nfour\nfive\n";
  const std::string theirs = "one\ntwo\nthree\nfour\nFIVE\n";
  const MergePlan plan = st::text::merge_three_way(base, ours, theirs);
  ST_CHECK_EQ(plan.conflicts, std::size_t{0});
  ST_CHECK_EQ(count_of(plan, MergeKind::OursOnly), std::size_t{1});
  ST_CHECK_EQ(count_of(plan, MergeKind::TheirsOnly), std::size_t{1});
  const std::string merged = st::text::merge_apply(plan);
  ST_CHECK(merged.find("TWO") != std::string::npos);
  ST_CHECK(merged.find("FIVE") != std::string::npos);
  ST_CHECK(merged.find("two") == std::string::npos);
  ST_CHECK(merged.find("five") == std::string::npos);
}

ST_TEST(merge_three_way_same_change_on_both_sides_is_not_a_conflict) {
  // ② 两侧改成同一内容：**不是冲突**。当冲突处理会让用户在两份一样的内容里选，
  // 而"随便点一个"的结果完全相同——那纯属噪音。
  const std::string base = "alpha\nbeta\ngamma\n";
  const std::string same = "alpha\nBETA\ngamma\n";
  const MergePlan plan = st::text::merge_three_way(base, same, same);
  ST_CHECK_EQ(plan.conflicts, std::size_t{0});
  ST_CHECK_EQ(count_of(plan, MergeKind::BothSame), std::size_t{1});
  ST_CHECK_EQ(st::text::merge_apply(plan), std::string("alpha\nBETA\ngamma\n"));

  // 两侧**都给同一处插入同一行**也一样。
  const MergePlan insert_same =
      st::text::merge_three_way("x\n", "x\nnew\n", "x\nnew\n");
  ST_CHECK_EQ(insert_same.conflicts, std::size_t{0});
  ST_CHECK_EQ(st::text::merge_apply(insert_same), std::string("x\nnew\n"));
}

ST_TEST(merge_three_way_one_side_only_is_taken_automatically) {
  // ③ 一侧改、另一侧没动：自动取改动方。
  const MergePlan ours_only =
      st::text::merge_three_way("a\nb\n", "a\nB\n", "a\nb\n");
  ST_CHECK_EQ(ours_only.conflicts, std::size_t{0});
  ST_CHECK_EQ(count_of(ours_only, MergeKind::OursOnly), std::size_t{1});
  ST_CHECK_EQ(st::text::merge_apply(ours_only), std::string("a\nB\n"));

  const MergePlan theirs_only =
      st::text::merge_three_way("a\nb\n", "a\nb\n", "a\nB\n");
  ST_CHECK_EQ(theirs_only.conflicts, std::size_t{0});
  ST_CHECK_EQ(count_of(theirs_only, MergeKind::TheirsOnly), std::size_t{1});
  ST_CHECK_EQ(st::text::merge_apply(theirs_only), std::string("a\nB\n"));
}

ST_TEST(merge_three_way_real_conflict_needs_a_choice) {
  // 两侧改同一处成**不同**内容：这才是冲突。
  const std::string base = "a\ntarget\nc\n";
  const std::string ours = "a\nOURS\nc\n";
  const std::string theirs = "a\nTHEIRS\nc\n";
  const MergePlan plan = st::text::merge_three_way(base, ours, theirs);
  ST_CHECK_EQ(plan.conflicts, std::size_t{1});
  ST_CHECK(plan.has_conflicts());
  const auto* conflict = first_of(plan, MergeKind::Conflict);
  ST_REQUIRE(conflict != nullptr);
  ST_CHECK_EQ(conflict->base.size(), std::size_t{1});
  ST_CHECK_EQ(conflict->base[0], std::string("target"));
  ST_CHECK_EQ(conflict->ours[0], std::string("OURS"));
  ST_CHECK_EQ(conflict->theirs[0], std::string("THEIRS"));
  ST_CHECK(conflict->conflicted());

  // 缺省取 ours（"保留我方"）——但真正要用的时候应当由用户选。
  ST_CHECK_EQ(st::text::merge_apply(plan), std::string("a\nOURS\nc\n"));

  // 显式选 theirs：`choices` 与区段同长。
  std::vector<int> choices(plan.segments.size(), 0);
  for (std::size_t index = 0; index < plan.segments.size(); ++index) {
    if (plan.segments[index].kind == MergeKind::Conflict) choices[index] = 1;
  }
  ST_CHECK_EQ(st::text::merge_apply(plan, choices), std::string("a\nTHEIRS\nc\n"));
}

ST_TEST(merge_three_way_adjacent_edits_do_not_overlap) {
  // ④ 相邻改动：两侧各改紧挨着的两行。区间切错的话这里会出现重复行或漏行。
  const std::string base = "a\nb\nc\nd\n";
  const std::string ours = "a\nB\nc\nd\n";      // 改第 2 行
  const std::string theirs = "a\nb\nC\nd\n";    // 改第 3 行（紧挨着）
  const MergePlan plan = st::text::merge_three_way(base, ours, theirs);
  ST_CHECK_EQ(plan.conflicts, std::size_t{0});
  const std::string merged = st::text::merge_apply(plan);
  ST_CHECK_EQ(merged, std::string("a\nB\nC\nd\n"));
}

ST_TEST(merge_three_way_handles_pure_insert_and_delete) {
  // 纯插入（`base_end == base_start`）与纯删除（`replacement` 空）都要正确。
  // 插入：两侧在不同位置各插一行。
  const MergePlan insert =
      st::text::merge_three_way("x\ny\n", "x\nA\ny\n", "x\ny\nB\n");
  ST_CHECK_EQ(insert.conflicts, std::size_t{0});
  ST_CHECK_EQ(st::text::merge_apply(insert), std::string("x\nA\ny\nB\n"));

  // 删除：ours 删一行，theirs 没动 → 自动取 ours（该行没了）。
  const MergePlan remove = st::text::merge_three_way("x\nDROP\ny\n", "x\ny\n", "x\nDROP\ny\n");
  ST_CHECK_EQ(remove.conflicts, std::size_t{0});
  ST_CHECK_EQ(count_of(remove, MergeKind::OursOnly), std::size_t{1});
  ST_CHECK_EQ(st::text::merge_apply(remove), std::string("x\ny\n"));

  // 两侧删**不同的行**：都该被删掉。
  const MergePlan remove_both =
      st::text::merge_three_way("a\nb\nc\n", "b\nc\n", "a\nb\n");
  ST_CHECK_EQ(remove_both.conflicts, std::size_t{0});
  ST_CHECK_EQ(st::text::merge_apply(remove_both), std::string("b\n"));
}

ST_TEST(merge_three_way_stats_and_resolved_count) {
  const std::string base = "1\n2\n3\n4\n";
  const std::string ours = "ONE\n2\n3\n4\n";        // ours 改第 1 行
  const std::string theirs = "1\n2\nTHREE\n4\n";    // theirs 改第 3 行
  const MergePlan plan = st::text::merge_three_way(base, ours, theirs);
  ST_CHECK_EQ(plan.conflicts, std::size_t{0});
  // 全部区段都可自动解（`resolved()` = 总数 - 冲突数）。
  ST_CHECK_EQ(plan.resolved(), plan.segments.size());
  ST_CHECK(plan.segments.size() >= 3U);   // 未变 + oursOnly + 未变 + theirsOnly + 未变
}

ST_TEST(merge_three_way_empty_inputs) {
  // 空 base（新文件）：两侧各写了不同内容 → 冲突；只有一侧写 → 自动取它。
  const MergePlan both =
      st::text::merge_three_way("", "ours\n", "theirs\n");
  ST_CHECK_EQ(both.conflicts, std::size_t{1});

  const MergePlan one = st::text::merge_three_way("", "only\n", "");
  ST_CHECK_EQ(one.conflicts, std::size_t{0});
  ST_CHECK_EQ(st::text::merge_apply(one), std::string("only\n"));

  // 全空：没有任何区段，合成空串（不崩）。
  const MergePlan empty = st::text::merge_three_way("", "", "");
  ST_CHECK(empty.segments.empty());
  ST_CHECK_EQ(st::text::merge_apply(empty), std::string(""));

  // 一侧全删（theirs 空、ours 没动）→ 自动取 theirs（内容没了）。
  const MergePlan deleted = st::text::merge_three_way("a\nb\n", "a\nb\n", "");
  ST_CHECK_EQ(deleted.conflicts, std::size_t{0});
  ST_CHECK_EQ(st::text::merge_apply(deleted), std::string(""));
}
