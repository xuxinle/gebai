/// Git 服务层数据测试（版本管理重构）：porcelain 分组语义 / 日志解析 / 分支解析。
///
/// **真仓库测试**（临时目录里真跑 git init/commit）——分组判据是 XY 矩阵的
/// 语义问题，喂假数据测不出「X=Y=非空时两边都出现」这种坑：只有真仓库能
/// 制造出「暂存过、又改了」的真实状态。git 不可用时跳过（与 LSP 真机冒烟同一策略）。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "st/core/process.hpp"
#include "st/test/test.hpp"

// git_service 是 gbcode 的应用层源（不在框架 sources 清单里）——测试目标
// 直接编译它的实现：include 源文件（单文件、无内部依赖，比改清单加钩子干净）。
#include "examples/gbcode/git_service.hpp"

namespace {

using gbcode::ChangeGroup;

/// 临时仓库（RAII 清理）。
struct Repo {
  std::filesystem::path dir{};
  bool ok{false};

  explicit Repo() {
    dir = std::filesystem::temp_directory_path() /
          std::format("st_git_svc_{}", static_cast<int>(::getpid()));
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
                   std::format("st_git_plain_{}", static_cast<int>(::getpid()));
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
