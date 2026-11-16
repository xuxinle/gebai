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
