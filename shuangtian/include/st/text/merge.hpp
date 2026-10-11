#pragma once

/// **三方合并**（`base` / `ours` / `theirs`）：把三份文本切成可逐块取舍的区段。
///
/// ## 为什么在 `st::text` 层而不是 git 层
///
/// 一眼看它是"Git 的冲突"，但按内容分它只做三件事：比较三份文本、切成区段、
/// 按选择合成结果——**与 git 无关**（配置合并、多份翻译稿合并、模板 diff 都同形）。
/// 放在这里能被纯文本用例完整覆盖，也让 git 之外的使用者不必绕过去。
/// 「哪三份」「冲突怎么产生」才是 git 层的事。
///
/// ## 与行级 diff 的关系
///
/// 复用 `st::text` 的行级 LCS（同一套 `backtrack`），所以「行级说没改、合并说改了」
/// 这类矛盾不会出现。三方合并不是三次两两 diff 的拼接——那会在
/// 「两侧都改了同一处」时给出错的区段（见下面的 `Conflict`）。
///
/// ## 区段模型
///
/// 把 `<ours>` 与 `<theirs>` 相对于 `<base>` 的改动对齐后，每段落在三态之一：
///
/// | 区段 | 含义 | 合成时 |
/// |---|---|---|
/// | `Unchanged` | 三方一致 | 原样保留 |
/// | `OursOnly` | 只有 ours 改了 | 取 ours |
/// | `TheirsOnly` | 只有 theirs 改了 | 取 theirs |
/// | `BothSame` | 两侧改成**同一内容** | 取该内容（**不是冲突**——这点最容易做错：把它当冲突会让"两边都加了同一行"白报一次冲突） |
/// | `Conflict` | 两侧改成**不同内容** | **需人来选** |
///
/// ## 行号口径
///
/// 每个区段的 `base_start`/`ours_start`/`theirs_start` 都**1 起**、`0` = 该侧此段不存在
/// （纯新增）。这与 `DiffLine` 的 `old_line`/`new_line` 同一套口径——**不要**让调用方
/// 自己从下标推行号（那是"纯删除段在改侧没有行号"这类语义最容易出错的地方）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::text {

/// 一段合并区段的三方状态。
enum class MergeKind : std::uint8_t {
  Unchanged,   ///< 三方一致
  OursOnly,    ///< 只有 ours 改了
  TheirsOnly,  ///< 只有 theirs 改了
  BothSame,    ///< 两侧改成同一内容（可选任一，结果等价）
  Conflict,    ///< 两侧改成不同内容——**必须**由人来选
};

/// 一段区段。
struct MergeSegment {
  MergeKind kind{MergeKind::Unchanged};
  /// 各位的起始行号（1 起；`0` = 该侧没有这一段的对应内容）。
  std::size_t base_start{0};
  std::size_t ours_start{0};
  std::size_t theirs_start{0};
  /// 三侧各自的行（不含换行符）。`Conflict` 时 `base`/`ours`/`theirs` 都有内容；
  /// `OursOnly` 时只有 `ours` 非空（`theirs` 与 `base` 一致）。
  std::vector<std::string> base{};
  std::vector<std::string> ours{};
  std::vector<std::string> theirs{};
  /// 该段在**合成结果**里是否保留了原样的两侧（供 UI 判断"这一段要不要画三栏"）。
  [[nodiscard]] auto conflicted() const noexcept -> bool { return kind == MergeKind::Conflict; }
};

/// 合并计划：全部区段 + 统计。
struct MergePlan {
  std::vector<MergeSegment> segments{};
  std::size_t conflicts{0};   ///< `Conflict` 区段数（UI 用它显示「还有 N 处冲突」）
  /// 自动可解的区段数（`Unchanged`/`OursOnly`/`TheirsOnly`/`BothSame`）。
  [[nodiscard]] auto resolved() const noexcept -> std::size_t {
    return segments.size() - conflicts;
  }
  [[nodiscard]] auto has_conflicts() const noexcept -> bool { return conflicts > 0; }
};

/// 计算三方合并计划。
///
/// 规模上限与行级 diff 同姿态：超过阈值退化为**整文件冲突**一段（如实反映"差异太大"），
/// 而不是静默给一个错的切分。
[[nodiscard]] auto merge_three_way(std::string_view base, std::string_view ours,
                                   std::string_view theirs) -> MergePlan;

/// 按选择合成最终文本。
///
/// `choices` 与 `plan.segments` **同长**：每项是 `0`（取 ours）或 `1`（取 theirs）。
/// `Conflict` 段必须给出选择；非冲突段的取值被忽略（它们只有一种正确结果）。
/// 缺省（`{}`）时冲突段**取 ours**——即「保留我方」，这是"先合上、待会儿再回头改"
/// 的常见操作，但调用方应当先问用户（本函数不替它决定）。
///
/// `eol` 决定每行末尾用什么；缺省 `"\n"`。**末行换行**与输入的最后一段一致
/// （`base`/`ours`/`theirs` 里最后一个非空的那份是否以换行结尾）——这条不保持的话，
/// 「合并后多/少一个换行」会成为提交里一个说不清来源的改动。
[[nodiscard]] auto merge_apply(const MergePlan& plan, const std::vector<int>& choices = {},
                               std::string_view eol = "\n") -> std::string;

/// 便捷：直接合成（冲突全取 ours）。等价 `merge_apply(merge_three_way(...))`。
[[nodiscard]] auto merge_prefer_ours(std::string_view base, std::string_view ours,
                                     std::string_view theirs) -> std::string;

}  // namespace st::text
