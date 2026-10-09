#include "git_service.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <map>

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

}  // namespace gbcode
