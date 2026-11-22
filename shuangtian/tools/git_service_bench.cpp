/// Git 服务层性能量尺：把「读一面数据要起几条子进程、花多少毫秒」量出来。
///
/// 用法: git_service_bench [repo] [iterations]
///
/// ## 为什么需要它（而不是"看着挺快"）
///
/// `git_service` 的每一次「刷新」都是**子进程**：`git status` 在本仓实测 80 ms，
/// 而 `read_log_snapshot` 一口气起 9 条（log/rev-list/branches/tags/remotes×3/
/// stash/refs/upstream/operation/audit）——串行相加就是数百毫秒到数秒
/// （gbcode 打开 `D:/coding/modeltrain-be` 时实测 3300 ms，见 `page_actions.cpp`
/// 的 `ensure_git_log` 注释）。这类"条数 × 单条成本"的问题**只能靠数**：
/// 肉眼看界面只觉得"卡一下"，量出来才知道是哪几条、各占多少。
///
/// ## 它量什么、不量什么
///
/// 量：每条公开读入口的**墙钟耗时**（best-of-N，去掉首次的冷启动与抖动）。
/// 不量：解析耗时（那是微秒级，与子进程不在一个量级）、界面重组。
/// 两者都不该进这个数——混进来就分不清"是 git 慢还是我们慢"。
///
/// 输出**逐条**列出，因为优化的靶子是**条数**：能合并的合并、能并发的并发，
/// 合并后单条会变慢一点（多取几个字段），但总条数与总墙钟会显著下降。

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "apps/gbcode/git_service.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"

namespace {

/// 跑 `fn` 共 `iterations` 次，取最快一次（毫秒）。
///
/// 取最快而不是平均：这个量尺关心的是"这条路最少要多久"（优化后的下界），
/// 平均值会被第一次的冷启动（OS 文件缓存未热）与偶发调度抬高。
template <class Fn>
[[nodiscard]] auto best_ms(int iterations, Fn&& fn) -> double {
  double best = 1e18;
  for (int i = 0; i < iterations; ++i) {
    const std::int64_t begin = st::time::now_ns();
    fn();
    const double elapsed = static_cast<double>(st::time::now_ns() - begin) / 1.0e6;
    best = std::min(best, elapsed);
  }
  return best;
}

void report(const std::string& label, double ms, std::size_t count, double* total) {
  *total += ms;
  st::print("{:>8.1f} ms  {:<26} {}\n", ms, label,
            count == 0 ? std::string{} : std::format("({} 条)", count));
}

}  // namespace

int main(int argc, char** argv) {
  const std::string repo = argc > 1 ? argv[1] : ".";
  const int iterations = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;

  // 预热一次：让 OS 文件缓存热起来（否则第一条被测项永远是最慢的，
  // 那是启动成本，不是这条路本身的成本）。
  (void)st::process::run("git", {"rev-parse", "--show-toplevel"});

  st::print("仓库：{}\n量尺：best-of-{}\n\n", repo, iterations);
  double total = 0.0;

  const auto snapshot = gbcode::read_snapshot(repo);
  report("read_snapshot (status)", best_ms(iterations, [&] { (void)gbcode::read_snapshot(repo); }),
         snapshot.changes.size(), &total);

  {
    const auto value = gbcode::read_branches(repo);
    report("read_branches", best_ms(iterations, [&] { (void)gbcode::read_branches(repo); }),
           value.size(), &total);
  }
  {
    const auto value = gbcode::read_tags(repo);
    report("read_tags", best_ms(iterations, [&] { (void)gbcode::read_tags(repo); }),
           value.size(), &total);
  }
  {
    const auto value = gbcode::read_remotes(repo);
    report("read_remotes", best_ms(iterations, [&] { (void)gbcode::read_remotes(repo); }),
           value.size(), &total);
  }
  {
    const auto value = gbcode::read_stashes(repo);
    report("read_stashes", best_ms(iterations, [&] { (void)gbcode::read_stashes(repo); }),
           value.size(), &total);
  }
  {
    const auto value = gbcode::read_refs(repo);
    report("read_refs", best_ms(iterations, [&] { (void)gbcode::read_refs(repo); }), value.size(),
           &total);
  }
  {
    const auto value = gbcode::read_log(repo, 200);
    report("read_log(200)", best_ms(iterations, [&] { (void)gbcode::read_log(repo, 200); }),
           value.size(), &total);
  }
  {
    const auto value = gbcode::read_log_and_graph(repo, 200);
    report("read_log_and_graph(200)",
           best_ms(iterations, [&] { (void)gbcode::read_log_and_graph(repo, 200); }),
           value.log.size(), &total);
  }
  {
    const auto value = gbcode::read_commit_graph(repo, 200);
    report("read_commit_graph(200)",
           best_ms(iterations, [&] { (void)gbcode::read_commit_graph(repo, 200); }),
           value.nodes.size(), &total);
  }
  report("read_upstream", best_ms(iterations, [&] { (void)gbcode::read_upstream(repo); }), 0,
         &total);
  {
    const auto value = gbcode::read_operation_state(repo);
    report("read_operation_state",
           best_ms(iterations, [&] { (void)gbcode::read_operation_state(repo); }),
           value.conflicts.size(), &total);
  }
  {
    const auto value = gbcode::read_audit(repo, 20);
    report("read_audit(20)", best_ms(iterations, [&] { (void)gbcode::read_audit(repo, 20); }),
           value.size(), &total);
  }
  {
    const auto value = gbcode::read_log_snapshot(repo);
    report("read_log_snapshot（整批）",
           best_ms(iterations, [&] { (void)gbcode::read_log_snapshot(repo); }), value.log.size(),
           &total);
  }
  {
    const auto value = gbcode::read_ref_snapshot(repo);
    report("read_ref_snapshot（引用族整批）",
           best_ms(iterations, [&] { (void)gbcode::read_ref_snapshot(repo); }),
           value.branches.size() + value.tags.size() + value.remotes.size() + value.stashes.size(),
           &total);
  }

  // ── 对照：**旧实现的命令序列**（原位复刻，不是估计） ──
  //
  // ## 为何要在量尺里重建旧路径
  //
  // “优化了多少”必须对着**真正跑过的旧代码**量，而不是拿文档里的历史数字比
  //（那批数字来自另一台机器/另一个仓库，不可比）。所以这里把优化前的命令序列
  // **一行不改地重跑一遍**：
  //
  //   旧 `read_log_snapshot`（ 12 条串行）＝
  //     log(无 %P) ＋ rev-list --parents ＋ branch --all ＋ for-each-ref(tags)
  //     ＋ for-each-ref(heads,remotes) ＋ remote ＋ 每个 remote 两条 get-url
  //     ＋ stash list ＋ rev-parse @{upstream} ＋ 状态文件探测 ＋ rev-parse --git-dir
  //
  // 新实现是 5 条（日志族 1 ＋ 引用族 3 条并发 ＋ 操作态 0 ＋ 审计 1）。
  // 两边都在**同一个仓库、同一台机器、同一个进程**里量，差值才是真实收益。
  st::print("\n── 对照：旧实现的命令序列（同机同仓复刻）──\n");
  const auto legacy_snapshot = [&repo]() {
    (void)st::process::run("git", {"log", "--no-color", "-200",
                                  "--pretty=format:%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%D",
                                  "--date=format:%Y-%m-%d %H:%M", "--all"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"rev-list", "--parents", "--topo-order", "-200", "--all"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"branch", "--all", "--no-abbrev"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"for-each-ref", "--sort=-creatordate",
                                   "--format=%(refname:short)\x1f%(objecttype)\x1f"
                                   "%(objectname:short)\x1f%(contents:subject)",
                                   "refs/tags"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"for-each-ref",
                                   "--format=%(refname)\x1f%(refname:short)\x1f%(objectname:short)",
                                   "refs/heads", "refs/remotes", "refs/tags"},
                          st::process::Options{.cwd = repo});
    // 远程：`git remote` + 每个远程两条 `get-url`（这里按真实仓库的远程数展开）。
    const auto names = st::process::run("git", {"remote"}, st::process::Options{.cwd = repo});
    if (names.has_value()) {
      std::string_view text = names->stdout_text;
      while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        const std::string name(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (name.empty()) continue;
        (void)st::process::run("git", {"remote", "get-url", name},
                              st::process::Options{.cwd = repo});
        (void)st::process::run("git", {"remote", "get-url", "--push", name},
                              st::process::Options{.cwd = repo});
      }
    }
    (void)st::process::run("git", {"stash", "list", "--format=%gd\x1f%gs"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"rev-parse", "--abbrev-ref", "--symbolic-full-name",
                                   "@{upstream}"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"rev-parse", "--absolute-git-dir"},
                          st::process::Options{.cwd = repo});
  };
  const double legacy_ms = best_ms(iterations, legacy_snapshot);
  st::print("{:>8.1f} ms  旧 read_log_snapshot（12 条串行）\n", legacy_ms);

  // 旧的两个“界面高频路径”也一并对照：
  //   · `read_snapshot`（`status` 非 `-z`）—— 这条本来就是一条子进程，收益只在解析；
  //   · 日志 + 拓扑（两条命令，同一段历史读两遍）—— 新实现一条。
  const double legacy_log_and_graph_ms = best_ms(iterations, [&repo] {
    (void)st::process::run("git", {"log", "--no-color", "-200",
                                  "--pretty=format:%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%D",
                                  "--date=format:%Y-%m-%d %H:%M", "--all"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"rev-list", "--parents", "--topo-order", "-200", "--all"},
                          st::process::Options{.cwd = repo});
  });
  st::print("{:>8.1f} ms  旧 日志+拓扑（log + rev-list 两条）\n", legacy_log_and_graph_ms);

  const double legacy_commit_detail_ms = best_ms(iterations, [&repo] {
    // 旧 `read_commit_detail`：三条命令取同一个提交。
    (void)st::process::run("git", {"show", "--no-color", "--format=%B", "-s", "HEAD"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"show", "--no-color", "--numstat", "--format=", "HEAD"},
                          st::process::Options{.cwd = repo});
    (void)st::process::run("git", {"show", "--no-color", "HEAD"},
                          st::process::Options{.cwd = repo});
  });
  st::print("{:>8.1f} ms  旧 read_commit_detail（3 条）\n", legacy_commit_detail_ms);

  {
    gbcode::LogEntry entry;
    entry.hash = "HEAD";
    const double now_ms =
        best_ms(iterations, [&] { (void)gbcode::read_commit_detail(repo, entry); });
    st::print("{:>8.1f} ms  新 read_commit_detail（1 条）\n", now_ms);
  }

  st::print("\n逐条串行合计（不含整批）：约 {:.1f} ms\n", total);
  return 0;
}
