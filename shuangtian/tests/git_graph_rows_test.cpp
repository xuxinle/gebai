/// 提交图：**行数 = 提交数**（拓扑图不含“纯连线行”）。
///
/// ## 这条守的是什么缺陷
///
/// 用户报“**合并中间的空行还在**”：列表里 Merge 提交与它的父提交之间空了一行。
///
/// 根因是 `git log --graph` 的排版副产品——它为在**字符网格**里塞下斜边，
/// 会插一整行只含图字符的行（`|\`）。旧实现在列表里为这些行**留了一格**，
/// 于是画面上就是一行空白（那时必须留：斜线要画在那格里）。
///
/// 改成节点-边拓扑后，跨道边是“两个节点之间的曲线”，**本来就跨行**，
/// 不需要中间行占位。所以拓扑图必须满足：**每个节点一笔提交，没有“空节点”**。
///
/// 判据（本文件）：
/// 1. `graph.nodes.size()` == `git rev-list` 的输出行数（一个不多一个不少）；
/// 2. 每行都在 `git log --pretty` 里对得上（没有凭空多出来的空行）；
/// 3. 边只连接**真实存在的节点**（不引用越界行——越界会被绘制端裁掉，
///    表现为“线断了一截”）。
///
/// ⚠ 不断言“行高/像素”——那是应用层的事；这条只钉数据契约。

#include "st/test/test.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <vector>

#include "apps/gbcode/git_graph.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"

namespace {

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

[[nodiscard]] auto split_lines(const std::string& text) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::size_t end = eol == std::string::npos ? text.size() : eol;
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) out.push_back(std::move(line));
    if (eol == std::string::npos) break;
    begin = eol + 1;
  }
  return out;
}

}  // namespace

/// 拓扑图的行数必须等于提交数——**不多一行**。
///
/// “多一行”就是那个空行：`--graph` 的连接行被当成了行。
ST_TEST(git_graph_rows_equal_commit_count_without_blank_rows) {
  const std::string repo = ".";
  // 独立的第二来源：`rev-list --count` 与逐行列表。
  const std::string hashes = git_text(repo, {"rev-list", "--topo-order", "-40", "--all"});
  if (hashes.empty()) {
    st::log::out("[git_graph] 不是 git 仓库或没有提交，跳过\n");
    return;
  }
  const std::vector<std::string> commits = split_lines(hashes);

  // 同时看清 `--graph` **多了几行**（这正是旧实现里那些空行的来源）。
  const std::string art = git_text(repo, {"log", "--graph", "--pretty=format:%H", "-40", "--all"});
  const std::size_t art_lines = split_lines(art).size();

  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, 40);
  st::log::out("[git_graph] 提交 {} 个；--graph 输出 {} 行（多出 {} 行纯连线行）；拓扑图 {} 行\n",
               commits.size(), art_lines, art_lines > commits.size() ? art_lines - commits.size() : 0,
               graph.nodes.size());
  ST_CHECK_EQ(graph.nodes.size(), commits.size());
  // `--graph` 一定比提交多行（至少合并处会插行）——若不多，这条用例就测不到本缺陷。
  ST_CHECK(art_lines >= commits.size());
}

/// 每个节点都必须对应一笔**真实提交**（hash 完全一致，顺序也一致）。
ST_TEST(git_graph_nodes_match_rev_list_in_order) {
  const std::string repo = ".";
  const std::string hashes = git_text(repo, {"rev-list", "--topo-order", "-25", "--all"});
  if (hashes.empty()) return;
  const std::vector<std::string> commits = split_lines(hashes);

  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, 25);
  ST_REQUIRE(graph.nodes.size() == commits.size());
  for (std::size_t index = 0; index < commits.size(); ++index) {
    if (graph.nodes[index].hash != commits[index]) {
      st::log::out("[git_graph] 第 {} 行 hash 不符：拓扑图 {} vs rev-list {}\n", index,
                   graph.nodes[index].hash.substr(0, 8), commits[index].substr(0, 8));
    }
    ST_CHECK_EQ(graph.nodes[index].hash, commits[index]);
  }
}

/// 边只引用**存在**的节点（越界边在绘制端会被裁掉，画面上表现为“线断一截”）。
ST_TEST(git_graph_edges_reference_existing_nodes_only) {
  const std::string repo = ".";
  const gbcode::CommitGraph graph = gbcode::read_commit_graph(repo, 40);
  if (graph.nodes.empty()) return;

  std::set<std::size_t> rows;
  for (const auto& node : graph.nodes) rows.insert(node.row);

  int dangling = 0;
  for (const auto& edge : graph.edges) {
    if (rows.count(edge.from_row) == 0 || rows.count(edge.to_row) == 0) ++dangling;
  }
  st::log::out("[git_graph] {} 条边，其中悬空 {} 条\n", graph.edges.size(), dangling);
  ST_CHECK_EQ(dangling, 0);
  // 边必须**自上而下**（子 → 父）：反了就是拓扑算错。
  for (const auto& edge : graph.edges) {
    ST_CHECK(edge.from_row < edge.to_row);
  }
}

/// **泳道分配的算法量级**：宽图（很多并行分支）上的耗时不能随**道数**爆。
///
/// ## 这条守的是什么
///
/// 旧 `read_commit_graph` 是三处线性扇描：找本行的道（扇 `lane_heads`）、
/// 落定挂起的边（扇整个 `pending`）、清空其它等本提交的道（再扇一遍 `lane_heads`）。
/// 仓库有几十条并行分支（monorepo 的常态）时 N·L 会变得很明显；而 `pending`
/// 里还堆着**跨 `limit` 边界永不匹配**的条目，每行都被重扇。
///
/// 现在三处都走哈希表/堆（`build_commit_graph`），**与道数无关**。这条用
/// **合成拓扑**直接量：4041 笔提交、40 条并行支线（每条 100 笔，逐个合入主干），
/// 要求在 **100 ms 内**完成。
///
/// ## 为何用合成数据而不是真仓库
///
/// 真仓库的道数不可控（造 40 条并行线要几百次真实 git 操作），而这条要钉的是
/// **算法量级**——`build_commit_graph` 本来就是纯函数、不碰子进程，合成输入
/// 就是它的全部输入。（与 git 命令相关的正确性由本目录另几条真仓库用例守。）
///
/// ## 拓扑必须真的合序（这条用例自己踩过的坑）
///
/// 输入要求是**拓扑序**（子在前、父在后）。第一版把根提交放在最前面、支线末端
/// 指回一个**已经出现过**的提交，于是那些道永远等不到匹配 ⇒ 实测分配出 79 道。
/// 那不是算法错，是**输入非法**。现在按真实历史生成：第 i 次merge 的分叉点是
/// `T_i`（它的行号**大于**支线的所有行，因为在拓扑序里祖先在后面）。
///
/// 上限 100 ms 的定法：实测 ~2 ms（4000 节点），两个数量级的余量——
/// 既能吸收机器差异，又拦得住“退回 N·L”这类量级回退
///（旧实现在同规模合成图上会落进几十毫秒档）。
ST_TEST(git_graph_wide_topology_stays_linear) {
  constexpr std::size_t kBranchCount = 40;
  constexpr std::size_t kBranchLength = 100;
  std::vector<gbcode::CommitRev> revs;
  revs.reserve(kBranchCount * kBranchLength + kBranchCount + 1);
  const auto hash_of = [](std::size_t index) { return std::format("{:040x}", index + 1); };

  // 形状：一个 `base`（根），40 条支线各 100 笔、都从 `base` 分叉；
  // 另有 40 个合并提交 `M_i`（i = 0..39）串成一条主干：
  //   `M_0.parents = {side_0[0], base}`、`M_i.parents = {side_i[0], M_{i-1}}`。
  //
  // 这样**同时活跃**的支线有 40 条（而不是一条走完再下一条）——这才量得到
  // “耗时随道数增长”这件事。上一版写成“逐条枝线依次合入”，同时只有 1 道，
  // 那是对算法的**无效量尺**（它只测到 4000 节点本身的开销）。
  const std::string base = hash_of(0);
  std::size_t next = 1;
  std::vector<std::string> merge_commit(kBranchCount);
  for (auto& hash : merge_commit) hash = hash_of(next++);
  std::vector<std::vector<std::string>> sides(kBranchCount);
  for (auto& side : sides) {
    side.reserve(kBranchLength);
    for (std::size_t step = 0; step < kBranchLength; ++step) side.push_back(hash_of(next++));
  }

  // 拓扑序（子 → 父）：主干从最新往回推，支线紧随其后（支线只以 `base` 为父，
  // 与主干之间只有“合并提交指向支线头”这一条关系，所以各支线之间可任意顺序）。
  for (std::size_t round = kBranchCount; round > 0; --round) {
    const std::size_t index = round - 1;
    const std::string& first_parent = index == 0 ? base : merge_commit[index - 1];
    revs.push_back(gbcode::CommitRev{.hash = merge_commit[index],
                                     .parents = {sides[index].front(), first_parent}});
  }
  for (const auto& side : sides) {
    for (std::size_t step = 0; step < side.size(); ++step) {
      gbcode::CommitRev rev{.hash = side[step]};
      if (step + 1 < side.size()) rev.parents.push_back(side[step + 1]);
      else rev.parents.push_back(base);   // 支线末端接回分叉点（祖先，在更后面）
      revs.push_back(std::move(rev));
    }
  }
  revs.push_back(gbcode::CommitRev{.hash = base});   // 根提交：无父

  const std::int64_t begin = st::time::now_ns();
  const gbcode::CommitGraph graph = gbcode::build_commit_graph(revs);
  const double elapsed_ms = static_cast<double>(st::time::now_ns() - begin) / 1.0e6;
  st::log::out("[git_graph] 合成宽图：{} 节点 / {} 边 / {} 道，耗时 {:.2f} ms\n",
               graph.nodes.size(), graph.edges.size(), graph.lane_count, elapsed_ms);

  ST_CHECK_EQ(graph.nodes.size(), revs.size());
  // 真有几十条并行道（否则这条量尺测不到“随道数增长”那件事）。
  ST_REQUIRE(graph.lane_count >= kBranchCount / 2);
  ST_CHECK(graph.lane_count <= kBranchCount + 2);
  // 边必须自上而下、且不引用窗口外的行（悬空边被丢掉是设计行为）。
  for (const auto& edge : graph.edges) {
    ST_CHECK(edge.from_row < edge.to_row);
    ST_CHECK(edge.to_row < graph.nodes.size());
  }
  ST_CHECK(elapsed_ms < 100.0);
}
