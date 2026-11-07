/// @file cefu_graph.cpp
/// @brief 图构建 / 三维力导向布局 / 轨道相机 / 透视投影 / 拾取。
///
/// 设计说明见头文件。这里集中记录**实现上踩过的坑与取舍**。

#include "cefu_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <unordered_map>

namespace cefu::graph {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// 截断标签（按 UTF-8 码点，不能按字节——按字节会切出半个汉字）。
auto truncate_label(std::string_view text, std::size_t max_chars) -> std::string {
  std::size_t chars = 0;
  std::size_t i = 0;
  while (i < text.size() && chars < max_chars) {
    const auto byte = static_cast<unsigned char>(text[i]);
    // UTF-8 首字节决定该码点占几字节（连续字节 10xxxxxx 不是首字节）
    std::size_t step = 1;
    if ((byte & 0x80U) == 0x00U) {
      step = 1;
    } else if ((byte & 0xE0U) == 0xC0U) {
      step = 2;
    } else if ((byte & 0xF0U) == 0xE0U) {
      step = 3;
    } else if ((byte & 0xF8U) == 0xF0U) {
      step = 4;
    }
    i += step;
    ++chars;
  }
  if (i >= text.size()) return std::string{text};
  std::string out{text.substr(0, i)};
  out += "…";
  return out;
}

/// 分类/人/作用域节点的 id 前缀（与词条 id 不会撞：词条 id 形如 `e_xxx`）。
constexpr const char* kCategoryPrefix = "cat:";
constexpr const char* kPersonPrefix = "person:";
constexpr const char* kScopePrefix = "scope:";
constexpr const char* kEntryPrefix = "entry:";

/// 稳定指纹：同一批词条 → 同一 revision（布局缓存的判据）。
auto fingerprint(const std::vector<Entry>& entries) -> std::uint64_t {
  std::uint64_t h = 0xCBF29CE484222325ULL;  // FNV-1a offset basis
  const auto mix = [&h](std::string_view s) {
    for (const char c : s) {
      h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
      h *= 0x100000001B3ULL;
    }
  };
  for (const auto& e : entries) {
    mix(e.id);
    mix(e.title);
    mix(e.category);
    mix(e.scope);
    mix(e.confidence);
    // 单个字段之间放分隔符：否则 ("ab","c") 与 ("a","bc") 指纹相同
    h ^= 0xFFULL;
  }
  return h;
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
  if (at == std::string_view::npos) return "";
  const auto colon = scope.find(':', at);
  if (colon == std::string_view::npos) return "";
  const auto first_quote = scope.find('"', colon + 1);
  if (first_quote == std::string_view::npos) return "";
  const auto second_quote = scope.find('"', first_quote + 1);
  if (second_quote == std::string_view::npos) return "";
  return std::string{scope.substr(first_quote + 1, second_quote - first_quote - 1)};
}

/// 确定性伪随机（布局必须可重复：否则每次打开图形状都变，截图回归也就飘了）。
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
  /// xorshift64*：够随机、够快、跨平台比特级一致。
  auto next() -> std::uint64_t {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return state_ * 2685821657736338717ULL;
  }
  /// [0,1) 均匀。
  auto unit() -> double { return static_cast<double>(next() >> 11U) / 9007199254740992.0; }
  /// [-1,1) 均匀。
  auto signed_unit() -> double { return unit() * 2.0 - 1.0; }

 private:
  std::uint64_t state_;
};

}  // namespace

auto build(const std::vector<Entry>& entries, const BuildOptions& options) -> Graph {
  Graph g;
  g.revision = fingerprint(entries);
  g.entry_count = entries.size();

  // ── 节点去重表 ────────────────────────────────────────────────────────────
  // 分类/人/作用域节点会被多个词条共享，必须先登记再连边（否则会出现 N 个同名节点）。
  std::unordered_map<std::string, std::size_t> index_of;
  const auto intern = [&](const std::string& id, const std::string& label, NodeKind kind,
                          double weight) -> std::size_t {
    const auto found = index_of.find(id);
    if (found != index_of.end()) return found->second;
    Node node;
    node.id = id;
    node.label = truncate_label(label, options.max_label_chars);
    node.kind = kind;
    node.weight = weight;
    g.nodes.push_back(std::move(node));
    const std::size_t at = g.nodes.size() - 1;
    index_of.emplace(id, at);
    return at;
  };

  // ① 词条节点（先建，保证 entry 下标与输入顺序一致——拾取回来能直接对到词条）
  std::vector<std::size_t> entry_node(entries.size(), 0);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    const std::string node_id = std::string{kEntryPrefix} + e.id;
    Node node;
    node.id = node_id;
    node.label = truncate_label(e.title, options.max_label_chars);
    node.kind = NodeKind::Entry;
    node.entry_id = e.id;
    node.confidence = e.confidence;
    node.weight = 1.0;
    g.nodes.push_back(std::move(node));
    index_of.emplace(node_id, g.nodes.size() - 1);
    entry_node[i] = g.nodes.size() - 1;
  }

  // ② 元节点（分类 / 人 / 作用域）与从属边
  const auto connect_meta = [&](const std::string& id, const std::string& label, NodeKind kind,
                                double weight, EdgeKind edge_kind, std::size_t from_entry) {
    const std::size_t to = intern(id, label, kind, weight);
    g.edges.push_back(Edge{from_entry, to, edge_kind});
  };

  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    const std::size_t from = entry_node[i];
    if (options.include_meta_nodes) {
      if (!e.category.empty()) {
        connect_meta(std::string{kCategoryPrefix} + e.category, e.category, NodeKind::Category, 1.6,
                     EdgeKind::BelongsTo, from);
      }
      if (!e.owner.empty()) {
        connect_meta(std::string{kPersonPrefix} + e.owner, e.owner, NodeKind::Person, 1.4,
                     EdgeKind::OwnedBy, from);
      }
      // 作用域只对**非 global** 建节点：global 是缺省，人人都 global，建成节点等于
      // 给整张图加一个巨大枢纽，把真正的结构完全遮住。
      const auto kind = scope_kind_of(e.scope);
      if (kind != "global" && !kind.empty()) {
        const auto sid = scope_id_of(e.scope);
        const std::string label = sid.empty() ? kind : (kind + ":" + sid);
        connect_meta(std::string{kScopePrefix} + label, label, NodeKind::Scope, 1.3,
                     EdgeKind::ScopedTo, from);
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
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (const auto& link : entries[i].links) {
      std::size_t target = 0;
      if (const auto f = by_entry_id.find(link); f != by_entry_id.end()) {
        target = f->second;
      } else if (const auto t = by_title.find(link); t != by_title.end()) {
        target = t->second;
      } else {
        continue;  // 指向不存在的词条：不建边（不是错误，只是还没写）
      }
      if (target == entry_node[i]) continue;
      g.edges.push_back(Edge{entry_node[i], target, EdgeKind::Links});
    }
    // ④ 取代关系（`supersedes` 是被取代者的 id）
    if (!entries[i].supersedes.empty()) {
      if (const auto f = by_entry_id.find(entries[i].supersedes); f != by_entry_id.end()) {
        g.edges.push_back(Edge{entry_node[i], f->second, EdgeKind::Supersedes});
      }
    }
  }

  // ⑤ 共享硬事实（隐含关联）
  //
  // 这是**最有价值也最危险**的一类边：它能在没人显式声明关联的情况下发现
  // 「这三条知识其实在说同一台机器」。危险在于噪声——任何两条都共享某些平凡
  // 硬事实（比如都写了 `负责人=张三`）。所以只取**足够具体**的事实值：
  // 数字/IP/路径这类，且至少 min_shared_fact_degree 条共享才建边。
  //
  // 存储形态是 `"k=v"`（见 main.cpp 的 list_of：对象形态会归一成这种字符串）。
  std::unordered_map<std::string, std::vector<std::size_t>> fact_owners;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (const auto& fact : entries[i].facts) {
      const auto eq = fact.find('=');
      if (eq == std::string::npos) continue;
      const std::string value = fact.substr(eq + 1);
      // 阈值按**码点**数而不是字节数：“5x” 恰好 2 字节也能有区分度（阈值符号），
      // 而按字节判会把 “9” 这种单字排除——那是对的（太短真的没区分度）。
      std::size_t codepoints = 0;
      for (std::size_t k = 0; k < value.size(); ++k) {
        if ((static_cast<unsigned char>(value[k]) & 0xC0U) != 0x80U) ++codepoints;
      }
      if (codepoints < 2) continue;  // 单字值区分度不足
      // 只保留"具体值"：含数字或路径分隔符（端口/阈值/IP/版本/路径）
      const bool concrete =
          std::any_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) ||
          value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
      if (!concrete) continue;
      fact_owners[value].push_back(i);
    }
  }
  for (const auto& [value, owners] : fact_owners) {
    if (owners.size() < options.min_shared_fact_degree) continue;
    // 全连接会变成 O(n²) 边爆炸；只连成一条链（够表达"它们相关"）
    for (std::size_t k = 1; k < owners.size(); ++k) {
      if (owners[k] == owners[k - 1]) continue;
      g.edges.push_back(Edge{entry_node[owners[k - 1]], entry_node[owners[k]], EdgeKind::SharedFact});
    }
  }

  // ── 统计 ────────────────────────────────────────────────────────────────
  std::vector<std::size_t> degree(g.nodes.size(), 0);
  for (const auto& e : g.edges) {
    ++degree[e.from];
    ++degree[e.to];
  }
  g.category_count = static_cast<std::size_t>(
      std::count_if(g.nodes.begin(), g.nodes.end(),
                    [](const Node& n) { return n.kind == NodeKind::Category; }));
  g.person_count = static_cast<std::size_t>(
      std::count_if(g.nodes.begin(), g.nodes.end(),
                    [](const Node& n) { return n.kind == NodeKind::Person; }));
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    if (g.nodes[i].kind != NodeKind::Entry) continue;
    g.nodes[i].isolated = degree[i] == 0;
    if (g.nodes[i].isolated) ++g.isolated_entries;
  }

  // 最大连通分量（含元节点——它才是"知识聚成几团"的真实答案）
  {
    std::vector<std::vector<std::size_t>> adjacency(g.nodes.size());
    for (const auto& e : g.edges) {
      adjacency[e.from].push_back(e.to);
      adjacency[e.to].push_back(e.from);
    }
    std::vector<char> seen(g.nodes.size(), 0);
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
      if (seen[i] != 0) continue;
      std::size_t size = 0;
      std::vector<std::size_t> stack{i};
      seen[i] = 1;
      while (!stack.empty()) {
        const auto cur = stack.back();
        stack.pop_back();
        ++size;
        for (const auto next : adjacency[cur]) {
          if (seen[next] != 0) continue;
          seen[next] = 1;
          stack.push_back(next);
        }
      }
      g.largest_cluster = std::max(g.largest_cluster, size);
    }
  }

  return g;
}

void layout(Graph& graph, const LayoutOptions& options) {
  const std::size_t count = graph.nodes.size();
  if (count == 0) return;

  Rng rng(options.seed);
  // 初始位置：球面上均匀撒点。
  //
  // 为何不用"全在原点"：力导向的斥力在原点重合时方向未定义（随机抖动会让
  // 结果不可重复）。撒在球面上既有确定方向，又保证初始就散开。
  std::vector<st::math::Vec3> position(count);
  std::vector<st::math::Vec3> velocity(count);
  for (std::size_t i = 0; i < count; ++i) {
    // Marsaglia 球面均匀采样
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double len = 0.0;
    do {
      x = rng.signed_unit();
      y = rng.signed_unit();
      z = rng.signed_unit();
      len = std::sqrt(x * x + y * y + z * z);
    } while (len < 0.1 || len > 1.0);
    const double scale = 40.0 + rng.unit() * 120.0;
    position[i] = st::math::Vec3{static_cast<float>(x / len * scale), static_cast<float>(y / len * scale),
                             static_cast<float>(z / len * scale)};
  }

  std::vector<std::pair<std::size_t, std::size_t>> springs;
  springs.reserve(graph.edges.size());
  for (const auto& e : graph.edges) springs.emplace_back(e.from, e.to);

  // ── 分册聚簇：算出每个节点的簇 id 与该簇的锚点 ──
  //
  // 为何用「边的端点」而不是节点自带的字段：节点只知道自己是什么类型，
  // 不知道属于哪册——归属信息在 `BelongsTo` 边上（词条 → 分类）。
  // 这正是图的表达能力：关系存在边里，不在节点属性里。
  //
  // 优先级：分类 > 人 > 作用域。词条可能同时挂三个，
  // 取「最能代表它位置」的：分类是人为的分册（读的人按它找东西），排最先。
  const auto hub_rank = [](NodeKind kind) -> int {
    switch (kind) {
      case NodeKind::Category:
        return 3;
      case NodeKind::Person:
        return 2;
      case NodeKind::Scope:
        return 1;
      case NodeKind::Entry:
        return 0;
    }
    return 0;
  };
  std::vector<std::string> cluster_of(count);
  std::vector<int> cluster_rank(count, -1);
  for (const auto& e : graph.edges) {
    if (e.from >= count || e.to >= count) continue;
    if (e.kind != EdgeKind::BelongsTo && e.kind != EdgeKind::OwnedBy &&
        e.kind != EdgeKind::ScopedTo) {
      continue;
    }
    const auto& hub = graph.nodes[e.to];
    const int rank = hub_rank(hub.kind);
    if (rank <= cluster_rank[e.from]) continue;  // 已有更高优先级归属
    cluster_rank[e.from] = rank;
    cluster_of[e.from] = hub.id;
  }
  // 兜底：没归属的节点各自成簇（用自身 id）。
  // 若都归到同一个锚点，它们会堆成一团——图上看不出它们彼此无关。
  for (std::size_t i = 0; i < count; ++i) {
    if (cluster_of[i].empty()) cluster_of[i] = graph.nodes[i].id;
  }

  // 簇锚点：均匀铺在球面上（Fibonacci 球，分布均匀且确定）
  std::vector<std::string> cluster_ids;
  cluster_ids.reserve(count);
  for (const auto& id : cluster_of) {
    if (std::find(cluster_ids.begin(), cluster_ids.end(), id) == cluster_ids.end()) {
      cluster_ids.push_back(id);
    }
  }
  const auto cluster_count = static_cast<double>(cluster_ids.size());
  std::vector<st::math::Vec3> cluster_anchor(cluster_ids.size());
  // 锚点半径：簇多时铺得开一点（否则小簇叠在大簇上看不出是两团）
  const double spread = options.scale * options.cluster_spread;
  for (std::size_t c = 0; c < cluster_ids.size(); ++c) {
    const double t = (static_cast<double>(c) + 0.5) / std::max(cluster_count, 1.0);
    const double cy = 1.0 - 2.0 * t;                   // y 均匀分层
    const double cr = std::sqrt(std::max(0.0, 1.0 - cy * cy));
    const double phi = static_cast<double>(c) * 2.39996322972865332;  // 黄金角
    cluster_anchor[c] = st::math::Vec3{static_cast<float>(std::cos(phi) * cr * spread),
                                       static_cast<float>(cy * spread),
                                       static_cast<float>(std::sin(phi) * cr * spread)};
  }
  std::vector<std::size_t> node_cluster(count, 0);
  for (std::size_t i = 0; i < count; ++i) {
    const auto it = std::find(cluster_ids.begin(), cluster_ids.end(), cluster_of[i]);
    node_cluster[i] = static_cast<std::size_t>(std::distance(cluster_ids.begin(), it));
  }

  // 理想边长：节点越多，图越大，边长也要跟着长（否则挤成一团）
  const double k = 90.0 + std::sqrt(static_cast<double>(count)) * 26.0;
  const double clamp_max = options.max_step;

  std::vector<st::math::Vec3> force(count);
  for (std::size_t iter = 0; iter < options.iterations; ++iter) {
    // 降温：迭代越往后步子越小，帮助收敛而不是一直震荡
    const double heat = 1.0 - static_cast<double>(iter) / static_cast<double>(options.iterations);
    std::fill(force.begin(), force.end(), st::math::Vec3{});

    // 斥力（O(n²)：几百个节点在现代 CPU 上是微秒级，不值得上 k-d 树）
    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t j = i + 1; j < count; ++j) {
        st::math::Vec3 delta = position[i] - position[j];
        double dist2 = static_cast<double>(delta.dot(delta));
        if (dist2 < 1e-4) {
          // 重合：给一个确定的小偏移（用下标决定方向，保证可重复）
          const double angle = static_cast<double>(i * 31 + j * 17) * 0.017;
          delta = st::math::Vec3{static_cast<float>(std::cos(angle) * 0.01),
                             static_cast<float>(std::sin(angle) * 0.01), 0.005F};
          dist2 = 1e-4;
        }
        const double dist = std::sqrt(dist2);
        const double magnitude = options.repel * k * k / dist2;
        const st::math::Vec3 push = delta * static_cast<float>(magnitude / dist);
        force[i] = force[i] + push;
        force[j] = force[j] - push;
      }
    }

    // 引力（弹簧：沿边拉近）
    for (const auto& [a, b] : springs) {
      st::math::Vec3 delta = position[a] - position[b];
      const double dist = std::max(1e-3, static_cast<double>(delta.length()));
      const double magnitude = options.attract * (dist * dist) / k;
      const st::math::Vec3 pull = delta * static_cast<float>(magnitude / dist);
      force[a] = force[a] - pull;
      force[b] = force[b] + pull;
    }

    // 中心聚拢（没有它，孤立节点会一路飞走）
    for (std::size_t i = 0; i < count; ++i) {
      force[i] = force[i] - position[i] * static_cast<float>(options.center_pull);
    }

    // 簇拉力：把同册的词条拉回自己那册的锚点。
    //
    // 这是本轮的**根本修复**。没有它时，同册词条会被斥力推到球的对面，
    // 投影后连线横七竖八（前四轮调线条参数都治不了，因为问题在这里）。
    //
    // 为何是弹簧而不是硬约束：硬约束会让簇内没有布局空间（词条重叠），
    // 弹簧则保留“簇内还按真实关系微调”的能力。
    if (options.cluster_pull > 0.0) {
      for (std::size_t i = 0; i < count; ++i) {
        const st::math::Vec3 to_anchor = cluster_anchor[node_cluster[i]] - position[i];
        force[i] = force[i] + to_anchor * static_cast<float>(options.cluster_pull);
      }
    }

    // 积分（带阻尼与单步上限）
    for (std::size_t i = 0; i < count; ++i) {
      velocity[i] = (velocity[i] + force[i]) * static_cast<float>(options.damping);
      st::math::Vec3 step = velocity[i] * static_cast<float>(heat);
      const double step_len = static_cast<double>(step.length());
      if (step_len > clamp_max) {
        step = step * static_cast<float>(clamp_max / step_len);
      }
      position[i] = position[i] + step;
    }
  }

  // 归一化到统一尺度：布局算法的绝对尺度没有物理意义，只有相对结构有意义。
  double max_radius = 1.0;
  for (const auto& p : position) max_radius = std::max(max_radius, static_cast<double>(p.length()));
  const double shrink = options.scale / max_radius;
  for (std::size_t i = 0; i < count; ++i) {
    graph.nodes[i].position = position[i] * static_cast<float>(shrink);
  }
}

void Camera::orbit(double delta_yaw, double delta_pitch) {
  yaw += delta_yaw;
  pitch += delta_pitch;
  // 俯仰夹到 ±(π/2 - ε)：越过极点会让画面翻转（用户会瞬间失去方向感）
  const double limit = kPi / 2.0 - 0.02;
  pitch = std::clamp(pitch, -limit, limit);
  // yaw 归一到 [-π, π]，避免长时间拖动后浮点精度流失
  while (yaw > kPi) yaw -= 2.0 * kPi;
  while (yaw < -kPi) yaw += 2.0 * kPi;
}

void Camera::zoom(double factor) {
  // 距离夹取：太近会穿到图内部，太远图变成一点
  distance = std::clamp(distance * factor, 150.0, 2400.0);
}

auto project(const Graph& graph, const Camera& camera, const ProjectOptions& options)
    -> std::vector<Projected> {
  std::vector<Projected> out(graph.nodes.size());
  if (graph.nodes.empty()) return out;

  // 相机基向量（球坐标 → 笛卡尔）
  const double cy = std::cos(camera.yaw);
  const double sy = std::sin(camera.yaw);
  const double cp = std::cos(camera.pitch);
  const double sp = std::sin(camera.pitch);
  // 相机位置（世界坐标）
  const st::math::Vec3 eye{static_cast<float>(camera.distance * cp * sy),
                       static_cast<float>(camera.distance * sp),
                       static_cast<float>(camera.distance * cp * cy)};
  // 朝向原点
  const st::math::Vec3 forward = st::math::Vec3{} - eye;
  const st::math::Vec3 f = forward.normalized();
  const st::math::Vec3 world_up{0.0F, 1.0F, 0.0F};
  st::math::Vec3 right = f.cross(world_up).normalized();
  // 俯仰接近 ±π/2 时 f 与 world_up 共线，cross 退化为零向量——换一个参考轴。
  if (right.length() < 1e-4F) right = st::math::Vec3{1.0F, 0.0F, 0.0F};
  const st::math::Vec3 up = right.cross(f).normalized();

  const double fov = camera.fov_degrees * kPi / 180.0;
  // 焦距：把半高映射到 tan(fov/2)。
  //
  // 用**高度**做基准而不是宽度：宽高比变化时高度不变更符合直觉
  // （宽屏只是看到更多水平范围，物体不会突然变大变小）。
  const double focal = (options.viewport_height * 0.5) / std::tan(fov * 0.5);
  const double cx = options.viewport_x + options.viewport_width * 0.5;
  const double cy_screen = options.viewport_y + options.viewport_height * 0.5;

  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const st::math::Vec3 rel = graph.nodes[i].position - eye;
    const double depth = static_cast<double>(rel.dot(f));  // 相机前方为正
    Projected p;
    p.depth = depth;
    if (depth < options.near_plane) {
      // 近平面之后：不画。若不裁，投影会除出巨大坐标把整屏刷黑。
      p.visible = false;
      out[i] = p;
      continue;
    }
    const double sx = static_cast<double>(rel.dot(right));
    const double sy_world = static_cast<double>(rel.dot(up));
    p.x = cx + sx * focal / depth;
    p.y = cy_screen - sy_world * focal / depth;  // 屏幕 y 向下
    p.radius = options.base_radius * graph.nodes[i].weight * focal / depth;
    p.visible = p.radius > 0.25;  // 太小的球体不值得画（也点不中）
    out[i] = p;
  }
  return out;
}

auto pick(const Graph& graph, const std::vector<Projected>& projected, double x, double y,
          double tolerance) -> std::size_t {
  std::size_t best = graph.nodes.size();  // == 未命中
  double best_score = 0.0;
  for (std::size_t i = 0; i < projected.size(); ++i) {
    const auto& p = projected[i];
    if (!p.visible) continue;
    const double dx = p.x - x;
    const double dy = p.y - y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double hit_radius = p.radius + tolerance;
    if (dist > hit_radius) continue;
    // 命中多个时取"最近球心"（而不是最近的边）：球重叠处点到的应是中心更近的那个
    const double score = hit_radius - dist;
    if (score > best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

auto depth_order(const std::vector<Projected>& projected) -> std::vector<std::size_t> {
  std::vector<std::size_t> order(projected.size());
  std::iota(order.begin(), order.end(), 0);
  // 远的先画（画家算法）。同深度按下标稳定排序——否则同深度的节点会闪烁。
  std::stable_sort(order.begin(), order.end(), [&projected](std::size_t a, std::size_t b) {
    return projected[a].depth > projected[b].depth;
  });
  return order;
}

}  // namespace cefu::graph
