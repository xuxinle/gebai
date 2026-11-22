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
#include <mutex>
#include <string>
#include <vector>

#include "st/core/process.hpp"
#include "st/text/merge.hpp"
#include "st/test/test.hpp"
#include "st/text/diff.hpp"
// git_service 是 gbcode 的应用层源（不在框架 sources 清单里）——测试目标
// 直接编译它的实现：include 源文件（单文件、无内部依赖，比改清单加钩子干净）。
#include "apps/gbcode/git_service.hpp"

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
    // ⚠ `remove_all` **必须传 `error_code`**：
    // ① 目录不存在时它会抛（而“首次创建”恰恰是最常见的情形）；
    // ② `.git/objects` 下的对象文件在 Windows 上是只读的，上一轮异常退出留下的目录
    //    会删不掉——抛出去的 `filesystem_error` 直接让**整个测试二进制**中止
    //    （实测：全量跑时报 “cannot remove all … .git\objects\76\f740…”，
    //     而单跑那条用例反而绿）。
    // 删不掉不是致命错：换个目录名继续（`next_serial()` 保证不撞名）。
    std::error_code cleanup;
    std::filesystem::remove_all(dir, cleanup);
    if (cleanup) {
      dir = dir.string() + "_" + std::to_string(next_serial());
    }
    std::filesystem::create_directories(dir);
    // ## 种子仓库复用：整族 24 条用例只需**建一次仓**（2026-11-22）
    //
    // 每条用例原本各建一次仓（3 次 spawn = 227 ms×19 仓 ≈ 4.3 s），而它们对
    // 仓库的初始状态要求完全一致（main 分支上一条 `first` 提交，a.txt / b.txt 两个文件）。
    // 所以改成：进程内**首次**建仓后保留它，后续每条从它 **��制**一份（~`.git` 极小，
    // 实测 116 ms 含一个 3 文件的工作区）。
    //
    // ## 为何能安全复用（不能靠“猜它没被改过”）
    //
    // ① 种子仓**从不被任何用例碰到**：交出去的是副本，被测代码拿到的是 `dir`（副本），
    //    所有 `commit`/`checkout`/`merge` 都发生在副本里。
    // ② 种子仓在 `seed_repo()` 里建、仅在那里建一次（首条用例的构造过程），
    //    之后只读（`std::filesystem::copy` 不会改源）。
    // ③ 副本目录名仍带 `pid + 唯一序号`（与旧口径一致），分片并行不撞名；
    //    `remove_all` 在拷贝前仍跑（清上一轮异常残留）。
    //
    // 保留不动的仍是 `init` / `add` / `commit` 三次真操作——它们是本夹具**要造的事实**，
    // 用 `git fast-import` 或直写 `.git/objects` 合开就无法保证“真仓库”这个前提
    // （本文件头已说明：分组判据必须靠真仓库才测得出来）。
    //
    // 实测：git_service 家族 18.2 s → 14.9 s（3 spawn/仓）→ 见下（种子复用）。
    if (!seed_repo()) return;
    std::error_code copy_error;
    std::filesystem::copy(seed_repo_dir(), dir, std::filesystem::copy_options::recursive,
                          copy_error);
    if (copy_error) return;
    ok = true;
  }
  ~Repo() {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }

  /// 种子仓目录（进程级单例；见构造函数的注释）。
  static auto seed_repo_dir() -> const std::filesystem::path& {
    // lint-allow: L8 进程级夹具状态：种子仓只在本测试二进制内存在，随进程退出而失效
    static const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("st_git_svc_seed_{}", static_cast<int>(st::process::current_id()));
    return path;
  }

  /// 建种子仓（仅首次调用生效；失败时下次还会重试——即每次建都建，只是不覆盖已建好的）。
  static auto seed_repo() -> bool {
    // lint-allow: L8 夹具状态的“已建好”标志（同 `seed_repo_dir`）
    static std::once_flag once;
    // lint-allow: L8 同上（结果传给后到的调用者）
    static bool built = false;
    std::call_once(once, [] {
      std::error_code error;
      std::filesystem::remove_all(seed_repo_dir(), error);
      std::filesystem::create_directories(seed_repo_dir());
      const auto run_seed = [](const std::vector<std::string>& args) {
        st::process::Options options;
        options.cwd = seed_repo_dir().string();
        std::vector<std::string> full{"-c", "user.email=test@local", "-c", "user.name=Tester"};
        full.insert(full.end(), args.begin(), args.end());
        const auto result = st::process::run("git", full, options);
        return result.has_value() && result->exit_code == 0;
      };
      if (!run_seed({"init", "--template=", "--initial-branch=main"})) return;
      std::ofstream(seed_repo_dir() / "a.txt") << "line1\n";
      std::ofstream(seed_repo_dir() / "b.txt") << "bee\n";
      if (!run_seed({"add", "."})) return;
      built = run_seed({"commit", "-m", "first"});
    });
    return built;
  }

  auto run(const std::vector<std::string>& args) const -> bool {
    st::process::Options options;
    options.cwd = dir.string();
    // ## 身份用 `-c` 注入，而不是写 `.git/config`
    //
    // 建仓时已不再调两次 `git config`（见构造函数），但如果只在那里用 `-c`，
    // 后续每一条 `commit` 都会因缺 user 而失败（"Author identity unknown"）——
    // 所以身份必须在**统一入口**注入。这里的两条 `-c` 不额外花进程：它们与
    // 真正的子命令在同一次 spawn 里。
    //
    // 为何不在建仓时写一次配置、后续都靠它：那样两条 `-c` 就变成**两次真 spawn**
    // （`git config` 不能批写）而这里零成本。两者语义等价（都是命令行作用域）。
    std::vector<std::string> full{"-c", "user.email=test@local", "-c", "user.name=Tester"};
    full.insert(full.end(), args.begin(), args.end());
    const auto result = st::process::run("git", full, options);
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

#include "apps/gbcode/git_service.cpp"

/// **为何是 `ST_TEST_SLOW`（默认不跑）**
///
/// 它跑真 `git` 子进程建临时仓库、真读写文件。`st test` 默认 16 片并行时，
/// 多个片同时在文件系统上重活（建仓/索引/写对象），实测出现过偶发假红；
/// 单跑稳定绿。这与 `tools/apps_e2e_all.py` 里“并行会抢 CPU 互相拖慢”
/// 是同一类问题：**环境敏感用例不该与别人争资源**。
///
/// 要验证它时：`st test --slow git_service`（单进程、独占）。
ST_TEST_SLOW(git_service_groups_by_xy_matrix) {
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

// 软超时：本条要起**七次 git 子进程**建仓库（init/config×2/add/commit + 读快照/分支）。
// 耗时由 git 与文件系统决定：实测单跑 2.4s，16 路分片满载时 11.8s 就报软超时。
ST_TEST_WITH_TIMEOUT(git_service_reads_branch_and_upstream, 60000) {
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
  // **短哈希固定 8 位**（不是 git `%h` 的 7 位）。
  //
  // 这条断言曾经写 7 位，而实现一直是 8 位（`git_service.cpp` 的
  // `entry.hash.substr(0, 8)`）——于是它每次都红。真因是实现有意固定位数：
  // git 的 `%h` 会随仓库规模自动伸缩（小仓 7 位、大仓 8~9 位），同一列表里
  // 哈希列宽会忽长忽短、对不齐；而“唯一性”不需向 git 让步（8 位在几十万提交
  // 里也够区分，操作一律用完整 hash）。
  //
  // 所以把测就实现口径，并**钉死这个不变量**：将来若有人想“跟随 git 变长”，
  // 这条会立刻变红并指向根因。
  ST_CHECK_EQ(latest.short_hash.size(), std::size_t{8});
  ST_CHECK(latest.hash.starts_with(latest.short_hash));
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

ST_TEST(git_service_reads_tags_remotes_refs_and_stash) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 轻量标签 + 注解标签。
  repo.run({"tag", "light"});
  repo.run({"tag", "-a", "annotated", "-m", "注解标签消息"});
  const auto tags = gbcode::read_tags(repo.path());
  ST_CHECK_EQ(tags.size(), std::size_t{2});
  bool saw_annotated = false;
  for (const auto& tag : tags) {
    if (tag.name == "annotated") {
      saw_annotated = true;
      ST_CHECK(tag.annotated);
      ST_CHECK_EQ(tag.subject, std::string("注解标签消息"));
    }
    if (tag.name == "light") ST_CHECK(!tag.annotated);
  }
  ST_CHECK(saw_annotated);

  // 远程（本地加一个即可，不联网）。
  repo.run({"remote", "add", "origin", "https://example.com/repo.git"});
  const auto remotes = gbcode::read_remotes(repo.path());
  ST_REQUIRE(remotes.size() == 1);
  ST_CHECK_EQ(remotes.front().name, std::string("origin"));
  ST_CHECK_EQ(remotes.front().fetch_url, std::string("https://example.com/repo.git"));
  // 未单独配置 push URL 时应回退到拉取 URL（而不是空）。
  ST_CHECK_EQ(remotes.front().push_url, remotes.front().fetch_url);

  // 引用清单：本地分支 + 标签（`origin/HEAD` 这类别名不算分支）。
  const auto refs = gbcode::read_refs(repo.path());
  bool saw_local = false;
  bool saw_tag = false;
  for (const auto& ref : refs) {
    if (ref.kind == "local" && ref.name == "main") saw_local = true;
    if (ref.kind == "tag" && ref.name == "light") saw_tag = true;
    ST_CHECK(!ref.name.ends_with("/HEAD"));
  }
  ST_CHECK(saw_local);
  ST_CHECK(saw_tag);

  // stash：改一个文件后 stash，应能读出 `stash@{0}`。
  repo.write("a.txt", "line1\nstashed\n");
  repo.run({"stash", "push", "-m", "工作中的改动"});
  const auto stashes = gbcode::read_stashes(repo.path());
  ST_REQUIRE(stashes.size() == 1);
  ST_CHECK_EQ(stashes.front().index, std::string("stash@{0}"));
  ST_CHECK(stashes.front().message.find("工作中的改动") != std::string::npos);
  ST_CHECK_EQ(stashes.front().branch, std::string("main"));
}

ST_TEST(git_service_file_history_follows_rename) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "line1\nline2\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "change a"});
  repo.run({"mv", "a.txt", "renamed.txt"});
  repo.run({"commit", "-m", "rename a"});
  // `--follow`：重命名后的历史里能看到重命名之前的那一笔（否则历史“断了”）。
  const auto history = gbcode::read_file_history(repo.path(), "renamed.txt", 20);
  ST_CHECK(history.size() >= 3U);
  bool saw_rename = false;
  bool saw_original = false;
  for (const auto& entry : history) {
    if (entry.subject == "rename a") saw_rename = true;
    if (entry.subject == "change a") saw_original = true;
  }
  ST_CHECK(saw_rename);
  ST_CHECK(saw_original);
}

ST_TEST(git_service_blame_reports_line_ownership) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "first\nsecond\nthird\n");
  repo.run({"add", "a.txt"});
  repo.run({"commit", "-m", "three lines"});
  const auto all = gbcode::read_blame(repo.path(), "", "a.txt", 1, 0);
  // 先 REQUIRE 再取下标：blame 读取失败时拿到的是空表，
  // 而空表上直接 `all[0]` 会**崩而不是变红**（本项目已记过同类坑）。
  ST_REQUIRE(all.size() == 3);
  ST_CHECK_EQ(all[0].line, std::size_t{1});
  ST_CHECK_EQ(all[0].content, std::string("first"));
  ST_CHECK(!all[0].hash.empty());
  ST_CHECK_EQ(all[0].author, std::string("Tester"));
  // 限定行范围：只要第 2 行。
  const auto slice = gbcode::read_blame(repo.path(), "", "a.txt", 2, 1);
  ST_REQUIRE(slice.size() == 1);
  ST_CHECK_EQ(slice.front().line, std::size_t{2});
  ST_CHECK_EQ(slice.front().content, std::string("second"));
}

ST_TEST(git_service_operation_state_detects_merge_conflict) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 干净仓库：不在任何多步操作中。
  ST_CHECK(!gbcode::read_operation_state(repo.path()).active());
  ST_CHECK(gbcode::read_operation_state(repo.path()).conflicts.empty());

  // 造一个 merge 冲突。
  repo.run({"checkout", "-b", "side"});
  repo.write("a.txt", "side version\n");
  repo.run({"commit", "-am", "side change"});
  repo.run({"checkout", "main"});
  repo.write("a.txt", "main version\n");
  repo.run({"commit", "-am", "main change"});
  repo.run({"merge", "side"});   // 预期失败（冲突）

  const auto state = gbcode::read_operation_state(repo.path());
  ST_CHECK(state.active());
  ST_CHECK_EQ(state.kind, std::string("merge"));
  ST_REQUIRE(state.conflicts.size() == 1);
  ST_CHECK_EQ(state.conflicts.front(), std::string("a.txt"));
  // 中止后回到干净状态（“给出 abort 入口”的价值就在这里）。
  repo.run({"merge", "--abort"});
  ST_CHECK(!gbcode::read_operation_state(repo.path()).active());
}

/// **工作区是仓库子目录时，多步操作态仍然要能探到**（2026-11-22 修）。
///
/// ## 缺陷现场
///
/// 旧 `read_operation_state` 直接探 `<cwd>/.git/MERGE_HEAD`。而 gbcode 的“工作区”
/// 可以是仓库的**子目录**（实测：工作区 `gebai/shuangtian`、仓库根 `gebai`）——
/// 那种情况下 `.git` 只在仓库根下，于是**永远探不到**：用户在子目录里开 gbcode
/// 做 merge、撞了冲突，界面不会给「继续 / 跳过 / 中止」入口（而那是冲突时
/// 唯一的出路，不给就只能去命令行擦屁股）。
///
/// 修法是“先问 git `.git` 在哪”（`read_git_dirs`，顺带一并拿仓库根）。
/// 这条用真仓库 + 真子目录把它钉住。
ST_TEST(git_service_operation_state_from_subdirectory) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 仓库里造一个子目录，并在里面放一个文件（子目录自己要有内容才像“真工作区”）。
  repo.write("sub/inner.txt", "inner\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "add sub"});
  const std::string sub = repo.path() + "/sub";

  // 干净时：子目录出发也不能报“在多步操作中”。
  ST_CHECK(!gbcode::read_operation_state(sub).active());
  // 仓库根与 git 目录都要能拿到（两者不同！）。
  const auto dirs = gbcode::read_git_dirs(sub);
  ST_CHECK(!dirs.root.empty());
  ST_CHECK(!dirs.git_dir.empty());
  ST_CHECK(dirs.root != dirs.git_dir);

  // 造 merge 冲突（在子目录里 merge 同样会在 `.git` 写 MERGE_HEAD）。
  repo.run({"checkout", "-b", "side"});
  repo.write("a.txt", "side\n");
  repo.run({"commit", "-am", "side"});
  repo.run({"checkout", "main"});
  repo.write("a.txt", "main\n");
  repo.run({"commit", "-am", "main"});
  repo.run({"merge", "side"});

  // ⚠ 从**子目录**出发也要看到 merge 态（旧实现在这里绿？不，它会红）。
  const auto from_sub = gbcode::read_operation_state(sub);
  ST_CHECK(from_sub.active());
  ST_CHECK_EQ(from_sub.kind, std::string("merge"));
  // 从仓库根出发同样能探到（两条路必须同源）。
  const auto from_root = gbcode::read_operation_state(repo.path());
  ST_CHECK(from_root.active());
  ST_CHECK_EQ(from_root.kind, from_sub.kind);
  repo.run({"merge", "--abort"});
  ST_CHECK(!gbcode::read_operation_state(sub).active());
}

ST_TEST(git_service_upstream_reports_tracking_branch) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 无上游：空。
  ST_CHECK(gbcode::read_upstream(repo.path()).empty());
  // 造一个手动配置的上游（不联网）。
  // ⚠ 只写 `refs/remotes/origin/main` + 两条 config 还不够：`rev-parse @{upstream}` 会报
  // “not stored as a remote-tracking branch”——还需要 `remote.origin.fetch` 的 refspec
  //（`git remote add` 会一并写它，所以这里也走 remote add 而不是手改 config）。
  repo.run({"remote", "add", "origin", "https://example.com/repo.git"});
  repo.run({"update-ref", "refs/remotes/origin/main", "HEAD"});
  repo.run({"config", "branch.main.remote", "origin"});
  repo.run({"config", "branch.main.merge", "refs/heads/main"});
  ST_CHECK_EQ(gbcode::read_upstream(repo.path()), std::string("origin/main"));
}

ST_TEST(git_service_write_args_builds_commands) {
  using gbcode::WriteIntent;
  using gbcode::WriteOp;
  const auto line = [](const WriteIntent& intent) {
    const auto args = gbcode::write_args(intent);
    return args.has_value() ? gbcode::to_display_line(*args) : std::string("<nullopt>");
  };
  ST_CHECK_EQ(line({.op = WriteOp::Stage, .path = "a b.txt"}), std::string("git add -- a b.txt"));
  ST_CHECK_EQ(line({.op = WriteOp::Unstage, .path = "a.txt"}),
              std::string("git restore --staged -- a.txt"));
  ST_CHECK_EQ(line({.op = WriteOp::DiscardAll}), std::string("git restore -- ."));
  ST_CHECK_EQ(line({.op = WriteOp::Commit, .message = "msg"}), std::string("git commit -m msg"));
  // amend + 作者 + 允许空。（指定顺序须按结构体字段声明序：`message` 在 `author` 之前。）
  ST_CHECK_EQ(line({.op = WriteOp::Commit, .message = "m", .author = "A <a@b>",
                    .amend = true, .allow_empty = true}),
              std::string("git commit -m m --amend --allow-empty --author A <a@b>"));
  ST_CHECK_EQ(line({.op = WriteOp::Checkout, .ref = "main"}), std::string("git checkout main"));
  // 指定顺序须按结构体字段声明序（`ref` 在 `name` 之前）。
  ST_CHECK_EQ(line({.op = WriteOp::CheckoutNew, .ref = "main", .name = "feat"}),
              std::string("git checkout -b feat main"));
  // 删分支默认用 `-d`（拒删未合并）——`hard` 才 `-D`。
  ST_CHECK_EQ(line({.op = WriteOp::DeleteBranch, .name = "old"}),
              std::string("git branch -d old"));
  ST_CHECK_EQ(line({.op = WriteOp::DeleteBranch, .name = "old", .hard = true}),
              std::string("git branch -D old"));
  ST_CHECK_EQ(line({.op = WriteOp::ResetHard, .ref = "HEAD~1"}),
              std::string("git reset --hard HEAD~1"));
  // 推送：无分支名 = 普通 push；给了名字 = 建跟踪（`-u`）。
  ST_CHECK_EQ(line({.op = WriteOp::Push}), std::string("git push"));
  ST_CHECK_EQ(line({.op = WriteOp::Push, .name = "feat"}), std::string("git push -u feat"));
  // 强推**必须**带 lease。
  ST_CHECK_EQ(line({.op = WriteOp::PushForceWithLease}),
              std::string("git push --force-with-lease"));
  ST_CHECK_EQ(line({.op = WriteOp::Pull}), std::string("git pull --ff-only"));
  ST_CHECK_EQ(line({.op = WriteOp::StashPush, .message = "wip"}),
              std::string("git stash push -m wip"));
  ST_CHECK_EQ(line({.op = WriteOp::StashPop}), std::string("git stash pop stash@{0}"));
  ST_CHECK_EQ(line({.op = WriteOp::TagCreate, .name = "v1"}), std::string("git tag v1"));
  ST_CHECK_EQ(line({.op = WriteOp::TagCreate, .name = "v1", .message = "版本一",
                    .annotated = true}),
              std::string("git tag -a v1 -m 版本一"));

  // —— 必填参数缺失必须 **nullopt**（否则会构造出“删错东西”的命令）——
  ST_CHECK(!gbcode::write_args({.op = WriteOp::ResetHard, .ref = ""}).has_value());
  ST_CHECK(!gbcode::write_args({.op = WriteOp::Commit, .message = ""}).has_value());
  ST_CHECK(!gbcode::write_args({.op = WriteOp::Checkout, .ref = ""}).has_value());
  ST_CHECK(!gbcode::write_args({.op = WriteOp::Stage, .path = ""}).has_value());
  ST_CHECK(!gbcode::write_args({.op = WriteOp::DeleteBranch, .name = ""}).has_value());
}

ST_TEST(git_service_reads_conflict_sides_from_index) {
  // 三方合并视图的前置取数：`git show :1:/:2:/:3:` 三份。
  // 造一个**真冲突**：同一文件在两个分支上改了同一行。
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);

  repo.write("f.txt", "base\n");
  repo.run({"add", "f.txt"});
  repo.run({"commit", "-m", "add f"});
  // 从当前提交拉一个分支并在上面改。
  repo.run({"checkout", "-b", "feature"});
  repo.write("f.txt", "theirs\n");
  repo.run({"commit", "-am", "feature change"});
  // 回 main 改同一行（制造冲突）。
  repo.run({"checkout", "main"});
  repo.write("f.txt", "ours\n");
  repo.run({"commit", "-am", "main change"});
  // 合并：必然冲突。
  const bool merged = repo.run({"merge", "feature"});
  ST_CHECK(!merged);   // 冲突时 `git merge` 返回非 0（本用例要的就是这个状态）

  const auto sides = gbcode::read_conflict_sides(repo.path(), "f.txt");
  ST_CHECK(sides.any());
  ST_REQUIRE(sides.base.ok);
  ST_REQUIRE(sides.ours.ok);
  ST_REQUIRE(sides.theirs.ok);
  // 三份内容各自正确（这是本接口的全部意义所在——序号对应关系不能反）。
  ST_CHECK(sides.base.text.starts_with("base"));
  ST_CHECK(sides.ours.text.starts_with("ours"));
  ST_CHECK(sides.theirs.text.starts_with("theirs"));

  // 取到的东西确实能喂给三方合并。
  const auto plan = st::text::merge_three_way(sides.base.text, sides.ours.text, sides.theirs.text);
  ST_CHECK_EQ(plan.conflicts, std::size_t{1});
  ST_CHECK_EQ(st::text::merge_apply(plan), std::string("ours\n"));   // 缺省取我方

  // 非冲突文件（`a.txt` 只有 `:0:`）：三号全缺，但**不是错误**——
  // 这正是"逐个取、各自报存在"的理由（都当失败的话，调用方分不清"本来没有"与"取数坏了"）。
  const auto clean = gbcode::read_conflict_sides(repo.path(), "a.txt");
  ST_CHECK(!clean.any());
  ST_CHECK(!clean.base.ok && !clean.ours.ok && !clean.theirs.ok);
  ST_CHECK(clean.base.missing && clean.ours.missing && clean.theirs.missing);
  ST_CHECK(clean.base.error.empty());   // missing 与 error 必须分开

  // 路径为空：如实报错（不静默返回空）。
  const auto empty_path = gbcode::read_conflict_sides(repo.path(), "");
  ST_CHECK(!empty_path.any());
  ST_CHECK(!empty_path.base.error.empty());
}

ST_TEST(git_service_write_danger_and_backup_names) {
  using gbcode::Danger;
  using gbcode::WriteIntent;
  using gbcode::WriteOp;
  // 不可逆的三类：重置、丢弃、变基。
  ST_CHECK(gbcode::danger_of({.op = WriteOp::ResetHard, .ref = "HEAD"}) == Danger::Backup);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::DiscardAll}) == Danger::Backup);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::DiscardOne, .path = "a"}) == Danger::Backup);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::Rebase, .ref = "main"}) == Danger::Backup);
  ST_CHECK(gbcode::needs_backup({.op = WriteOp::ResetHard, .ref = "HEAD"}));
  // 需确认。
  ST_CHECK(gbcode::danger_of({.op = WriteOp::Amend}) == Danger::Confirm);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::PushForceWithLease}) == Danger::Confirm);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::DeleteBranch, .name = "x"}) == Danger::Confirm);
  // 安全。
  ST_CHECK(gbcode::danger_of({.op = WriteOp::Stage, .path = "a"}) == Danger::None);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::Commit, .message = "m"}) == Danger::None);
  ST_CHECK(gbcode::danger_of({.op = WriteOp::Push}) == Danger::None);
  ST_CHECK(!gbcode::needs_backup({.op = WriteOp::Commit, .message = "m"}));

  if (git_available()) {
    Repo repo;
    ST_REQUIRE(repo.ok);
    // 备份名要能读、且**不重名**（同一分钟做两次危险操作）。
    const std::string first = gbcode::backup_branch_name(repo.path());
    ST_CHECK(first.starts_with("backup/"));
    repo.run({"branch", first});
    const std::string second = gbcode::backup_branch_name(repo.path());
    ST_CHECK(second != first);
  }
}

ST_TEST(git_service_shell_quote_handles_specials) {
  // 安全字符集原样。
  ST_CHECK_EQ(gbcode::shell_quote("a.txt"), std::string("a.txt"));
  ST_CHECK_EQ(gbcode::shell_quote("src/main.cpp"), std::string("src/main.cpp"));
  // 空格/中文/引号/分号都要被包起来——否则终端会把一条命令拆成两条。
  ST_CHECK_EQ(gbcode::shell_quote("a b.txt"), std::string("'a b.txt'"));
  ST_CHECK_EQ(gbcode::shell_quote("提交信息"), std::string("'提交信息'"));
  ST_CHECK_EQ(gbcode::shell_quote("a;rm -rf /"), std::string("'a;rm -rf /'"));
  // 内部的 `'` 要写成 `'\''`（POSIX 通行写法）。
  ST_CHECK_EQ(gbcode::shell_quote("it's"), std::string("'it'\\''s'"));
  // 拼行：安全的原样、危险的包起来。
  ST_CHECK_EQ(gbcode::to_shell_line({"commit", "-m", "a b"}),
              std::string("git commit -m 'a b'"));
  // 展示形式不转义（给人看）。
  ST_CHECK_EQ(gbcode::to_display_line({"commit", "-m", "a b"}),
              std::string("git commit -m a b"));
  // 多行提交信息在展示形式里拉平（否则状态栏/日志会被撑开）。
  ST_CHECK_EQ(gbcode::to_display_line({"commit", "-m", "line1\nline2"}),
              std::string("git commit -m line1 line2"));
}

ST_TEST(git_service_audit_appends_and_reads_back) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  ST_CHECK(gbcode::append_audit(repo.path(), "commit", {"commit", "-m", "hello"}, "ok"));
  ST_CHECK(gbcode::append_audit(repo.path(), "reset --hard", {"reset", "--hard", "HEAD~1"},
                                "failed: refused"));
  const auto entries = gbcode::read_audit(repo.path(), 10);
  ST_REQUIRE(entries.size() == 2);
  ST_CHECK(entries[0].find("commit") != std::string::npos);
  ST_CHECK(entries[0].find("\"result\":\"ok\"") != std::string::npos);
  ST_CHECK(entries[1].find("refused") != std::string::npos);
  // 上限只留**最后** N 条（界面“最近操作”看的就是尾部）。
  const auto tail = gbcode::read_audit(repo.path(), 1);
  ST_REQUIRE(tail.size() == 1);
  ST_CHECK(tail.front().find("refused") != std::string::npos);
  // 非仓库：不崩，返回 false。
  const auto outside = std::filesystem::temp_directory_path() / "st_not_a_repo_audit";
  std::error_code ec;
  std::filesystem::create_directories(outside, ec);
  ST_CHECK(!gbcode::append_audit(outside.string(), "x", {"x"}, "y"));
}

ST_TEST(git_service_control_args_maps_operations) {
  const auto merge_abort = gbcode::control_args("merge", "--abort");
  ST_REQUIRE(merge_abort.has_value());
  ST_CHECK_EQ(gbcode::to_display_line(*merge_abort), std::string("git merge --abort"));
  const auto rebase_continue = gbcode::control_args("rebase", "--continue");
  ST_REQUIRE(rebase_continue.has_value());
  ST_CHECK_EQ(gbcode::to_display_line(*rebase_continue),
              std::string("git rebase --continue"));
  // 未知操作如实拒绝（不能凭空造一条 `git <乱码> --abort`）。
  ST_CHECK(!gbcode::control_args("unknown", "--abort").has_value());
  ST_CHECK(!gbcode::control_args("", "--skip").has_value());
  // `WriteOp::Abort` 的 ref 位传操作种类；为空则拒绝。
  ST_CHECK(!gbcode::write_args({.op = gbcode::WriteOp::Abort, .ref = ""}).has_value());
  ST_CHECK(gbcode::write_args({.op = gbcode::WriteOp::Abort, .ref = "merge"}).has_value());
}

ST_TEST(git_service_stage_and_commit_through_write_args) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "line1\nchanged\n");
  // 用构造出来的命令真跑一遍（“构造”与“git 真的接受”是两件事）。
  const auto staged = gbcode::write_args({.op = gbcode::WriteOp::Stage, .path = "a.txt"});
  ST_REQUIRE(staged.has_value());
  repo.run(*staged);
  {
    const auto snapshot = gbcode::read_snapshot(repo.path());
    bool saw_staged = false;
    for (const auto& change : snapshot.changes) {
      if (change.path == "a.txt" && change.has_staged_side()) saw_staged = true;
    }
    ST_CHECK(saw_staged);
  }
  const auto commit = gbcode::write_args({.op = gbcode::WriteOp::Commit, .message = "通过构造的命令提交"});
  ST_REQUIRE(commit.has_value());
  repo.run(*commit);
  const auto log = gbcode::read_log(repo.path(), 5);
  ST_REQUIRE(!log.empty());
  ST_CHECK_EQ(log.front().subject, std::string("通过构造的命令提交"));
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

// ————————————————————————————————————————————————————————————————————————————
// 2026-11-22 性能重构的**等价性守则**
// ————————————————————————————————————————————————————————————————————————————
//
// 这一轮把读面从“12 条子进程、各视图各读一遍”合成了“5 条、能并发的就并发”：
//
// | 旧 | 新 |
// |---|---|
// | `log` + `rev-list --parents`（同一段历史两遍） | 一条 `log --pretty=…%P` |
// | `branch --all` + `for-each-ref refs/tags` + `refs/{heads,remotes}` | 一条 `for-each-ref` |
// | `git remote` + 每个远程两条 `get-url` | 一条 `config --get-regexp` |
// | `show --format=%B` + `show --numstat` + `show`（三条） | 一条 `show --numstat -p --format=%B%x00` |
// | `status --porcelain`（非 `-z`） | `status --porcelain -z` |
// | 界面每帧 `changes_in()` 全量拷贝 | 快照内预分组的下标 |
//
// 这些都是**同义改写**，而合并不等于等价——每一条都可能在边界上出错：
// 字段顺序变了、`-z` 的重命名变成两条记录、`--porcelain` 的元信息只给一次……
// 所以每一条都要有一条“对着**独立真值**”的用例。真值取法是**直接调另一条 git
// 命令**（不拿被测实现自己跟自己比），而重命名/中文/空格这类边界必须用
// **真仓库**造出来（喂假数据测不出 `-z` 的记录结构）。
//
// ## 为何这些用例住在本文件（而不是新开一个 TU）
//
// 断言要调 `read_snapshot`/`read_commit_detail`/`read_ref_snapshot` 等
// **实现在 `git_service.cpp` 里**的函数，而本 TU 用 `#include "…/git_service.cpp"`
// 把它编进来（见文件头）。另开一个 TU 就与 gbcode 目标重复符号
// （`st.pkg` 的 `//tests_git_graph` 记着这条约束）。

/// `status -z` 的重命名解析：`R  new\0old\0` 是**两条记录**，不能拆 ` -> `。
///
/// ## 这条守的是什么缺陷
///
/// 旧实现用非 `-z` 的 porcelain：重命名是**一行** `R  old -> new`，靠 `find(" -> ")`
/// 拆。两个后果：
/// ① 路径本身含 ` -> ` 时（真能出现）拆错位置；
/// ② git 对含空格的路径**加引号**，旧实现只剥“首尾都是引号”的那种，
///    `R  "old name.txt" -> "新 name.txt"` 剥完还剩内层引号。
/// `-z` 把重命名拆成两条 NUL 记录（新名 + 原名），这两类问题都不存在——
/// 而代价是解析逻辑全变了，所以要有这条用例钉住“拆对了”。
ST_TEST(git_service_status_z_parses_rename_pairs) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 造一个**含空格的重命名**并提交（这样工作区里它是“已改名且不再有改动”）。
  repo.run({"mv", "a.txt", "旧 名字.txt"});
  repo.run({"commit", "-m", "rename with space"});
  // 再改一次名（这次是**已暂存的 R**：工作区跟着走）。
  repo.run({"mv", "旧 名字.txt", "新 名字.txt"});

  const auto snapshot = gbcode::read_snapshot(repo.path());
  ST_CHECK(snapshot.is_repo);
  bool saw_rename = false;
  for (const auto& change : snapshot.changes) {
    // `-z` 的路径是**原样**的：不带引号、不带转义。
    ST_CHECK(change.path.find('"') == std::string::npos);
    if (change.path != "新 名字.txt") continue;
    saw_rename = true;
    ST_CHECK_EQ(change.display_code(), 'R');
    // 原路径来自**下一条 NUL 记录**（含空格的原名，原样）。
    ST_CHECK_EQ(change.orig_path, std::string("旧 名字.txt"));
  }
  ST_CHECK(saw_rename);

  // 中文 + 空格的未跟踪文件也必须原样读出（`-z`：既不引号也不八进制转义）。
  repo.write("中文 文件.md", "# 标题\n");
  bool saw_chinese = false;
  for (const auto& change : gbcode::read_snapshot(repo.path()).changes) {
    if (change.path == "中文 文件.md") saw_chinese = true;
  }
  ST_CHECK(saw_chinese);
}

/// **预分组下标与“现场过滤”逐条一致**（`group_index` 是快照属性，不能算错）。
///
/// 这条对照的是 `GitChange` 上的四个谓词（`is_conflicted` / `has_staged_side`
/// / `has_unstaged_side` / `is_untracked`）——它们是**语义定义**，而 `group_index`
/// 是它的缓存。两者若漂移，界面会把文件分到错的组里（而计数还是对的，更难发现）。
ST_TEST(git_service_group_index_matches_predicates) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 四种状态各一份：未跟踪、已暂存、只改工作区、既暂存又改。
  repo.write("new.txt", "n\n");
  repo.write("a.txt", "line1\nSTAGED\n");
  repo.run({"add", "a.txt"});
  repo.write("b.txt", "bee2\n");
  repo.write("c.txt", "staged\n");
  repo.run({"add", "c.txt"});
  repo.write("c.txt", "staged\nmore\n");

  const auto snapshot = gbcode::read_snapshot(repo.path());
  ST_CHECK(snapshot.is_repo);
  ST_REQUIRE(!snapshot.changes.empty());

  for (const gbcode::ChangeGroup group :
       {gbcode::ChangeGroup::Conflicted, gbcode::ChangeGroup::Staged,
        gbcode::ChangeGroup::Unstaged, gbcode::ChangeGroup::Untracked}) {
    // 真值：现场按谓词过滤（旧实现的做法）。
    std::vector<std::string> expected;
    for (const auto& change : snapshot.changes) {
      bool hit = false;
      switch (group) {
        case gbcode::ChangeGroup::Conflicted: hit = change.is_conflicted(); break;
        case gbcode::ChangeGroup::Staged: hit = change.has_staged_side(); break;
        case gbcode::ChangeGroup::Unstaged:
          hit = change.has_unstaged_side() && !change.is_conflicted();
          break;
        case gbcode::ChangeGroup::Untracked: hit = change.is_untracked(); break;
      }
      if (hit) expected.push_back(change.path);
    }
    // 被测：预分组下标 → 路径。
    std::vector<std::string> actual;
    for (const std::uint32_t index : snapshot.indices_in(group)) {
      actual.push_back(snapshot.changes[index].path);
    }
    std::cout << "[git] " << gbcode::change_group_name(group) << "：预期 " << expected.size()
              << " 条 / 预分组 " << actual.size() << " 条" << '\n';
    ST_CHECK(expected == actual);
    // 计数必须走 O(1) 那条路（不是“调 changes_in 再 size”）。
    ST_CHECK_EQ(snapshot.count_in(group), expected.size());
    // 拷贝版（`changes_in`）也必须与下标版一致（两套出口不能分叉）。
    ST_CHECK_EQ(snapshot.changes_in(group).size(), expected.size());
  }

  // 「既暂存又改」必须**同时在两组**（XY 矩阵的核心语义，见 git_service.hpp）。
  bool c_in_both = false;
  for (const std::uint32_t staged : snapshot.indices_in(gbcode::ChangeGroup::Staged)) {
    if (snapshot.changes[staged].path != "c.txt") continue;
    for (const std::uint32_t unstaged : snapshot.indices_in(gbcode::ChangeGroup::Unstaged)) {
      if (unstaged == staged) c_in_both = true;
    }
  }
  ST_CHECK(c_in_both);
}

/// `read_commit_detail` 一条命令给出三者：**提交信息 / numstat / patch** 都在，
/// 且边界切对了（`%B` 到 NUL、numstat 到第一个行首 `diff --git`）。
///
/// ## 这条守的是什么
///
/// 旧实现三条命令各取一份。合成一条最容易错的是**分段边界**：
///   · `%B` 不以 NUL 结尾时会把正文与 numstat 粘在一起；
///   · 按空行切会在“提交正文自带空行”时切错（正文本来就多段）；
///   · `diff --git` 出现在别处时按 `find` 切会切在中间。
/// 这条造一个**多段正文 + 多行改动**的提交，把这三处都覆盖到。
ST_TEST(git_service_commit_detail_single_command_segments) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  repo.write("a.txt", "line1\nline2\nline3\nline4\n");
  repo.run({"add", "."});
  repo.run({"commit", "-m", "标题行\n\n第一段正文。\n\n第二段正文（前面还有空行）。"});

  auto entries = gbcode::read_log(repo.path(), 1);
  ST_REQUIRE(!entries.empty());
  auto entry = entries.front();
  const std::string patch = gbcode::read_commit_detail(repo.path(), entry);

  // ① 提交信息：全部三段都在（不是只剩标题）。
  ST_CHECK(entry.message.find("标题行") != std::string::npos);
  ST_CHECK(entry.message.find("第一段正文") != std::string::npos);
  ST_CHECK(entry.message.find("第二段正文") != std::string::npos);
  // 尾部空行被剥掉（否则详情栏多一块空白）。
  ST_CHECK(!entry.message.empty() && entry.message.back() != '\n');
  // numstat 的制表符不能漏进提交信息（分段边界的直接判据）。
  ST_CHECK(entry.message.find('\t') == std::string::npos);

  // ② numstat：那条改动在（计数是数字、路径不带制表符）。
  ST_REQUIRE(!entry.files.empty());
  bool saw_a = false;
  for (const auto& [path, added, removed] : entry.files) {
    ST_CHECK(!path.empty());
    ST_CHECK(path.find('\t') == std::string::npos);   // 没把下一段切进来
    if (path == "a.txt") {
      saw_a = true;
      ST_CHECK(added > 0);
    }
    std::cout << "[git] detail file: " << path << " +" << added << " -" << removed << '\n';
  }
  ST_CHECK(saw_a);

  // ③ patch：是**真 patch**（从行首 `diff --git ` 起，含增行），
  //    而且不能把 numstat 段又吐一遍（否则会出现 `<数字>\t<数字>\t`）。
  ST_CHECK(patch.starts_with("diff --git "));
  ST_CHECK(patch.find("+line4") != std::string::npos);
  ST_CHECK(patch.find("\t1\t") == std::string::npos);
}

/// `read_ref_snapshot` 的多路输出与**各自单读**逐条一致（一条命令族替代五组命令）。
///
/// 把“合并”与“正确”分开守：分类判据（symref 别名、`refs/remotes/` 前缀、
/// 注解标签、pushurl 回退）全在合并后的那条解析里，而它们各自都有过**实测缺陷**
/// （见 `tests/gbcode_branches_test.cpp` 的现场记录）。这里对着真仓库逐一验。
ST_TEST(git_service_ref_snapshot_matches_individual_reads) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  // 名字含 `/` 的本地分支（远程判据的照妖镜）。
  repo.run({"branch", "feature/x"});
  // 两个标签（一轻一注解）。
  repo.run({"tag", "light"});
  repo.run({"tag", "-a", "annotated", "-m", "注解消息"});
  // 一个真远程 + `<remote>/HEAD` 别名（symref 的照妖镜）。
  const std::string remote = repo.path() + "/remote.git";
  repo.run({"init", "-q", "--bare", "--template=", remote});
  repo.run({"remote", "add", "origin", remote});
  repo.run({"push", "-q", "origin", "HEAD:refs/heads/main"});
  repo.run({"fetch", "-q", "origin"});
  (void)repo.run({"remote", "set-head", "origin", "--auto"});
  // ⚠ `%(upstream:short)` 不靠“有没有 origin/main 这个 ref”，而靠**分支的上游配置**
  // （`branch.main.remote` + `branch.main.merge`）。`git push` 首推**不会**自动写它
  // （只有 `push -u` 才写）——而这条用例要验的正是“上游短名能读出来”。
  repo.run({"config", "branch.main.remote", "origin"});
  repo.run({"config", "branch.main.merge", "refs/heads/main"});
  // 带 pushurl 的第二个远程（pushurl 回退逻辑的照妖镜）。
  repo.run({"remote", "add", "beta", "https://beta.example/repo.git"});
  repo.run({"config", "remote.beta.pushurl", "https://beta.example/push.git"});
  // 一条 stash。
  repo.write("a.txt", "line1\nstashed\n");
  repo.run({"stash", "push", "-m", "工作中的改动"});

  const auto snapshot = gbcode::read_ref_snapshot(repo.path());

  // —— 分支：与 `read_branches`（同一条路的便捷入口）逐条一致 ——
  const auto branches = gbcode::read_branches(repo.path());
  ST_REQUIRE(!branches.empty());
  ST_CHECK_EQ(snapshot.branches.size(), branches.size());
  for (std::size_t index = 0; index < std::min(snapshot.branches.size(), branches.size());
       ++index) {
    ST_CHECK_EQ(snapshot.branches[index].name, branches[index].name);
    ST_CHECK_EQ(snapshot.branches[index].is_remote, branches[index].is_remote);
    ST_CHECK_EQ(snapshot.branches[index].is_current, branches[index].is_current);
  }
  // `origin`（remote 的 HEAD 别名）不得出现在任何分支里。
  for (const auto& branch : snapshot.branches) ST_CHECK(branch.name != "origin");
  // 名字含 `/` 的本地分支必须留在本地组。
  const auto feature =
      std::find_if(snapshot.branches.begin(), snapshot.branches.end(),
                   [](const gbcode::BranchInfo& branch) { return branch.name == "feature/x"; });
  ST_REQUIRE(feature != snapshot.branches.end());
  ST_CHECK(!feature->is_remote);
  // 当前分支恰好一条，且带上游短名（`origin/main`）。
  std::size_t current = 0;
  for (const auto& branch : snapshot.branches) {
    if (!branch.is_current) continue;
    ++current;
    ST_CHECK(!branch.is_remote);
    ST_CHECK_EQ(branch.upstream_short, std::string("origin/main"));
  }
  ST_CHECK_EQ(current, std::size_t{1});

  // —— 标签：两个都在，注解/轻量分得清 ——
  ST_CHECK_EQ(snapshot.tags.size(), std::size_t{2});
  bool saw_annotated = false;
  bool saw_light = false;
  for (const auto& tag : snapshot.tags) {
    if (tag.name == "annotated") {
      saw_annotated = true;
      ST_CHECK(tag.annotated);
      ST_CHECK_EQ(tag.subject, std::string("注解消息"));
    }
    if (tag.name == "light") {
      saw_light = true;
      ST_CHECK(!tag.annotated);
    }
  }
  ST_CHECK(saw_annotated);
  ST_CHECK(saw_light);
  // 单读版（`read_tags`）与合并版一致（含排序）。
  const auto tags_again = gbcode::read_tags(repo.path());
  ST_CHECK_EQ(tags_again.size(), snapshot.tags.size());
  for (std::size_t index = 0; index < std::min(tags_again.size(), snapshot.tags.size()); ++index) {
    ST_CHECK_EQ(tags_again[index].name, snapshot.tags[index].name);
  }

  // —— 远程：两个都在；未配 pushurl 的回退到拉取 URL ——
  ST_CHECK_EQ(snapshot.remotes.size(), std::size_t{2});
  for (const auto& info : snapshot.remotes) {
    ST_CHECK(!info.fetch_url.empty());
    ST_CHECK(!info.push_url.empty());   // 回退逻辑：不许空
    if (info.name == "beta") {
      ST_CHECK_EQ(info.push_url, std::string("https://beta.example/push.git"));
    }
  }
  const auto remotes = gbcode::read_remotes(repo.path());
  ST_CHECK_EQ(remotes.size(), snapshot.remotes.size());
  for (std::size_t index = 0; index < std::min(remotes.size(), snapshot.remotes.size());
       ++index) {
    ST_CHECK_EQ(remotes[index].name, snapshot.remotes[index].name);
    ST_CHECK_EQ(remotes[index].fetch_url, snapshot.remotes[index].fetch_url);
    ST_CHECK_EQ(remotes[index].push_url, snapshot.remotes[index].push_url);
  }

  // —— stash：一条，消息读得出 ——
  ST_REQUIRE(snapshot.stashes.size() == 1);
  ST_CHECK_EQ(snapshot.stashes.front().index, std::string("stash@{0}"));
  ST_CHECK(snapshot.stashes.front().message.find("工作中的改动") != std::string::npos);

  // —— 引用清单：本地/远程/标签三类都有，且不含 `<remote>/HEAD` ——
  bool saw_local = false;
  bool saw_remote = false;
  bool saw_tag = false;
  for (const auto& ref : snapshot.refs) {
    if (ref.kind == "local" && ref.name == "main") saw_local = true;
    if (ref.kind == "remote" && ref.name == "origin/main") saw_remote = true;
    if (ref.kind == "tag" && ref.name == "light") saw_tag = true;
    ST_CHECK(!ref.name.ends_with("/HEAD"));
  }
  ST_CHECK(saw_local);
  ST_CHECK(saw_remote);
  ST_CHECK(saw_tag);
  ST_CHECK_EQ(gbcode::read_refs(repo.path()).size(), snapshot.refs.size());
}

/// `read_blame` 用 `--porcelain`（同 commit 元信息只出现一次）**不能丢作者/日期**。
///
/// ## 这条守的是什么
///
/// `--line-porcelain` 每行都重发一整套元信息，`--porcelain` **只在某个 commit
/// 第一次出现时**发（实测同一文件 549 KB → 109 KB）。省下来的体积换来一个真实风险：
/// 后续同 commit 的行如果按“块首行 → 立刻读元信息”的朴素写法解析，就会拿到
/// 上一块的空/错误值。这条造“一个提交写 8 行”，此时大部分行共用同一 commit，
/// 正是那条路径。
ST_TEST(git_service_blame_porcelain_reuses_commit_meta) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  std::string text;
  for (int line = 1; line <= 8; ++line) text += std::format("line {}\n", line);
  repo.write("blame.txt", text);
  repo.run({"add", "blame.txt"});
  repo.run({"commit", "-m", "eight lines"});

  const auto lines = gbcode::read_blame(repo.path(), "", "blame.txt", 1, 0);
  ST_REQUIRE(lines.size() == 8);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    // 行号连号（`-L` 不生效时这里会先红）。
    ST_CHECK_EQ(lines[index].line, index + 1);
    // **每一行**都有作者与日期（不是只有第一行有：那是 `--porcelain` 的坑）。
    ST_CHECK_EQ(lines[index].author, std::string("Tester"));
    ST_CHECK_EQ(lines[index].date.size(), std::size_t{10});
    ST_CHECK(!lines[index].hash.empty());
    ST_CHECK_EQ(lines[index].content, std::format("line {}", index + 1));
  }
  // 限定区间：只取第 3 行起 2 行。
  const auto slice = gbcode::read_blame(repo.path(), "", "blame.txt", 3, 2);
  ST_REQUIRE(slice.size() == 2);
  ST_CHECK_EQ(slice.front().line, std::size_t{3});
  ST_CHECK_EQ(slice.front().content, std::string("line 3"));
  ST_CHECK_EQ(slice.back().content, std::string("line 4"));
  // 区间首行的作者也不能空（区间从中间开始时，那个 commit 的元信息**在这段输出里**
  // 仍是第一次出现，所以能读到）。
  ST_CHECK_EQ(slice.front().author, std::string("Tester"));
}

/// `read_audit` 的尾部读取：**超过 64 KB 时只读尾部**，且结果与整读一致。
///
/// ## 这条守的是什么
///
/// 审计文件只追加（每次写操作一条），而界面只要最后 N 条。改成“读文件尾部
/// 64 KB”后有两个边界：
/// ① 窗口首部会**切断一条记录**——必须丢掉那半条（否则上层拿到非法 JSON 行）；
/// ② 截断后仍要给出**准确的最旧在前 N 条**（不是“窗口里的全部”）。
ST_TEST(git_service_audit_tail_reads_last_lines) {
  if (!git_available()) {
    std::cout << "[git] git 未安装，跳过" << '\n';
    return;
  }
  Repo repo;
  ST_REQUIRE(repo.ok);
  const auto git_dir = st::process::run("git", {"rev-parse", "--absolute-git-dir"},
                                       st::process::Options{.cwd = repo.path()});
  ST_REQUIRE(git_dir.has_value());
  const std::string dir = std::string(st::trim(git_dir->stdout_text));
  ST_REQUIRE(!dir.empty());
  const std::string path = st::fs::join(dir, "st-audit.jsonl");

  // 直接灌 800 条（`append_audit` 一次一条；文件格式是公开契约）——每条约 130 字节，
  // 合计 ~105 KB，**超过 64 KB 的尾部窗口**（这正是要测的边界）。
  std::string bulk;
  bulk.reserve(160 * 800);
  for (int index = 0; index < 800; ++index) {
    bulk += std::format(
        "{{\"time\":\"2026-01-01T00:00:00Z\",\"action\":\"op-{:04d}\","
        "\"command\":\"git status --porcelain\",\"result\":\"ok\"}}\n",
        index);
  }
  ST_REQUIRE(st::fs::write_text(path, bulk).has_value());
  std::cout << "[git] 审计文件 " << bulk.size() << " 字节（超过尾部窗口 64 KB）" << '\n';
  ST_REQUIRE(bulk.size() > 64 * 1024);

  const auto tail = gbcode::read_audit(repo.path(), 20);
  ST_REQUIRE(tail.size() == 20);
  // 最旧在前：最后 20 条 = op-0780 .. op-0799。
  ST_CHECK(tail.front().find("op-0780") != std::string::npos);
  ST_CHECK(tail.back().find("op-0799") != std::string::npos);
  // 每一条都是**完整** JSON 行（截断的那半条被丢掉了，不是坏了还在）。
  for (const std::string& line : tail) {
    ST_CHECK(line.starts_with("{\"time\""));
    ST_CHECK(line.ends_with("}"));
  }
  // 小文件路径（窗口未触发时应给出全部）：写一份两条的。
  ST_REQUIRE(st::fs::write_text(path, "{\"a\":1}\n{\"b\":2}\n").has_value());
  const auto small = gbcode::read_audit(repo.path(), 20);
  ST_REQUIRE(small.size() == 2);
  ST_CHECK_EQ(small.front(), std::string("{\"a\":1}"));
  ST_CHECK_EQ(small.back(), std::string("{\"b\":2}"));
}
