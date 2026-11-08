/// @file cefu_graph_test.cpp
/// @brief 册府知识图谱的**领域适配层**：词条 → 通用关系图（+ 配色与人话标签）。
///
/// ## 分工：这里只测"册府独特的那部分"
///
/// 布局、相机、投影、拾取、截断等**通用算法**在 `tests/ui_graph_layout_test.cpp`
/// （框架层，换领域也能复用）。本文件只守护用册府才说得清的语义：
///
/// | 断言 | 它防的具体缺陷 |
/// |---|---|
/// | 分类/人/作用域去重 | 三条同册 → 三个同名节点叠在一起 |
/// | `links` 认 id 也认标题 | 用户写标题就失效（"写了没反应"） |
/// | 自指不建边 | 一条边指向自己，画面上一根零长棍 |
/// | 取代关系建边 | 新旧做法看不出先后 |
/// | 共享硬事实连链不连团 | 全连接让边数 O(n²) 爆炸 |
/// | 平凡值不建边 | "负责人=张三"这种共享把整张图连成一团 |
/// | global 作用域不建节点 | 人人 global → 一个巨大枢纽遮住真结构 |
/// | 置信度配色可区分 | 一眼看出哪些知识还没被确认（图谱的语义价值） |
/// | 聚簇优先级 分类>人>作用域 | 多归属时被拉成三角张力，簇内反而挤 |

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "apps/cefu/src/cefu_graph.hpp"
// 直接包含实现（本仓既有做法：`tests/caiyun_layer_schema_test.cpp` 也是这么写的）。
//
// 为何不靠构建系统把 `.cpp` 编进测试二进制：测试目标只收 `src/` 与 `tests/`，
// `apps/` 下的实现**只经此途径**进得来——build.cpp 的 `included_impl_files`
// 专门从 `.d` 里抽被 include 的实现文件来算 stamp，否则“改了实现→测试跑旧二进制”
// 会是完全静默的假绿。
#include "apps/cefu/src/cefu_graph.cpp"
#include "st/test/test.hpp"

namespace {

using cefu::Entry;
using cefu::graph::BuildOptions;
using st::ui::graph::Edge;
using st::ui::graph::Graph;
using st::ui::graph::Node;

auto make_entry(std::string id, std::string title, std::string category,
                std::string confidence = "verified") -> Entry {
  Entry e;
  e.id = std::move(id);
  e.title = std::move(title);
  e.category = std::move(category);
  e.confidence = std::move(confidence);
  e.scope = R"({"kind":"global"})";
  return e;
}

auto count_edges(const Graph& g, std::string_view kind) -> std::size_t {
  std::size_t n = 0;
  for (const auto& e : g.edges) {
    if (e.kind == kind) ++n;
  }
  return n;
}

auto count_nodes(const Graph& g, std::string_view kind) -> std::size_t {
  std::size_t n = 0;
  for (const auto& node : g.nodes) {
    if (node.kind == kind) ++n;
  }
  return n;
}

}  // namespace

// ── 建图：实体与边 ────────────────────────────────────────────────────────────

ST_TEST(cefu_graph_empty_build) {
  const auto g = cefu::graph::build({});
  ST_CHECK_EQ(g.nodes.size(), std::size_t{0});
  ST_CHECK_EQ(g.edges.size(), std::size_t{0});
  ST_CHECK_EQ(g.count_kind(cefu::graph::kKindEntry), std::size_t{0});
  ST_CHECK_EQ(g.largest_cluster, std::size_t{0});
}

ST_TEST(cefu_graph_category_node_deduped) {
  std::vector<Entry> entries{
      make_entry("e1", "写入延迟高", "运维"),
      make_entry("e2", "写入放大", "运维"),
      make_entry("e3", "配额找谁", "运维"),
  };
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_nodes(g, cefu::graph::kKindEntry), std::size_t{3});
  // 三条同册 → 只应有一个分类节点（不建则三个同名节点叠在一起）
  ST_CHECK_EQ(count_nodes(g, cefu::graph::kKindCategory), std::size_t{1});
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeBelongsTo), std::size_t{3});
}

ST_TEST(cefu_graph_owner_node_deduped) {
  std::vector<Entry> entries{
      make_entry("e1", "甲", "运维"),
      make_entry("e2", "乙", "运维"),
  };
  entries[0].owner = "张三";
  entries[1].owner = "张三";
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_nodes(g, cefu::graph::kKindPerson), std::size_t{1});
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeOwnedBy), std::size_t{2});
}

ST_TEST(cefu_graph_links_by_id_and_title) {
  std::vector<Entry> entries{
      make_entry("e1", "写入延迟高", "运维"),
      make_entry("e2", "写入放大", "运维"),
      make_entry("e3", "磁盘满", "运维"),
  };
  entries[0].links.push_back("e2");      // 按 id
  entries[0].links.push_back("磁盘满");  // 按标题
  entries[0].links.push_back("不存在");  // 指向不存在：不建边、不崩
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeLinks), std::size_t{2});
}

ST_TEST(cefu_graph_self_link_ignored) {
  std::vector<Entry> entries{make_entry("e1", "自己链自己", "运维")};
  entries[0].links.push_back("e1");
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeLinks), std::size_t{0});
}

ST_TEST(cefu_graph_supersedes_edge) {
  std::vector<Entry> entries{
      make_entry("old", "旧做法", "运维"),
      make_entry("new", "新做法", "运维"),
  };
  entries[1].supersedes = "old";
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeSupersedes), std::size_t{1});
}

ST_TEST(cefu_graph_shared_fact_chain_not_clique) {
  std::vector<Entry> entries{
      make_entry("e1", "阈值一", "运维"),
      make_entry("e2", "阈值二", "运维"),
      make_entry("e3", "阈值三", "运维"),
  };
  entries[0].facts.push_back("阈值=5x");
  entries[1].facts.push_back("阈值=5x");
  entries[2].facts.push_back("阈值=5x");
  const auto g = cefu::graph::build(entries);
  // 连成链（2 条边）而不是全连接（3 条边）：全连接会让大图边数爆炸
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeSharedFact), std::size_t{2});
}

ST_TEST(cefu_graph_shared_fact_skips_vague_values) {
  std::vector<Entry> plain{
      make_entry("p1", "甲", "运维"),
      make_entry("p2", "乙", "运维"),
  };
  plain[0].facts.push_back("负责人=张三");
  plain[1].facts.push_back("负责人=张三");
  const auto g = cefu::graph::build(plain);
  // 纯文字值区分度不足：否则"负责人=张三"这种平凡共享会把整张图连成一团
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeSharedFact), std::size_t{0});
}

ST_TEST(cefu_graph_shared_fact_needs_two_owners) {
  std::vector<Entry> entries{make_entry("e1", "只有我", "运维")};
  entries[0].facts.push_back("端口=8428");
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeSharedFact), std::size_t{0});
}

ST_TEST(cefu_graph_global_scope_not_a_node) {
  std::vector<Entry> entries{
      make_entry("e1", "公共知识", "运维"),
      make_entry("e2", "本群规矩", "运维"),
  };
  entries[1].scope = R"({"kind":"chat","id":"oc_abc"})";
  const auto g = cefu::graph::build(entries);
  // global 是缺省：建成节点等于给图加一个巨大枢纽，把真正的结构完全遮住
  ST_CHECK_EQ(count_nodes(g, cefu::graph::kKindScope), std::size_t{1});
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeScopedTo), std::size_t{1});
}

ST_TEST(cefu_graph_meta_nodes_can_be_disabled) {
  std::vector<Entry> entries{
      make_entry("e1", "甲", "运维"),
      make_entry("e2", "乙", "运维"),
  };
  entries[0].owner = "张三";
  BuildOptions options;
  options.include_meta_nodes = false;
  const auto g = cefu::graph::build(entries, options);
  // "最干净"档：只有词条节点，没有分类/人/作用域
  ST_CHECK_EQ(g.nodes.size(), std::size_t{2});
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeBelongsTo), std::size_t{0});
  ST_CHECK_EQ(count_edges(g, cefu::graph::kEdgeOwnedBy), std::size_t{0});
}

ST_TEST(cefu_graph_isolated_entry_counted) {
  std::vector<Entry> entries{
      make_entry("e1", "甲", "运维"),
      make_entry("e2", "乙", ""),  // 无分类无负责人 → 孤立
  };
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(g.isolated_count, std::size_t{1});
}

ST_TEST(cefu_graph_revision_tracks_content) {
  std::vector<Entry> a{make_entry("e1", "甲", "运维")};
  const auto g1 = cefu::graph::build(a);
  const auto g2 = cefu::graph::build(a);
  ST_CHECK_EQ(g1.revision, g2.revision);  // 同内容 → 同指纹（布局缓存能命中）

  a[0].title = "改过的标题";
  const auto g3 = cefu::graph::build(a);
  ST_CHECK(g1.revision != g3.revision);  // 内容变了 → 必须重算
}

// ── 聚簇语义（领域侧） ────────────────────────────────────────────────────────

/// 归属边必须被标成 `clustering`：这是"同册聚成一团"的信息来源。
///
/// 另一侧（通用层真的会因此成团）由 `tests/ui_graph_layout_test.cpp` 的
/// `graph_layout_clusters_group_together` 守——两边分开测，是因为它们的失败方式不同：
/// 这里漏标 → 布局**完全没有簇信息**；通用层失效 → 簇信息在但没用上。
ST_TEST(cefu_graph_membership_edges_are_clustering) {
  std::vector<Entry> entries{make_entry("e1", "甲", "运维")};
  entries[0].owner = "张三";
  entries[0].scope = R"({"kind":"chat","id":"oc_abc"})";
  const auto g = cefu::graph::build(entries);
  std::size_t clustering = 0;
  for (const auto& edge : g.edges) {
    if (!edge.clustering) continue;
    ++clustering;
    // 归属边一律是「词条 → 元节点」，且元节点是簇心（rank 更高）
    ST_CHECK(g.nodes[edge.to].cluster_rank > g.nodes[edge.from].cluster_rank);
  }
  ST_CHECK_EQ(clustering, std::size_t{3});  // 分类 + 负责人 + 作用域
  // 词条之间的关系边**不**参与聚簇（否则会决定它归哪一册）
  for (const auto& edge : g.edges) {
    if (edge.kind == cefu::graph::kEdgeBelongsTo) continue;
    if (edge.kind == cefu::graph::kEdgeOwnedBy) continue;
    if (edge.kind == cefu::graph::kEdgeScopedTo) continue;
    ST_CHECK(!edge.clustering);
  }
}

ST_TEST(cefu_graph_cluster_rank_priority) {
  // 分类 > 人 > 作用域（读的人按分类找东西，所以它排最先）
  ST_CHECK(cefu::graph::node_color(cefu::graph::kKindCategory, "") != 0U);
  std::vector<Entry> entries{make_entry("e1", "甲", "运维")};
  entries[0].owner = "张三";
  entries[0].scope = R"({"kind":"chat","id":"oc_abc"})";
  const auto g = cefu::graph::build(entries);

  double category_rank = 0.0;
  double person_rank = 0.0;
  double scope_rank = 0.0;
  for (const auto& node : g.nodes) {
    if (node.kind == cefu::graph::kKindCategory) category_rank = node.cluster_rank;
    if (node.kind == cefu::graph::kKindPerson) person_rank = node.cluster_rank;
    if (node.kind == cefu::graph::kKindScope) scope_rank = node.cluster_rank;
  }
  ST_CHECK(category_rank > person_rank);
  ST_CHECK(person_rank > scope_rank);
  ST_CHECK(scope_rank > 0.0);
}

// ── 配色与标签（领域侧） ──────────────────────────────────────────────────────

ST_TEST(cefu_graph_confidence_colors_distinguishable) {
  // 图谱的语义价值：一眼看出哪些知识还没被确认 → 三种置信度必须可区分
  const auto verified = cefu::graph::node_color(cefu::graph::kKindEntry, "verified");
  const auto inferred = cefu::graph::node_color(cefu::graph::kKindEntry, "inferred");
  const auto stale = cefu::graph::node_color(cefu::graph::kKindEntry, "stale");
  ST_CHECK(verified != inferred);
  ST_CHECK(verified != stale);
  ST_CHECK(inferred != stale);
  // 三者都必须够亮（深底上要读得到）：实测把过期那条染成暗灰后字就没了。
  const auto luminance = [](std::uint32_t argb) -> double {
    const double r = static_cast<double>((argb >> 16U) & 0xFFU);
    const double g = static_cast<double>((argb >> 8U) & 0xFFU);
    const double b = static_cast<double>(argb & 0xFFU);
    return (r + g + b) / 3.0;
  };
  ST_CHECK(luminance(verified) > 120.0);
  ST_CHECK(luminance(inferred) > 120.0);
  ST_CHECK(luminance(stale) > 120.0);
}

ST_TEST(cefu_graph_meta_kinds_have_distinct_colors) {
  const auto category = cefu::graph::node_color(cefu::graph::kKindCategory, "");
  const auto person = cefu::graph::node_color(cefu::graph::kKindPerson, "");
  const auto scope = cefu::graph::node_color(cefu::graph::kKindScope, "");
  ST_CHECK(category != person);
  ST_CHECK(category != scope);
  ST_CHECK(person != scope);
}

ST_TEST(cefu_graph_edge_kinds_have_distinct_colors) {
  std::vector<std::uint32_t> colors{
      cefu::graph::edge_color(cefu::graph::kEdgeBelongsTo),
      cefu::graph::edge_color(cefu::graph::kEdgeLinks),
      cefu::graph::edge_color(cefu::graph::kEdgeSupersedes),
      cefu::graph::edge_color(cefu::graph::kEdgeOwnedBy),
      cefu::graph::edge_color(cefu::graph::kEdgeSharedFact),
      cefu::graph::edge_color(cefu::graph::kEdgeScopedTo),
  };
  for (std::size_t i = 0; i < colors.size(); ++i) {
    for (std::size_t j = i + 1; j < colors.size(); ++j) {
      ST_CHECK(colors[i] != colors[j]);
    }
  }
}

ST_TEST(cefu_graph_scope_labels_are_human_readable) {
  // 存储值直接显示会得到 `chatoc_f71d4c…` 这种看不懂的串
  ST_CHECK_EQ(cefu::graph::scope_label("chat"), std::string("本群"));
  ST_CHECK_EQ(cefu::graph::scope_label("persona"), std::string("小霜的"));
  ST_CHECK_EQ(cefu::graph::scope_label("person"), std::string("个人的"));
  ST_CHECK_EQ(cefu::graph::scope_label("global"), std::string("全局"));
  // 未知类型原样显示（未知 kind 说明作者用了新值，显示原值比报错更有用）
  ST_CHECK_EQ(cefu::graph::scope_label("team"), std::string("team"));
}

ST_TEST(cefu_graph_scope_label_omits_opaque_ids) {
  std::vector<Entry> entries{make_entry("e1", "本群规矩", "运维")};
  // 内部 id 形如 `oc_xxx`：40 多个字符画到图上就是一根宽横条
  entries[0].scope = R"({"kind":"chat","id":"oc_f71d4c992494a66b60c82000b48aa00"})";
  const auto g = cefu::graph::build(entries);
  for (const auto& node : g.nodes) {
    if (node.kind != cefu::graph::kKindScope) continue;
    ST_CHECK_EQ(node.label, std::string("本群"));  // 不含 id
  }
}

ST_TEST(cefu_graph_label_truncates_on_codepoints) {
  std::vector<Entry> entries{make_entry("e1", "一二三四五六七八九十", "册")};
  BuildOptions opts;
  opts.max_label_chars = 5;
  const auto g = cefu::graph::build(entries, opts);
  ST_REQUIRE(!g.nodes.empty());
  const auto& label = g.nodes[0].label;
  // 5 个汉字（每字 3 字节）+ 省略号 3 字节 = 18 字节
  ST_CHECK_EQ(label.size(), std::size_t{5 * 3 + 3});
  // 截断本身（按码点）由通用层的用例细测；这里只确认册府确实把它用上了。
  ST_CHECK(label.ends_with("…"));
}

// ── 布局包装（领域缺省参数） ──────────────────────────────────────────────────

ST_TEST(cefu_graph_layout_runs_and_is_deterministic) {
  std::vector<Entry> entries;
  for (int i = 0; i < 12; ++i) {
    entries.push_back(make_entry("e" + std::to_string(i), "词条" + std::to_string(i), "运维"));
  }
  auto first = cefu::graph::build(entries);
  auto second = cefu::graph::build(entries);
  cefu::graph::layout(first);
  cefu::graph::layout(second);
  // 布局确定性（算法本身在通用层测；这条守的是"领域包装没有引入随机性"）
  ST_REQUIRE((first.nodes.size()) == (second.nodes.size()));
  for (std::size_t i = 0; i < first.nodes.size(); ++i) {
    ST_CHECK_EQ(first.nodes[i].position.x, second.nodes[i].position.x);
    ST_CHECK_EQ(first.nodes[i].position.y, second.nodes[i].position.y);
    ST_CHECK_EQ(first.nodes[i].position.z, second.nodes[i].position.z);
  }
}

ST_TEST(cefu_graph_layout_scales_to_view) {
  std::vector<Entry> entries;
  for (int i = 0; i < 40; ++i) {
    entries.push_back(make_entry("e" + std::to_string(i), "词条" + std::to_string(i),
                                 "册" + std::to_string(i % 4)));
  }
  auto g = cefu::graph::build(entries);
  cefu::graph::layout(g);
  // 归一化尺度：最大半径应落在 `scale`（170）附近，而不是随节点数无限增长
  const double radius = st::ui::graph::bounding_radius(g);
  ST_CHECK(radius > 100.0);
  ST_CHECK(radius < 220.0);
}
