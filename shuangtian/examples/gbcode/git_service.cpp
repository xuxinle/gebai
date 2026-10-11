#include "git_service.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <map>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"

namespace gbcode {

namespace {

/// 跑一条 git 子命令（同步）。失败返回 nullopt（stderr 进 `*error`）。
[[nodiscard]] auto git(const std::string& cwd, const std::vector<std::string>& args,
                       std::string* error = nullptr) -> std::optional<std::string> {
  // `-c core.quotepath=false`：porcelain/numstat 对非 ASCII 路径默认会加引号并
  // 八进制转义（`"\344\270\255..."`）——中文文件名全变成鬼画符。全局关掉它，
  // 我们按 UTF-8 原样收（本框架全链路 UTF-8）。
  st::process::Options options;
  options.cwd = cwd;
  std::vector<std::string> full_args{"-c", "core.quotepath=false"};
  full_args.insert(full_args.end(), args.begin(), args.end());
  const auto result = st::process::run("git", full_args, options);
  if (!result) {
    if (error != nullptr) *error = "git 不可用：" + result.error().to_string();
    return std::nullopt;
  }
  if (result->exit_code != 0) {
    if (error != nullptr) {
      *error = st::trim(result->stderr_text.empty() ? result->stdout_text : result->stderr_text);
      if (error->empty()) *error = "git 拒绝执行";
    }
    return std::nullopt;
  }
  return result->stdout_text;
}

/// 按行切（去 `\r`、去尾部空行）。
[[nodiscard]] auto lines_of(const std::string& text) -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::size_t end = eol == std::string::npos ? text.size() : eol;
    std::string_view line(text.data() + begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    if (eol == std::string::npos) break;
    begin = eol + 1;
  }
  return lines;
}

}  // namespace

auto change_group_name(ChangeGroup group) -> std::string_view {
  switch (group) {
    case ChangeGroup::Conflicted: return "冲突";
    case ChangeGroup::Staged: return "已暂存";
    case ChangeGroup::Unstaged: return "未暂存";
    case ChangeGroup::Untracked: return "未跟踪";
  }
  return "";
}

auto RepoSnapshot::changes_in(ChangeGroup group) const -> std::vector<GitChange> {
  std::vector<GitChange> matched;
  matched.reserve(changes.size());
  for (const auto& change : changes) {
    bool hit = false;
    switch (group) {
      case ChangeGroup::Conflicted: hit = change.is_conflicted(); break;
      case ChangeGroup::Staged: hit = change.has_staged_side(); break;
      case ChangeGroup::Unstaged: hit = change.has_unstaged_side() && !change.is_conflicted(); break;
      case ChangeGroup::Untracked: hit = change.is_untracked(); break;
    }
    if (hit) matched.push_back(change);
  }
  return matched;
}

auto RepoSnapshot::count_in(ChangeGroup group) const -> std::size_t {
  return changes_in(group).size();
}

auto read_snapshot(const std::string& cwd) -> RepoSnapshot {
  RepoSnapshot snapshot{};
  if (cwd.empty()) {
    snapshot.error = "没有工作区";
    return snapshot;
  }
  // `--no-optional-locks`：status 顺手写 index 会与并行的 git 进程抢锁
  //（用户在终端里跑 git 命令时，我们的刷新可能让他报 "Unable to create index.lock"）。
  // ⚠ 它是 **git 全局选项**（要放在子命令前），塞给 status 会被当成未知选项
  //（实测 exit 129、stderr 空——我们的错误信息退成"git 拒绝执行"，方向完全错）。
  const auto out = git(cwd, {"--no-optional-locks", "status", "--porcelain=v1", "-b"},
                       &snapshot.error);
  if (!out.has_value()) {
    // "不是仓库"是**状态**不是故障——`fatal: not a git repository` 的判定要稳：
    // 不依赖错误文案（本地化会变），用再跑一条 `rev-parse` 交叉验证。
    const auto probe = git(cwd, {"rev-parse", "--is-inside-work-tree"});
    snapshot.is_repo = probe.has_value() && st::trim(*probe) == "true";
    if (!snapshot.is_repo) snapshot.error.clear();   // 面板显示"不是仓库"而不是故障态
    return snapshot;
  }
  snapshot.is_repo = true;

  for (std::string_view line : lines_of(*out)) {
    if (line.starts_with("## ")) {
      // `## main...origin/main [ahead 1, behind 2]`
      line.remove_prefix(3);
      const std::size_t bracket = line.find(" [");
      std::string_view head = bracket == std::string_view::npos
                                  ? line
                                  : line.substr(0, bracket);
      // 分支名到 `...` 或行尾。
      const std::size_t dots = head.find("...");
      snapshot.branch = std::string(dots == std::string_view::npos ? head : head.substr(0, dots));
      if (dots != std::string_view::npos) snapshot.upstream = std::string(head.substr(dots + 3));
      if (bracket != std::string_view::npos) {
        // `[ahead 1, behind 2]` → 两个数（只有一端时也各自成立）。
        const std::string_view info = line.substr(bracket);
        if (const std::size_t at = info.find("ahead "); at != std::string_view::npos) {
          snapshot.ahead = static_cast<std::size_t>(std::max<std::int64_t>(
              0, std::strtoll(std::string(info.substr(at + 6)).c_str(), nullptr, 10)));
        }
        if (const std::size_t at = info.find("behind "); at != std::string_view::npos) {
          snapshot.behind = static_cast<std::size_t>(std::max<std::int64_t>(
              0, std::strtoll(std::string(info.substr(at + 7)).c_str(), nullptr, 10)));
        }
      }
      continue;
    }
    if (line.size() < 4) continue;
    GitChange change{};
    change.index_status = line[0];
    change.work_status = line[1];
    std::string_view rest = line.substr(3);
    // 重命名形态：`R  old -> new`（箭头两边的空格都可能是转义过的——porcelain
    // 对含空格路径加引号；这里不做完整的 unquote，只剥最常见的引号对）。
    if (const std::size_t arrow = rest.find(" -> "); arrow != std::string_view::npos) {
      change.orig_path = std::string(rest.substr(0, arrow));
      rest = rest.substr(arrow + 4);
    }
    std::string path(rest);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
      path = path.substr(1, path.size() - 2);
    }
    change.path = std::move(path);
    snapshot.changes.push_back(std::move(change));
  }
  if (snapshot.branch.empty()) snapshot.branch = "（游离 HEAD）";
  return snapshot;
}

auto read_log(const std::string& cwd, std::size_t limit, const std::string& ref)
    -> std::vector<LogEntry> {
  std::vector<LogEntry> entries;
  // 格式定死（字段间 `\x1f` 分隔——提交信息里出现任何可打印字符都不怕）：
  // hash\x1fshort\x1fsubject\x1fauthor\x1fdate\x1frefs
  // 图形列由 `--graph` 的前缀自己画（`* `、`| `、`|\`…），不额外解析。
  const auto out = git(cwd,
                       {"log", "--graph", "--no-color", std::format("-{}", limit),
                        std::string("--pretty=format:%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%D"),
                        std::string("--date=format:%Y-%m-%d %H:%M"),
                        (ref.empty() ? std::string("--all") : ref)},
                       nullptr);
  if (!out.has_value()) return entries;

  for (const std::string_view line : lines_of(*out)) {
    if (line.empty() || line == "*") continue;
    // `--graph` 前缀（`*`/`|`/`/`/`\` 与空格）之后才是正文。
    std::size_t body = 0;
    std::size_t lane = 0;
    while (body < line.size() && (line[body] == '|' || line[body] == '/' || line[body] == '\\' ||
                                  line[body] == ' ' || line[body] == '*' ||
                                  line[body] == '_' || line[body] == '-')) {
      if (line[body] == '*') lane = body / 2;   // 泳道列号（每列 2 字符宽）
      ++body;
    }
    if (body >= line.size()) continue;
    const std::string_view rest = line.substr(body);
    // 拆 \x1f 字段。
    std::array<std::string_view, 6> fields{};
    std::size_t count = 0;
    std::size_t at = 0;
    while (count < fields.size()) {
      const std::size_t sep = rest.find('\x1f', at);
      fields[count] = rest.substr(at, sep == std::string_view::npos ? std::string::npos
                                                                    : sep - at);
      ++count;
      if (sep == std::string_view::npos) break;
      at = sep + 1;
    }
    LogEntry entry{};
    entry.hash = std::string(fields[0]);
    entry.short_hash = std::string(fields[1]);
    entry.subject = std::string(fields[2]);
    entry.author = std::string(fields[3]);
    entry.date = std::string(fields[4]);
    // refs：`HEAD -> main, origin/main` → 逐个。
    std::string_view refs_field = fields[5];
    if (!refs_field.empty()) {
      std::size_t cursor = 0;
      while (cursor <= refs_field.size()) {
        const std::size_t comma = refs_field.find(", ", cursor);
        const std::string_view one =
            refs_field.substr(cursor, comma == std::string_view::npos ? std::string::npos
                                                                     : comma - cursor);
        if (!one.empty()) entry.refs.push_back(std::string(one));
        if (comma == std::string_view::npos) break;
        cursor = comma + 2;
      }
    }
    entry.lane = lane;
    if (!entry.hash.empty()) entries.push_back(std::move(entry));
  }
  return entries;
}

auto read_commit_detail(const std::string& cwd, LogEntry& entry) -> std::string {
  entry.files.clear();
  // numstat：`3\t1\tsrc/a.cpp`（重命名带 `=>`，按原样显示）。
  if (const auto stats = git(cwd, {"show", "--no-color", "--numstat", "--format=", entry.hash})) {
    for (const std::string_view line : lines_of(*stats)) {
      if (line.empty()) continue;
      const std::size_t tab1 = line.find('\t');
      const std::size_t tab2 = tab1 == std::string_view::npos ? std::string::npos
                                                              : line.find('\t', tab1 + 1);
      if (tab2 == std::string_view::npos) continue;
      const auto added = static_cast<std::size_t>(
          std::max<std::int64_t>(0, std::strtoll(std::string(line.substr(0, tab1)).c_str(),
                                                 nullptr, 10)));
      const auto removed = static_cast<std::size_t>(
          std::max<std::int64_t>(0, std::strtoll(std::string(line.substr(tab1 + 1, tab2 - tab1 - 1))
                                                     .c_str(),
                                                 nullptr, 10)));
      entry.files.emplace_back(std::string(line.substr(tab2 + 1)), added, removed);
    }
  }
  const auto diff = git(cwd, {"show", "--no-color", entry.hash});
  return diff.has_value() ? *diff : std::string();
}

auto read_branches(const std::string& cwd) -> std::vector<BranchInfo> {
  std::vector<BranchInfo> branches;
  // `%(HEAD)` 给 `*` 前缀；`%(upstream:short)` 给跟踪分支。
  const auto out = git(cwd, {"branch", "--all", "--verbose", "--no-abbrev",
                             std::string("--format=%(HEAD)%(if)%(upstream:short)%(then)@%(upstream:short)%(end)%09%(refname:short)")});
  if (!out.has_value()) return branches;
  for (std::string_view line : lines_of(*out)) {
    if (line.empty() || line.starts_with("  remotes/origin/HEAD")) continue;
    BranchInfo info{};
    std::string_view rest = line;
    if (!rest.empty() && rest.front() == '*') {
      info.is_current = true;
      rest.remove_prefix(1);
    }
    // 跟踪段（`@origin/main`）与名字段用 `\t` 分隔（format 里只有一个 \t）。
    const std::size_t tab = rest.find('\t');
    if (tab == std::string_view::npos) continue;
    std::string_view track = rest.substr(0, tab);
    std::string_view name = rest.substr(tab + 1);
    if (!track.empty() && track.front() == '@') info.upstream_short = std::string(track.substr(1));
    info.is_remote = name.starts_with("origin/") || name.find('/') != std::string_view::npos;
    // `origin/main` 这种远程分支：refname:short 已去掉 refs/remotes/ 前缀。
    info.name = std::string(name);
    if (!info.name.empty()) branches.push_back(std::move(info));
  }
  return branches;
}

auto read_worktree_diff(const std::string& cwd, const std::string& path, bool staged)
    -> std::string {
  const auto out = staged ? git(cwd, {"diff", "--no-color", "--cached", "--", path})
                          : git(cwd, {"diff", "--no-color", "--", path});
  return out.has_value() ? *out : std::string();
}

// ————————————————————————————————————————————————————————————————————————————
// 任意两端对比
// ————————————————————————————————————————————————————————————————————————————

auto DiffEndpoint::parse(std::string_view text) -> DiffEndpoint {
  std::string folded = st::ascii_lower(st::trim(text));
  if (folded.empty() || folded == ".") return DiffEndpoint{};   // 空 = 未指定
  if (folded == "worktree" || folded == "working" || folded == "working_tree") {
    return DiffEndpoint{.kind = DiffEnd::Worktree};
  }
  if (folded == "index" || folded == "staged" || folded == "--cached") {
    return DiffEndpoint{.kind = DiffEnd::Index};
  }
  // 其余一律当 rev 透传：**不做第二套 rev 语法**（两套判据必然漂移，
  // 而 git 自己的报错比我们猜的准确）。
  return DiffEndpoint{.kind = DiffEnd::Revision, .revision = std::string(st::trim(text))};
}

auto DiffEndpoint::to_text() const -> std::string {
  switch (kind) {
    case DiffEnd::Worktree: return "WORKTREE";
    case DiffEnd::Index: return "INDEX";
    case DiffEnd::Empty: return {};
    case DiffEnd::Revision: return revision;
  }
  return {};
}

auto diff_args(const DiffEndpoint& from, const DiffEndpoint& to, bool merge_base)
    -> std::optional<std::vector<std::string>> {
  const auto is_empty = [](const DiffEndpoint& end) { return end.empty(); };
  // 表格（与头文件里的语义表一一对应）：先归一空端点的含义，再拼参数。
  // 空 + 空 = 工作区 ↔ 暂存区（`git diff` 的裸语义）。
  if (is_empty(from) && is_empty(to)) return std::vector<std::string>{};
  if (from.is_worktree() && (is_empty(to) || to.is_worktree())) return std::vector<std::string>{};
  // 空 → WORKTREE：也是「工作区 ↔ 暂存区」（两个端点是同一件事的两种写法）。
  if (is_empty(from) && to.is_worktree()) return std::vector<std::string>{};
  // 已暂存：HEAD ↔ 暂存区（`--cached`）。
  if (to.is_index() && (is_empty(from) || from.is_worktree())) return {{"--cached"}};
  if (from.is_index() && is_empty(to)) return {{"--cached"}};
  // 空 → rev：`git diff REF`（「工作区 ↔ 那个 rev」）。
  if (is_empty(from) && to.kind == DiffEnd::Revision) return {{to.revision}};
  // rev ↔ 工作区：`git diff REF`。
  if (from.kind == DiffEnd::Revision && (is_empty(to) || to.is_worktree())) {
    return {{from.revision}};
  }
  // rev ↔ 暂存区：`git diff --cached REF`。
  if (from.kind == DiffEnd::Revision && to.is_index()) return {{"--cached", from.revision}};
  if (from.is_index() && to.kind == DiffEnd::Revision) return {{"--cached", to.revision}};
  // rev ↔ rev。三点用**一个**参数 `A...B`（再跟一个 `B` 会被当成路径过滤器，
  // git 会报 “ambiguous argument”——实测）。
  if (from.kind == DiffEnd::Revision && to.kind == DiffEnd::Revision) {
    if (merge_base) return {{from.revision + "..." + to.revision}};
    return {{from.revision, to.revision}};
  }
  return std::nullopt;   // 不支持的组合（如 INDEX → INDEX）
}

auto is_root_commit(const std::string& cwd, const std::string& rev) -> bool {
  if (rev.empty()) return false;
  // `rev^` 不存在但 rev 本身是合法 commit ⇒ 根提交。
  // （直接 `rev-list --max-parents=0` 也行，但它对非法 rev 不报错，区分不出两类失败。）
  if (const auto parent = git(cwd, {"rev-parse", "--verify", "--quiet", rev + "^"});
      parent.has_value()) {
    return false;
  }
  return git(cwd, {"rev-parse", "--verify", "--quiet", rev + "^{commit}"}).has_value();
}

auto read_compare(const std::string& cwd, const CompareOptions& options) -> CompareResult {
  CompareResult result;
  result.from_text = options.from.to_text();
  result.to_text = options.to.to_text();
  const auto args = diff_args(options.from, options.to, options.merge_base);
  if (!args.has_value()) {
    // 不支持的组合要**如实报错**，不能当成“无差异”（那会把配置错误伪装成干净状态）。
    result.error = "不支持的差异端点组合：" + result.from_text + " → " + result.to_text;
    return result;
  }
  std::vector<std::string> base = *args;
  // `diff_args` 给的是**子参数**，子命令要自己带上（漏掉会变成 `git <rev> …`，
  // 报 “'HEAD~1' is not a git command”——看着像 rev 不合法，实际是命令拼错了）。
  base.insert(base.begin(), "diff");
  // 参数拼装：入参名不叫 `args`（那是上面 `diff_args` 结果的同名变量，
  // `-Wshadow` 会当场报错——而它拦的正是“以为在用外层的那个”这类误会）。
  const auto common = [&](std::vector<std::string>& out_args) {
    out_args.push_back("--no-color");
    out_args.push_back("--no-ext-diff");
    out_args.push_back("--find-renames");
    if (options.ignore_whitespace) out_args.push_back("-w");
    // ⚠ **不要在这里加 `-U<n>`**：它隐含 `-p`（patch 输出）——实测
    // `-U3 --numstat -z` 会在 numstat 记录后面再吐一整篇 patch 文本
    //（按名字解析时那些碎片会被当成“路径”收进来）。上下文行数只对
    //「取 patch 文本」那一条路有意义（`read_diff_text`）。
    if (!options.path.empty()) {
      out_args.push_back("--");
      out_args.push_back(options.path);
    }
  };

  // ⚠ `--name-status` 与 `--numstat` **不能写在同一条命令里**：两者同时给时 git
  // 只输出前者（实测：一条命令下 numstat 行整段消失，而“文件没变化”与“计数丢失”
  // 在结果里长得一模一样）。因此跑**两条**命令，靠 `-z` 的 NUL 分隔各自解好再按下标合并
  // ——两条命令的列表顺序一致，合并是确定的。
  std::vector<std::string> status_args = base;
  common(status_args);
  status_args.push_back("--name-status");
  status_args.push_back("-z");
  std::string status_error;
  const auto status_out = git(cwd, status_args, &status_error);
  if (!status_out.has_value()) {
    result.error = status_error;
    return result;
  }
  std::vector<std::string> stat_args = base;
  common(stat_args);
  stat_args.push_back("--numstat");
  stat_args.push_back("-z");
  std::string stat_error;
  const auto stat_out = git(cwd, stat_args, &stat_error);
  if (!stat_out.has_value()) {
    result.error = stat_error;
    return result;
  }

  const auto tokens = [](const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char ch : text) {
      if (ch == '\0' || ch == '\n') {
        if (!current.empty()) {
          out.push_back(std::move(current));
          current.clear();
        }
      } else {
        current += ch;
      }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
  };

  // `--name-status -z`：`M\0path\0`；重命名/复制是 `R057\0old\0new\0`（**两个名字**）。
  const std::vector<std::string> status_tokens = tokens(*status_out);
  for (std::size_t index = 0; index < status_tokens.size();) {
    const std::string& code = status_tokens[index];
    if (code.empty()) {
      ++index;
      continue;
    }
    ComparedFile file;
    file.status = code.front();
    const bool two_names = (file.status == 'R' || file.status == 'C');
    if (two_names) {
      if (index + 2 >= status_tokens.size()) break;
      file.old_path = status_tokens[index + 1];
      file.path = status_tokens[index + 2];
      index += 3;
    } else {
      if (index + 1 >= status_tokens.size()) break;
      file.path = status_tokens[index + 1];
      index += 2;
    }
    result.files.push_back(std::move(file));
  }

  // `--numstat -z`：`加\t删\t路径\0`；重命名是 `加\t删\t\0old\0new\0`（名字在后面）。
  struct Stat {
    std::size_t additions{0};
    std::size_t deletions{0};
    bool binary{false};
  };
  std::vector<Stat> stats;
  std::vector<std::string> stat_paths;
  const std::vector<std::string> stat_tokens = tokens(*stat_out);
  for (std::size_t index = 0; index < stat_tokens.size();) {
    const std::string& token = stat_tokens[index];
    const std::size_t tab1 = token.find('\t');
    if (tab1 == std::string::npos) {   // 不是 numstat 记录：跳过一个 token 继续找
      ++index;
      continue;
    }
    const std::size_t tab2 = token.find('\t', tab1 + 1);
    if (tab2 == std::string::npos) {   // 名字在后面的形式：`加\t删\t` + 两个名字
      ++index;
      continue;
    }
    Stat stat;
    const std::string add = token.substr(0, tab1);
    const std::string del = token.substr(tab1 + 1, tab2 - tab1 - 1);
    stat.binary = add == "-" || del == "-";
    stat.additions = stat.binary ? 0 : static_cast<std::size_t>(std::strtoull(add.c_str(), nullptr, 10));
    stat.deletions = stat.binary ? 0 : static_cast<std::size_t>(std::strtoull(del.c_str(), nullptr, 10));
    std::string path = token.substr(tab2 + 1);
    if (path.empty()) {
      // 重命名：名字在后两个 token（以新名字为准，与 status 行对齐）。
      if (index + 2 >= stat_tokens.size()) break;
      path = stat_tokens[index + 2];
      index += 3;
    } else {
      ++index;
    }
    stats.push_back(stat);
    stat_paths.push_back(std::move(path));
  }

  // 按下标合并（两条命令的列表顺序一致）；数量不一致时以 status 那侧为准，
  // 但对不上就往好处“降级而不能编造计数”：只给能对上的，其余保持 0。
  for (std::size_t index = 0; index < result.files.size(); ++index) {
    if (index >= stats.size()) break;
    ComparedFile& file = result.files[index];
    if (!stat_paths[index].empty() && stat_paths[index] != file.path) {
      // 两侧列表没有对齐（外部改动了仓库？）——不猜，按路径重新找一次。
      const auto hit = std::find(stat_paths.begin(), stat_paths.end(), file.path);
      if (hit == stat_paths.end()) continue;
      const std::size_t at = static_cast<std::size_t>(hit - stat_paths.begin());
      file.additions = stats[at].additions;
      file.deletions = stats[at].deletions;
      file.binary = stats[at].binary;
    } else {
      file.additions = stats[index].additions;
      file.deletions = stats[index].deletions;
      file.binary = stats[index].binary;
    }
  }
  for (const auto& file : result.files) {
    result.additions += file.additions;
    result.deletions += file.deletions;
  }
  // 文件数上限：超出时**只保留前 N 个并把事实写进 error**（不静默截断）——
  // “看不到后面那些文件”必须与“那些文件不存在”区分开。
  if (options.max_files != 0 && result.files.size() > options.max_files) {
    result.files.resize(options.max_files);
    result.error = std::format("变更文件过多，只列出前 {} 个（可用路径过滤收窄）", options.max_files);
  }
  result.ok = true;
  return result;
}

auto read_diff_text(const std::string& cwd, const CompareOptions& options) -> std::string {
  const auto mapped = diff_args(options.from, options.to, options.merge_base);
  if (!mapped.has_value()) return {};
  std::vector<std::string> args = *mapped;
  args.insert(args.begin(), "diff");
  args.push_back("--no-color");
  args.push_back("--no-ext-diff");
  args.push_back("--find-renames");
  if (options.ignore_whitespace) args.push_back("-w");
  args.push_back(std::format("-U{}", std::min<std::size_t>(options.context, 20)));
  if (!options.path.empty()) {
    args.push_back("--");
    args.push_back(options.path);
  }
  const auto out = git(cwd, args);
  return out.has_value() ? *out : std::string();
}

auto read_content_at(const std::string& cwd, const DiffEndpoint& end, const std::string& path)
    -> ContentAt {
  ContentAt result;
  if (path.empty()) {
    result.error = "路径为空";
    return result;
  }
  if (end.is_worktree()) {
    // 工作区：直接读磁盘（不经过 git：未跟踪文件也该能当“另一侧”看）。
    auto text = st::fs::read_text(st::fs::join(cwd, path));
    if (!text.has_value()) {
      result.missing = true;
      result.error = "文件不存在或读不到";
      return result;
    }
    result.text = std::move(*text);
    result.size = result.text.size();
    result.ok = true;
    return result;
  }
  // 暂存区 / 某个提交：`git show :0:<path>` / `git show <rev>:<path>`。
  const std::string spec = end.is_index() ? ":0:" + path
                                         : (end.kind == DiffEnd::Revision ? end.revision + ":" + path
                                                                          : std::string{});
  if (spec.empty()) {
    result.error = "不支持的端点（无法取内容）";
    return result;
  }
  const auto out = git(cwd, {"show", "--no-color", spec}, &result.error);
  if (!out.has_value()) {
    // “那一侧没有这个文件”不是错误（新增/删除就是这种情况）——**与真正的失败分开**。
    result.missing = true;
    result.error.clear();
    return result;
  }
  result.text = *out;
  result.size = result.text.size();
  result.binary = result.text.find('\0') != std::string::npos;
  result.ok = true;
  return result;
}

}  // namespace gbcode
