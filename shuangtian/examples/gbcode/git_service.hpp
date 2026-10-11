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
  /// 展示用的路径（含目录段，便于用户区分同名文件）。
  /// 超长路径截断中段（保留尾部的文件名 + 前两级目录）——变更列表窄，
  /// 完整路径放不下，而“最尾部”才是用户在对账的部分。
  [[nodiscard]] auto display_path() const -> std::string {
    constexpr std::size_t kMax = 46;
    if (path.size() <= kMax) return path;
    const std::size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string dir = slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
    if (name.size() + 2 >= kMax) return "…/" + name;
    const std::size_t keep = kMax - name.size() - 2;
    return "…" + dir.substr(dir.size() > keep ? dir.size() - keep : 0) + name;
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

  /// 相等比较：`State<std::vector<LogEntry>>` 需要它（`State` 用相等性做变更检测，
  /// 否则每次 `set` 都会当成“变了”而触发重组）。
  ///
  /// 只比**身份与展示字段**（hash/subject/author/date）：`files` 是“读详情时才填”的
  /// 重量级字段，把它拉进来会让比较退化成逐条统计表对账。
  auto operator==(const LogEntry& other) const noexcept -> bool {
    return hash == other.hash && short_hash == other.short_hash && subject == other.subject &&
           author == other.author && date == other.date && lane == other.lane &&
           refs == other.refs;
  }
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

// ————————————————————————————————————————————————————————————————————————————
// 读面：标签 / 远程 / 暂存（stash）/ 引用清单 / 单文件历史 / blame / 多步操作
// （对标歌白工作台的五大视图：变更、日志、分支、标签、暂存、远程）
// ————————————————————————————————————————————————————————————————————————————

/// 一个标签。
struct TagInfo {
  std::string name{};
  std::string target{};    ///< 指向的提交短 hash（注解标签已解引用到提交）
  std::string subject{};   ///< 注解标签的消息首行 / 轻量标签的提交 subject
  bool annotated{false};
};
[[nodiscard]] auto read_tags(const std::string& cwd) -> std::vector<TagInfo>;

/// 一个远程（名字 + 拉/推 URL）。
struct RemoteInfo {
  std::string name{};
  std::string fetch_url{};
  std::string push_url{};
};
[[nodiscard]] auto read_remotes(const std::string& cwd) -> std::vector<RemoteInfo>;

/// 一条 stash 记录。
struct StashEntry {
  std::string index{};      ///< `stash@{0}`
  std::string branch{};     ///< 当时所在分支
  std::string message{};
};
[[nodiscard]] auto read_stashes(const std::string& cwd) -> std::vector<StashEntry>;

/// 引用清单（标签列表与「日志范围」选择器共用）。
struct RefInfo {
  std::string name{};       ///< 短名（`main` / `origin/main` / `v1.0`）
  std::string hash{};
  /// `head` / `local` / `remote` / `tag`。
  std::string kind{};
};
[[nodiscard]] auto read_refs(const std::string& cwd) -> std::vector<RefInfo>;

/// 单文件历史（`--follow`，跨重命名）。
[[nodiscard]] auto read_file_history(const std::string& cwd, const std::string& path,
                                     std::size_t limit = 100) -> std::vector<LogEntry>;

/// 一段 blame（仅取指定行范围，供编辑器的行尾注解用）。
struct BlameLine {
  std::size_t line{0};        ///< 1 起（文件当前内容里的行号）
  std::string hash{};         ///< 短 hash
  std::string author{};
  std::string date{};         ///< `YYYY-MM-DD`
  std::string content{};      ///< 该行内容（不含换行）
};
/// `start` / `count` 都从 1 起；`count = 0` 表示到文件末。
[[nodiscard]] auto read_blame(const std::string& cwd, const std::string& rev,
                              const std::string& path, std::size_t start = 1,
                              std::size_t count = 0) -> std::vector<BlameLine>;

/// 仓库当前是否处于某个**多步操作**中（merge / rebase / cherry-pick / revert / bisect）。
///
/// 为什么要它：处于这种状态时，界面必须给出「继续 / 跳过 / 中止」入口——
/// 不给的话用户只能去命令行擦屁股（而冲突恰恰是 merge/rebase 的常态）。
struct OperationState {
  std::string kind{};   ///< 空 = 不在多步操作中；否则 `merge`/`rebase`/`cherry-pick`/`revert`/`bisect`
  /// 冲突文件（`git diff --name-only --diff-filter=U`）。
  std::vector<std::string> conflicts{};
  [[nodiscard]] auto active() const noexcept -> bool { return !kind.empty(); }
};
[[nodiscard]] auto read_operation_state(const std::string& cwd) -> OperationState;

/// 当前分支的上游（`origin/main`；无上游返回空）——推送/拉取按钮据此决定参数。
[[nodiscard]] auto read_upstream(const std::string& cwd) -> std::string;

// ————————————————————————————————————————————————————————————————————————————
// 写面：命令**构造** + 备份分支 + 多步控制
// ————————————————————————————————————————————————————————————————————————————
//
// ## 为何这里只“构造命令”而不执行
//
// 写操作在 gbcode 里一律交给**终端作业通道**（`Terminal::run`）：输出实时回流、
// 可中止、用户能看到 git 到底干什么（歌白服务端是同步跑完再返回——Web 形态没有终端）。
// 因此本文件只负责「把一次操作翻译成**一条可读的命令**」，不亲自跑。
// 好处是命令与界面所见**同一份事实**：用户在终端里看到的与我们在做的一致。
//
// ## 为什么不拼字符串
//
// 参数里可能出现空格、中文、引号（路径/提交信息）。拼字符串就得引号转义，
// 而转义写错的表现是“命令看着对、实际拉错了文件”（或反过来注入了 `;`）。
// 这里给出的是**参数数组**（`std::vector<std::string>`），由调用方逐参传递；
// `shell_quote` 只在“必须交给 shell 的交互式终端”那一边用，且是显式的一步。

/// 写操作种类（决定参数构造、危险度与是否需要备份）。
enum class WriteOp : std::uint8_t {
  Stage,          ///< `git add -- <path>`
  Unstage,        ///< `git restore --staged -- <path>`
  DiscardOne,     ///< `git restore -- <path>`（丢弃工作区改动）
  DiscardAll,     ///< `git checkout -- .`（丢弃全部未暂存）
  Commit,         ///< `git commit -m <msg>`（可 amend）
  Amend,          ///< `git commit --amend`
  Checkout,       ///< `git checkout <ref>`
  CheckoutNew,    ///< `git checkout -b <name> [<start>]`
  DeleteBranch,   ///< `git branch -d|-D <name>`
  Merge,          ///< `git merge <ref>`
  Rebase,         ///< `git rebase <upstream>`
  CherryPick,     ///< `git cherry-pick <ref>`
  Revert,         ///< `git revert --no-edit <ref>`
  ResetSoft,      ///< `git reset --soft <ref>`
  ResetMixed,     ///< `git reset <ref>`
  ResetHard,      ///< `git reset --hard <ref>`（**不可逆**，必带备份）
  TagCreate,      ///< `git tag [-a] <name> [-m <msg>]`
  TagDelete,      ///< `git tag -d <name>`
  StashPush,      ///< `git stash push [-m <msg>]`
  StashPop,       ///< `git stash pop <index>`
  StashApply,     ///< `git stash apply <index>`
  StashDrop,      ///< `git stash drop <index>`
  Fetch,          ///< `git fetch --all --prune`
  Pull,           ///< `git pull --ff-only`（默认不制造意外 merge）
  Push,           ///< `git push`（无上游则 `-u origin <branch>`）
  PushForceWithLease,  ///< `git push --force-with-lease`（**不用 `--force`**：无 lease 的强推会覆盖别人的提交）
  // —— 多步操作控制 ——
  Continue,       ///< `git <op> --continue`
  Skip,           ///< `git <op> --skip`
  Abort,          ///< `git <op> --abort`
  /// **部分暂存**：`git apply --cached -`（补丁走 **stdin**）。
  ///
  /// 为何单独一个 op 而不是复用 Stage：补丁不能走 argv——
  /// ① 它可能有几十 KB（超出命令行长度限制）；
  /// ② 路径以 `-` 开头时会被当成选项。
  /// 所以这一项构造的是「到 `--` 为止的命令」，补丁本体由 `run_write` 从 stdin 喂。
  StagePatch,
  /// **部分取消暂存**：`git apply --cached --reverse -`（同样走 stdin）。
  ///
  /// 它删的是**暂存区**的改动（不碰工作区），所以危险度是 `None`——
  /// 取消暂存随时可以再暂存回去。
  UnstagePatch,
};

/// 一次写操作的意图（参数全在这里；空参数按各 op 的默认语义处理）。
struct WriteIntent {
  WriteOp op{WriteOp::Stage};
  std::string path{};        ///< 单文件操作的目标（相对仓库根，`/` 分隔）
  std::string ref{};         ///< 分支/标签/提交（checkout/merge/rebase/cherry-pick/reset/tag 起点）
  std::string name{};        ///< 新分支名 / 新标签名
  std::string message{};     ///< 提交信息 / 标签注解 / stash 消息
  std::string author{};      ///< 覆盖提交作者（`Name <email>`；空 = 用仓库配置）
  bool amend{false};         ///< 提交时改写 HEAD（危险：会改写已推送的历史）
  bool allow_empty{false};   ///< 允许空提交
  bool hard{false};          ///< `git branch -D` / `git tag -d` 之外的强制位
  bool annotated{false};     ///< 建标签时用注解标签
  /// `StagePatch`/`UnstagePatch` 的**补丁本体**（走 stdin，见那两个 op 的说明）。
  ///
  /// 为何不放在 `path` 里或另加一个路径字段：它是**内容**而不是路径，
  /// 且可能几十 KB——混进路径字段会让“这是文件还是补丁”在调用处看不出来。
  std::string patch{};
};

/// 构造写操作的**命令行**（不含 `git` 本身，第一个元素是子命令）。
///
/// 返回 `nullopt` = 这个意图不合法（缺必填参数）——调用方**如实拒绝**，
/// 而不是构造一条会删错东西的命令。
[[nodiscard]] auto write_args(const WriteIntent& intent) -> std::optional<std::vector<std::string>>;

/// 该操作是否需要**先建备份分支**（不可逆且会改工作区/历史）。
[[nodiscard]] auto needs_backup(const WriteIntent& intent) -> bool;

/// 该操作的危险度（界面据此决定“要不要弹确认”）。
enum class Danger : std::uint8_t {
  None,      ///< 安全（暂存/提交/推送等可重来或只前进）
  Confirm,   ///< 需二次确认（amend/删除已合并分支/强制推送）
  Backup,    ///< 不可逆：**必须先建备份分支**再执行（reset --hard / 丢弃改动 / rebase 改写历史）
};
[[nodiscard]] auto danger_of(const WriteIntent& intent) -> Danger;

/// 生成备份分支名（`backup/<日期>-<短 hash>`）。
///
/// 为何用“日期 + hash”而不是时间戳：名字要能被人**读懂并手动回滚**
/// （`git reset --hard backup/20261011-a1b2c3d`），而一长串秒数做不到。
[[nodiscard]] auto backup_branch_name(const std::string& cwd) -> std::string;

/// 把单个参数包成 shell 安全的形式（仅当必须交给 shell 时用）。
/// 规则：安全的字符集内原样；否则用单引号包裹并把内部的 `'` 写成 `'\''`。
[[nodiscard]] auto shell_quote(std::string_view argument) -> std::string;

/// 把命令数组拼成一行可读命令（终端里执行用；每参经 `shell_quote`）。
[[nodiscard]] auto to_shell_line(const std::vector<std::string>& args) -> std::string;

/// 把命令数组拼成**人类可读的展示形式**（日志/审计/状态栏用；不保证可执行）。
[[nodiscard]] auto to_display_line(const std::vector<std::string>& args) -> std::string;

/// 多步操作的控制子命令（`--continue` / `--skip` / `--abort`）。
///
/// `kind` 取 `OperationState::kind` 的值（`merge`/`rebase`/`cherry-pick`/`revert`/`am`）。
[[nodiscard]] auto control_args(std::string_view kind, std::string_view action)
    -> std::optional<std::vector<std::string>>;

// —— 审计 ——

/// 记一条写操作审计（JSONL，落在 `<cwd>/.git/st-audit.jsonl`）。
///
/// 为何落在 `.git` 里：它是**仓库级**的事实，跟着仓库走（换机器、换工作副本都还在），
/// 且默认不会进版本库（`.git` 不被跟踪）。不落盘的应用目录会在换设备时丢失。
///
/// 失败不抛也不报错（审计不能成为操作失败的原因）——但**返回是否写入成功**，
/// 让调用方能在必要时如实告诉用户“审计没记上”。
[[nodiscard]] auto append_audit(const std::string& cwd, std::string_view action,
                                const std::vector<std::string>& args, std::string_view result)
    -> bool;

/// 读审计尾部（最多 `limit` 条，最旧在前）——界面「操作历史」用。
[[nodiscard]] auto read_audit(const std::string& cwd, std::size_t limit = 100)
    -> std::vector<std::string>;

}  // namespace gbcode
