/// git 提交图：**从真实拓扑算出来**，不是解析 `git log --graph` 的字符画。
///
/// ## 为什么不能解析字符画
/// `git log --graph` 的 ASCII 图区是**渲染产物，不是数据**：它的列宽、
/// 斜线位置、连接行数量都是 git 内部的排版决定，没有稳定契约。
/// 拿它当数据模型**结构性不可靠**：
///
/// * **斜线位置不可依赖**。实测：`|\` 的斜线落在**奇数列**（`|` 在 col0、`\` 在 col1），
///   而“按道扫描”只读偶数格（`* ` / `| ` 两字符一道），于是跨道边被整条丢掉。
/// * **拓扑信息被编码成了像素**。要回答“这笔提交有几个父提交、连到哪一道”，
///   得从字符位置反推；而 git 本来就直接知道答案。
/// * **会多出整行**。为一个合并要在字符网格里塞下斜边，git 会插一整行连接行，
///   列表为它留位就是一个**空行**（出现在两条相邻提交之间）。
///
/// ## 现在怎么做
///
/// 用 `git rev-list --parents` 拿**每个提交的父提交列表**（真实拓扑），
/// 自己按拓扑序遍历并分配泳道：
///
/// * 每一行是一个提交（**没有“纯连线行”这个概念了**——连接行本来就是
///   `--graph` 为了在字符网格里塞下斜边而产生的排版副产品）；
/// * 一笔提交在**第 0 道被匹配**时占用该道；它的第一个父提交继承同一道；
///   后续父提交（合并的另一侧）放进**空道**——这就是“分叉”；
/// * 一条道上的连续提交之间是**同道边**（竖线）；父提交若落在不同道，
///   就是**跨道边**（曲线）。
///
/// 这样“边”是**显式的 (fromLane, toLane)**，绘制端不需要再理解字符。
///
/// ## 与日志**共用一次子进程**（2026-11-22，性能）
///
/// 历史实现是两条命令：`git log`（取展示字段）与 `git rev-list --parents`
/// （取拓扑）——同一次刷新里对同一段历史跑两遍，实测各 77~99 ms。
/// 现在 `build_commit_graph` 只吃**已解析的 (hash, parents) 序列**，
/// 而那份序列由 `read_log_and_graph` 从**一条** `git log --pretty=…%P` 里同时拿到
/// （`%P` 就是父提交列表，与 `rev-list --parents` 的第二列同源）。
///
/// 为什么可以合并（不是“看起来一样就用”）：`git log --topo-order` 与
/// `git rev-list --parents --topo-order` 在**同一范围**上的提交顺序是同一套
/// 拓扑遍历的输出——实测本仓 200 条逐条比对，hash 序与父列表**完全一致**
/// （`tests/git_graph_test.cpp` 的 `git_graph_merged_log_matches_rev_list` 钉住这条）。
/// 泳道分配的前提是“子一定在父之前出现”，两者都满足。

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace gbcode {

struct GraphNode {
  std::string hash{};
  std::size_t row{0};     ///< 视觉行号（0 = 最上面）
  std::size_t lane{0};    ///< 泳道（0 起）
  std::size_t parents{0}; ///< 父提交个数（0 = 根，>1 = 合并）
};

/// 一条边：`fromLane` 道的 `fromRow` 连到 `toLane` 道的 `toRow`。
///
/// `fromRow < toRow`（拓扑序：子在上、父在下）。
/// 同道边（`fromLane == toLane`）画竖线；跨道边画曲线。
struct GraphEdge {
  std::size_t from_row{0};
  std::size_t from_lane{0};
  std::size_t to_row{0};
  std::size_t to_lane{0};
};

struct CommitGraph {
  std::vector<GraphNode> nodes{};   ///< 按 row 升序（与展示顺序一致）
  std::vector<GraphEdge> edges{};
  /// 用到的最大泳道号 + 1（绘制端据此算泳道带宽）。
  std::size_t lane_count{0};
  /// 每个 row 的父提交数 > 1（合并点）——绘制端给合并点画“大一点”的点。
  [[nodiscard]] auto is_merge(std::size_t row) const noexcept -> bool {
    return row < nodes.size() && nodes[row].parents > 1;
  }
};

/// 一笔提交的**拓扑信息**（泳道分配的唯一输入）。
///
/// 放在头里是因为它同时是 `git log --pretty=…%P` 与 `rev-list --parents`
/// 两条来源的**公共中间形态**：谁取到的都长这样，`build_commit_graph` 不关心来源。
struct CommitRev {
  std::string hash{};
  std::vector<std::string> parents{};   ///< 第一个父 = 主时间线（继承同道）
};

/// 从**已解析**的 (hash, parents) 序列算泳道与边（纯函数，不碰子进程）。
///
/// 前提：`revs` 按**拓扑序**排（子一定在父之前）。这条不满足时泳道会错，
/// 但不会崩——所以来源必须是 `--topo-order` 的遍历结果。
[[nodiscard]] auto build_commit_graph(std::span<const CommitRev> revs) -> CommitGraph;

/// 读提交图（`limit` 个提交；从 `ref`/全部引用起）。
///
/// 只依赖 `rev-list --parents --topo-order`——**不读 `--graph` 的字符画**。
/// 拓扑序保证“子一定在父之前出现”，这正是逐行分配泳道所需的前提。
///
/// ⚠ 需要日志与拓扑**同时**要时不要分别调它与 `read_log`：那会把同一段历史
/// 读两遍。用 `read_log_and_graph`（一条命令同时给出两者），
/// 或自己解析后调 `build_commit_graph`。
[[nodiscard]] auto read_commit_graph(const std::string& cwd, std::size_t limit,
                                     const std::string& ref = {}) -> CommitGraph;

}  // namespace gbcode
