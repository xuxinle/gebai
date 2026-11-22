/// `read_branches` 的**分类正确性**：remote 的 HEAD 别名不能混进本地分支。
///
/// ## 缺陷现场（用户截图）
///
/// “本地 (3)”下面除了 `master` 还多了 `github` 与 `origin` 两项——它们不是分支，
/// 是 **remote 的 HEAD 别名**（`refs/remotes/origin/HEAD` → 指向 `refs/remotes/origin/master`）。
/// 点这两项切分支必然失败（`git checkout origin` 会当作“切到远程分支”而进 detached）。
///
/// ## 两个判据都必须用上（各挡一类错）
///
/// 1. **符号引用**：`%(symref)` 非空 ⇒ 丢弃。
///    旧实现按字符串前缀认 `remotes/origin/HEAD`，而 `%(refname:short)` 输出的是
///    裸的 `origin`（看不出是别名）→ 漏过滤。
/// 2. **远程判定看完整 refname 的 `refs/remotes/` 前缀**，不看“名字里有没有 `/`”。
///    本地分支名本来就可以含 `/`（`feature/x`）——用“含 `/`”会把它们误判成远程，
///    于是本地组里看不到、远程组里凭空多出来。
///
/// 这两条合起来才是“分支与远程不错乱”。

#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "apps/gbcode/git_service.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"

namespace {

[[nodiscard]] auto git_ok(const std::string& cwd, const std::vector<std::string>& args) -> bool {
  st::process::Options options;
  options.cwd = cwd;
  // 身份+quotepath 一次带齐：这三条 `-c` 不额外花进程（与子命令同一次 spawn），
  // 而写成 `git config x y` 就是三次**真的** spawn——实测单次 50-65 ms，
  // 本文件 4 条用例 × 每仓 3 次 = 12 次 ≈ 0.7 s 纯浪费。
  std::vector<std::string> full{"-c", "core.quotepath=false", "-c", "user.email=t@t", "-c",
                                "user.name=t"};
  full.insert(full.end(), args.begin(), args.end());
  const auto result = st::process::run("git", full, options);
  return result.has_value() && result->exit_code == 0;
}

/// 造一个**有 remote 别名 + 带斜杠的本地分支**的仓库。
///
/// 三样缺一不可：
/// ① 一个真 remote（才会有 `refs/remotes/<name>/HEAD` 这种别名）；
/// ② `git remote set-head --auto`（否则别名不存在，测不到缺陷）；
/// ③ 一个**名字含 `/` 的本地分支**（`feature/x`）——它是第 2 条判据的照妖镜。
///
/// ## 种子复用：4 条用例只**建一次**仓（2026-11-22）
///
/// 4 条用例对仓库的初始状态要求**完全一致**（这就是本函数的全部产物），而建一次要
/// 11 次 git 子进程（实测单次 50-65 ms，合计 ~1.2 s）。所以：进程内首次建好后保留，
/// 后续每条 `copy` 一份（`.git` + 3 个文件，实测 ~116 ms）。
///
/// 安全前提（与 `git_service_test.cpp` 的 `Repo` 同一套）：种子仓**从不被用例碰到**——
/// 交出去的是副本，`read_branches` 只读。分片并行仍安全：副本名带 `pid + 序号`。
///
/// ⚠ 必须是**副本**而不是共享同一个目录：`read_branches` 虽然只读，但同一进程里
/// 多条用例并发跑（用例在线程里），而且外部 git 可能在仓里写 index 缓存——
/// 副本把“同族用例共享可变资源”这个风险彻底关掉。
[[nodiscard]] auto seed_repo_dir() -> const std::string& {
  // lint-allow: L8 夹具种子仓：仅本测试二进制内、随进程退出失效
  static const std::string path =
      (std::filesystem::temp_directory_path() /
       std::format("st_branches_seed_{}", static_cast<int>(st::process::current_id())))
          .string();
  return path;
}

[[nodiscard]] auto build_seed_repo(const std::string& root) -> bool;

[[nodiscard]] auto make_repo(const std::string& root) -> bool {
  // lint-allow: L8 夹具的“种子已就绪”标志（与 seed_repo_dir 配套）
  static std::once_flag once;
  // lint-allow: L8 同上
  static bool seeded = false;
  std::call_once(once, [] {
    std::error_code error;
    std::filesystem::remove_all(seed_repo_dir(), error);
    std::filesystem::create_directories(seed_repo_dir());
    seeded = build_seed_repo(seed_repo_dir());
  });
  if (!seeded) return false;
  std::error_code copy_error;
  std::filesystem::remove_all(root, copy_error);
  std::filesystem::copy(seed_repo_dir(), root, std::filesystem::copy_options::recursive,
                        copy_error);
  return !copy_error;
}

[[nodiscard]] auto build_seed_repo(const std::string& root) -> bool {
  if (!git_ok(root, {"init", "-q", "--template=", "."})) return false;
  if (!st::fs::write_text(root + "/a.txt", "a\n").has_value()) return false;
  if (!git_ok(root, {"add", "."})) return false;
  if (!git_ok(root, {"commit", "-qm", "base"})) return false;
  // 名字带 `/` 的本地分支：用来验“远程判定不能靠含 `/`”。
  if (!git_ok(root, {"branch", "feature/x"})) return false;
  // 一个 bare 远程仓库 + 把 base 推上去（这样 remote 有真分支，set-head 才有目标）。
  const std::string remote = root + "/remote.git";
  if (!git_ok(root, {"init", "-q", "--bare", "--template=", remote})) return false;
  if (!git_ok(root, {"remote", "add", "origin", remote})) return false;
  if (!git_ok(root, {"push", "-q", "origin", "HEAD:refs/heads/master"})) return false;
  if (!git_ok(root, {"fetch", "-q", "origin"})) return false;
  // 建 `<remote>/HEAD` 别名（关键一步）。
  (void)git_ok(root, {"remote", "set-head", "origin", "--auto"});
  return true;
}

}  // namespace

/// **remote 的 HEAD 别名不得出现在任何分支列表里**（它是别名，不是可切分支）。
ST_TEST(branches_skip_remote_head_symrefs) {
  const auto temp = st::fs::make_temp_dir("st_branches_symref");
  ST_REQUIRE(temp.has_value());
  const std::string repo = *temp;
  ST_REQUIRE(make_repo(repo));

  const auto branches = gbcode::read_branches(repo);
  st::log::out("[branches] 读到 {} 项：", branches.size());
  for (const auto& branch : branches) {
    st::log::out(" {}{}", branch.name, branch.is_remote ? "(远程)" : "");
  }
  st::log::out("\n");

  // 名字恰好是 remote 名的（`origin`）必然是别名 —— 不许出现。
  for (const auto& branch : branches) {
    if (branch.name == "origin") {
      st::log::out("[branches] 泄漏：origin 被当成{}分支\n", branch.is_remote ? "远程" : "本地");
    }
    ST_CHECK(branch.name != "origin");
  }
}

/// **名字含 `/` 的本地分支必须留在本地组**（远程判定看 refname 前缀，不看 `/`）。
ST_TEST(branches_with_slash_stay_local) {
  const auto temp = st::fs::make_temp_dir("st_branches_slash");
  ST_REQUIRE(temp.has_value());
  const std::string repo = *temp;
  ST_REQUIRE(make_repo(repo));

  const auto branches = gbcode::read_branches(repo);
  const auto found = std::find_if(branches.begin(), branches.end(),
                                  [](const gbcode::BranchInfo& branch) {
                                    return branch.name == "feature/x";
                                  });
  ST_REQUIRE(found != branches.end());
  st::log::out("[branches] feature/x is_remote={}（应为 false）\n", found->is_remote);
  ST_CHECK(!found->is_remote);
  // 短名就是它自己（本地分支不剥前缀）。
  ST_CHECK_EQ(std::string(gbcode::short_branch_name(*found)), std::string("feature/x"));
  // 远程名（remote_of_branch）对本地分支为空。
  ST_CHECK(gbcode::remote_of_branch(*found).empty());
}

/// **远程分支**：必须标成远程，且短名剥掉 remote 前缀、能查出所属 remote。
ST_TEST(branches_remote_flags_and_short_names) {
  const auto temp = st::fs::make_temp_dir("st_branches_remote");
  ST_REQUIRE(temp.has_value());
  const std::string repo = *temp;
  ST_REQUIRE(make_repo(repo));

  const auto branches = gbcode::read_branches(repo);
  const auto found = std::find_if(branches.begin(), branches.end(),
                                  [](const gbcode::BranchInfo& branch) {
                                    return branch.name == "origin/master";
                                  });
  ST_REQUIRE(found != branches.end());
  ST_CHECK(found->is_remote);
  // 短名去掉 `origin/`（展示与搜索都用它）。
  ST_CHECK_EQ(std::string(gbcode::short_branch_name(*found)), std::string("master"));
  ST_CHECK_EQ(gbcode::remote_of_branch(*found), std::string("origin"));
}

/// 当前分支恰好一个（`is_current` 互斥），且它在**本地**组里。
ST_TEST(branches_have_exactly_one_current) {
  const auto temp = st::fs::make_temp_dir("st_branches_current");
  ST_REQUIRE(temp.has_value());
  const std::string repo = *temp;
  ST_REQUIRE(make_repo(repo));

  const auto branches = gbcode::read_branches(repo);
  std::size_t current = 0;
  for (const auto& branch : branches) {
    if (!branch.is_current) continue;
    ++current;
    ST_CHECK(!branch.is_remote);   // 当前分支必然是本地分支
  }
  st::log::out("[branches] 当前分支数 = {}\n", current);
  ST_CHECK_EQ(current, std::size_t{1});
}
