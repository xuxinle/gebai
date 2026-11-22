// 一次性 A/B 量尺：同一合成拓扑分别喂给「旧实现」与「新实现」，量泳道分配耗时。
//
// 旧实现是 `git_graph.cpp` 在 2026-11-22 重构前的版本（从 `git show HEAD:` 取出，
// 只把 `build_commit_graph` 的前半段（解析 rev-list 文本）换成一个接收
// `vector<CommitRev>` 的入口——算法本体一行不改，否则比的不是同一个东西）。
//
// 用法: git_graph_ab_probe [branches] [length]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "apps/gbcode/git_graph.hpp"
#include "st/core/time.hpp"

namespace {

const auto hash_of = [](std::size_t index) { return std::format("{:040x}", index + 1); };

/// 造宽图：一个 base + N 条支线（各 L 笔）+ N 个合并提交串成主干。
[[nodiscard]] auto make_wide_graph(std::size_t branch_count, std::size_t branch_length)
    -> std::vector<gbcode::CommitRev> {
  std::vector<gbcode::CommitRev> revs;
  revs.reserve(branch_count * branch_length + branch_count + 1);
  const std::string base = hash_of(0);
  std::size_t next = 1;
  std::vector<std::string> merges(branch_count);
  for (auto& hash : merges) hash = hash_of(next++);
  std::vector<std::vector<std::string>> sides(branch_count);
  for (auto& side : sides) {
    side.reserve(branch_length);
    for (std::size_t step = 0; step < branch_length; ++step) side.push_back(hash_of(next++));
  }
  for (std::size_t round = branch_count; round > 0; --round) {
    const std::size_t index = round - 1;
    const std::string& first_parent = index == 0 ? base : merges[index - 1];
    revs.push_back(gbcode::CommitRev{.hash = merges[index],
                                     .parents = {sides[index].front(), first_parent}});
  }
  for (const auto& side : sides) {
    for (std::size_t step = 0; step < side.size(); ++step) {
      gbcode::CommitRev rev{.hash = side[step]};
      if (step + 1 < side.size()) rev.parents.push_back(side[step + 1]);
      else rev.parents.push_back(base);
      revs.push_back(std::move(rev));
    }
  }
  revs.push_back(gbcode::CommitRev{.hash = base});
  return revs;
}

// ————————————————————————————————————————————————————————————————————————————
// 旧实现（2026-11-22 之前的 `read_commit_graph`，算法本体逐行保留）
// ————————————————————————————————————————————————————————————————————————————
[[nodiscard]] auto legacy_build(const std::vector<gbcode::CommitRev>& revs) -> gbcode::CommitGraph {
  gbcode::CommitGraph graph;
  if (revs.empty()) return graph;

  std::vector<std::optional<std::string>> lane_heads;
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
    const auto& rev = revs[row];
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

    graph.nodes.push_back(gbcode::GraphNode{.hash = rev.hash, .row = row, .lane = lane,
                                            .parents = rev.parents.size()});

    for (auto it = pending.begin(); it != pending.end();) {
      if (it->first != rev.hash) {
        ++it;
        continue;
      }
      graph.edges.push_back(gbcode::GraphEdge{.from_row = it->second.from_row,
                                              .from_lane = it->second.from_lane,
                                              .to_row = row,
                                              .to_lane = lane});
      it = pending.erase(it);
    }

    for (std::size_t index = 0; index < lane_heads.size(); ++index) {
      if (index == lane) continue;
      if (lane_heads[index].has_value() && *lane_heads[index] == rev.hash) {
        lane_heads[index] = std::nullopt;
      }
    }

    if (!rev.parents.empty()) {
      lane_heads[lane] = rev.parents[0];
      pending.emplace_back(rev.parents[0], PendingEdge{.from_row = row, .from_lane = lane});
    } else {
      lane_heads[lane] = std::nullopt;
    }

    for (std::size_t index = 1; index < rev.parents.size(); ++index) {
      const std::size_t side = smallest_free_lane();
      lane_heads[side] = rev.parents[index];
      pending.emplace_back(rev.parents[index], PendingEdge{.from_row = row, .from_lane = lane});
    }
  }

  std::size_t used = 0;
  for (const auto& node : graph.nodes) used = std::max(used, node.lane + 1U);
  for (const auto& edge : graph.edges) {
    used = std::max({used, edge.from_lane + 1U, edge.to_lane + 1U});
  }
  graph.lane_count = used;
  return graph;
}

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

}  // namespace

int main(int argc, char** argv) {
  const std::size_t branches =
      argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 40;
  const std::size_t length = argc > 2 ? static_cast<std::size_t>(std::atoll(argv[2])) : 100;
  const auto revs = make_wide_graph(branches, length);
  std::printf("合成拓扑：%zu 提交（%zu 支线 × %zu）\n", revs.size(), branches, length);

  const auto legacy = legacy_build(revs);
  const auto modern = gbcode::build_commit_graph(revs);
  // 两条必须给出**完全一样**的结果（算法改了，语义不能改）。
  bool same = legacy.nodes.size() == modern.nodes.size() &&
              legacy.edges.size() == modern.edges.size() &&
              legacy.lane_count == modern.lane_count;
  for (std::size_t index = 0; same && index < legacy.nodes.size(); ++index) {
    same = legacy.nodes[index].lane == modern.nodes[index].lane &&
           legacy.nodes[index].parents == modern.nodes[index].parents;
  }
  for (std::size_t index = 0; same && index < legacy.edges.size(); ++index) {
    same = legacy.edges[index].from_row == modern.edges[index].from_row &&
           legacy.edges[index].from_lane == modern.edges[index].from_lane &&
           legacy.edges[index].to_row == modern.edges[index].to_row &&
           legacy.edges[index].to_lane == modern.edges[index].to_lane;
  }
  std::printf("结果一致：%s（旧 %zu 节点/%zu 边/%zu 道；新 %zu/%zu/%zu）\n",
              same ? "是" : "**否**", legacy.nodes.size(), legacy.edges.size(),
              legacy.lane_count, modern.nodes.size(), modern.edges.size(),
              modern.lane_count);

  std::printf("\n%8.2f ms  旧 build_commit_graph（三处线性扇描）\n",
              best_ms(7, [&] { (void)legacy_build(revs); }));
  std::printf("%8.2f ms  新 build_commit_graph（哈希表 + 最小堆）\n",
              best_ms(7, [&] { (void)gbcode::build_commit_graph(revs); }));
  return same ? 0 : 1;
}
