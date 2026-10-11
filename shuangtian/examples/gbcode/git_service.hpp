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

// ————————————————————————————————————————————————————————————————————————————
// 任意两端对比（参照歌白文件工作台的端点模型）
// ————————————————————————————————————————————————————————————————————————————
//
// ## 端点语义表（**唯一权威**：任何取内容/取清单的路径都必须经 `diff_args` 推导）
//
// | from | to | 命令 | 含义 |
// |---|---|---|---|
// | 空/WORKTREE | 空/WORKTREE | `git diff` | 暂存区 ↔ 工作区（未暂存改动） |
// | 空/WORKTREE | INDEX | `git diff --cached` | HEAD ↔ 暂存区（已暂存） |
// | 空 | REF | `git diff REF` | REF ↔ 工作区 |
// | INDEX | REF | `git diff --cached REF` | REF ↔ 暂存区 |
// | REF1 | REF2 | `git diff REF1 REF2` | 任意两端（反向对比即交换参数） |
// | REF1 | REF2 + merge_base | `git diff REF1...REF2` | 三点：共同祖先起算（分支对比的正确语义） |
//
// **为什么必须只有这一个推导函数**（歌白实测过的坑，代价是一批静默错的结果）：
// “取文件清单”与“取文件内容”两条路若各写一套端点解释，把 `from=INDEX&to=WORKTREE`
// 喂给比较接口会被解释成 `--cached`（即 HEAD↔暂存区）——**不报错、只是返回另一批文件**。
// 本文件里两者共用 `diff_args()`，因此不可能分叉。

/// 端点保留字。
enum class DiffEnd : std::uint8_t {
  Worktree,   ///< 工作区（磁盘上的当前内容）
  Index,      ///< 暂存区（已 `git add` 的内容）
  Empty,      ///< 未指定（按上表与另一端组合决定）
  Revision,   ///< 任意 rev（分支名 / 标签 / 短 hash / `HEAD~3` / `main@{1}` / `A^`）
};

/// 解析后的一个端点。
struct DiffEndpoint {
  DiffEnd kind{DiffEnd::Empty};
  std::string revision{};   ///< `kind == Revision` 时的原文（透传给 git，不做规范化）

  /// 解析：大小写不敏感地识别保留字，其余均为 rev（**不校验合法性**：
  /// git 会拒，而我们不做第二套语法——两套判据必然漂移）。
  [[nodiscard]] static auto parse(std::string_view text) -> DiffEndpoint;
  /// 回写文本（保留字原名，rev 原样）。
  [[nodiscard]] auto to_text() const -> std::string;
  [[nodiscard]] auto is_worktree() const noexcept -> bool { return kind == DiffEnd::Worktree; }
  [[nodiscard]] auto is_index() const noexcept -> bool { return kind == DiffEnd::Index; }
  [[nodiscard]] auto empty() const noexcept -> bool { return kind == DiffEnd::Empty; }
};

/// 空树（git 的固定对象 id）——**根提交的 `<rev>^` 归一目标**。
///
/// 根提交没有父提交，照惯例写 `HEAD^` 会让 git 报错；而“跟一个不存在的父比较”
/// 在语义上就是“跟空树比较”。歌白那边的实测代价：不归一时上层把 422 当“无差异”
/// 吞掉，于是跨文件的导航按钮凭空消失。
inline constexpr std::string_view kEmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

/// 比较选项。
struct CompareOptions {
  DiffEndpoint from{};
  DiffEndpoint to{};
  /// 三点对比：`from...to`（按**共同祖先**起算）。分支对比的正确默认语义。
  bool merge_base{false};
  /// 路径过滤器（git pathspec；空 = 整仓）。
  std::string path{};
  /// 空白忽略（`-w`）。
  bool ignore_whitespace{false};
  /// 上下文行数（夹取到 0..20；默认 3，与 git 一致）。
  std::size_t context{3};
  /// 只取名称与状态（`--name-status` 的开关位；清单内部用得上）。
  bool name_only{false};
  /// `--numstat` 允许的最大行数（超出则**只给路径不给计数**，并在结果里如实标记；
  /// `0` = 不限）。歌白那边的口径：截掉的文件仍要有真实的 +N/−N（否则 UI 显示 `+0/−0`
  /// 比不显示更糟），因此默认给一个较宽的上限而不是截断正文。
  std::size_t max_files{400};
};

/// 一个变更文件（比较结果的条目）。
struct ComparedFile {
  std::string path{};
  std::string old_path{};      ///< 重命名/复制的原路径（非重命名时为空）
  char status{'M'};            ///< `M`/`A`/`D`/`R`/`C`/`T`（`--name-status` 的首字母）
  std::size_t additions{0};    ///< `+N`；二进制为 0（**不等于“没有变化”**）
  std::size_t deletions{0};
  bool binary{false};          ///< 二进制（无逐行内容）
};

/// 一次比较的结果。
struct CompareResult {
  std::vector<ComparedFile> files{};
  std::size_t additions{0};
  std::size_t deletions{0};
  std::string from_text{};     ///< 回写用（规范化后的端点文本）
  std::string to_text{};
  std::string merge_base_hash{};   ///< 三点对比时的共同祖先（短 hash；否则空）
  std::string error{};             ///< 非空 = 本次比较失败
  bool ok{false};
};

/// 一侧的文件内容（并列差异的两侧文本）。
struct ContentAt {
  std::string text{};
  bool ok{false};
  bool missing{false};    ///< 该端点下没有这个文件（新增/删除的另一侧）
  bool binary{false};
  std::size_t size{0};
  std::string error{};
};

/// 把端点组合翻译成 `git diff` 的**子参数**（**不含** `diff` 子命令本身与全局选项）。
/// 不支持的组合（如 `INDEX → INDEX`）返回 `nullopt`——调用方据此**如实报错**，
/// 而不是自己编一个命令或当成“无差异”。
[[nodiscard]] auto diff_args(const DiffEndpoint& from, const DiffEndpoint& to, bool merge_base)
    -> std::optional<std::vector<std::string>>;

/// 一次比较（文件清单 + 增删统计 + 重命名识别）。
[[nodiscard]] auto read_compare(const std::string& cwd, const CompareOptions& options)
    -> CompareResult;

/// 读一次比较的**原始 patch 文本**（`context` 在这里才起作用）。
/// 与 `read_compare` 共用 `diff_args()`，因此两侧的端点解释不可能分叉。
[[nodiscard]] auto read_diff_text(const std::string& cwd, const CompareOptions& options)
    -> std::string;

/// 读一个端点在历史里的**文件内容**（并列差异的“另一侧”）。
///
/// `end` 为 `Worktree` 时读磁盘；`Index` 读暂存区（`git show :0:<path>`）；
/// `Revision` 读该提交（`git show <rev>:<path>`）。
/// **文件在那一侧不存在不是错误**（新增/删除就是这种情况）：`ok=false` + `missing=true`。
[[nodiscard]] auto read_content_at(const std::string& cwd, const DiffEndpoint& end,
                                   const std::string& path) -> ContentAt;

/// 该提交是不是**根提交**（无父）。`rev` 不可解析时返回 false。
[[nodiscard]] auto is_root_commit(const std::string& cwd, const std::string& rev) -> bool;

}  // namespace gbcode
