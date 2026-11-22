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

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "git_graph.hpp"

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
  /// 展示用的“文件名”（不含目录）——两段式列表的上行。
  ///
  /// 为何拆出文件名与目录两个：IDEA 的变更列表是**两行**——上行文件名（亮色）、
  /// 下行目录 + 状态字母（灰）。拼成一行时目录会把文件名挤到中间，
  /// 扫列表时反而找不到“我改的是哪个文件”。
  [[nodiscard]] auto file_name() const -> std::string {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  /// 展示用的“目录部分”（末尾不带 `/`；根目录下的文件为空）——两段式列表的下行。
  [[nodiscard]] auto dir_path() const -> std::string {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
  }

  /// 展示用的路径（完整相对路径，**不截断**）。
  ///
  /// 为何不在这里截：可用宽度只有布局那一步知道（侧栏宽窄、有无行内动作钮），
  /// 而这里硬编一个字符上限必然二者对不上——实测就是“文件名被截成
  /// `shuangtian_tools.tes...`，而右侧还有一大片空白”。
  /// `Button` 已经按**实际可用宽度**走 `TextPort::ellipsize`（保尾部、中间省略），
  /// 那才是唯一对的截断点（它知道字宽，也知道这里到底剩多少像素）。
  [[nodiscard]] auto display_path() const -> const std::string& { return path; }

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

  /// **预分组的下标**（`read_snapshot` 解析时就填好；键是 `ChangeGroup` 的枚举值）。
  ///
  /// ## 为何要预分组（性能，与 IDEA 的 Changelist 同一思路）
  ///
  /// 旧实现里 `changes_in(group)` 每次都**拷贝一份 `GitChange` 向量**——
  /// 而它被侧栏每帧调 4 次（四个分组）+ `count_in` 又各调一次（又是 4 次全量拷贝）。
  /// 实测 2000 个变更的仓库、60 fps 下这是每帧 8 × 2000 次字符串拷贝 = **近百万字节/帧**，
  /// 全部只为“把列表画出来”。分组是**快照的属性**（数据不变分组就不变），
  /// 应该算一次。现在只存下标（`uint32_t`），读时零拷贝。
  ///
  /// ⚠ 一条变更可能同时出现在两个组（既暂存又改：暂存版 + 未暂存版），
  /// 所以这里是**四个独立的下标表**，不是“每条归一类”。
  std::array<std::vector<std::uint32_t>, 4> group_index{};

  /// 按组取条目（**拷贝一份**；只在“要自己持有这批条目”时用）。
  [[nodiscard]] auto changes_in(ChangeGroup group) const -> std::vector<GitChange>;
  /// 按组取下标（**零拷贝**；界面逐行遍历用这个）。
  [[nodiscard]] auto indices_in(ChangeGroup group) const -> std::span<const std::uint32_t>;
  /// 组内条数（O(1)——不拷贝、不遍历）。
  [[nodiscard]] auto count_in(ChangeGroup group) const -> std::size_t;

  /// 重建分组下标（`changes` 被外部改过之后必须调；`read_snapshot` 已自动调）。
  void regroup();
};

/// 读一份状态快照（同步、毫秒级）。`cwd` 是工作区目录。
[[nodiscard]] auto read_snapshot(const std::string& cwd) -> RepoSnapshot;

/// 日志条目（一条提交；展示用的字段）。
struct LogEntry {
  std::string hash{};          ///< 完整 hash（操作用）；`short_hash` 给显示
  std::string short_hash{};
  std::string subject{};       ///< 首行
  /// **完整提交信息**（含标题与正文，`%B`）。读详情时才填（列表不用）。
  ///
  /// 为何不直接用 `subject`：提交说明常常是多段（变更动机、影响面、关联 issue），
  /// 只显示首行就等于把“为什么改”丢了——而详情栏里的 Markdown 渲染就是为了它。
  std::string message{};
  std::string author{};
  std::string date{};          ///< 原样（`%ad`，格式 `--date=format:` 已定）
  /// refs（HEAD/分支/标签），已解析成逐个胶囊。
  std::vector<std::string> refs{};
  /// 该提交的增删统计（`--numstat` 段；读详情时才填，列表不填）。
  std::vector<std::tuple<std::string, std::size_t, std::size_t>> files{};

  /// 相等比较：`State<std::vector<LogEntry>>` 需要它（`State` 用相等性做变更检测，
  /// 否则每次 `set` 都会当成“变了”而触发重组）。
  ///
  /// 只比**身份与展示字段**：`files` 是“读详情时才填”的重量级字段，
  /// 把它拉进来会让比较退化成逐条统计表对账。
  auto operator==(const LogEntry& other) const noexcept -> bool {
    return hash == other.hash && short_hash == other.short_hash && subject == other.subject &&
           author == other.author && date == other.date && refs == other.refs;
  }
};

/// 读日志（`limit` 条；从 `ref` 起，空 = HEAD）。
///
/// 只返回**提交**（不包含任何“行”概念）——供“按提交维度”做的逻辑（详情/选中）用。
/// 泳道与边由 `git_graph.*` 的 `read_commit_graph` 单独提供。
///
/// ⚠ **日志与拓扑同时要时用 `read_log_and_graph`**：它一条命令就给出两者，
/// 而分别调本函数与 `read_commit_graph` 会让同一段历史会被读两遍
/// （实测本仓 200 条：一条 97 ms / 两条 174 ms，纯浪费）。
[[nodiscard]] auto read_log(const std::string& cwd, std::size_t limit,
                           const std::string& ref = {}) -> std::vector<LogEntry>;

/// 日志 + 拓扑图的**一次读**（日志表与泳道图共用一条子进程）。
///
/// ## 为何能合（不是“看着一样就用”）
///
/// `git log --topo-order --pretty=…%P` 与 `git rev-list --parents --topo-order`
/// 在**同一范围**上的输出是同一套拓扑遍历：提交顺序一致，`%P` 与 rev-list 的
/// 第二列同源（都是父提交的完整 hash）。实测本仓 200 条逐条比对，hash 序与
/// 父列表**完全一致**。
///
/// 这条等价性由 `tests/git_graph_test.cpp` 的
/// `git_graph_merged_log_matches_rev_list` 钉住——将来 git 若改了 `%P`
/// 或 `--topo-order` 的语义，那条用例会立刻变红。
struct LogAndGraph {
  std::vector<LogEntry> log{};
  CommitGraph graph{};
};
[[nodiscard]] auto read_log_and_graph(const std::string& cwd, std::size_t limit,
                                      const std::string& ref = {}) -> LogAndGraph;

/// 读一个提交的详情（变更文件 + numstat；写入 `entry.files` 与 `entry.message`，返回 patch 文本）。
///
/// 只起**一条**子进程（`show --numstat -p --format=%B%x00`）：提交信息、逐文件计数、
/// patch 正文三者一次拿齐（详见实现里的分段解析注释）。旧实现是三条。
[[nodiscard]] auto read_commit_detail(const std::string& cwd, LogEntry& entry) -> std::string;

/// 读分支列表（本地 + 远程；含当前标记与上游跟踪）——**单读版本的便捷入口**；
/// 同时要标签/暂存/远程时用 `read_ref_snapshot`（一条命令族就给出全部）。
struct BranchInfo {
  std::string name{};         ///< 完整名（`origin/main` 含远程前缀）
  bool is_remote{false};
  bool is_current{false};
  std::string upstream_short{};   ///< 跟踪的远程分支短名（本地分支才有）
};

[[nodiscard]] auto read_branches(const std::string& cwd) -> std::vector<BranchInfo>;

/// 远程分支所属的 remote 名（`origin/feature/x` → `origin`；本地分支 → 空串）。
///
/// 按**第一个** `/` 切：远程名本身不含 `/`，而分支名可以含（`feature/x`）。
[[nodiscard]] auto remote_of_branch(const BranchInfo& branch) -> std::string;

/// 分支的**短名**（去掉 remote 前缀）：`origin/feature/x` → `feature/x`。
///
/// 展示与搜索都用它：用户脑子里想的是 "feature"，不是 "origin/feature"。
[[nodiscard]] auto short_branch_name(const BranchInfo& branch) -> std::string_view;

/// 某分支（从它还可达的）提交集合：分支全名 → 它的 hash 列表（近 `limit` 条）。
///
/// ## 为何需要它（日志筛选）
///
/// “只看某个分支的提交”不能靠**泳道号**推断——泳道号是绘制层的分配结果，
/// 只有当那个分支恰好是 HEAD 那条时才可靠。用户点的是任意一条分支，
/// 正确的判据是 `git rev-list <ref>`：**从该引用向前可达的提交集**。
///
/// 返回空集有两种情况（调用方需区分）：引用不存在 / git 执行失败。
[[nodiscard]] auto read_branch_commits(const std::string& cwd, std::string_view ref,
                                       std::size_t limit) -> std::vector<std::string>;

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
///
/// ⚠ `cwd` 是**仓库根**而不是“用户打开的工作区目录”：git 命令会自己向上找仓库，
/// 但 `Worktree` 分支是**直接拼磁盘路径**的，它不会。两者不同时（工作区是仓库的
/// 子目录）拼出来的路径会多套一层——表现是“文件读不到” ⇒ 被判成**删除文件**，
/// 右栏只剩对齐用的空填充行（实测：gbcode 工作区是 `gebai/shuangtian`、
/// 仓库根是 `gebai`，点开 page.hpp 显示“（删除文件）+0 −2132”、右侧全空白）。
/// 调用方取仓库根用 `read_repo_root()`。
[[nodiscard]] auto read_content_at(const std::string& cwd, const DiffEndpoint& end,
                                   const std::string& path) -> ContentAt;

/// 仓库根目录（`git rev-parse --show-toplevel`）。
///
/// 为何需要它：git 输出的所有路径（`status --porcelain`、`--numstat`…）都相对**仓库根**，
/// 而用户打开的“工作区”可以是仓库里的任意子目录。两者不等时，任何“按路径读盘”的操作
/// 都会拼错（见 `read_content_at` 的说明）。
///
/// 非仓库 / git 不可用时返回空串（调用方退回传入目录——那是唯一能给的最佳猜测）。
[[nodiscard]] auto read_repo_root(const std::string& cwd) -> std::string;

/// 仓库的两个地址（仓库根 + `.git` 目录位置）——**一条 `rev-parse` 同时给出**。
///
/// ## 为何两者要分开
///
/// 用户打开的工作区可以是仓库的**子目录**（`gebai/shuangtian` 之于仓库 `gebai`），
/// 而 `.git` 只在仓库根下。旧 `read_operation_state` 直接探 `<cwd>/.git/…`——
/// 工作区是子目录时就**永远探不到**（merge 中也不会显示“继续/中止”入口，
/// 而那是冲突时的唯一出路）。顺带也少一条子进程（批量路径原本要两条 `rev-parse`）。
struct GitDirs {
  std::string root{};      ///< 仓库根（`--show-toplevel`）
  std::string git_dir{};   ///< `.git` 真实位置（worktree/submodule 下是文件）
};
[[nodiscard]] auto read_git_dirs(const std::string& cwd) -> GitDirs;

/// **冲突文件的三份内容**（`merge`/`rebase` 撞冲突后的“我方 / 共同祖先 / 他方”）。
///
/// 三份都在**暂存区**里，靠序号区分（这也是 `git show :N:path` 里 `N` 的含义）：
///
/// | N | 内容 | 字段 |
/// |---|---|---|
/// | `:1:` | **共同祖先**（base） | `base` |
/// | `:2:` | **我方**（ours，当前分支） | `ours` |
/// | `:3:` | **他方**（theirs，被合入的分支） | `theirs` |
///
/// ⚠ 三个序号**只在冲突时同时存在**。非冲突文件只有 `:0:`；而“删除/新增冲突”
/// 会**缺某一号**（如 `modify/delete` 冲突缺 `:2:` 或 `:3:`）。所以本接口
/// **逐个取、各自报存在与否**，而不是“要么三份都有、要么失败”——
/// 后者会把“删除/新增冲突”整类情况误报成取数失败。
struct ConflictSides {
  ContentAt base{};     ///< `:1:` 共同祖先（**可能是空的**：两侧都在同一位置新增）
  ContentAt ours{};     ///< `:2:` 我方
  ContentAt theirs{};   ///< `:3:` 他方
  /// 至少取到一份内容（否则多半不是冲突文件、或路径写错）。
  [[nodiscard]] auto any() const noexcept -> bool {
    return base.ok || ours.ok || theirs.ok;
  }
};
[[nodiscard]] auto read_conflict_sides(const std::string& cwd, const std::string& path)
    -> ConflictSides;

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

/// 读**全部引用**（分支 / 标签 / 暂存 / 远程）的**一次读**（一条命令族）。
///
/// ## 为何合成一条（性能，2026-11-22）
///
/// 旧实现是“每个视图各一条命令”：`branch --all`（实测本仓 50 ms）、
/// `for-each-ref refs/tags`（63 ms）、`for-each-ref refs/{heads,remotes}`（65 ms），
/// 加 `git remote` + 每个远程两条 `get-url` + `stash list`：五个视图共 9 条。
/// 而它们读的是**同一批 ref**——`for-each-ref` 一个命令就能把三者需要的字段一并给出
/// （`%(HEAD)` / `%(upstream:short)` / `%(symref)` / `%(objecttype)` /
/// `%(objectname:short)` / `%(contents:subject)` / `%(creatordate:unix)`），
/// 远程 URL 在配置里（`config --get-regexp`，一条拿全部）。
///
/// ⚠ 三个视图的**分类判据不变**（那是正确性问题，不是性能问题），只是数据源合一：
///   · 分支：丢 `%(symref)` 非空的（remote 的 HEAD 别名）；远程看 `refs/remotes/` 前缀；
///   · 标签：`%(objecttype)` 为 `tag` 的是注解标签，其 `objectname` 是标签对象
///     而非提交——短 hash 仍用它（与旧实现同口径）；结果按 `creatordate` 倒序；
///   · 引用清单：本地 / 远程（丢 `<remote>/HEAD` 别名）/ 标签三类。
///
/// 上游（`upstream`）**不在这里取**：它得看当前分支，而 `RepoSnapshot` 那条
/// `status -b` 已经给了（`## main...origin/main`）；而分支条目里的
/// `upstream_short` 就是当前分支的上游————需要单独取时用 `read_upstream`。
struct RefSnapshot {
  std::vector<BranchInfo> branches{};
  std::vector<TagInfo> tags{};
  std::vector<RefInfo> refs{};
  std::vector<StashEntry> stashes{};
  std::vector<RemoteInfo> remotes{};
  std::string error{};   ///< 非空 = 引用族读取失败（各视图一起退化成空，而不是各行其是）
};

/// `read_ref_parts` 要哪几路（位图）。
///
/// ## 为何要它（实测回归）
///
/// `read_ref_snapshot` 是**全族**读；而 `read_branches`/`read_tags`/… 这些
/// **单读入口**只想要其中一路。若单读也跑全族，它们会从“一条命令”（50 ms）
/// 变成“三条”（152 ms）——这个回归确实发生过（`tools/git_service_bench.cpp` 量到
/// `read_branches` 50 → 152 ms）。位图让两条路**共用同一份解析代码**，
/// 而各自只付自己需要的成本。
enum class RefPart : unsigned {
  Refs = 1U << 0,      ///< `for-each-ref`（分支 / 标签 / 引用清单三者的共同数据源）
  Stashes = 1U << 1,
  Remotes = 1U << 2,
};

/// 位图拼合（`RefPart::Refs | RefPart::Remotes`）。
[[nodiscard]] constexpr auto operator|(RefPart left, RefPart right) -> RefPart {
  return static_cast<RefPart>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

[[nodiscard]] auto read_ref_snapshot(const std::string& cwd) -> RefSnapshot;
/// 只读指定的那几路（单读入口用它；`read_ref_snapshot` = 全部三路）。
[[nodiscard]] auto read_ref_parts(const std::string& cwd, RefPart parts) -> RefSnapshot;

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

/// 同上，但**直接给 `.git` 位置**（批量路径已取过 `read_git_dirs` 时用）。
///
/// 为何需要它：这个函数的全部判据都是 `<git_dir>/<状态文件>` 的**存在性**
/// （文件系统调用，µs 级），与仓库根无关——把 `.git` 位置作为参数就能零子进程，
/// 而批量路径已经拿到那份值了。
[[nodiscard]] auto read_operation_state_at(const std::string& git_dir, const std::string& cwd)
    -> OperationState;

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
    bool annotated{false};   ///< 建标签时用注解标签
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
///
/// 实现**只读文件尾部**（审计只追加，而界面只要最后 N 条）：旧实现整读 + 全文扫。
[[nodiscard]] auto read_audit(const std::string& cwd, std::size_t limit = 100)
    -> std::vector<std::string>;

/// 同上，但**直接给 `.git` 位置**（批量路径已取过 `read_git_dirs` 时用，省一条 `rev-parse`）。
[[nodiscard]] auto read_audit_at(const std::string& git_dir, std::size_t limit)
    -> std::vector<std::string>;

/// 一批「回顾面」数据的快照（日志/分支/标签/远程/暂存/引用/上游/操作态/审计）。
///
/// 为何要一个结构：旧实现是 9 条 git 子进程串行，实测在真实仓库上要 **~3.3 秒**。
/// 放在主线程同步做会让界面“出来了但点不动”。拆成「后台算数据 → 主线程落地」时，
/// 后台不能直接写页面的成员与 `State`（重组单线程，并发写是数据竞争），
/// 所以用这份纯数据整包过手。
///
/// ## 条数（2026-11-22 优化后的契约）
///
/// 现在整批只起 **5 条子进程**（旧 12 条），且互不依赖的几条**并发**跑：
///   · `log_and_graph`：`git log --pretty=…%P`（同时给出日志与拓扑）
///   · `ref_snapshot`：`for-each-ref refs/heads refs/remotes refs/tags` +
///     `stash list` + `config --get-regexp remote.*`（其中前两条并发）
///   · `operation`：`.git` 下状态文件探测（只在不处于多步操作时零子进程）
///   · `audit`：`rev-parse --absolute-git-dir` + 读文件
/// 并发宽度取 4（它跑在应用的后台线程里，而那条线程本来就是一个任务；
/// 再宽会把 git 自己的 index 锁竞争放大，实测没有收益）。
struct LogSnapshot {
  std::vector<LogEntry> log;
  /// **拓扑图**（节点 + 边，从 `rev-list --parents` 自算）——列表绘制的唯一真值源。
  CommitGraph graph;
  std::vector<BranchInfo> branches;
  std::vector<TagInfo> tags;
  std::vector<RemoteInfo> remotes;
  std::vector<StashEntry> stashes;
  std::vector<RefInfo> refs;
  std::string upstream;
  OperationState operation;
  std::vector<std::string> audit;
};

/// 一次读完以上全部（同步、阻塞；调用方负责别在主线程调）。
[[nodiscard]] auto read_log_snapshot(const std::string& cwd) -> LogSnapshot;
}  // namespace gbcode
