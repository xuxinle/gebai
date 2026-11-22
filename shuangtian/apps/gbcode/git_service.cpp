#include "git_service.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <future>
#include <map>
#include <unordered_map>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

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

/// 按行切（去 `\r`、去尾部空行）——**零拷贝**（返回指向入参的视图）。
///
/// 参数是 `string_view` 而不是 `const std::string&`：日志/numstat 这类解析是对
/// **一段已经拿到的文本**做切分，而那段文本可能只是某个更大缓冲区的一部分
/// （如 `read_commit_detail` 把一条输出切成“统计区”和“patch 区”分别解析）。
[[nodiscard]] auto lines_of(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  // 先数一遍 `\n` 定容量：万行日志反复扩容会把所有 `string_view` 搬十几次。
  std::size_t count = 0;
  for (const char ch : text) {
    if (ch == '\n') ++count;
  }
  lines.reserve(count + 1);
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

namespace {

/// 组 → 它在 `group_index` 里的槽位（与 `ChangeGroup` 的枚举顺序一致）。
[[nodiscard]] constexpr auto group_slot(ChangeGroup group) -> std::size_t {
  return static_cast<std::size_t>(group);
}

/// 这条变更属于哪几个组（位图；`change_group` 的单一真值源）。
///
/// 为何返回位图而不是一个组：一条变更可以**同时在两组**
/// （X=M、Y=M：暂存版进「已暂存」、工作区版进「未暂存」，这是 IDEA 的显示口径）。
/// 冲突单独成组且**不重复计入暂存/未暂存**（它挡着提交，语义上先处理它）。
[[nodiscard]] constexpr auto change_groups(const GitChange& change) -> std::uint8_t {
  // 用 `unsigned` 做位运算再窄化一次：`-Wconversion` 会把 `1U << n` 的
  // `unsigned` → `uint8_t` 判成可能改值（`|=` 也是赋值）。
  unsigned mask = 0;
  if (change.is_conflicted()) {
    mask |= 1U << group_slot(ChangeGroup::Conflicted);
    return static_cast<std::uint8_t>(mask);   // 冲突不再参与另两组
  }
  if (change.has_staged_side()) mask |= 1U << group_slot(ChangeGroup::Staged);
  if (change.has_unstaged_side()) mask |= 1U << group_slot(ChangeGroup::Unstaged);
  if (change.is_untracked()) mask |= 1U << group_slot(ChangeGroup::Untracked);
  return static_cast<std::uint8_t>(mask);
}

}  // namespace

auto RepoSnapshot::regroup() -> void {
  for (auto& slot : group_index) slot.clear();
  for (std::size_t index = 0; index < changes.size(); ++index) {
    const unsigned mask = change_groups(changes[index]);
    for (std::size_t group = 0; group < group_index.size(); ++group) {
      if ((mask & (1U << group)) != 0U) {
        group_index[group].push_back(static_cast<std::uint32_t>(index));
      }
    }
  }
}

auto RepoSnapshot::changes_in(ChangeGroup group) const -> std::vector<GitChange> {
  const auto indices = indices_in(group);
  std::vector<GitChange> matched;
  matched.reserve(indices.size());
  for (const std::uint32_t index : indices) matched.push_back(changes[index]);
  return matched;
}

auto RepoSnapshot::indices_in(ChangeGroup group) const -> std::span<const std::uint32_t> {
  const std::size_t slot = group_slot(group);
  if (slot >= group_index.size()) return {};
  return group_index[slot];
}

auto RepoSnapshot::count_in(ChangeGroup group) const -> std::size_t {
  return indices_in(group).size();
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
  //
  // ## 为何用 `-z`（2026-11-22）
  //
  // 非 `-z` 时 git 对**含特殊字符的路径会加双引号并做 C 风格转义**（即使
  // `core.quotepath=false` 关了非 ASCII 的八进制转义，引号还在）——于是
  //   · 解析要自己处理 `"a b" -> "c d"` 的引号与 `\"` 反转义；
  //   · 重命名要拆 ` -> `（而路径本身就含 ` -> ` 时会拆错）。
  // `-z` 把这些全部结构化了：字段以 NUL 分隔、重命名是**两个独立记录**
  // （`R  new\0old\0`），路径原样（不经引号、不经转义）。
  // 实测本仓：解析层不再需要 unquote，且重命名语义变确定（见下面的注释）。
  const auto out = git(cwd, {"--no-optional-locks", "status", "--porcelain=v1", "-z", "-b"},
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

  // `-z` 把“行”与“字段”都换成了 NUL：先把记录拆出来，再逐条解释。
  // 首条记录是 `-b` 的头（`## main...origin/main [ahead 1]`）。
  std::vector<std::string_view> records;
  {
    std::string_view text = *out;
    while (!text.empty()) {
      const std::size_t nul = text.find('\0');
      const std::string_view record = nul == std::string_view::npos ? text : text.substr(0, nul);
      records.push_back(record);
      if (nul == std::string_view::npos) break;
      text.remove_prefix(nul + 1);
    }
  }

  std::size_t cursor = 0;
  // —— ① 头：分支 / 上游 / ahead-behind ——
  if (cursor < records.size() && records[cursor].starts_with("## ")) {
    std::string_view line = records[cursor];
    line.remove_prefix(3);
    cursor += 1;
    const std::size_t bracket = line.find(" [");
    const std::string_view head =
        bracket == std::string_view::npos ? line : line.substr(0, bracket);
    // 分支名到 `...` 或行尾。
    const std::size_t dots = head.find("...");
    snapshot.branch = std::string(dots == std::string_view::npos ? head : head.substr(0, dots));
    if (dots != std::string_view::npos) {
      // 上游可能带 ` [gone]` 后缀（上游分支已被删除）——那不是名字的一部分。
      std::string upstream(head.substr(dots + 3));
      if (const std::size_t gone = upstream.find(" ["); gone != std::string::npos) {
        upstream.resize(gone);
      }
      snapshot.upstream = std::move(upstream);
    }
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
  }

  // —— ② 变更记录 ——
  // 每条：`XY SP path`；重命名/复制时 **紧跟一条纯路径记录**（原路径）。
  // 逐条预分配容量，避免 2000 条变更时反复扩容（每次扩容都挪动全部字符串）。
  snapshot.changes.reserve(records.size() > cursor ? records.size() - cursor : 0);
  while (cursor < records.size()) {
    const std::string_view record = records[cursor];
    ++cursor;
    if (record.size() < 3) continue;   // `XY SP` 至少 3 字符
    GitChange change{};
    change.index_status = record[0];
    change.work_status = record[1];
    // 第 3 个字符是分隔空格（`XY path`）；路径从下标 3 起，**原样**（`-z` 不转义）。
    change.path = std::string(record.substr(3));
    // 重命名/复制的原路径是**下一条记录**（没有 `XY ` 前缀）。
    if ((change.index_status == 'R' || change.index_status == 'C' ||
         change.work_status == 'R' || change.work_status == 'C') &&
        cursor < records.size()) {
      change.orig_path = std::string(records[cursor]);
      ++cursor;
    }
    snapshot.changes.push_back(std::move(change));
  }
  if (snapshot.branch.empty()) snapshot.branch = "（游离 HEAD）";
  // 分组下标在这里一次算好：界面每帧要读 4 个组（且计数各读一次），
  // 如果每次重算就是每帧 8 次全量拷贝（见 `group_index` 的注释）。
  snapshot.regroup();
  return snapshot;
}

/// 把 `git log --pretty=format:` 的输出解析成条目（两个读日志的入口共用）。
///
/// ## 与拓扑图**共用同一次输出**（2026-11-22 性能）
///
/// 格式末位多了一个 `%P`（父提交列表）：展示字段照旧取前六段，
/// 而 `%P` 同时喂给 `build_commit_graph`——于是日志与泳道图**一条命令**就都有了。
/// 旧的“两条命令各读一遍同一段历史”正是 174 ms → 97 ms 的差值来源。
///
/// `revs` 非空时按**同一顺序**填回去（与 `entries` 一一对应），供 `read_log_and_graph` 用；
/// 单要日志时传 `nullptr`（多拆一个字段的代价可忽）。
[[nodiscard]] auto parse_log_entries(std::string_view out_view,
                                    std::vector<CommitRev>* revs = nullptr)
    -> std::vector<LogEntry>;

auto read_log_and_graph(const std::string& cwd, std::size_t limit, const std::string& ref)
    -> LogAndGraph {
  LogAndGraph result;
  // 格式定死（字段间 `\x1f` 分隔——提交信息里出现任何可打印字符都不怕）：
  // hash\x1fshort\x1fsubject\x1fauthor\x1fdate\x1frefs\x1fparents
  //
  // ⚠ **不带 `--graph`**：那是给人看的渲染产物（列宽、斜线落在奇/偶格、
  // 为一个合并插几个连接行，都由 git 的排版决定，没有稳定契约），而且它会把
  // 拓扑编码成像素，逼着我们从字符位置反推“这笔提交有几个父、连到哪道”。
  // 现在拓扑来自 `%P`（与 `rev-list --parents` 同源），泳道自己算。
  //
  // ⚠ **`--topo-order` 是必需的**（不是可有可无的排序偏好）：逐行分配泳道的前提是
  // “子一定在父之前出现”。默认的 `--date-order` 在此不成立（它会先按日期倒序铺一层）。
  const auto out = git(cwd,
                       {"log", "--no-color", "--topo-order", std::format("-{}", limit),
                        std::string("--pretty=format:%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%D\x1f%P"),
                        std::string("--date=format:%Y-%m-%d %H:%M"),
                        (ref.empty() ? std::string("--all") : ref)},
                       nullptr);
  if (!out.has_value()) return result;
  std::vector<CommitRev> revs;
  result.log = parse_log_entries(*out, &revs);
  result.graph = build_commit_graph(revs);
  return result;
}

auto read_log(const std::string& cwd, std::size_t limit, const std::string& ref)
    -> std::vector<LogEntry> {
  // 单要日志时不多算拓扑（把 `%P` 那段当普通字段丢掉即可）。
  const auto out = git(cwd,
                       {"log", "--no-color", std::format("-{}", limit),
                        std::string("--pretty=format:%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%D"),
                        std::string("--date=format:%Y-%m-%d %H:%M"),
                        (ref.empty() ? std::string("--all") : ref)},
                       nullptr);
  if (!out.has_value()) return {};
  return parse_log_entries(*out);
}

auto parse_log_entries(std::string_view out_view, std::vector<CommitRev>* revs)
    -> std::vector<LogEntry> {
  std::vector<LogEntry> entries;
  if (revs != nullptr) revs->clear();
  // 行数先估个容量（`\n` 计数是线性的、比反复扩容便宜；200 条日志只有几次扩容，
  // 但万条日志时差别就明显了）。
  {
    std::size_t lines = 0;
    for (const char ch : out_view) {
      if (ch == '\n') ++lines;
    }
    entries.reserve(lines + 1);
    if (revs != nullptr) revs->reserve(lines + 1);
  }

  for (const std::string_view line : lines_of(out_view)) {
    if (line.empty()) continue;
    // 拆 \x1f 字段（末位 `%P` 只在 `read_log_and_graph` 那条路上有）。
    std::array<std::string_view, 7> fields{};
    std::size_t count = 0;
    std::size_t at = 0;
    while (count < fields.size()) {
      const std::size_t sep = line.find('\x1f', at);
      fields[count] = line.substr(at, sep == std::string_view::npos ? std::string_view::npos
                                                                    : sep - at);
      ++count;
      if (sep == std::string_view::npos) break;
      at = sep + 1;
    }
    if (fields[0].empty()) continue;
    LogEntry entry{};
    entry.hash = std::string(fields[0]);
    // **短哈希固定 8 位**（从完整 hash 截，不信 `%h`）：
    //
    // git 的 `%h` 会随仓库规模自动伸缩（小仓 7 位、大仓 8~9 位），于是同一个
    // 列表里哈希列宽度会忽长忽短，对不齐。而“唯一性”不需要向 git 让步：
    // 8 位在几十万提交的仓里也足够区分（前端只用于展示与筛选，操作一律用完整 hash）。
    entry.short_hash = entry.hash.substr(0, 8);
    entry.subject = std::string(fields[2]);
    entry.author = std::string(fields[3]);
    entry.date = std::string(fields[4]);
    // refs：`HEAD -> main, origin/main` → 逐个。
    std::string_view refs_field = fields[5];
    if (!refs_field.empty()) {
      std::size_t cursor = 0;
      while (cursor <= refs_field.size()) {
        const std::size_t comma = refs_field.find(", ", cursor);
        const std::size_t stop = comma == std::string_view::npos ? refs_field.size() : comma;
        std::string_view one = refs_field.substr(cursor, stop - cursor);
        if (!one.empty() && one.front() == ' ') one.remove_prefix(1);
        if (!one.empty()) entry.refs.emplace_back(one);
        if (comma == std::string_view::npos) break;
        cursor = comma + 2;
      }
    }
    if (revs != nullptr) {
      // `%P`：父提交的**完整 hash**，空格分隔（与 `rev-list --parents` 的第二列同源）。
      CommitRev rev;
      rev.hash = entry.hash;
      std::string_view parents_field = fields[6];
      while (!parents_field.empty()) {
        const std::size_t space = parents_field.find(' ');
        const std::string_view token =
            space == std::string_view::npos ? parents_field : parents_field.substr(0, space);
        if (!token.empty()) rev.parents.emplace_back(token);
        if (space == std::string_view::npos) break;
        parents_field.remove_prefix(space + 1);
      }
      revs->push_back(std::move(rev));
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

auto read_commit_detail(const std::string& cwd, LogEntry& entry) -> std::string {
  entry.files.clear();
  entry.message.clear();
  // ## 为何只跑一条（2026-11-22）
  //
  // 旧实现是三条：`show --format=%B -s`（提交信息）、`show --numstat --format=`
  // （统计）、`show`（patch）。实测三条各 46~55 ms，而**一条命令就能同时给出三者**：
  //
  // | 参数 | 给出什么 |
  // |---|---|
  // | `--format=%B%x00` | 提交信息（`%B` = 标题+正文），尾部 NUL 当终结符 |
  // | `--numstat` | 逐文件统计（`加\t删\t路径`；重命名是 `0\t0\told => new`） |
  // | `-p` | patch 正文（响应字段里的 `diff`） |
  //
  // ⚠ 顺序是**输出顺序**：`%B` 先到（顶上的 NUL 就是它的终点），然后一段空行，
  // 然后 numstat 区，最后 patch。三段的边界靠**第一个 NUL** 与**第一个 `diff --git`**
  // 切：不能按空行切——`--format=` 为空时 git 仍会在 numstat 与 patch 之间插一个
  // 空行，而 numstat 区自己不带前导空行（实测）。
  const auto out = git(cwd, {"show", "--no-color", "--numstat", "-p", "--format=%B%x00",
                             entry.hash});
  if (!out.has_value()) return {};
  std::string_view rest = *out;

  // —— ① 提交信息：到第一个 NUL 为止 ——
  const std::size_t nul = rest.find('\0');
  if (nul != std::string_view::npos) {
    entry.message = std::string(rest.substr(0, nul));
    rest.remove_prefix(nul + 1);
  }
  // 去掉尾部的多余空行（`%B` 总以换行结尾，多留几行会让渲染出的布局多一块空白）。
  while (!entry.message.empty() &&
         (entry.message.back() == '\n' || entry.message.back() == '\r')) {
    entry.message.pop_back();
  }

  // —— ② patch 起点：第一个 `diff --git ` 行 ——
  //
  // 为何按行首找而不是 `find("diff --git")`：提交信息里的正文、numstat 里的
  // 路径都可能包含这串字（重命名到名为 `diff --git a` 的文件是真能出现的）。
  // 行首 + 后面跟一个空格才是真的段分隔。
  std::size_t patch_at = std::string_view::npos;
  {
    std::size_t cursor = 0;
    while (cursor < rest.size()) {
      const std::size_t eol = rest.find('\n', cursor);
      const std::size_t end = eol == std::string_view::npos ? rest.size() : eol;
      if (rest.substr(cursor, end - cursor).starts_with("diff --git ")) {
        patch_at = cursor;
        break;
      }
      if (eol == std::string::npos) break;
      cursor = eol + 1;
    }
  }
  const std::string_view stats_text =
      patch_at == std::string_view::npos ? rest : rest.substr(0, patch_at);
  const std::string_view diff_text =
      patch_at == std::string_view::npos ? std::string_view{} : rest.substr(patch_at);

  // —— ③ numstat：`3\t1\tsrc/a.cpp`（重命名带 `=>`，按原样显示） ——
  for (const std::string_view line : lines_of(stats_text)) {
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
  return std::string(diff_text);
}

namespace {

/// 按 `sep` 把一行拆成 `N` 段（零拷贝）。
///
/// 为何不直接用 `st::split`：它返回 `vector` 且**保留空段**（那个语义是对的），
/// 而这里要的是“定长字段表 + 缺段时告我一声”——写成局部模板比在每个解析点
/// 重复一遍 while 循环干净。
template <std::size_t N>
[[nodiscard]] auto split_n(std::string_view line, char sep, std::size_t& count)
    -> std::array<std::string_view, N> {
  std::array<std::string_view, N> fields{};
  count = 0;
  std::size_t at = 0;
  while (count < N) {
    const std::size_t found = line.find(sep, at);
    fields[count] = line.substr(at, found == std::string_view::npos ? std::string_view::npos
                                                                   : found - at);
    ++count;
    if (found == std::string_view::npos) break;
    at = found + 1;
  }
  return fields;
}

}  // namespace

namespace {

constexpr RefPart kAllRefParts = RefPart::Refs | RefPart::Stashes | RefPart::Remotes;

/// 这个位图里有没有某一路。
[[nodiscard]] constexpr auto has_part(RefPart value, RefPart bit) -> bool {
  return (static_cast<unsigned>(value) & static_cast<unsigned>(bit)) != 0U;
}

}  // namespace

auto read_ref_snapshot(const std::string& cwd) -> RefSnapshot {
  return read_ref_parts(cwd, kAllRefParts);
}

auto read_ref_parts(const std::string& cwd, RefPart parts) -> RefSnapshot {
  RefSnapshot snapshot;

  // ── ① 全部引用：一条 `for-each-ref` 给出分支 / 标签 / 引用清单三者的字段 ──
  //
  // 旧实现是三条：`branch --all`（50 ms）+ `for-each-ref refs/tags`（63 ms）
  // + `for-each-ref refs/{heads,remotes}`（65 ms）——读的是同一批 ref。现在一条。
  //
  // 字段（用 `\t` 分隔，不用 `\x1f`：refname 里不可能有制表符，而 `\t` 让命令行
  // 仍是可读的）：
  //   HEAD标记 \t upstream短名 \t symref \t 完整refname \t 短名 \t objecttype
  //   \t objectname短 \t 标签消息首行 \t 创建时间（unix 秒）
  //
  // ⚠ `%(HEAD)` 只有**refs/heads 里的当前分支**会带 `*`——这正是我们要的；
  // 远程分支上它恒为空（不会被误判成当前分支）。
  // ⚠ `%(upstream:short)` 只对本地分支有意义；上游被删（gone）时它会给出
  // **空串**（而不是名字），而我们只需要名字用于展示，空就是空。
  // ⚠ `%(contents:subject)` 对注解标签是**标签消息**的首行，对轻量标签回退到
  // 目标提交的 subject（git 自己的行为，与旧实现同口径）。
  if (has_part(parts, RefPart::Refs)) {
    const auto refs_out = git(cwd,
                              {"for-each-ref",
                               "--format=%(HEAD)\t%(upstream:short)\t%(symref)\t%(refname)"
                               "\t%(refname:short)\t%(objecttype)\t%(objectname:short)"
                               "\t%(contents:subject)\t%(creatordate:unix)",
                               "refs/heads", "refs/remotes", "refs/tags"},
                              &snapshot.error);
    if (!refs_out.has_value()) return snapshot;

    // 标签创建时间（排序用；与 `tags` 同下标）。`--sort` 是**全局**选项，
    // 一条 for-each-ref 里排了标签也会排分支——而分支必须保持 git 自己给的
    // “本地在前、远程在后”的稳定序（与旧 `branch --all` 一致）。所以这里
    // 只在 C++ 层对标签单独排序。
    std::vector<std::int64_t> tag_stamps;
    constexpr std::string_view kHeads = "refs/heads/";
    constexpr std::string_view kRemotes = "refs/remotes/";
    constexpr std::string_view kTags = "refs/tags/";
    for (const std::string_view line : lines_of(*refs_out)) {
      if (line.empty()) continue;
      std::size_t count = 0;
      const auto fields = split_n<9>(line, '\t', count);
      if (count < 8U) continue;
      const std::string_view refname = fields[3];
      const std::string_view short_name = fields[4];
      if (refname.starts_with(kTags)) {
        TagInfo tag;
        tag.name = std::string(short_name);
        // `%(objecttype)` = `tag` ⇒ 注解标签（它的 objectname 是标签对象，不是提交）；
        // 轻量标签直接指向提交（`commit`）。
        tag.annotated = fields[5] == "tag";
        tag.target = std::string(fields[6]);
        tag.subject = std::string(fields[7]);
        tag_stamps.push_back(count >= 9U && !fields[8].empty()
                                 ? static_cast<std::int64_t>(
                                       std::strtoll(std::string(fields[8]).c_str(), nullptr, 10))
                                 : 0);
        snapshot.tags.push_back(std::move(tag));
        RefInfo info;
        info.name = std::string(short_name);
        info.hash = std::string(fields[6]);
        info.kind = "tag";
        snapshot.refs.push_back(std::move(info));
        continue;
      }
      if (refname.starts_with(kHeads) || refname.starts_with(kRemotes)) {
        const bool remote = refname.starts_with(kRemotes);
        // `%(symref)` 非空 ⇒ 符号引用（`refs/remotes/origin/HEAD` 这种别名），
        // 它不是可切的分支——两个视图（分支/引用）都不收。
        if (!fields[2].empty()) continue;
        BranchInfo branch;
        branch.name = std::string(short_name);
        branch.is_current = !fields[0].empty() && fields[0].front() == '*';
        branch.is_remote = remote;
        if (!remote && !fields[1].empty()) branch.upstream_short = std::string(fields[1]);
        snapshot.branches.push_back(std::move(branch));

        RefInfo info;
        info.name = std::string(short_name);
        info.hash = std::string(fields[6]);
        info.kind = remote ? "remote" : "local";
        snapshot.refs.push_back(std::move(info));
      }
    }

    // ── 标签按创建时间**倒序**（与旧 `read_tags` 的 `--sort=-creatordate` 同口径）──
    // 稳定排序：同一时间（同一秒创建的两个标签）保持 for-each-ref 的名字序。
    std::vector<std::size_t> order(snapshot.tags.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
      return tag_stamps[left] > tag_stamps[right];
    });
    std::vector<TagInfo> sorted;
    sorted.reserve(snapshot.tags.size());
    for (const std::size_t index : order) sorted.push_back(std::move(snapshot.tags[index]));
    snapshot.tags = std::move(sorted);
  }

  // ── ② stash 与 ③ 远程：互不依赖，也都不依赖上一步的解析结果 ⇒ **并发**发出去 ──
  //
  // 旧实现是“`git remote` + 每个 remote 两条 `get-url`”：三个远程就是 7 条子进程
  // （各 ~46 ms）。而远程名与 URL 本来就在配置里：`remote.<名>.url` /
  // `remote.<名>.pushurl`。一条 `config --get-regexp` 就够，且 `-z` 让值里的
  // 换行/制表符不需要转义（URL 里通常没有，但 `pushurl` 可以是任意串）。
  std::optional<std::future<std::optional<std::string>>> stash_future;
  std::optional<std::future<std::optional<std::string>>> config_future;
  if (has_part(parts, RefPart::Stashes)) {
    stash_future = std::async(std::launch::async, [cwd] {
      return git(cwd, {"stash", "list", "--format=%gd\x1f%gs"});
    });
  }
  if (has_part(parts, RefPart::Remotes)) {
    config_future = std::async(std::launch::async, [cwd] {
      return git(cwd, {"config", "-z", "--get-regexp", "^remote\\..*\\.(url|pushurl)$"});
    });
  }

  if (stash_future.has_value()) {
    const auto stash_out = stash_future->get();
    if (stash_out.has_value()) {
      for (const std::string_view line : lines_of(*stash_out)) {
        if (line.empty()) continue;
        std::size_t count = 0;
        const auto fields = split_n<2>(line, '\x1f', count);
        if (count < 2U) continue;
        StashEntry entry;
        entry.index = std::string(fields[0]);
        entry.message = std::string(fields[1]);
        // 两种前缀：自动消息 `WIP on main: 1234abc 提交信息`、`-m` 自定义消息 `On main: 消息`。
        // 只认前一种会让“自定义消息的 stash”读不出分支（实测）。
        const std::size_t colon = entry.message.find(": ");
        if (colon != std::string::npos) {
          std::string_view head(entry.message.data(), colon);
          if (head.starts_with("WIP on ")) head.remove_prefix(7);
          if (head.starts_with("On ")) head.remove_prefix(3);
          if (!head.empty() && head.find(' ') == std::string_view::npos) {
            entry.branch = std::string(head);
          }
        }
        snapshot.stashes.push_back(std::move(entry));
      }
    }
  }

  if (config_future.has_value()) {
    const auto config_out = config_future->get();
    if (config_out.has_value()) {
      // `config -z --get-regexp` 的输出：`<key>\n<value>\0` 重复（注意是 `\n`
      // 分键值、`\0` 分条目——不是 `\t`。实测）。
      //
      // 用 `unordered_map` + 名字向量保序：配置里的顺序就是 `git remote` 的顺序
      // （git 按段名排序输出配置），而界面上远程列表的顺序不该随哈希桶跳。
      std::unordered_map<std::string, std::size_t> index_of;
      std::string_view text = *config_out;
      std::size_t cursor = 0;
      const auto ensure_remote = [&](const std::string& name) -> RemoteInfo& {
        const auto found = index_of.find(name);
        if (found != index_of.end()) return snapshot.remotes[found->second];
        index_of.emplace(name, snapshot.remotes.size());
        snapshot.remotes.push_back(RemoteInfo{.name = name});
        return snapshot.remotes.back();
      };
      while (cursor < text.size()) {
        const std::size_t nul = text.find('\0', cursor);
        const std::string_view record =
            nul == std::string_view::npos ? text.substr(cursor) : text.substr(cursor, nul - cursor);
        cursor = nul == std::string_view::npos ? text.size() : nul + 1;
        if (record.empty()) continue;
        const std::size_t newline = record.find('\n');
        if (newline == std::string_view::npos) continue;
        const std::string_view key = record.substr(0, newline);
        const std::string_view value = record.substr(newline + 1);
        // key = `remote.<名>.<字段>`（名里可以含 `.`——所以按**首尾**切，不按 split）。
        constexpr std::string_view kPrefix = "remote.";
        if (!key.starts_with(kPrefix)) continue;
        const std::string_view body = key.substr(kPrefix.size());
        const std::size_t last_dot = body.rfind('.');
        if (last_dot == std::string_view::npos) continue;
        RemoteInfo& remote = ensure_remote(std::string(body.substr(0, last_dot)));
        const std::string_view field = body.substr(last_dot + 1);
        if (field == "url") {
          remote.fetch_url = std::string(value);
        } else if (field == "pushurl") {
          remote.push_url = std::string(value);
        }
      }
      // 未单独配置 pushurl 时回退到拉取 URL（拉/推同 URL 是常态；空串会让界面显示 `→ `）。
      for (RemoteInfo& remote : snapshot.remotes) {
        if (remote.push_url.empty()) remote.push_url = remote.fetch_url;
      }
    }
  }
  return snapshot;
}

auto read_branches(const std::string& cwd) -> std::vector<BranchInfo> {
  return read_ref_parts(cwd, RefPart::Refs).branches;
}

auto read_tags(const std::string& cwd) -> std::vector<TagInfo> {
  return read_ref_parts(cwd, RefPart::Refs).tags;
}

auto read_remotes(const std::string& cwd) -> std::vector<RemoteInfo> {
  return read_ref_parts(cwd, RefPart::Remotes).remotes;
}

auto read_stashes(const std::string& cwd) -> std::vector<StashEntry> {
  return read_ref_parts(cwd, RefPart::Stashes).stashes;
}

auto read_refs(const std::string& cwd) -> std::vector<RefInfo> {
  return read_ref_parts(cwd, RefPart::Refs).refs;
}

auto read_branch_commits(const std::string& cwd, std::string_view ref, std::size_t limit)
    -> std::vector<std::string> {
  std::vector<std::string> hashes;
  if (ref.empty()) return hashes;
  // `--not --all` 不用：要的就是“从该引用可达的全部”（含合并带来的支线）。
  // `--topo-order` 与日志取数同序，方便外面直接做集合比对而不必再排。
  const auto out = git(cwd, {"rev-list", "--topo-order", std::format("-{}", limit),
                             std::string(ref)},
                       nullptr);
  if (!out.has_value()) return hashes;
  for (std::string_view line : lines_of(*out)) {
    if (!line.empty()) hashes.emplace_back(line);
  }
  return hashes;
}

auto read_worktree_diff(const std::string& cwd, const std::string& path, bool staged)    -> std::string {
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

// ————————————————————————————————————————————————————————————————————————————
// 读面：标签 / 远程 / 暂存 / 引用 / 单文件历史 / blame / 多步操作
// ————————————————————————————————————————————————————————————————————————————

auto read_file_history(const std::string& cwd, const std::string& path, std::size_t limit)
    -> std::vector<LogEntry> {
  std::vector<LogEntry> entries;
  if (path.empty()) return entries;
  // `--follow` 跟着重命名走（“这个文件的历史”在重命名后不断）。
  const auto out = git(cwd, {"log", "--follow", "--no-color", std::format("-{}", limit),
                             std::string("--pretty=format:%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%D"),
                             std::string("--date=format:%Y-%m-%d %H:%M"), "--", path});
  if (!out.has_value()) return entries;
  // 解析与 `parse_log_entries` **同一份口径**（字段拆分/短哈希/refs 胶囊），
  // 只是不带 `%P`（单文件历史不需要拓扑）。
  return parse_log_entries(*out);
}

namespace {

/// 把 `author-time`（unix 秒）转成 `YYYY-MM-DD`（blame 注解只要日期）。
///
/// 为何在这里自己格式化：`git blame` **不认 `--date=` 格式**（porcelain 固定给
/// 时间戳）——而“日期串为空”会让注解只剩 hash。而 `--porcelain` 又是**同 commit
/// 只给一次元信息**的形式，所以必须自己把时间戳缓存下来复用（见 `read_blame`）。
[[nodiscard]] auto blame_date_of(std::int64_t seconds) -> std::string {
  const std::string iso = st::time::iso8601_utc(seconds * 1000);
  return iso.size() >= 10 ? iso.substr(0, 10) : std::string{};
}

}  // namespace

auto read_blame(const std::string& cwd, const std::string& rev, const std::string& path,
                std::size_t start, std::size_t count) -> std::vector<BlameLine> {
  std::vector<BlameLine> lines;
  if (path.empty()) return lines;
  // ⚠ **`git blame` 没有 `--no-color`**：它是 `git log` 组的选项，传给 blame 会被当成
  // `--no-color-…` 的歧义缩写而**整条命令失败**（实测：git 报 ambiguous option，
  // 输出全空，调用方拿到空表——看着像“这个文件没有 blame 信息”）。
  // blame 输出本来就无颜色，不需要那个选项。
  //
  // ## 为何是 `--porcelain` 而不是 `--line-porcelain`（2026-11-22 性能）
  //
  // `--line-porcelain` 对**每一行**都重发一整套元信息（author/mail/time/tz/
  // committer*/summary/filename），那些东西在同一个 commit 上是**完全相同**的。
  // 实测本仓 `git_service.cpp` 整篇：`--porcelain` 109 KB / `--line-porcelain`
  // 549 KB——**5 倍**。而 blame 是同步子进程 + 管道读取，多出来的 440 KB 全部
  // 是白搬的。`--porcelain` 的形式是：块首行给 `<hash> <原行> <终行> [组行数]`，
  // 元信息**只在该 commit 第一次出现时**输出，后续同 commit 的块直接是首行 + 内容。
  // 所以解析要按“记住每个 hash 的元信息”来做（下面 `meta_of`）。
  std::vector<std::string> args{"blame", "-w", "--porcelain"};
  if (!rev.empty()) args.push_back(rev);
  // `-L a,+b` 只算指定区间（大文件里给编辑器取当前视口就够——不必整篇 blame）。
  if (start > 0 && count > 0) {
    args.push_back(std::format("-L{},+{}", start, count));
  } else if (start > 1) {
    args.push_back(std::format("-L{},+999999", start));
  }
  args.push_back("--");
  args.push_back(path);
  const auto out = git(cwd, args);
  if (!out.has_value()) return lines;

  /// 一个 commit 的（作者, 日期）——`--porcelain` 只给一次，块之间要复用。
  struct BlameMeta {
    std::string author{};
    std::string date{};
  };
  std::unordered_map<std::string, BlameMeta> meta_of;
  /// 当前正在解析的那个 commit（元信息行属于它）。
  std::string current_hash;
  /// 当前块的（作者, 日期）：要么来自本次新解析的元信息，要么查缓存。
  BlameMeta current;

  std::size_t line_no = start > 0 ? start : 1;
  for (const std::string_view raw : lines_of(*out)) {
    if (raw.empty()) continue;
    // 内容行：以 `\t` 开头（porcelain 的硬契约——正文可能以任何字符开头，
    // 所以 git 用前导 tab 把内容与元信息分开）。
    if (raw.front() == '\t') {
      BlameLine line;
      line.line = line_no++;
      line.hash = current_hash.substr(0, 7);   // 界面只要 7 位
      line.author = current.author;
      line.date = current.date;
      line.content = std::string(raw.substr(1));
      lines.push_back(std::move(line));
      continue;
    }
    // 块首行：`<40位 hash> <原行> <终行> [组行数]`。
    //
    // 判据是“第 40 个字符是空格”而不是“长度 > 40”：`--porcelain` 的 hash 恒 40 位，
    // 而元信息行（`author t`）远短于 40——两者不会混淆。
    if (raw.size() > 40 && raw[40] == ' ') {
      current_hash.assign(raw.substr(0, 40));
      // 该 commit 之前出现过 ⇒ 直接复用元信息（不再读下面那几行——它们也不会再来）。
      if (const auto cached = meta_of.find(current_hash); cached != meta_of.end()) {
        current = cached->second;
      } else {
        current = BlameMeta{};
      }
      continue;
    }
    if (raw.starts_with("author ")) {
      current.author = std::string(raw.substr(7));
    } else if (raw.starts_with("author-time ")) {
      // `std::stoll` 在非法输入上是**抛异常**——而 porcelain 是我们自己的解析路径，
      // 抛出去会穿过 `read_blame`（它的调用方没准备接）。这里用 `strtoll` 并校验。
      const std::string text(raw.substr(12));
      char* end = nullptr;
      const long long seconds = std::strtoll(text.c_str(), &end, 10);
      if (end != text.c_str()) current.date = blame_date_of(static_cast<std::int64_t>(seconds));
    } else if (raw.starts_with("filename ")) {
      // 元信息到此结束（`filename` 是 porcelain 每个块元信息的最后一行）：
      // 存进缓存供后续同 commit 的块复用。
      if (!current_hash.empty()) meta_of.insert_or_assign(current_hash, current);
    }
  }
  return lines;
}

namespace {

/// MSYS 路径（`/c/foo`）→ 磁盘路径（`C:\foo`）。
///
/// Windows 上 `rev-parse --show-toplevel` 给的是前者（`--absolute-git-dir` 亦然），
/// 拼磁盘路径前必须换回来——否则 `st::fs::join(root, "a/b")` 会得到一个
/// 以 `/c/...` 开头的字符串，那是**不存在的路径**（`read_content_at` 的
/// “工作区端点”就是拼磁盘路径读的，实测表现是“文件读不到 ⇒ 判成删除文件”）。
[[nodiscard]] auto normalize_git_path(std::string path) -> std::string {
  if (path.size() >= 3 && path[0] == '/' && path[2] == '/') {
    path = std::string(1, static_cast<char>(path[1])) + ":" + path.substr(2);
    std::ranges::replace(path, '/', '\\');
  }
  return path;
}

}  // namespace

auto read_git_dirs(const std::string& cwd) -> GitDirs {
  // 一条命令两个答案：`rev-parse` 支持多参数、按参数顺序逐行输出。
  const auto out = git(cwd, {"rev-parse", "--show-toplevel", "--absolute-git-dir"});
  GitDirs dirs;
  if (!out.has_value()) return dirs;
  std::size_t line = 0;
  for (std::string_view raw : lines_of(*out)) {
    if (raw.empty()) continue;
    if (line == 0) {
      dirs.root = normalize_git_path(std::string(raw));
    } else if (line == 1) {
      dirs.git_dir = normalize_git_path(std::string(raw));
      break;
    }
    ++line;
  }
  return dirs;
}

auto read_operation_state(const std::string& cwd) -> OperationState {
  return read_operation_state_at(read_git_dirs(cwd).git_dir, cwd);
}

auto read_operation_state_at(const std::string& git_dir, const std::string& cwd) -> OperationState {
  OperationState state;
  // `git_dir` 为空 = 不是仓库：没有多步操作状态可言（也**不能**去探 `.git`，
  // 那会在普通目录上凭空造出目录）。
  if (git_dir.empty()) return state;
  const auto exists = [&git_dir](std::string_view relative) {
    return st::fs::exists(st::fs::join(git_dir, std::string(relative)));
  };
  // 判据落 **`.git` 下的状态文件**（不解析 git 的输出文案：文案会随版本变，
  // 而状态文件的存在性是硬契约）。几条 `exists` 是**文件系统调用**（µs 级），
  // 不离子进程；所以这里零子进程就能回答“在不在多步操作中”。
  if (exists("rebase-merge")) {
    state.kind = "rebase";
  } else if (exists("rebase-apply")) {
    state.kind = exists("rebase-apply/rebasing") ? "rebase" : "am";
  } else if (exists("MERGE_HEAD")) {
    state.kind = "merge";
  } else if (exists("CHERRY_PICK_HEAD")) {
    state.kind = "cherry-pick";
  } else if (exists("REVERT_HEAD")) {
    state.kind = "revert";
  } else if (exists("BISECT_LOG")) {
    state.kind = "bisect";
  }
  if (!state.kind.empty()) {
    if (const auto conflicts = git(cwd, {"diff", "--name-only", "--diff-filter=U"})) {
      for (const std::string_view line : lines_of(*conflicts)) {
        if (!line.empty()) state.conflicts.emplace_back(line);
      }
    }
  }
  return state;
}

auto read_upstream(const std::string& cwd) -> std::string {
  const auto out = git(cwd, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"});
  if (!out.has_value()) return {};
  return std::string(st::trim(*out));
}

auto read_repo_root(const std::string& cwd) -> std::string {
  // git 自己会向上找仓库，所以这里传“用户打开的目录”就对了。
  return read_git_dirs(cwd).root;
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

auto read_conflict_sides(const std::string& cwd, const std::string& path) -> ConflictSides {
  ConflictSides sides;
  if (path.empty()) {
    sides.base.error = "路径为空";
    sides.ours.error = sides.base.error;
    sides.theirs.error = sides.base.error;
    return sides;
  }
  // 三份各自取（不复用 `read_content_at`：它的端点模型里没有“第 N 号暂存条目”
  // 这个概念——硬塞一个伪端点会让那个接口多一个只有此处用的分支）。
  const auto read_stage = [&cwd, &path](int stage) -> ContentAt {
    ContentAt side;
    const std::string spec = std::format(":{}:{}", stage, path);
    const auto out = git(cwd, {"show", "--no-color", spec}, &side.error);
    if (!out.has_value()) {
      // “这一号不存在”不是错误：非冲突文件只有 `:0:`，而删除/新增冲突会缺某一号。
      // 与 `read_content_at` 同口径（`missing = true` 且清空 error），
      // 否则调用方无法把“本来就没有”与“取数失败”分开。
      side.missing = true;
      side.error.clear();
      return side;
    }
    side.text = *out;
    side.size = side.text.size();
    side.binary = side.text.find('\0') != std::string::npos;
    side.ok = true;
    return side;
  };
  sides.base = read_stage(1);
  sides.ours = read_stage(2);
  sides.theirs = read_stage(3);
  return sides;
}

// ————————————————————————————————————————————————————————————————————————————
// 写面：命令构造 / 危险度 / 备份 / 多步控制 / 审计
// ————————————————————————————————————————————————————————————————————————————

auto write_args(const WriteIntent& intent) -> std::optional<std::vector<std::string>> {
  // 必填参数缺失就如实返回 nullopt：构造一条“看起来对但删错东西”的命令
  // 比直接拒绝危险得多（例如 reset --hard 的 ref 为空会变成 `git reset --hard`
  // ＝丢掉全部工作区改动）。
  const auto need = [](const std::string& value) { return !value.empty(); };
  switch (intent.op) {
    case WriteOp::Stage:
      if (!need(intent.path)) return std::nullopt;
      return std::vector<std::string>{"add", "--", intent.path};
    case WriteOp::Unstage:
      if (!need(intent.path)) return std::nullopt;
      return std::vector<std::string>{"restore", "--staged", "--", intent.path};
    case WriteOp::DiscardOne:
      if (!need(intent.path)) return std::nullopt;
      return std::vector<std::string>{"restore", "--", intent.path};
    case WriteOp::DiscardAll:
      return std::vector<std::string>{"restore", "--", "."};
    case WriteOp::Commit: {
      if (!need(intent.message)) return std::nullopt;
      std::vector<std::string> args{"commit", "-m", intent.message};
      if (intent.amend) args.push_back("--amend");
      if (intent.allow_empty) args.push_back("--allow-empty");
      if (need(intent.author)) {
        args.push_back("--author");
        args.push_back(intent.author);
      }
      return args;
    }
    case WriteOp::Amend:
      return std::vector<std::string>{"commit", "--amend", "--no-edit"};
    case WriteOp::Checkout:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"checkout", intent.ref};
    case WriteOp::CheckoutNew: {
      if (!need(intent.name)) return std::nullopt;
      std::vector<std::string> args{"checkout", "-b", intent.name};
      if (need(intent.ref)) args.push_back(intent.ref);
      return args;
    }
    case WriteOp::DeleteBranch: {
      if (!need(intent.name)) return std::nullopt;
      // `-d` vs `-D`：前者拒删未合并分支（安全的默认），后者强制。
      return std::vector<std::string>{"branch", intent.hard ? "-D" : "-d", intent.name};
    }
    case WriteOp::Merge:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"merge", "--no-edit", intent.ref};
    case WriteOp::Rebase:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"rebase", intent.ref};
    case WriteOp::CherryPick:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"cherry-pick", intent.ref};
    case WriteOp::Revert:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"revert", "--no-edit", intent.ref};
    case WriteOp::ResetSoft:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"reset", "--soft", intent.ref};
    case WriteOp::ResetMixed:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"reset", intent.ref};
    case WriteOp::ResetHard:
      if (!need(intent.ref)) return std::nullopt;
      return std::vector<std::string>{"reset", "--hard", intent.ref};
    case WriteOp::TagCreate: {
      if (!need(intent.name)) return std::nullopt;
      std::vector<std::string> args{"tag"};
      if (intent.annotated) {
        args.push_back("-a");
        args.push_back(intent.name);
        args.push_back("-m");
        args.push_back(need(intent.message) ? intent.message : intent.name);
      } else {
        args.push_back(intent.name);
      }
      if (need(intent.ref)) args.push_back(intent.ref);
      return args;
    }
    case WriteOp::TagDelete:
      if (!need(intent.name)) return std::nullopt;
      return std::vector<std::string>{"tag", "-d", intent.name};
    case WriteOp::StashPush: {
      std::vector<std::string> args{"stash", "push"};
      if (need(intent.message)) {
        args.push_back("-m");
        args.push_back(intent.message);
      }
      return args;
    }
    case WriteOp::StashPop:
      return std::vector<std::string>{"stash", "pop",
                                      need(intent.ref) ? intent.ref : "stash@{0}"};
    case WriteOp::StashApply:
      return std::vector<std::string>{"stash", "apply",
                                      need(intent.ref) ? intent.ref : "stash@{0}"};
    case WriteOp::StashDrop:
      return std::vector<std::string>{"stash", "drop",
                                      need(intent.ref) ? intent.ref : "stash@{0}"};
    case WriteOp::Fetch:
      return std::vector<std::string>{"fetch", "--all", "--prune"};
    case WriteOp::Pull:
      // `--ff-only`：不制造意外 merge（IDEA 的 Update Project 默认也是 fast-forward 优先）。
      return std::vector<std::string>{"pull", "--ff-only"};
    case WriteOp::Push: {
      std::vector<std::string> args{"push"};
      if (need(intent.name)) {
        args.push_back("-u");
        args.push_back(intent.name);
      }
      return args;
    }
    case WriteOp::PushForceWithLease:
      // **不用 `--force`**：无 lease 的强推会盖掉别人的提交，而 lease 会在远端
      // 已经前进时**拒推**（这才是想要的安全网）。
      return std::vector<std::string>{"push", "--force-with-lease"};
    case WriteOp::Continue:
    case WriteOp::Skip:
    case WriteOp::Abort: {
      if (!need(intent.ref)) return std::nullopt;   // `ref` 位传操作种类（merge/rebase/…）
      const std::string action = intent.op == WriteOp::Continue ? "--continue"
                                : intent.op == WriteOp::Skip   ? "--skip"
                                                               : "--abort";
      return control_args(intent.ref, action);
    }
  }
  return std::nullopt;
}

auto danger_of(const WriteIntent& intent) -> Danger {
  switch (intent.op) {
    // 不可逆：改工作区/历史且没有内置回退。
    case WriteOp::DiscardOne:
    case WriteOp::DiscardAll:
    case WriteOp::ResetHard:
    case WriteOp::Rebase:
      return Danger::Backup;
    // 需确认：可吸收/可重来，但会造成不小的影响。
    case WriteOp::Amend:
    case WriteOp::DeleteBranch:
    case WriteOp::PushForceWithLease:
    case WriteOp::StashDrop:
    case WriteOp::TagDelete:
    case WriteOp::CherryPick:
    case WriteOp::Revert:
    case WriteOp::Merge:
    case WriteOp::ResetMixed:
    case WriteOp::ResetSoft:
      return Danger::Confirm;
    default:
      return Danger::None;
  }
}

auto needs_backup(const WriteIntent& intent) -> bool {
  return danger_of(intent) == Danger::Backup;
}

auto backup_branch_name(const std::string& cwd) -> std::string {
  const std::string iso = st::time::iso8601_utc(st::time::now_ms());
  std::string date = iso.substr(0, 10);
  date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
  const auto head = git(cwd, {"rev-parse", "--short=7", "HEAD"});
  const std::string hash = head.has_value() ? std::string(st::trim(*head)) : "unknown";
  // 重名时加序号（同一分钟内做两次危险操作是常见的）。
  std::string name = std::format("backup/{}-{}", date, hash);
  int suffix = 2;
  while (git(cwd, {"rev-parse", "--verify", "--quiet", "refs/heads/" + name}).has_value()) {
    name = std::format("backup/{}-{}-{}", date, hash, suffix++);
  }
  return name;
}

auto shell_quote(std::string_view argument) -> std::string {
  // 安全字符集之外一律用单引号包裹；`'` 写成 `'\''`（POSIX sh 的通行写法）。
  const bool safe = !argument.empty() &&
                    std::ranges::all_of(argument, [](char ch) {
                      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' || ch == '_' ||
                             ch == '-' || ch == '=' || ch == ':' || ch == '@' || ch == ',' ||
                             ch == '+' || ch == '%' || ch == '^' || ch == '~';
                    });
  if (safe) return std::string(argument);
  std::string out;
  out.reserve(argument.size() + 2);
  out += '\'';
  for (const char ch : argument) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out += ch;
    }
  }
  out += '\'';
  return out;
}

auto to_shell_line(const std::vector<std::string>& args) -> std::string {
  std::string out = "git";
  for (const std::string& arg : args) {
    out += ' ';
    out += shell_quote(arg);
  }
  return out;
}

auto to_display_line(const std::vector<std::string>& args) -> std::string {
  // 展示形式不转义（人要读），但把换行拉平——提交信息是多行的。
  std::string out = "git";
  for (const std::string& arg : args) {
    out += ' ';
    std::string flat = arg;
    std::ranges::replace(flat, '\n', ' ');
    out += flat;
  }
  return out;
}

auto control_args(std::string_view kind, std::string_view action)
    -> std::optional<std::vector<std::string>> {
  std::string command(kind);
  if (command == "cherry-pick" || command == "revert" || command == "merge" ||
      command == "rebase" || command == "am" || command == "bisect") {
    return std::vector<std::string>{command, std::string(action)};
  }
  return std::nullopt;
}

auto append_audit(const std::string& cwd, std::string_view action,
                  const std::vector<std::string>& args, std::string_view result) -> bool {
  // ⚠ 不能直接写 `<cwd>/.git/…`：`fs::append_text` 会**自动建父目录**，
  // 于是一个非仓库目录会被我们凭空造出 `.git/`——而 `.git` 的存在会让 git
  // 之后把该目录**当成一个仓库**（实测：在临时目录上写审计后 `git status` 变了脸）。
  // 正确做法是先问 git 真实的 git 目录（`--absolute-git-dir` 顺带支持
  // worktree/submodule 那种 `.git` 是**文件**的形态）。
  const auto git_dir = git(cwd, {"rev-parse", "--absolute-git-dir"});
  if (!git_dir.has_value()) return false;
  const std::string dir = std::string(st::trim(*git_dir));
  if (dir.empty()) return false;
  const std::string path = st::fs::join(dir, "st-audit.jsonl");
  const std::string line =
      std::format("{{\"time\":\"{}\",\"action\":\"{}\",\"command\":\"{}\","
                  "\"result\":\"{}\"}}\n",
                  st::time::iso8601_now(), action, to_display_line(args), result);
  // 写失败不是错误（审计不能成为操作失败的原因）——只如实返回结果。
  return st::fs::append_text(path, line).has_value();
}

auto read_audit(const std::string& cwd, std::size_t limit) -> std::vector<std::string> {
  // 单独调时自己问一次 `.git` 在哪（批量路径用 `read_audit_at` 复用已取到的结果，
  // 省一条 `rev-parse`——它在 46 ms 量级，而批量路径已经有那份值了）。
  return read_audit_at(read_git_dirs(cwd).git_dir, limit);
}

auto read_audit_at(const std::string& git_dir, std::size_t limit) -> std::vector<std::string> {
  if (git_dir.empty()) return {};
  const std::string path = st::fs::join(git_dir, "st-audit.jsonl");
  if (limit == 0) return {};
  // **只读文件尾部**：审计是只追加的 JSONL，而界面只要最后 N 条。
  //
  // 旧实现 `read_text` 整文件进来再按行切——审计文件是**只增不减**的
  // （每次写操作追加一条），跑上几个月就是几 MB，而每轮刷新都读一遍全文。
  // 尾部窗口取 64 KB：一条审计约 200 字节，20 条约 4 KB，给 16 倍余量
  // （带长提交信息的命令能到几百字节一条）。
  constexpr std::size_t kTailBytes = 64 * 1024;
  std::string text;
  if (const auto size = st::fs::file_size(path); size.has_value() && *size > kTailBytes) {
    const auto tail = st::fs::read_span(path, *size - kTailBytes, kTailBytes);
    if (!tail.has_value()) return {};
    text.assign(tail->begin(), tail->end());
    // 窗口首部那半条是被截断的记录：丢掉（半条 JSON 解析不了，
    // 而“最近 N 条里有半条坏行”会被上层当成“审计坏了”）。
    if (const std::size_t eol = text.find('\n'); eol != std::string::npos) {
      text.erase(0, eol + 1);
    } else {
      text.clear();   // 整段无换行 = 只有一条超长记录且被截断：没得用
    }
  } else {
    const auto all = st::fs::read_text(path);
    if (!all.has_value()) return {};
    text = *all;
  }

  // 从**尾部**往前找 N 条（旧实现扫全文、逐行拷贝 `std::string`，再切片）。
  std::vector<std::string> lines;
  lines.reserve(limit);
  std::size_t end = text.size();
  while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
  while (end > 0 && lines.size() < limit) {
    const std::size_t nl = text.rfind('\n', end - 1);
    const std::size_t begin = nl == std::string::npos ? 0 : nl + 1;
    std::string_view line(text.data() + begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) lines.emplace_back(line);
    if (nl == std::string::npos) break;
    end = nl;
  }
  // 上面是“最新在前”；契约是“最旧在前”，反转一次。
  std::reverse(lines.begin(), lines.end());
  return lines;
}

auto read_log_snapshot(const std::string& cwd) -> LogSnapshot {
  LogSnapshot snapshot;
  // ## 五条子进程、三条并发（2026-11-22）
  //
  // 旧实现是**12 条串行**：log、rev-list（同一段历史读两遍）、branch、tags、
  // refs、remote + 每个 remote 两条 get-url、stash、upstream、operation、audit。
  // 实测本仓：单条 46~99 ms，整批 500 ms 量级（大仓实测 3.3 s）。
  //
  // 现在：
  //   · log + rev-list 合并成一条（`read_log_and_graph`，`%P` 就是父列表）；
  //   · branch + tags + refs 合并成一条（一条 `for-each-ref`）；
  //   · remote + 每个 get-url 合并成一条（`config --get-regexp`）；
  //   · upstream 不再单独读（分支列表里的 `%(upstream:short)` 给了**当前分支**的
  //     上游——与 `rev-parse @{upstream}` 等价）；
  //   · 剩下三条（日志族 / 引用族 / 操作态+审计）**互不依赖** ⇒ 并发跑。
  //
  // 并发宽度取 3：这是“无依赖的三件事”，而不是“把一件事拆成 N 片”。
  // 再拆没有意义（子进程各起一次，粒度不再细），而多开线程只会让 git 自己的
  // index 锁竞争变多（`--no-optional-locks` 只挡了 status 的写）。
  //
  // ⚠ 这里**不能用应用层的 `st::app::Tasks`**：`read_log_snapshot` 是数据层，
  // 它既要能被后台线程调、也要能被测试直接调（没有 Application 上下文可用）。
  // 用 `std::async` 配 `std::launch::async`——**必须显式带这个 launch 策略**：
  // 默认策略允许延迟执行（`deferred`），那就在这里退化成完全串行。
  auto log_future =
      std::async(std::launch::async, [cwd] { return read_log_and_graph(cwd, 200); });
  auto refs_future = std::async(std::launch::async, [cwd] { return read_ref_snapshot(cwd); });
  auto state_future = std::async(std::launch::async, [cwd] {
    struct Extra {
      OperationState operation{};
      std::vector<std::string> audit{};
    };
    Extra extra;
    // 一条 `rev-parse` 同时拿仓库根与 `.git` 位置，两者都喂给后面两路
    //（旧写法两条 `rev-parse` 各问一次同一个问题，白花 ~46 ms）。
    const GitDirs dirs = read_git_dirs(cwd);
    extra.operation = read_operation_state_at(dirs.git_dir, cwd);
    extra.audit = read_audit_at(dirs.git_dir, 20);
    return extra;
  });

  LogAndGraph log_and_graph = log_future.get();
  snapshot.log = std::move(log_and_graph.log);
  snapshot.graph = std::move(log_and_graph.graph);

  RefSnapshot refs = refs_future.get();
  snapshot.branches = std::move(refs.branches);
  snapshot.tags = std::move(refs.tags);
  snapshot.remotes = std::move(refs.remotes);
  snapshot.stashes = std::move(refs.stashes);
  snapshot.refs = std::move(refs.refs);

  // 上游：从分支列表里找当前分支（远轻于再起一条 `rev-parse @{upstream}`）。
  for (const BranchInfo& branch : snapshot.branches) {
    if (branch.is_current) {
      snapshot.upstream = branch.upstream_short;
      break;
    }
  }

  auto extra = state_future.get();
  snapshot.operation = std::move(extra.operation);
  snapshot.audit = std::move(extra.audit);
  return snapshot;
}

}  // namespace gbcode
