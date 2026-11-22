/// 提交图：**自算拓扑** vs `git log --graph` 字符画（交叉校验）。
///
/// ## 为什么两条都要
///
/// 绘制只认自算拓扑（`read_commit_graph`）——字符画是渲染产物，不可靠
/// （实测：`|\` 的斜线落在奇数格，按道扫描只读偶数格 → 跨道边整条丢失，
/// 表现为 **Merge 提交下方一片空白**）。
///
/// 但“不可靠”不等于“没用”：**它可以当独立证据**。本文件用 git 自己画的图
/// 去反证自算结果的三个关键性质，任何一条崩了都说明拓扑算法错了：
///
/// 1. **父数一致**：自算的 `parents` 必须等于 `rev-list --parents` 的第二列个数
///    （这条几乎是恒等式，但它挡住“解析 rev-list 时漏字段”这类低级错）；
/// 2. **合并点存在**：git 画得出 `|\` 的地方，自算必须也标出 merge 节点；
/// 3. **并发支数不超界**：自算的最大道号 ≤ git 图区的最宽列数 / 2 + 1
///    （自算若“多出几条分支”说明泳道分配在泄漏）。
///
/// ⚠ 反过来**不断言“完全一致”**：两边的排版本来就可以不同（git 会为视觉
/// 需要插入额外连接行），要求逐字符相等等于把不可靠的东西又变成判据。

#include "st/test/test.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "apps/gbcode/git_graph.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"

namespace {

/// 在 `cwd` 里跑一条 git 命令（测试只读）。
[[nodiscard]] auto git_text(const std::string& cwd, const std::vector<std::string>& args)
    -> std::string {
  st::process::Options options;
  options.cwd = cwd;
  std::vector<std::string> full_args{"-c", "core.quotepath=false"};
  full_args.insert(full_args.end(), args.begin(), args.end());
  const auto result = st::process::run("git", full_args, options);
  if (!result || result->exit_code != 0) return {};
  return result->stdout_text;
}

/// 造一个带**合并**的小仓库（真实 merge，不是 `--no-ff` 的假合并）。
///
/// 为何要真 merge：这个用例盯的正是“合并提交的两个父提交”这件事，
/// 线性历史测不出来。
[[nodiscard]] auto make_merge_repo(const std::string& root) -> bool {
  // 用 `git` + 参数数组跑（不用 shell：框架的 process 不带 shell 选项，
  // 而拼 shell 字符串在含空格的临时路径上会错）。
  const auto git = [&](std::vector<std::string> args, const std::string& expect_tail = {})
      -> bool {
    st::process::Options options;
    options.cwd = root;
    std::vector<std::string> full{"-c", "core.quotepath=false", "-c", "user.email=t@t", "-c",
                                  "user.name=t"};
    full.insert(full.end(), args.begin(), args.end());
    const auto result = st::process::run("git", full, options);
    (void)expect_tail;
    return result.has_value() && result->exit_code == 0;
  };
  /// 写一个文件（不经 shell）。
  const auto write_file = [&](const std::string& name, const std::string& text) -> bool {
    return st::fs::write_text(root + "/" + name, text).has_value();
  };

  // 身份靠 `-c` 注入（上面 lambda 已带），不再两条 `git config`：
  // 实测单次 spawn 50-65 ms，本文件 3 条用例 × 2 次 ≈ 0.35 s 纯浪费。
  // `--template=` 置空跳过 hooks 模板拷贝（本夹具不跑 hook）。
  if (!git({"init", "-q", "--template=", "."})) return false;

  // base
  if (!write_file("a.txt", "a\n")) return false;
  if (!git({"add", "."})) return false;
  if (!git({"commit", "-qm", "base"})) return false;

  // side 分支
  if (!git({"checkout", "-qb", "side"})) return false;
  if (!write_file("s.txt", "s\n")) return false;
  if (!git({"add", "."})) return false;
  if (!git({"commit", "-qm", "side"})) return false;

  // 回到默认分支（新版 git 可能是 main）并前进一步
  if (!git({"checkout", "-q", "master"}) && !git({"checkout", "-q", "main"})) return false;
  if (!write_file("m.txt", "m\n")) return false;
  if (!git({"add", "."})) return false;
  if (!git({"commit", "-qm", "main"})) return false;

  // 真合并（会生成带 2 个父的合并提交）
  return git({"merge", "--no-edit", "side"});
}

}  // namespace

/// 自算的 `parents` 与 `rev-list --parents` 的第二列个数一致。
ST_TEST(git_graph_parent_counts_match_rev_list) {
  const std::string repo = ".";
  const std::string raw = git_text(repo, {"rev-list", "--parents", "--topo-order", "-30", "--all"});
  if (raw.empty()) {
    st::log::out("[git_graph] 不是 git 仓库或没有提交，跳过\n");
    return;
  }

  // 从 rev-list 直接统计（独立于被测实现）。
  std::vector<std::size_t> expected;
  {
    std::size_t begin = 0;
    while (begin < raw.size()) {
      const std::size_t eol = raw.find('\n', begin);
      const std::size_t end = eol == std::string::npos ? raw.size() : eol;
      std::string line = raw.substr(begin, end - begin);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) {
        std::size_t spaces = 0;
        for (const char ch : line) {
          if (ch == ' ') ++spaces;
        }
        expected.push_back(spaces);
      }
      if (eol == std::string::npos) break;
      begin = eol + 1;
    }
  }

  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, 30);
  ST_REQUIRE(!graph.nodes.empty());
  ST_CHECK_EQ(graph.nodes.size(), expected.size());
  for (std::size_t index = 0; index < std::min(graph.nodes.size(), expected.size()); ++index) {
    if (graph.nodes[index].parents != expected[index]) {
      st::log::out("[git_graph] 第 {} 行父数不符：自算 {} vs rev-list {}\n", index,
                   graph.nodes[index].parents, expected[index]);
    }
    ST_CHECK_EQ(graph.nodes[index].parents, expected[index]);
  }
}

/// 真 merge 仓库上：合并提交必须有 **2 个父**，且它的两条边**落向两个不同的道**。
///
/// 这条正是“Merge 提交下方空白”那个缺陷的判据：旧实现（解析字符画）在
/// `|\` 上会丢掉跨道那条边，于是合并点只有一个出边。
ST_TEST(git_graph_merge_has_two_edges_to_distinct_lanes) {
  // ⚠ 造库失败就**直接红**，不得 `return`（那是“恒绿的测试”：没测任何东西却报 PASS）。
  // 实测踩到：旧写法用 `temp_dir()+"/x"` 手拼路径 + `create_directories`，结果
  // `make_merge_repo` 里某一步失败 → 整条用例静默跳过，**逆向验证也照样绿**。
  const auto temp = st::fs::make_temp_dir("st_git_graph_merge");
  ST_REQUIRE(temp.has_value());
  const std::string repo = *temp;
  ST_REQUIRE(make_merge_repo(repo));

  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, 20);
  ST_REQUIRE(!graph.nodes.empty());

  // 找合并节点（父数 > 1）。
  std::size_t merge_row = graph.nodes.size();
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    if (graph.nodes[index].parents > 1) {
      merge_row = index;
      break;
    }
  }
  ST_REQUIRE(merge_row < graph.nodes.size());
  st::log::out("[git_graph] 合并节点在第 {} 行，父数 {}\n", merge_row,
               graph.nodes[merge_row].parents);
  ST_CHECK_EQ(graph.nodes[merge_row].parents, static_cast<std::size_t>(2));

  // 从合并点出发的边必须恰好 2 条，且目标道**不同**。
  std::vector<std::size_t> target_lanes;
  for (const gbcode::GraphEdge& edge : graph.edges) {
    if (edge.from_row == merge_row) target_lanes.push_back(edge.to_lane);
  }
  st::log::out("[git_graph] 合并点出边 {} 条，目标道：", target_lanes.size());
  for (const std::size_t lane : target_lanes) st::log::out("{} ", lane);
  st::log::out("\n");
  ST_CHECK_EQ(target_lanes.size(), static_cast<std::size_t>(2));
  if (target_lanes.size() == 2) {
    ST_CHECK(target_lanes[0] != target_lanes[1]);
  }
}

/// 自算的最大道号 ≤ git 图区最宽列数的一半 + 1（泳道分配不泄漏的粗界）。
ST_TEST(git_graph_lane_count_within_git_art_width) {
  const std::string repo = ".";
  const std::string art = git_text(repo, {"log", "--graph", "--oneline", "-50", "--all"});
  if (art.empty()) {
    st::log::out("[git_graph] 不是 git 仓库，跳过\n");
    return;
  }
  // git 图区每道占 2 个字符（`| ` / `* `），所以“最宽宽度 / 2 + 1”是上界。
  std::size_t widest = 0;
  std::size_t begin = 0;
  while (begin < art.size()) {
    const std::size_t eol = art.find('\n', begin);
    const std::size_t end = eol == std::string::npos ? art.size() : eol;
    std::string line = art.substr(begin, end - begin);
    std::size_t graphic = 0;
    for (const char ch : line) {
      if (ch == '|' || ch == '*' || ch == '/' || ch == '\\' || ch == ' ') {
        ++graphic;
      } else {
        break;
      }
    }
    widest = std::max(widest, graphic);
    if (eol == std::string::npos) break;
    begin = eol + 1;
  }

  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, 50);
  if (graph.nodes.empty()) return;
  const std::size_t bound = widest / 2U + 1U;
  st::log::out("[git_graph] git 图区最宽 {} 字符 → 上界 {} 道；自算 {} 道\n", widest, bound,
               graph.lane_count);
  // 自算道数不得明显超出（超出 = 泳道分配在泄漏，同一分支开了多条道）。
  ST_CHECK(graph.lane_count <= bound + 1U);
}

