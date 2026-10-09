#pragma once

/// gbcode 的 Git 集成（版本管理重构，2026-10-09）。
///
/// 参照歌白文件工作台的设计（`docs/file-workbench-design.md` §4.7 +
/// `packages/web/src/files/changes.ts`/`git.ts` 的实际实现），把 git 能力
/// 分成**两块节奏不同的东西**：
///
/// - **左侧「变更」工具窗**（本文件的 `RepoSnapshot`/`ChangeGroup`）：
///   "改了什么 / 要提交什么"——编码时最频繁看的；
/// - **底部 Git 工具窗**（`LogEntry`/`parse_log`）："分支 | 日志 | 提交内容"——
///   回顾历史时才看的。
///
/// ## porcelain 状态码的语义（容易做错的地方）
///
/// `git status --porcelain=v1` 每行前两字符是 XY 矩阵：**X = 暂存区（index）
/// 相对 HEAD 的状态，Y = 工作区（worktree）相对暂存区的状态**。分组判据：
///
/// | X | Y | 含义 | 归组 |
/// |---|---|------|------|
/// | ? | ? | 未跟踪 | 未跟踪 |
/// | U | U/A/B | 冲突（merge/rebase 中） | 冲突（最优先——它挡着提交） |
/// | 空格 | M/D | 只改了工作区 | 未暂存 |
/// | M/A/D | 空格 | 已暂存、无进一步改动 | 已暂存 |
/// | M | M | 既暂存过、又改了 | **两边都出现**（暂存版 + 未暂存版） |
///
/// 「两边都出现」是工作台那边实测过的坑：只按 X 分组会丢 Y 的信息（用户改了
/// 但没再暂存的部分看不到）；只按 Y 分组会把"已暂存"的文件又出现在"未暂存"里。
/// 正确做法是一条 change 按需在两个组各出现一次（IDEA 也这么显示）。
///
/// ## 执行模型
///
/// **读**（status/log/diff 内容）同步跑（毫秒级、无副作用）；
/// **写**（add/checkout/commit…）不在这里跑——调用方把它交给终端作业通道
/// （输出实时回流、可中止；同步阻塞会把界面卡住几十秒，`run_task` 时代实测过）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gbcode {

/// 一条变更（porcelain 行的解析结果）。
struct GitChange {
  std::string path{};
  char index_status{' '};    ///< X：暂存区相对 HEAD
  char work_status{' '};     ///< Y：工作区相对暂存区
  /// 重命名/复制的原路径（`R  old -> new` 的 old；空 = 不是重命名）。
  std::string orig_path{};

  [[nodiscard]] auto is_untracked() const noexcept -> bool {
    return index_status == '?' && work_status == '?';
  }
  [[nodiscard]] auto is_conflicted() const noexcept -> bool {
    return index_status == 'U' || work_status == 'U' ||
           (index_status == 'A' && work_status == 'A') ||
           (index_status == 'D' && work_status == 'D');
  }
  /// 暂存侧有变化（X 非空且非 ?）——该出现在「已暂存」组。
  [[nodiscard]] auto has_staged_side() const noexcept -> bool {
    return index_status != ' ' && index_status != '?' && !is_conflicted();
  }
  /// 工作侧有变化（Y 非空且非 ?）——该出现在「未暂存」组。
  [[nodiscard]] auto has_unstaged_side() const noexcept -> bool {
    return work_status != ' ' && work_status != '?';
  }
  /// 展示用的单字母（IDEA 的状态字母：M/A/D/R/U/?）。
  [[nodiscard]] auto display_code() const noexcept -> char {
    if (is_untracked()) return '?';
    if (is_conflicted()) return 'U';
    if (index_status != ' ') return index_status;
    return work_status;
  }
};

/// 分组键（顺序即展示顺序：冲突最前——它挡着提交）。
enum class ChangeGroup : std::uint8_t { Conflicted, Staged, Unstaged, Untracked };

[[nodiscard]] auto change_group_name(ChangeGroup group) -> std::string_view;

/// 一份仓库状态快照（`read_snapshot` 的产物）。
struct RepoSnapshot {
  std::string branch{};
  /// 上游（`main...origin/main` 的右半；空 = 无上游）。
  std::string upstream{};
  /// 领先/落后（`[ahead 1, behind 2]`；0 = 无或未解析）。
  std::size_t ahead{0};
  std::size_t behind{0};
  std::vector<GitChange> changes{};
  std::string error{};       ///< 非空 = 本次读取失败（面板显示可重试的故障态）
  bool is_repo{false};       ///< false = 目录不是 git 仓库（与 error 分开：不是故障）

  /// 按组取条目（同一条 change 可能出现在已暂存与未暂存两组，见文件头注释）。
  [[nodiscard]] auto changes_in(ChangeGroup group) const -> std::vector<GitChange>;
  /// 组内条数。
  [[nodiscard]] auto count_in(ChangeGroup group) const -> std::size_t;
};

/// 读一份状态快照（同步、毫秒级）。`cwd` 是工作区目录。
[[nodiscard]] auto read_snapshot(const std::string& cwd) -> RepoSnapshot;

/// 日志条目（`git log --graph` 的解析结果）。
struct LogEntry {
  std::string hash{};          ///< 完整 hash（操作用）；`short_hash` 给显示
  std::string short_hash{};
  std::string subject{};       ///< 首行
  std::string author{};
  std::string date{};          ///< 原样（`%ad`，格式 `--date=format:` 已定）
  /// refs（HEAD/分支/标签），已解析成逐个胶囊。
  std::vector<std::string> refs{};
  /// 泳道图列号（0 起；`|`/`/` 线条的解析结果——绘制用）。
  std::size_t lane{0};
  /// 该提交的增删统计（`--numstat` 段；读详情时才填，列表不填）。
  std::vector<std::tuple<std::string, std::size_t, std::size_t>> files{};
};

/// 读日志（`limit` 条；从 `ref` 起，空 = HEAD）。
[[nodiscard]] auto read_log(const std::string& cwd, std::size_t limit,
                            const std::string& ref = {}) -> std::vector<LogEntry>;

/// 读一个提交的详情（变更文件 + numstat；写入 `entry.files` 并返回 diff 文本）。
[[nodiscard]] auto read_commit_detail(const std::string& cwd, LogEntry& entry) -> std::string;

/// 读分支列表（本地 + 远程；含当前标记与上游跟踪）。
struct BranchInfo {
  std::string name{};         ///< 完整名（`origin/main` 含远程前缀）
  bool is_remote{false};
  bool is_current{false};
  std::string upstream_short{};   ///< 跟踪的远程分支短名（本地分支才有）
};
[[nodiscard]] auto read_branches(const std::string& cwd) -> std::vector<BranchInfo>;

/// 读工作区文件与 HEAD 的差异文本（只读 diff；不落任何状态）。
[[nodiscard]] auto read_worktree_diff(const std::string& cwd, const std::string& path,
                                      bool staged) -> std::string;

}  // namespace gbcode
