/// git 提交图：从**真实拓扑**算泳道与边（不解析 `--graph` 字符画）。
///
/// 结构与取舍见 `git_graph.hpp` 的头注释。

#include <cstddef>
#include <string>
#include <vector>

#include "git_graph.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "st/core/process.hpp"
#include "st/core/string.hpp"

namespace gbcode {

namespace {

/// 跑一条 git 子命令（与 `git_service.cpp` 同口径：失败返回 nullopt）。
[[nodiscard]] auto git(const std::string& cwd, const std::vector<std::string>& args)
    -> std::optional<std::string> {
  st::process::Options options;
  options.cwd = cwd;
  std::vector<std::string> full_args{"-c", "core.quotepath=false"};
  full_args.insert(full_args.end(), args.begin(), args.end());
  const auto result = st::process::run("git", full_args, options);
  if (!result) return std::nullopt;
  if (result->exit_code != 0) return std::nullopt;
  return result->stdout_text;
}

}  // namespace

auto read_commit_graph(const std::string& cwd, std::size_t limit, const std::string& ref)
    -> CommitGraph {
  CommitGraph graph;

  // `rev-list --parents --topo-order`：每行 `<hash> <parent>…`。
  //
  // 为何是 `--topo-order`：它保证**子提交一定排在父提交之前**——这是逐行分配泳道
  // 的前提（否则“父提交还没到，不知道该把它放进哪一道”）。
  // 为何不用 `--graph`：那是渲染产物（见头注释里的实测教训）。
  std::vector<std::string> args{"rev-list", "--parents", "--topo-order",
                                std::format("-{}", limit)};
  if (ref.empty()) {
    args.push_back("--all");
  } else {
    args.push_back(ref);
  }
  const auto out = git(cwd, args);
  if (!out.has_value()) return graph;

  // 逐行解析成 (hash, parents)。
  struct RevLine {
    std::string hash{};
    std::vector<std::string> parents{};
  };
  std::vector<RevLine> revs;
  {
    std::size_t begin = 0;
    const std::string& text = *out;
    while (begin < text.size()) {
      const std::size_t eol = text.find('\n', begin);
      const std::size_t end = eol == std::string::npos ? text.size() : eol;
      std::string_view line(text.data() + begin, end - begin);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (!line.empty()) {
        RevLine rev;
        std::size_t cursor = 0;
        bool first = true;
        while (cursor <= line.size()) {
          const std::size_t space = line.find(' ', cursor);
          const std::size_t stop = space == std::string_view::npos ? line.size() : space;
          std::string token(line.substr(cursor, stop - cursor));
          if (!token.empty()) {
            if (first) {
              rev.hash = std::move(token);
              first = false;
            } else {
              rev.parents.push_back(std::move(token));
            }
          }
          if (space == std::string_view::npos) break;
          cursor = space + 1;
        }
        if (!rev.hash.empty()) revs.push_back(std::move(rev));
      }
      if (eol == std::string::npos) break;
      begin = eol + 1;
    }
  }
  if (revs.empty()) return graph;

  // ── 泳道分配 ──────────────────────────────────────────────────────────────
  //
  // `lane_heads[i]` = “第 i 道正在等哪个提交”。逐行（拓扑序）走：
  //
  // 1. **本行提交落在哪道**：找到等着它的那道（正常情况下恰好一道；
  //    同一提交被多道等待时取最左那道——分叉合并后多条线汇到同一点）。
  //    没有任何道等它（根提交之后的孤立分支起点）就取最小的空闲道。
  // 2. 该道**紧接着接上本提交的第一个父提交**（子 → 父是同一条时间线）。
  // 3. 其余父提交（合并的另一侧）放进**最小空闲道**，并在那里登记等待。
  // 4. 本行之前“等着本提交”的其它道**清空**（它们已经汇到这里了）。
  //
  // 这样每一道的 `lane_heads` 天然给出“下一步连到哪”，边就是
  // “本行本道 → 那一道上等待的那个提交所在的行”。
  std::vector<std::optional<std::string>> lane_heads;

  /// 每条边先以“等待中的道”形式挂起：`pending[to_hash] += {from_row, from_lane}`。
  /// 当 `to_hash` 那一行真正出现时才落成 GraphEdge（此时知道它的 row 与 lane）。
  struct PendingEdge {
    std::size_t from_row{0};
    std::size_t from_lane{0};
  };
  std::vector<std::pair<std::string, PendingEdge>> pending;

  const auto smallest_free_lane = [&]() -> std::size_t {
    for (std::size_t lane = 0; lane < lane_heads.size(); ++lane) {
      if (!lane_heads[lane].has_value()) return lane;
    }
    lane_heads.emplace_back(std::nullopt);
    return lane_heads.size() - 1U;
  };

  for (std::size_t row = 0; row < revs.size(); ++row) {
    const RevLine& rev = revs[row];

    // ① 本行落在哪道：优先“正等着它的那道”（取最左）。
    std::size_t lane = 0;
    bool found = false;
    for (std::size_t index = 0; index < lane_heads.size(); ++index) {
      if (lane_heads[index].has_value() && *lane_heads[index] == rev.hash) {
        lane = index;
        found = true;
        break;
      }
    }
    if (!found) {
      lane = smallest_free_lane();
      lane_heads[lane] = rev.hash;
    }

    graph.nodes.push_back(GraphNode{.hash = rev.hash, .row = row, .lane = lane,
                                    .parents = rev.parents.size()});

    // ② 之前挂着“等着本提交”的边，现在可以落成（目标是本行本道）。
    for (auto it = pending.begin(); it != pending.end();) {
      if (it->first != rev.hash) {
        ++it;
        continue;
      }
      graph.edges.push_back(GraphEdge{.from_row = it->second.from_row,
                                      .from_lane = it->second.from_lane,
                                      .to_row = row,
                                      .to_lane = lane});
      it = pending.erase(it);
    }

    // ③ 同道的其它等待者清空（它们汇到本行了）。
    for (std::size_t index = 0; index < lane_heads.size(); ++index) {
      if (index == lane) continue;
      if (lane_heads[index].has_value() && *lane_heads[index] == rev.hash) {
        lane_heads[index] = std::nullopt;
      }
    }

    // ④ 第一个父提交：**继承本道**（同一条时间线）。
    if (!rev.parents.empty()) {
      lane_heads[lane] = rev.parents[0];
      pending.emplace_back(rev.parents[0], PendingEdge{.from_row = row, .from_lane = lane});
    } else {
      lane_heads[lane] = std::nullopt;   // 根提交：道到此为止
    }

    // ⑤ 其余父提交（合并的另一侧）：放进最小空闲道。
    for (std::size_t index = 1; index < rev.parents.size(); ++index) {
      const std::size_t side = smallest_free_lane();
      lane_heads[side] = rev.parents[index];
      pending.emplace_back(rev.parents[index],
                           PendingEdge{.from_row = row, .from_lane = lane});
    }
  }

  // ⑤ 悬空的边（父提交不在本次 limit 内）：**丢掉**。
  //
  // 为什么不画“连向屏幕外的线”：用户明确指出过“任何线都要在节点之间，
  // 不要有单独的线头”。父提交没被取到时，那条线的另一端根本不存在。
  // （裁到可视区是绘制端的事，那里的节点是**真的存在**、只是滚出屏幕。）

  // 收尾：清理末尾的空道，算出实际用到几道。
  std::size_t used = 0;
  for (const GraphNode& node : graph.nodes) used = std::max(used, node.lane + 1U);
  for (const GraphEdge& edge : graph.edges) {
    used = std::max({used, edge.from_lane + 1U, edge.to_lane + 1U});
  }
  graph.lane_count = used;
  return graph;
}

}  // namespace gbcode
