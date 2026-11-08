/// @file cefu_graph.cpp
/// @brief 册府知识图谱的领域适配层：词条 → 通用关系图（+ 配色与人话标签）。
///
/// 设计说明见头文件。这里记**领域侧的取舍**（通用布局的取舍在
/// `st/ui/graph_layout.cpp`，那里是另一套理由）。

#include "cefu_graph.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cefu::graph {
namespace {

using st::ui::graph::Edge;
using st::ui::graph::Node;

/// 节点 id 前缀。与词条 id 不会撞：词条 id 形如 `e_xxx`。
constexpr const char* kCategoryPrefix = "cat:";
constexpr const char* kPersonPrefix = "person:";
constexpr const char* kScopePrefix = "scope:";
constexpr const char* kEntryPrefix = "entry:";

/// 聚簇优先级（数值大者优先当簇心）。
///
/// 为什么这个次序：**分类是人为的分册**（读的人按它找东西），排最先；
/// 负责人次之（"这块归谁"）；作用域最后（它是可见性约束，不是组织方式）。
/// 词条自己永远不当簇心（rank 0 = 只有它没归属时才兜底自成一簇）。
double cluster_rank_of(std::string_view kind) {
  if (kind == kKindCategory) return 3.0;
  if (kind == kKindPerson) return 2.0;
  if (kind == kKindScope) return 1.0;
  return 0.0;
}

/// 解析 scope JSON 串里的 `kind`（够用即可，不引 JSON 库）。
auto scope_kind_of(std::string_view scope) -> std::string {
  const auto at = scope.find("\"kind\"");
  if (at == std::string_view::npos) return "global";
  const auto colon = scope.find(':', at);
  if (colon == std::string_view::npos) return "global";
  const auto first_quote = scope.find('"', colon + 1);
  if (first_quote == std::string_view::npos) return "global";
  const auto second_quote = scope.find('"', first_quote + 1);
  if (second_quote == std::string_view::npos) return "global";
  return std::string{scope.substr(first_quote + 1, second_quote - first_quote - 1)};
}

/// 解析 scope 里的 `id`。
auto scope_id_of(std::string_view scope) -> std::string {
  const auto at = scope.find("\"id\"");
  if (at == std::string_view::npos) return {};
  const auto colon = scope.find(':', at);
  if (colon == std::string_view::npos) return {};
  const auto first_quote = scope.find('"', colon + 1);
  if (first_quote == std::string_view::npos) return {};
  const auto second_quote = scope.find('"', first_quote + 1);
  if (second_quote == std::string_view::npos) return {};
  return std::string{scope.substr(first_quote + 1, second_quote - first_quote - 1)};
}

/// 把内部 id 缩成一个**人能认的尾巴**，不可读时返回空。
///
/// 为何默认不拼 id：`oc_f71d4c992494a66b60c82000b48aa00` 有 40 多个字符，画到图上
/// 就是一根宽横条，把真正的内容挤掉；即便取前 10 位（`oc_f71d4cc`）人也读不出是
/// 哪个群——「本群 oc_f71d4cc」相对「本群」没有任何信息增量。
///
/// 所以：只在 id **看起来是人写的**（短、且不像随机串）时才显示。识别随机串的判据：
/// 含下划线且长度 > 8——本仓的内部 id 都是这个形态（`oc_xxx` 会话、`ou_xxx` 用户、
/// `e_xxx` 词条）。
auto short_id_tail(std::string_view id) -> std::string {
  if (id.empty() || id.size() > 12) return {};
  if (id.find('_') != std::string_view::npos && id.size() > 8) return {};
  return std::string(id);
}

/// 字段值的码点数（"5x" 恰好 2 字节也能有区分度；按字节判会把 "9" 这种单字排除——
/// 那是对的，太短真的没区分度）。
auto codepoint_count(std::string_view text) -> std::size_t {
  std::size_t count = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;
  }
  return count;
}

}  // namespace

auto build(const std::vector<Entry>& entries, const BuildOptions& options)
    -> st::ui::graph::Graph {
  st::ui::graph::Graph g;

  // ── 节点去重表 ────────────────────────────────────────────────────────────
  // 分类/人/作用域节点会被多个词条共享，必须先登记再连边（否则会出现 N 个同名节点）。
  std::unordered_map<std::string, std::size_t> index_of;
  const auto intern = [&](const std::string& id, const std::string& label, std::string_view kind,
                          double weight, double rank) -> std::size_t {
    const auto found = index_of.find(id);
    if (found != index_of.end()) return found->second;
    Node node;
    node.id = id;
    node.label = st::ui::graph::truncate_label(label, options.max_label_chars);
    node.kind = std::string(kind);
    node.cluster_rank = rank;
    node.weight = weight;
    node.argb = node_color(kind, {});
    // 枢纽（分类/人/作用域）的文字比词条更重要：避让时它先被排下。
    node.label_weight = kind == kKindEntry ? 0.35F : 0.9F;
    // 两者都画底板：无底板时分类字会被背景 + 连线弄成一团糊影；词条底板更浓。
    node.label_plate = true;
    node.plate_alpha = kind == kKindEntry ? 0xE6 : 0xB4;
    g.nodes.push_back(std::move(node));
    const std::size_t at = g.nodes.size() - 1;
    index_of.emplace(id, at);
    return at;
  };

  // ① 词条节点（先建，保证 entry 下标与输入顺序一致——拾取回来能直接对到词条）
  std::vector<std::size_t> entry_node(entries.size(), 0);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    Node node;
    node.id = std::string{kEntryPrefix} + entry.id;
    node.label = st::ui::graph::truncate_label(entry.title, options.max_label_chars);
    node.kind = std::string(kKindEntry);
    node.cluster_rank = 0.0;
    node.weight = 1.0;
    node.argb = node_color(kKindEntry, entry.confidence);
    // 重要度：词条基础 0.35；已确认的往上抬（重要的先被读到），过期的往下压。
    float label_weight = 0.35F;
    if (entry.confidence == "verified") label_weight += 0.15F;
    if (entry.confidence == "stale") label_weight -= 0.1F;
    node.label_weight = std::clamp(label_weight, 0.0F, 1.0F);
    node.label_plate = true;
    node.plate_alpha = 0xE6;
    g.nodes.push_back(std::move(node));
    index_of.emplace(std::string{kEntryPrefix} + entry.id, g.nodes.size() - 1);
    entry_node[i] = g.nodes.size() - 1;
  }

  // ② 元节点（分类 / 人 / 作用域）与归属边
  const auto connect_meta = [&](const std::string& id, const std::string& label,
                                std::string_view kind, double weight,
                                std::string_view edge_kind, std::size_t from_entry) {
    const std::size_t to = intern(id, label, kind, weight, cluster_rank_of(kind));
    Edge edge;
    edge.from = from_entry;
    edge.to = to;
    edge.kind = std::string(edge_kind);
    edge.argb = edge_color(edge_kind);
    // 归属边**参与聚簇**（这是"同册聚成一团"的信息来源）。
    edge.clustering = true;
    g.edges.push_back(std::move(edge));
  };

  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    const std::size_t from = entry_node[i];
    if (options.include_meta_nodes) {
      if (!entry.category.empty()) {
        connect_meta(std::string{kCategoryPrefix} + entry.category, entry.category, kKindCategory,
                     1.6, kEdgeBelongsTo, from);
      }
      if (!entry.owner.empty()) {
        connect_meta(std::string{kPersonPrefix} + entry.owner, entry.owner, kKindPerson, 1.4,
                     kEdgeOwnedBy, from);
      }
      // 作用域只对**非 global** 建节点：global 是缺省，人人都 global，建成节点等于
      // 给整张图加一个巨大枢纽，把真正的结构完全遮住。
      const auto kind = scope_kind_of(entry.scope);
      if (kind != "global" && !kind.empty()) {
        const auto sid = scope_id_of(entry.scope);
        // 标签用**人能读的类型名**，不拼内部 id（见 scope_label 的说明）。
        std::string label = scope_label(kind);
        if (!sid.empty()) {
          const std::string tail = short_id_tail(sid);
          if (!tail.empty()) label += " " + tail;
        }
        connect_meta(std::string{kScopePrefix} + kind + label, label, kKindScope, 1.3,
                     kEdgeScopedTo, from);
      }
    }
  }

  // ③ 词条间显式关联（`links` 里存的是词条 id 或标题，两种都认）
  std::unordered_map<std::string, std::size_t> by_entry_id;
  std::unordered_map<std::string, std::size_t> by_title;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    by_entry_id.emplace(entries[i].id, entry_node[i]);
    by_title.emplace(entries[i].title, entry_node[i]);
  }
  const auto push_entry_edge = [&](std::size_t from, std::size_t to, std::string_view kind,
                                   double width_scale) {
    Edge edge;
    edge.from = from;
    edge.to = to;
    edge.kind = std::string(kind);
    edge.argb = edge_color(kind);
    edge.width_scale = width_scale;
    edge.clustering = false;  // 词条之间的关系不决定它归哪一册
    g.edges.push_back(std::move(edge));
  };

  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (const auto& link : entries[i].links) {
      std::size_t target = 0;
      if (const auto by_id = by_entry_id.find(link); by_id != by_entry_id.end()) {
        target = by_id->second;
      } else if (const auto by_name = by_title.find(link); by_name != by_title.end()) {
        target = by_name->second;
      } else {
        continue;  // 指向不存在的词条：不建边（不是错误，只是还没写）
      }
      if (target == entry_node[i]) continue;  // 自指无意义
      push_entry_edge(entry_node[i], target, kEdgeLinks, 1.0);
    }
    // ④ 取代关系（`supersedes` 是被取代者的 id）
    if (!entries[i].supersedes.empty()) {
      if (const auto replaced = by_entry_id.find(entries[i].supersedes);
          replaced != by_entry_id.end()) {
        push_entry_edge(entry_node[i], replaced->second, kEdgeSupersedes, 1.0);
      }
    }
  }

  // ⑤ 共享硬事实（隐含关联）
  //
  // 存储形态是 `"k=v"`（见 main.cpp 的 list_of：对象形态会归一成这种字符串）。
  std::unordered_map<std::string, std::vector<std::size_t>> fact_owners;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (const auto& fact : entries[i].facts) {
      const auto eq = fact.find('=');
      if (eq == std::string::npos) continue;
      const std::string value = fact.substr(eq + 1);
      if (codepoint_count(value) < 2) continue;  // 单字值区分度不足
      // 只保留"具体值"：含数字或路径分隔符（端口/阈值/IP/版本/路径）
      const bool concrete =
          std::any_of(value.begin(), value.end(),
                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) ||
          value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
      if (!concrete) continue;
      fact_owners[value].push_back(i);
    }
  }
  for (const auto& [value, owners] : fact_owners) {
    (void)value;
    if (owners.size() < options.min_shared_fact_degree) continue;
    // 全连接会变成 O(n²) 边爆炸；只连成一条链（够表达"它们相关"）。
    // 线更细：这是**推断**出来的关联，不该和显式声明的一样重。
    for (std::size_t k = 1; k < owners.size(); ++k) {
      if (owners[k] == owners[k - 1]) continue;
      push_entry_edge(entry_node[owners[k - 1]], entry_node[owners[k]], kEdgeSharedFact, 0.75);
    }
  }

  // 统计（孤立节点数、最大连通分量、指纹）
  st::ui::graph::finalize(g);
  return g;
}

void layout(st::ui::graph::Graph& graph) {
  st::ui::graph::LayoutOptions options;
  // 册府的观感参数（调过四轮：迭代太少不收敛，簇拉力太小"同册散开"）。
  options.iterations = 220;
  options.cluster_pull = 0.055;
  options.cluster_spread = 0.92;
  options.scale = 170.0;
  st::ui::graph::layout(graph, options);
}

auto node_color(std::string_view kind, std::string_view confidence) -> std::uint32_t {
  if (kind == kKindCategory) return 0xFFFFD782U;  // 册：暖金（枢纽，深底上最好认）
  if (kind == kKindPerson) return 0xFF72EDBEU;    // 人：薄荷绿
  if (kind == kKindScope) return 0xFFCFA8FFU;     // 作用域：紫
  // 词条文字用**高亮近白**：它是主角，必须先读得到。
  // 语义（已确认 / 待确认 / 过期）靠色相微差区分，但三者都保证在深底上可读——
  // 实测把过期那条染成暗灰后字就没了。
  if (confidence == "verified") return 0xFFE8F2FFU;  // 略带蓝的白
  if (confidence == "stale") return 0xFFC4CBD4U;     // 过期：中性灰白（比暗灰亮很多）
  return 0xFFFFE9C4U;                                // 待确认：暖白
}

auto edge_color(std::string_view kind) -> std::uint32_t {
  if (kind == kEdgeBelongsTo) return 0xFFB89448U;   // 从属：金
  if (kind == kEdgeLinks) return 0xFF4E96E0U;       // 显式关联：蓝
  if (kind == kEdgeSupersedes) return 0xFFD05C4CU;  // 取代：红
  if (kind == kEdgeOwnedBy) return 0xFF42B88CU;     // 负责人：绿
  if (kind == kEdgeSharedFact) return 0xFF9A78D0U;  // 共享硬事实：紫
  if (kind == kEdgeScopedTo) return 0xFF7484C8U;    // 作用域：蓝紫
  return 0xFF64748BU;
}

auto scope_label(std::string_view scope_kind) -> std::string {
  if (scope_kind == "chat") return "本群";
  if (scope_kind == "persona") return "小霜的";  // 人格级知识（只给该人格用）
  if (scope_kind == "person") return "个人的";
  if (scope_kind == "global") return "全局";
  // 未知类型：原样显示，不拼 id（未知 kind 本身就说明作者用了新值，
  // 显示原值比报错更有用）。
  return std::string(scope_kind);
}

}  // namespace cefu::graph
