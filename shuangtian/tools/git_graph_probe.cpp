// ⚠ 已失效（2026-11-22 盘点）
//
// 失效原因：`apps/gbcode/git_graph.hpp` 已移除
//
// 这个探针**当前编译不过**——它依赖的 API 已经删改。保留它是因为：
// 它记录过的实测结论可能仍被文档引用，删掉文件会让那些数字永久无法复核。
//
// 请把它当**历史档案**读，不要当活工具用。
// 真要复活：先按上面的原因把 API 改回当前形态，再验它测的东西是否还成立
//（很多测量前提已经变了，结论不保证可移植）。
//
// 盘点清单与分类判据见 tools/README.md「探针的分类与为何不全删」。

/// 提交图算法探针：把「自算的拓扑图」和「git --graph 的字符画」并排打出来对账。
///
/// 用法: git_graph_probe [repo] [limit]
///
/// 它是**开发期的对账工具**（不是测试）：同时打印两条证据，
/// 让人一眼看出自算的泳道/边是否与 git 自己画的图表达同一拓扑。
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "apps/gbcode/git_graph.hpp"
#include "st/core/process.hpp"

namespace {

auto git_text(const std::string& cwd, const std::vector<std::string>& args) -> std::string {
  st::process::Options options;
  options.cwd = cwd;
  std::vector<std::string> full_args{"-c", "core.quotepath=false"};
  full_args.insert(full_args.end(), args.begin(), args.end());
  const auto result = st::process::run("git", full_args, options);
  if (!result || result->exit_code != 0) return {};
  return result->stdout_text;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string repo = argc > 1 ? argv[1] : ".";
  const std::size_t limit = argc > 2 ? static_cast<std::size_t>(std::atoll(argv[2])) : 20;

  // ① git 自己的字符画（人眼参照）
  std::printf("=== git --graph（字符画，仅作参照）===\n");
  std::printf("%s\n", git_text(repo, {"log", "--graph", "--oneline", std::format("-{}", limit),
                                      "--all"})
                           .c_str());

  // ② 自算拓扑
  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, limit);
  std::printf("=== 自算拓扑：%zu 个节点 / %zu 条边 / %zu 道 ===\n", graph.nodes.size(),
              graph.edges.size(), graph.lane_count);

  std::printf("\n-- 节点（row / lane / 父数 / hash）--\n");
  for (const auto& node : graph.nodes) {
    std::printf("  row %2zu  lane %zu  parents %zu  %s%s\n", node.row, node.lane, node.parents,
                node.hash.substr(0, 8).c_str(), node.parents > 1 ? "  <== MERGE" : "");
  }

  std::printf("\n-- 边（from(row,lane) -> to(row,lane)）--\n");
  for (const auto& edge : graph.edges) {
    const bool cross = edge.from_lane != edge.to_lane;
    std::printf("  (%2zu,%zu) -> (%2zu,%zu)%s\n", edge.from_row, edge.from_lane, edge.to_row,
                edge.to_lane, cross ? "   跨道" : "");
  }

  // ③ ASCII 复刻：用自算的节点/边画出“应该是”的样子，便于与 ① 对照
  std::printf("\n-- 自算结果的 ASCII 复刻（每道一列，`*`=节点 `|`=同道边 `\\`/`/`=跨道）--\n");
  const std::size_t lanes = graph.lane_count == 0 ? 1 : graph.lane_count;
  for (const auto& node : graph.nodes) {
    std::string line(lanes * 2, ' ');
    // 同道竖线：本行有“从更上一行连到本行”的同道边时画在节点上方（这里只标列）
    for (const auto& edge : graph.edges) {
      if (edge.from_row == node.row && edge.from_lane == edge.to_lane &&
          edge.to_row > node.row) {
        // 向下的同道边：标在节点下方（下一步再画，这里跳过）
        (void)edge;
      }
    }
    line[node.lane * 2] = '*';
    // 本行出发的跨道边
    for (const auto& edge : graph.edges) {
      if (edge.from_row != node.row || edge.from_lane == edge.to_lane) continue;
      std::string tag = "  (跨道到 lane " + std::to_string(edge.to_lane) + ")";
      line += tag;
      break;
    }
    std::printf("  %s\n", line.c_str());
  }
  return 0;
}
