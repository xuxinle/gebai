// 「工作区是仓库子目录」时的路径口径。
//
// 缺陷实录（2026-11-05）：用户在 `gebai/shuangtian` 打开 gbcode（仓库根是 `gebai`），
// 点变更列表里的 `shuangtian/examples/gbcode/page.hpp` —— 差异视图显示
// **「（删除文件）+0 −2132」**，右栏 2132 行全是空行。
//
// 根因：git 报告的路径**相对仓库根**，而 `read_content_at(Worktree)` 直接
// `join(cwd, path)`。cwd 传的是“用户打开的工作区”，于是拼成
// `gebai/shuangtian/shuangtian/examples/...` —— 读不到 ⇒ 判成“删除文件”。
// 左栏走 `git show`（git 自己向上找仓库）所以是好的，于是差异显示成“整篇删掉”。
//
// 修法：先 `git rev-parse --show-toplevel` 取仓库根，所有“拼磁盘路径”的地方都用它。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "st/test/test.hpp"

#include "examples/gbcode/git_service.hpp"

namespace {

/// 建一个真 git 仓库：`repo/sub/deep`，返回仓库根。
struct NestedRepo {
  std::filesystem::path root;
  std::filesystem::path inner;   // 仓库里的子目录（模拟“用户打开的工作区”）
  bool ok{false};

  NestedRepo() {
    std::error_code ec;
    root = std::filesystem::temp_directory_path(ec) / "st_nested_repo_probe";
    std::filesystem::remove_all(root, ec);
    inner = root / "sub";
    std::filesystem::create_directories(inner, ec);
    if (ec) return;
    if (!run_git(root, {"init", "-q", "-b", "main"})) return;
    (void)run_git(root, {"config", "user.email", "t@local"});
    (void)run_git(root, {"config", "user.name", "T"});
    std::ofstream(inner / "file.txt") << "alpha\nbeta\n";
    (void)run_git(root, {"add", "."});
    ok = run_git(root, {"commit", "-qm", "first"});
    if (ok) {
      // 改一行（留在工作区、不提交）——差异的“新侧”就该有它。
      std::ofstream(inner / "file.txt") << "alpha\nBETA\n";
    }
  }

  ~NestedRepo() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  static auto run_git(const std::filesystem::path& cwd,
                      const std::vector<std::string>& args) -> bool {
    std::string command = "git";
    for (const auto& arg : args) {
      command += " ";
      command += arg;
    }
    command = "cd /d \"" + cwd.string() + "\" && " + command + " >nul 2>&1";
    return std::system(command.c_str()) == 0;
  }
};

}  // namespace

ST_TEST(git_read_repo_root_finds_toplevel_from_subdirectory) {
  NestedRepo repo;
  if (!repo.ok) return;   // 没有 git 就跳过（不把环境缺失算成失败）

  const std::string from_inner = gbcode::read_repo_root(repo.inner.string());
  ST_REQUIRE(!from_inner.empty());
  // 从子目录出发也要拿到**仓库根**（而不是传进去的那个子目录）。
  //
  // ⚠ 不能直接比字符串：Windows 上 git 可能回 8.3 短名（`ADMINI~1`）而
  // `temp_directory_path()` 给长名（`Administrator`）——两者是同一个目录。
  // 用 `equivalent` 比**文件系统身份**才是正确的判据。
  std::error_code ec;
  ST_CHECK(std::filesystem::equivalent(std::filesystem::path(from_inner),
                                      std::filesystem::canonical(repo.root), ec));
  ST_CHECK(!ec);

  // 从仓库根出发同样对得上（幂等）。
  ec.clear();
  ST_CHECK(std::filesystem::equivalent(
      std::filesystem::path(gbcode::read_repo_root(repo.root.string())),
      std::filesystem::canonical(repo.root), ec));
  ST_CHECK(!ec);
}

ST_TEST(git_read_content_at_worktree_uses_repo_root_not_workspace) {
  NestedRepo repo;
  if (!repo.ok) return;

  // ⚠ 这是缺陷的核心：路径是**相对仓库根**的（`sub/file.txt`），
  // 而“用户打开的工作区”是 `repo/sub`。
  const std::string root = gbcode::read_repo_root(repo.inner.string());
  ST_REQUIRE(!root.empty());

  const auto good = gbcode::read_content_at(root, gbcode::DiffEndpoint::parse("WORKTREE"),
                                            "sub/file.txt");
  ST_CHECK(good.ok);
  ST_CHECK(!good.missing);
  ST_CHECK(good.text.find("BETA") != std::string::npos);

  // 用**工作区**当根（旧行为）会拼出 `repo/sub/sub/file.txt` ⇒ 读不到 ⇒ missing。
  // 这条断言是“缺陷可复现”的证明：它保证上面那行不是碰巧通过。
  const auto bad = gbcode::read_content_at(repo.inner.string(),
                                           gbcode::DiffEndpoint::parse("WORKTREE"),
                                           "sub/file.txt");
  ST_CHECK(bad.missing);
}

ST_TEST(git_read_content_at_missing_file_is_not_an_error) {
  NestedRepo repo;
  if (!repo.ok) return;
  const std::string root = gbcode::read_repo_root(repo.inner.string());
  ST_REQUIRE(!root.empty());
  // “那一侧没有这个文件”必须与“真失败”分开：新增/删除就是这样。
  const auto absent = gbcode::read_content_at(root, gbcode::DiffEndpoint::parse("WORKTREE"),
                                              "sub/nope.txt");
  ST_CHECK(absent.missing);
  ST_CHECK(!absent.ok);
  ST_CHECK(absent.error.empty());   // 不是错误
}
