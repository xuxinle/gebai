/// git 提交图：从**真实拓扑**算泳道与边（不解析 `--graph` 字符画）。
///
/// 结构与取舍见 `git_graph.hpp` 的头注释。

#include <cstddef>
#include <string>
#include <vector>

#include "git_graph.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <queue>
#include <span>
#include <unordered_map>
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

auto build_commit_graph(std::span<const CommitRev> revs) -> CommitGraph {
  CommitGraph graph;
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
  //
  // ## 复杂度（2026-11-22 性能）
  //
  // 旧写法是**三处线性扫描**：找本行的道（扫 lane_heads）、落定挂起的边
  // （扫整个 pending 数组）、清空其它等本提交的道（再扫一遍 lane_heads）、
  // 每个父提交又扫一次找空道。提交数 N、道数 L 时是 O(N·L + N·P)，
  // 且 `pending` 里堆着**跨 limit 边界也永不匹配**的条目（父提交没被取到），
  // 那些条目每一行都被重新扫一遍——1 万条日志时这是几十万次字符串比较。
  //
  // 现在三处都改成 **hash → 槽位** 的哈希表：
  //   · `lanes_waiting_hash`：hash → 正在等它的道（本行“落在哪道”一次查表）；
  //   · `pending_of`：hash → 该 hash 的挂起边列表（落定时 O(1) 定位）；
  //   · `free_lanes`：空闲道的最小堆（找空道 O(log L)，不再线性扫）。
  // 于是整体是 O(N + E + N·log L)，与**道数无关**；字符串只在建/拆哈希表时比较一次。

  graph.nodes.reserve(revs.size());
  graph.edges.reserve(revs.size() + revs.size() / 4);

  /// 每一道在等哪个提交（`nullopt` = 空闲）。只在分配时用，不跨行保留。
  std::vector<std::optional<std::string>> lane_heads;
  /// 与 `lane_heads` 平行的“空闲”标记：**防双重释放**。
  ///
  /// 没它的话，一条道被释放两次就会在 `free_lanes` 堆里出现两次，而堆是**不去重**的
  /// ——同一个道号会被分配两次，两个节点落到同一列（画面上就是两条线重叠、
  /// 而泳道数偏小）。这是无错但很难定位的一类错，所以在这里封死。
  std::vector<bool> lane_free;

  /// 一条边先以“等待中的道”形式挂起：等 `to_hash` 那一行出现时才落成
  /// GraphEdge（此时才知道它的 row 与 lane）。
  struct PendingEdge {
    std::size_t from_row{0};
    std::size_t from_lane{0};
  };
  /// `hash → 挂起边列表`：本行是某个 hash 时，一次查表就把等它的边全部落定。
  std::unordered_map<std::string, std::vector<PendingEdge>> pending_of;

  /// 空闲道（**最小堆**：`smallest_free_lane` 的语义是“最小的空闲道”，
  /// 而堆顶天然就是最小——用 `vector<bool>` + 线性扫会退化成 O(L)/次）。
  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> free_lanes;

  /// `hash → 正在等它的道`（一个 hash 可能被多道等待：分叉后汇到同一点）。
  std::unordered_map<std::string, std::vector<std::size_t>> lanes_waiting_hash;

  const auto acquire_lane = [&]() -> std::size_t {
    if (free_lanes.empty()) {
      lane_heads.emplace_back(std::nullopt);
      lane_free.push_back(false);
      return lane_heads.size() - 1U;
    }
    const std::size_t lane = free_lanes.top();
    free_lanes.pop();
    lane_free[lane] = false;
    return lane;
  };

  const auto wait_on = [&](std::size_t lane, const std::string& hash) {
    lane_heads[lane] = hash;
    lane_free[lane] = false;
    lanes_waiting_hash[hash].push_back(lane);
  };

  const auto release_lane = [&](std::size_t lane) {
    if (lane >= lane_free.size() || lane_free[lane]) return;   // 已经空闲：不重复入堆
    if (lane_heads[lane].has_value()) {
      // 从 `lanes_waiting_hash` 里摘掉本道（它不再等那个 hash 了）。
      if (const auto found = lanes_waiting_hash.find(*lane_heads[lane]);
          found != lanes_waiting_hash.end()) {
        auto& lanes = found->second;
        const auto at = std::find(lanes.begin(), lanes.end(), lane);
        if (at != lanes.end()) lanes.erase(at);
        if (lanes.empty()) lanes_waiting_hash.erase(found);
      }
    }
    lane_heads[lane] = std::nullopt;
    lane_free[lane] = true;
    free_lanes.push(lane);
  };

  for (std::size_t row = 0; row < revs.size(); ++row) {
    const CommitRev& rev = revs[row];

    // ① 本行落在哪道：优先“正等着它的那道”（取最左）。
    //
    // ⚠ 必须**先把等待表里那一项整个取出来**再处理其余道：`release_lane` /
    // 内联清空都会回头改 `lanes_waiting_hash[rev.hash]` 这一项（甚至把它删掉），
    // 边遍历 `hit->second` 边改它就是迭代器失效（实测会崩在 `vector::erase` 里）。
    std::size_t lane = 0;
    bool found = false;
    std::vector<std::size_t> waiters;
    if (const auto hit = lanes_waiting_hash.find(rev.hash);
        hit != lanes_waiting_hash.end()) {
      waiters = std::move(hit->second);
      lanes_waiting_hash.erase(hit);
    }
    if (!waiters.empty()) {
      lane = *std::min_element(waiters.begin(), waiters.end());
      // 同道汇入：其余等本提交的道**清空**（它们汇到本行了）——见 ③。
      for (const std::size_t other : waiters) {
        if (other == lane) continue;
        lane_heads[other] = std::nullopt;
        lane_free[other] = true;
        free_lanes.push(other);
      }
      lane_free[lane] = false;
      found = true;
    }
    if (!found) lane = acquire_lane();

    graph.nodes.push_back(GraphNode{.hash = rev.hash, .row = row, .lane = lane,
                                    .parents = rev.parents.size()});

    // ② 之前挂着“等着本提交”的边，现在可以落成（目标是本行本道）。
    if (const auto waiting = pending_of.find(rev.hash); waiting != pending_of.end()) {
      for (const PendingEdge& edge : waiting->second) {
        graph.edges.push_back(GraphEdge{.from_row = edge.from_row,
                                        .from_lane = edge.from_lane,
                                        .to_row = row,
                                        .to_lane = lane});
      }
      pending_of.erase(waiting);
    }

    // ③ 第一个父提交：**继承本道**（同一条时间线）。
    if (!rev.parents.empty()) {
      wait_on(lane, rev.parents[0]);
      pending_of[rev.parents[0]].push_back(PendingEdge{.from_row = row, .from_lane = lane});
    } else {
      release_lane(lane);   // 根提交：道到此为止
    }

    // ④ 其余父提交（合并的另一侧）：放进最小空闲道。
    for (std::size_t index = 1; index < rev.parents.size(); ++index) {
      const std::size_t side = acquire_lane();
      wait_on(side, rev.parents[index]);
      pending_of[rev.parents[index]].push_back(
          PendingEdge{.from_row = row, .from_lane = lane});
    }
  }

  // ⑤ 悬空的边（父提交不在本次 limit 内）：**丢掉**。
  //
  // 为什么不画“连向屏幕外的线”：用户明确指出过“任何线都要在节点之间，
  // 不要有单独的线头”。父提交没被取到时，那条线的另一端根本不存在。
  // （裁到可视区是绘制端的事，那里的节点是**真的存在**、只是滚出屏幕。）
  // ——`pending_of` 里剩下的条目正是这批，直接随作用域销毁。

  // 收尾：算出**实际用到**几道（只看节点与边）。
  //
  // 为何不用 `lane_heads.size()`：那是“曾经分配过的道数”——合并的另一侧被分到
  // 一道、而它的父提交不在本次窗口内时，那条道**一个节点一条边都没有**。
  // 计入它会让泳道带宽虚胖（绘图层按 `lane_count` 算列宽），而屏幕上根本看不到。
  std::size_t used = 0;
  for (const GraphNode& node : graph.nodes) used = std::max(used, node.lane + 1U);
  for (const GraphEdge& edge : graph.edges) {
    used = std::max({used, edge.from_lane + 1U, edge.to_lane + 1U});
  }
  graph.lane_count = used;
  return graph;
}

auto read_commit_graph(const std::string& cwd, std::size_t limit, const std::string& ref)
    -> CommitGraph {
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
  if (!out.has_value()) return {};

  // 逐行解析成 (hash, parents)：`<hash> <parent>…`（空格分隔）。
  std::vector<CommitRev> revs;
  revs.reserve(limit == 0 ? 256 : std::min<std::size_t>(limit, 4096));
  {
    const std::string& text = *out;
    std::size_t begin = 0;
    while (begin < text.size()) {
      const std::size_t eol = text.find('\n', begin);
      const std::size_t end = eol == std::string::npos ? text.size() : eol;
      std::string_view line(text.data() + begin, end - begin);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (!line.empty()) {
        CommitRev rev;
        // hash 是第一个空格之前；其余空格分隔的是父提交。
        std::string_view rest = line;
        const std::size_t space = rest.find(' ');
        if (space == std::string_view::npos) {
          rev.hash = std::string(rest);
        } else {
          rev.hash = std::string(rest.substr(0, space));
          rest.remove_prefix(space + 1);
          while (!rest.empty()) {
            const std::size_t next = rest.find(' ');
            const std::string_view token =
                next == std::string_view::npos ? rest : rest.substr(0, next);
            if (!token.empty()) rev.parents.emplace_back(token);
            if (next == std::string_view::npos) break;
            rest.remove_prefix(next + 1);
          }
        }
        if (!rev.hash.empty()) revs.push_back(std::move(rev));
      }
      if (eol == std::string::npos) break;
      begin = eol + 1;
    }
  }
  return build_commit_graph(revs);
}

}  // namespace gbcode
