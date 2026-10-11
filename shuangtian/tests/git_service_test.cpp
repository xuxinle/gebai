/// Git 服务层数据测试（版本管理重构）：porcelain 分组语义 / 日志解析 / 分支解析。
///
/// **真仓库测试**（临时目录里真跑 git init/commit）——分组判据是 XY 矩阵的
/// 语义问题，喂假数据测不出「X=Y=非空时两边都出现」这种坑：只有真仓库能
/// 制造出「暂存过、又改了」的真实状态。git 不可用时跳过（与 LSP 真机冒烟同一策略）。

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "st/core/process.hpp"
#include "st/test/test.hpp"
#include "st/text/diff.hpp"
// git_service 是 gbcode 的应用层源（不在框架 sources 清单里）——测试目标
// 直接编译它的实现：include 源文件（单文件、无内部依赖，比改清单加钩子干净）。
#include "examples/gbcode/git_service.hpp"

namespace {

using gbcode::ChangeGroup;

/// 进程内唯一序号（每个临时目录取一个）——见 `Repo` 的注释。
// lint-allow: L8 测试夹具的进程内计数器：必须跨用例共享（每个用例各取一个序号），
// 作用域限于本测试二进制（不跨进程、不跨用例泄漏状态）。
[[nodiscard]] auto next_serial() -> std::size_t {
  static std::atomic<std::size_t> counter{0};
  return counter.fetch_add(1);
}

/// 临时仓库（RAII 清理）。
///
/// 目录名必须**同时**带进程 id 与进程内序号：只用 `current_id()`（＝pid）时，
/// 同一个测试二进制里的两个用例是同一个 pid ⇒ **同一个目录**，分片并行
/// （`--test-jobs N`，用例在线程里并行）就会互相写同一个仓库、互相 `commit`，
/// 表现为「日志条数比预期多一条」这种看着像被测代码出错的红（实测命中
/// `git_service_parses_log_and_detail`，`entries.size() == 3 != 2`）。
struct Repo {
  std::filesystem::path dir{};
  bool ok{false};

  explicit Repo() {
    dir = std::filesystem::temp_directory_path() /
          std::format("st_git_svc_{}_{}", static_cast<int>(st::process::current_id()),
                      next_serial());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    if (!run({"init", "--initial-branch=main"})) return;
    run({"config", "user.email", "test@local"});
    run({"config", "user.name", "Tester"});
    write("a.txt", "line1\n");
    write("b.txt", "bee\n");
    run({"add", "."});
    run({"commit", "-m", "first"});
    ok = true;
  }
  ~Repo() {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }

  auto run(const std::vector<std::string>& args) const -> bool {
    st::process::Options options;
    options.cwd = dir.string();
    const auto result = st::process::run("git", args, options);
    return result.has_value() && result->exit_code == 0;
  }

  void write(const std::string& name, const std::string& content) const {
    std::filesystem::path path = dir / name;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path);
    out << content;
  }

  [[nodiscard]] auto path() const -> std::string { return dir.string(); }
};

[[nodiscard]] auto git_available() -> bool {
  return st::process::run("git", {"--version"}).has_value();
}

}  // namespace

#include "examples/gbcode/git_service.cpp"

ST_TEST(git_service_groups_by_xy_matrix) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);

  // ① 未跟踪：新文件。
  repo.write("new.txt", "n");
  // ② 已暂存：改 a.txt 后 add。
  repo.write("a.txt", "line1\nline2\n");
  repo.run({"add", "a.txt"});
  // ③ 只改工作区：改 b.txt 不 add。
  repo.write("b.txt", "bee2\n");
  // ④ 既暂存又改：c.txt 先 add 再改。
  repo.write("c.txt", "staged\n");
  repo.run({"add", "c.txt"});
  repo.write("c.txt", "staged\nmore\n");

  const auto snapshot = gbcode::read_snapshot(repo.path());
  ST_CHECK(snapshot.is_repo);
  ST_CHECK_EQ(snapshot.branch, std::string("main"));
  ST_CHECK(snapshot.error.empty());

  // 分组计数。
  ST_CHECK_EQ(snapshot.count_in(ChangeGroup::Untracked), std::size_t{1});
  ST_CHECK_EQ(snapshot.count_in(ChangeGroup::Staged), std::size_t{2});      // a + c
  ST_CHECK_EQ(snapshot.count_in(ChangeGroup::Unstaged), std::size_t{2});    // b + c（工作侧）
  ST_CHECK_EQ(snapshot.count_in(ChangeGroup::Conflicted), std::size_t{0});

  // 「两边都出现」：c 在两组各有一份（这是工作台那边实测过的坑——只按 X 或只按 Y
  // 分组都会丢另一半信息）。
  const auto staged = snapshot.changes_in(ChangeGroup::Staged);
  const auto unstaged = snapshot.changes_in(ChangeGroup::Unstaged);
  bool c_in_staged = false;
  bool c_in_unstaged = false;
  for (const auto& change : staged) {
    if (change.path == "c.txt") c_in_staged = true;
  }
  for (const auto& change : unstaged) {
    if (change.path == "c.txt") c_in_unstaged = true;
  }
  ST_CHECK(c_in_staged);
  ST_CHECK(c_in_unstaged);

  // 状态字母。
  for (const auto& change : snapshot.changes) {
    if (change.path == "new.txt") ST_CHECK_EQ(change.display_code(), '?');
    if (change.path == "b.txt") ST_CHECK_EQ(change.display_code(), 'M');
  }
}

ST_TEST(git_service_reads_branch_and_upstream) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  const auto snapshot = gbcode::read_snapshot(repo.path());
  ST_CHECK(snapshot.is_repo);
  // 无远程：upstream 空、ahead/behind 为 0。
  ST_CHECK(snapshot.upstream.empty());
  ST_CHECK_EQ(snapshot.ahead, std::size_t{0});
  ST_CHECK_EQ(snapshot.behind, std::size_t{0});
  // 干净仓库：无变更。
  ST_CHECK(snapshot.changes.empty());

  // 分支列表：只有 main、是当前分支。
  const auto branches = gbcode::read_branches(repo.path());
  ST_REQUIRE(!branches.empty());
  ST_CHECK_EQ(branches.front().name, std::string("main"));
  ST_CHECK(branches.front().is_current);
  ST_CHECK(!branches.front().is_remote);
}

ST_TEST(git_service_not_a_repo_is_state_not_error) {
  // 普通目录（没 git init）：`is_repo=false` 且**不是故障**（error 空）——
  // 面板要显示「不是仓库」的空态而不是可重试的故障态，两者语义不同
  //（工作台设计 §4.7.H 的约定）。
  const auto dir = std::filesystem::temp_directory_path() /
                   std::format("st_git_plain_{}_{}", static_cast<int>(st::process::current_id()),
                               next_serial());
  std::filesystem::create_directories(dir);
  const auto snapshot = gbcode::read_snapshot(dir.string());
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  ST_CHECK(!snapshot.is_repo);
  ST_CHECK(snapshot.error.empty());
  ST_CHECK(snapshot.changes.empty());
}

ST_TEST(git_service_parses_log_and_detail) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 再来一笔提交（含多文件与中文）。
  repo.write("a.txt", "line1\nline2\nline3\n");
  repo.write("中文.md", "# 标题\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "second：加行与中文文件"});

  const auto entries = gbcode::read_log(repo.path(), 10);
  ST_CHECK_EQ(entries.size(), std::size_t{2});
  const auto& latest = entries.front();
  ST_CHECK_EQ(latest.subject, std::string("second：加行与中文文件"));
  ST_CHECK(!latest.short_hash.empty());
  ST_CHECK_EQ(latest.short_hash.size(), std::size_t{7});
  ST_CHECK(!latest.author.empty());
  // HEAD ref（首条应有 HEAD -> main）。
  bool has_head_ref = false;
  for (const auto& ref : latest.refs) {
    if (ref.find("HEAD") != std::string::npos) has_head_ref = true;
  }
  ST_CHECK(has_head_ref);

  // 详情：numstat 与 diff 文本。
  auto copy = latest;
  const std::string diff = gbcode::read_commit_detail(repo.path(), copy);
  ST_CHECK(!diff.empty());
  ST_CHECK(diff.find("+line3") != std::string::npos);
  bool saw_chinese = false;
  std::size_t chinese_added = 0;
  for (const auto& [path, added, removed] : copy.files) {
    if (path == "中文.md") {
      saw_chinese = true;
      chinese_added = added;
    }
  }
  ST_CHECK(saw_chinese);
  ST_CHECK(chinese_added > 0);
}

ST_TEST(git_service_worktree_diff_by_staged_side) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "line1\nSTAGED\n");
  repo.run({"add", "a.txt"});
  repo.write("a.txt", "line1\nSTAGED\nWORKTREE\n");

  // 暂存侧 diff：只有 STAGED。
  const auto staged_diff = gbcode::read_worktree_diff(repo.path(), "a.txt", true);
  ST_CHECK(staged_diff.find("+STAGED") != std::string::npos);
  ST_CHECK(staged_diff.find("WORKTREE") == std::string::npos);
  // 工作侧 diff：有 WORKTREE。
  const auto work_diff = gbcode::read_worktree_diff(repo.path(), "a.txt", false);
  ST_CHECK(work_diff.find("+WORKTREE") != std::string::npos);
}

// ————————————————————————————————————————————————————————————————————————————
// 任意两端对比（参照歌白文件工作台的端点模型）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(git_service_endpoint_parse_round_trip) {
  // 保留字大小写不敏感；其余一律当 rev **透传**（不做第二套 rev 语法）。
  ST_CHECK(gbcode::DiffEndpoint::parse("WORKTREE").is_worktree());
  ST_CHECK(gbcode::DiffEndpoint::parse(" worktree ").is_worktree());
  ST_CHECK(gbcode::DiffEndpoint::parse("INDEX").is_index());
  ST_CHECK(gbcode::DiffEndpoint::parse("staged").is_index());
  ST_CHECK(gbcode::DiffEndpoint::parse("").empty());
  const auto rev = gbcode::DiffEndpoint::parse("HEAD~2");
  ST_CHECK(rev.kind == gbcode::DiffEnd::Revision);
  ST_CHECK_EQ(rev.revision, std::string("HEAD~2"));
  ST_CHECK_EQ(rev.to_text(), std::string("HEAD~2"));
  // `main@{1}`、`A^` 这类复杂 rev 原样保留。
  ST_CHECK_EQ(gbcode::DiffEndpoint::parse("main@{1}").revision, std::string("main@{1}"));
}

ST_TEST(git_service_diff_args_semantic_table) {
  using gbcode::DiffEndpoint;
  const auto worktree = DiffEndpoint::parse("WORKTREE");
  const auto index = DiffEndpoint::parse("INDEX");
  const auto empty = DiffEndpoint{};
  const auto head1 = DiffEndpoint::parse("HEAD~1");
  const auto head = DiffEndpoint::parse("HEAD");
  const auto args = [](const DiffEndpoint& a, const DiffEndpoint& b, bool three) {
    const auto mapped = gbcode::diff_args(a, b, three);
    return mapped.has_value() ? *mapped : std::vector<std::string>{};
  };
  const auto unsupported = [](const DiffEndpoint& a, const DiffEndpoint& b) {
    return !gbcode::diff_args(a, b, false).has_value();
  };

  // 空 + 空 = 工作区 ↔ 暂存区（`git diff` 的裸语义）。
  ST_CHECK(args(empty, empty, false).empty());
  ST_CHECK(args(worktree, empty, false).empty());
  // 已暂存：`--cached`（三种写法等价）。
  ST_CHECK_EQ(args(empty, index, false).size(), std::size_t{1});
  ST_CHECK_EQ(args(empty, index, false).front(), std::string("--cached"));
  ST_CHECK_EQ(args(worktree, index, false).front(), std::string("--cached"));
  ST_CHECK_EQ(args(index, empty, false).front(), std::string("--cached"));
  // 空 → rev：`git diff REF`（工作区 ↔ 该 rev）。
  ST_CHECK_EQ(args(empty, head1, false).front(), std::string("HEAD~1"));
  ST_CHECK_EQ(args(empty, head1, false).size(), std::size_t{1});
  // rev ↔ 工作区。
  ST_CHECK_EQ(args(head1, empty, false).front(), std::string("HEAD~1"));
  ST_CHECK_EQ(args(head1, worktree, false).front(), std::string("HEAD~1"));
  // rev ↔ 暂存区。
  const auto to_index = args(head1, index, false);
  ST_REQUIRE(to_index.size() == 2);
  ST_CHECK_EQ(to_index[0], std::string("--cached"));
  ST_CHECK_EQ(to_index[1], std::string("HEAD~1"));
  // rev ↔ rev（正反都是两个参数，只是顺序不同——反向对比就是交换）。
  const auto pair = args(head1, head, false);
  ST_REQUIRE(pair.size() == 2);
  ST_CHECK_EQ(pair[0], std::string("HEAD~1"));
  ST_CHECK_EQ(pair[1], std::string("HEAD"));
  // 三点：**一个**参数 `A...B`（再多一个 `B` 会被当成路径过滤器）。
  const auto three_dot = args(head1, head, true);
  ST_REQUIRE(three_dot.size() == 1);
  ST_CHECK_EQ(three_dot.front(), std::string("HEAD~1...HEAD"));
  // 不支持的组合要**返回 nullopt**（由调用方报错）——而不是默默给一个错的命令。
  ST_CHECK(unsupported(index, index));
  ST_CHECK(unsupported(index, worktree));
}

ST_TEST(git_service_compare_two_commits) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "line1\nline2\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "second"});
  gbcode::CompareOptions options;
  options.from = gbcode::DiffEndpoint::parse("HEAD~1");
  options.to = gbcode::DiffEndpoint::parse("HEAD");
  const auto result = gbcode::read_compare(repo.path(), options);
  ST_CHECK(result.ok);
  ST_CHECK(result.error.empty());
  ST_REQUIRE(result.files.size() == 1);
  ST_CHECK_EQ(result.files.front().path, std::string("a.txt"));
  ST_CHECK_EQ(result.files.front().status, 'M');
  ST_CHECK_EQ(result.files.front().additions, std::size_t{1});
  ST_CHECK_EQ(result.additions, std::size_t{1});
  // 反向对比：状态变成删除侧（语义对称）。
  gbcode::CompareOptions reversed;
  reversed.from = options.to;
  reversed.to = options.from;
  const auto back = gbcode::read_compare(repo.path(), reversed);
  ST_CHECK(back.ok);
  ST_REQUIRE(back.files.size() == 1);
  ST_CHECK_EQ(back.files.front().deletions, std::size_t{1});
}

ST_TEST(git_service_compare_worktree_and_index) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 已暂存：改 a.txt 后 add；未暂存：改 b.txt 不 add。
  repo.write("a.txt", "line1\nSTAGED\n");
  repo.run({"add", "a.txt"});
  repo.write("b.txt", "bee2\n");

  // ① 工作区 ↔ 暂存区（未暂存改动）：只有 b.txt。
  gbcode::CompareOptions unstaged;
  unstaged.to = gbcode::DiffEndpoint::parse("WORKTREE");
  const auto working = gbcode::read_compare(repo.path(), unstaged);
  ST_CHECK(working.ok);
  ST_REQUIRE(working.files.size() == 1);
  ST_CHECK_EQ(working.files.front().path, std::string("b.txt"));

  // ② HEAD ↔ 暂存区（已暂存）：只有 a.txt。
  //   —— 这是端点语义表的核心区分：同一个“工作区还是暂存区”的选项错一位，
  //   就会**不报错地返回另一批文件**（歌白实测过的坑）。
  gbcode::CompareOptions staged;
  staged.to = gbcode::DiffEndpoint::parse("INDEX");
  const auto index_view = gbcode::read_compare(repo.path(), staged);
  ST_CHECK(index_view.ok);
  ST_REQUIRE(index_view.files.size() == 1);
  ST_CHECK_EQ(index_view.files.front().path, std::string("a.txt"));
  ST_CHECK_EQ(index_view.files.front().additions, std::size_t{1});

  // ③ 非法组合（暂存区 ↔ 暂存区）要如实报错，而不是当成“没有差异”。
  gbcode::CompareOptions bad;
  bad.from = gbcode::DiffEndpoint::parse("INDEX");
  bad.to = gbcode::DiffEndpoint::parse("INDEX");
  const auto failed = gbcode::read_compare(repo.path(), bad);
  ST_CHECK(!failed.ok);
  ST_CHECK(!failed.error.empty());
}

ST_TEST(git_service_compare_detects_rename_and_binary) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 重命名（内容相同，靠相似度识别）：`git mv` 后 commit。
  repo.run({"mv", "a.txt", "renamed.txt"});
  repo.run({"commit", "-m", "rename"});
  // 二进制文件（含 NUL）：不应被当作文本列出增删行。
  repo.write("blob.bin", std::string("abc\0def", 7));
  repo.run({"add", "blob.bin"});
  repo.run({"commit", "-m", "binary"});

  gbcode::CompareOptions options;
  options.from = gbcode::DiffEndpoint::parse("HEAD~2");
  options.to = gbcode::DiffEndpoint::parse("HEAD");
  const auto result = gbcode::read_compare(repo.path(), options);
  ST_CHECK(result.ok);
  bool saw_rename = false;
  bool saw_binary = false;
  for (const auto& file : result.files) {
    if (file.status == 'R' && file.path == "renamed.txt") {
      saw_rename = true;
      ST_CHECK_EQ(file.old_path, std::string("a.txt"));
    }
    if (file.path == "blob.bin") {
      saw_binary = true;
      ST_CHECK(file.binary);
      // 二进制的 +N/−N 归零（**不等于没有变化**——靠 binary 位区分）。
      ST_CHECK_EQ(file.additions, std::size_t{0});
    }
  }
  ST_CHECK(saw_rename);
  ST_CHECK(saw_binary);
}

ST_TEST(git_service_compare_merge_base_three_dot) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // main 上再加一笔。
  repo.write("a.txt", "line1\nmain\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "main advances"});
  // 分支：从 HEAD~1 开一个分支，改另一个文件。
  repo.run({"checkout", "-b", "side", "HEAD~1"});
  repo.write("side.txt", "side\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "side adds"});
  repo.run({"checkout", "main"});

  // 两点 vs 三点：三点只看**从共同祖先起我在这一侧改了什么**。
  gbcode::CompareOptions two_dot;
  two_dot.from = gbcode::DiffEndpoint::parse("main");
  two_dot.to = gbcode::DiffEndpoint::parse("side");
  const auto two = gbcode::read_compare(repo.path(), two_dot);
  ST_CHECK(two.ok);
  gbcode::CompareOptions three_dot = two_dot;
  three_dot.merge_base = true;
  const auto three = gbcode::read_compare(repo.path(), three_dot);
  ST_CHECK(three.ok);
  // 三点下列出的应只有 side.txt（main 那一侧的 a.txt 改动不属于“本分支的改动”）。
  bool side_only = false;
  for (const auto& file : three.files) {
    if (file.path == "side.txt") side_only = true;
  }
  ST_CHECK(side_only);
  // 两点下会同时看到两侧的改动（对比基数不同，这是正确行为而非缺陷）。
  ST_CHECK(two.files.size() >= three.files.size());
}

ST_TEST(git_service_content_at_endpoints) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "line1\nCOMMITTED\n");
  repo.run({"add", "a.txt"});
  repo.run({"commit", "-m", "edit"});
  repo.write("a.txt", "line1\nCOMMITTED\nWORKTREE\n");

  // HEAD：只有已提交的内容（不含工作区那行）。
  const auto committed = gbcode::read_content_at(
      repo.path(), gbcode::DiffEndpoint::parse("HEAD"), "a.txt");
  ST_CHECK(committed.ok);
  ST_CHECK(committed.text.find("WORKTREE") == std::string::npos);
  // 工作区：含未提交那行。
  const auto working =
      gbcode::read_content_at(repo.path(), gbcode::DiffEndpoint::parse("WORKTREE"), "a.txt");
  ST_CHECK(working.ok);
  ST_CHECK(working.text.find("WORKTREE") != std::string::npos);
  // 那一侧没有的文件：`missing`（**不是错误**——新增/删除就是这种情况）。
  const auto absent =
      gbcode::read_content_at(repo.path(), gbcode::DiffEndpoint::parse("HEAD"), "nope.txt");
  ST_CHECK(!absent.ok);
  ST_CHECK(absent.missing);
  ST_CHECK(absent.error.empty());
  // 并列差异的两侧文本组合起来必须能还原出差异（结构同源）。
  const auto hunks = st::text::diff_hunks(committed.text, working.text);
  ST_CHECK_EQ(st::text::count_changes(hunks).first, std::size_t{1});
}

ST_TEST(git_service_root_commit_normalizes_to_empty_tree) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 仓库只有一笔提交（first）⇒ 它就是根提交。
  ST_CHECK(gbcode::is_root_commit(repo.path(), "HEAD"));
  ST_CHECK(!gbcode::is_root_commit(repo.path(), "HEAD^"));   // 不存在 ⇒ 不是“根提交”
  ST_CHECK(!gbcode::is_root_commit(repo.path(), "不存在的引用"));
  // 根提交与空树比较：全部文件都是“新增”（这才是「看首个提交改了什么」的正确语义）。
  gbcode::CompareOptions options;
  options.from = gbcode::DiffEndpoint::parse(std::string(gbcode::kEmptyTree));
  options.to = gbcode::DiffEndpoint::parse("HEAD");
  const auto result = gbcode::read_compare(repo.path(), options);
  ST_CHECK(result.ok);
  ST_REQUIRE(!result.files.empty());
  for (const auto& file : result.files) {
    ST_CHECK_EQ(file.status, 'A');
  }
}

