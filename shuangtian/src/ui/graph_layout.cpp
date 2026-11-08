/// @file graph_layout.cpp
/// @brief 图统计 / 力导向 + 分簇布局 / 轨道相机 / 透视投影 / 拾取。
///
/// 设计说明见头文件。这里集中记录**实现上踩过的坑与取舍**（都是从册府那版搬过来的
/// 实测结论，不是推论）。

#include "st/ui/graph_layout.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::ui::graph {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// 确定性伪随机（布局必须可重复：否则每次打开形状都变，截图回归也就飘了）。
///
/// 为什么用 xorshift64\* 而不是 `std::mt19937`：① 后者在不同标准库实现下**不保证**
/// 逐位相同的序列（libstdc++ 与 libc++ 的 `uniform_real_distribution` 算法不同），
/// 而布局确定性是截图回归的前提；② 这里只需要"看起来随机、可重复"，不需要
/// 统计质量。xorshift64\* 的比特级一致是规范保证的。
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
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

/// 数据指纹（FNV-1a）：同一批数据 → 同一 revision（布局缓存的判据）。
///
/// **字段之间必须放分隔符**：否则 ("ab","c") 与 ("a","bc") 指纹相同，改一次数据
/// 而指纹不变 → 布局不重算 → 界面上是旧形状而数据是新的（最难查的一类不同步）。
auto fingerprint(const Graph& graph) -> std::uint64_t {
  std::uint64_t h = 0xCBF29CE484222325ULL;  // FNV-1a offset basis
  const auto mix = [&h](std::string_view text) {
    for (const char c : text) {
      h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
      h *= 0x100000001B3ULL;
    }
    // **每个字段之后都要放分隔符**，不是只在节点之间。
    //
    // 为何：FNV-1a 对“拼接”是可穿透的——若 id="ab" + label="c" 与
    // id="a" + label="bc" 之间没有分隔，两者的字节序列完全一样，指纹也完全一样。
    // 后果不是哈希碰撞那么学术：改了标签（把“a”的标题改成“bc”）而指纹不变，
    // 布局缓存命中、界面停在旧形状，而数据已经是新的。
    // 单测 `graph_layout_revision_separates_field_boundaries` 钉的正是这条。
    h ^= 0xFFULL;
    h *= 0x100000001B3ULL;
  };
  for (const auto& node : graph.nodes) {
    mix(node.id);
    mix(node.label);
    mix(node.kind);
    mix(std::to_string(node.cluster_rank));
    mix(std::to_string(node.weight));
  }
  for (const auto& edge : graph.edges) {
    mix(std::to_string(edge.from));
    mix(std::to_string(edge.to));
    mix(edge.kind);
  }
  return h;
}

/// 无向邻接表（连通分量与孤立判定都用它）。
auto adjacency_of(const Graph& graph) -> std::vector<std::vector<std::size_t>> {
  std::vector<std::vector<std::size_t>> adjacency(graph.nodes.size());
  for (const auto& edge : graph.edges) {
    if (edge.from >= graph.nodes.size() || edge.to >= graph.nodes.size()) continue;
    adjacency[edge.from].push_back(edge.to);
    adjacency[edge.to].push_back(edge.from);
  }
  return adjacency;
}

}  // namespace

// ── 图统计 ────────────────────────────────────────────────────────────────────

auto Graph::count_kind(std::string_view kind) const -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(nodes.begin(), nodes.end(), [kind](const Node& node) {
        return node.kind == kind;
      }));
}

void finalize(Graph& graph) {
  graph.revision = fingerprint(graph);
  const auto adjacency = adjacency_of(graph);

  graph.isolated_count = 0;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    graph.nodes[i].isolated = adjacency[i].empty();
    if (graph.nodes[i].isolated) ++graph.isolated_count;
  }

  // 最大连通分量：**含全部节点类型**——"知识聚成几团"要按整张图算，
  // 只数叶子节点会把「十个分类连成一片」看成十个小岛。
  graph.largest_cluster = 0;
  std::vector<char> seen(graph.nodes.size(), 0);
  std::vector<std::size_t> stack;
  for (std::size_t start = 0; start < graph.nodes.size(); ++start) {
    if (seen[start] != 0) continue;
    std::size_t size = 0;
    stack.assign(1, start);
    seen[start] = 1;
    while (!stack.empty()) {
      const auto current = stack.back();
      stack.pop_back();
      ++size;
      for (const auto next : adjacency[current]) {
        if (seen[next] != 0) continue;
        seen[next] = 1;
        stack.push_back(next);
      }
    }
    graph.largest_cluster = std::max(graph.largest_cluster, size);
  }
}

// ── 布局 ──────────────────────────────────────────────────────────────────────

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
    // Marsaglia 球面均匀采样（`[0.1, 1.0]` 区间排除近原点，免得方向病态）
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
    position[i] = st::math::Vec3{static_cast<float>(x / len * scale),
                                 static_cast<float>(y / len * scale),
                                 static_cast<float>(z / len * scale)};
  }

  std::vector<std::pair<std::size_t, std::size_t>> springs;
  springs.reserve(graph.edges.size());
  for (const auto& edge : graph.edges) {
    if (edge.from >= count || edge.to >= count) continue;
    springs.emplace_back(edge.from, edge.to);
  }

  // ── 分簇：每个节点归到哪个簇、每簇的锚点在哪 ──
  //
  // 归属信息在**边**上（`Edge::clustering`），不在节点属性里——这正是图的表达
  // 能力：关系存在边里。多归属时取 `cluster_rank` 最高的一端（见头文件
  // `Node::cluster_rank` 的说明）。
  const auto rank_of = [&graph](std::size_t index) -> double {
    return index < graph.nodes.size() ? graph.nodes[index].cluster_rank : 0.0;
  };
  std::vector<std::string> cluster_of(count);
  std::vector<double> cluster_rank(count, -1e300);
  for (const auto& edge : graph.edges) {
    if (edge.from >= count || edge.to >= count) continue;
    if (!edge.clustering) continue;
    // 归属方向：`from` 是成员、`to` 是簇心（方向由调用方约定，与边的语义名字无关）。
    const double rank = rank_of(edge.to);
    if (rank <= cluster_rank[edge.from]) continue;  // 已有更高优先级归属
    cluster_rank[edge.from] = rank;
    cluster_of[edge.from] = graph.nodes[edge.to].id;
  }
  // 兜底：没归属的节点各自成簇（用自身 id）。
  // 若都归到同一个锚点，它们会堆成一团——图上看不出它们彼此无关。
  for (std::size_t i = 0; i < count; ++i) {
    if (cluster_of[i].empty()) cluster_of[i] = graph.nodes[i].id;
  }

  // 簇锚点：均匀铺在球面上（Fibonacci 球，分布均匀且确定）
  std::vector<std::string> cluster_ids;
  cluster_ids.reserve(count);
  std::unordered_map<std::string, std::size_t> cluster_index;
  for (const auto& id : cluster_of) {
    if (cluster_index.find(id) != cluster_index.end()) continue;
    cluster_index.emplace(id, cluster_ids.size());
    cluster_ids.push_back(id);
  }
  const auto cluster_count = static_cast<double>(cluster_ids.size());
  std::vector<st::math::Vec3> cluster_anchor(cluster_ids.size());
  // 锚点半径：`cluster_spread` 决定簇之间铺多开
  const double spread = options.scale * options.cluster_spread;
  for (std::size_t c = 0; c < cluster_ids.size(); ++c) {
    const double t = (static_cast<double>(c) + 0.5) / std::max(cluster_count, 1.0);
    const double sphere_y = 1.0 - 2.0 * t;  // y 均匀分层
    const double ring = std::sqrt(std::max(0.0, 1.0 - sphere_y * sphere_y));
    const double phi = static_cast<double>(c) * 2.39996322972865332;  // 黄金角
    cluster_anchor[c] = st::math::Vec3{static_cast<float>(std::cos(phi) * ring * spread),
                                       static_cast<float>(sphere_y * spread),
                                       static_cast<float>(std::sin(phi) * ring * spread)};
  }
  std::vector<std::size_t> node_cluster(count, 0);
  for (std::size_t i = 0; i < count; ++i) {
    node_cluster[i] = cluster_index[cluster_of[i]];
  }

  // 理想边长：节点越多图越大，边长也要跟着长（否则挤成一团）
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
        double distance_squared = static_cast<double>(delta.dot(delta));
        if (distance_squared < 1e-4) {
          // 重合：给一个确定的小偏移（用下标决定方向，保证可重复）
          const double angle = static_cast<double>(i * 31 + j * 17) * 0.017;
          delta = st::math::Vec3{static_cast<float>(std::cos(angle) * 0.01),
                                 static_cast<float>(std::sin(angle) * 0.01), 0.005F};
          distance_squared = 1e-4;
        }
        const double distance = std::sqrt(distance_squared);
        const double magnitude = options.repel * k * k / distance_squared;
        const st::math::Vec3 push = delta * static_cast<float>(magnitude / distance);
        force[i] = force[i] + push;
        force[j] = force[j] - push;
      }
    }

    // 引力（弹簧：沿边拉近）
    for (const auto& [a, b] : springs) {
      const st::math::Vec3 delta = position[a] - position[b];
      const double distance = std::max(1e-3, static_cast<double>(delta.length()));
      const double magnitude = options.attract * (distance * distance) / k;
      const st::math::Vec3 pull = delta * static_cast<float>(magnitude / distance);
      force[a] = force[a] - pull;
      force[b] = force[b] + pull;
    }

    // 中心聚拢（没有它，孤立节点会一路飞走）
    for (std::size_t i = 0; i < count; ++i) {
      force[i] = force[i] - position[i] * static_cast<float>(options.center_pull);
    }

    // 簇拉力：把同簇节点拉回自己那簇的锚点。
    //
    // 这是"连线横七竖八"的**根本修复**（册府前四轮都在调线条参数，治不了，
    // 因为问题在布局不在画法）。
    //
    // 为何是弹簧而不是硬约束：硬约束会让簇内没有布局空间（节点重叠），
    // 弹簧则保留"簇内还按真实关系微调"的能力。
    //
    // ## 只有一个簇时必须关掉（实测踩到）
    //
    // 簇力是**向心**的：一簇就只有一个锚点，所有节点都会被拉向那一点，
    // 斥力只能把它们推成一个很窄的球面壳。实测（册府：只有一册 8 条词条）
    // 截图上所有标签挤在画面中间一小块，周围大片空白。
    // 而“同簇聚成一团”这个目标在只有一簇时**本来就已达到**（全部在一起），
    // 再拉只会过度收缩——交给力导向自己撑开。
    if (options.cluster_pull > 0.0 && cluster_ids.size() > 1) {
      for (std::size_t i = 0; i < count; ++i) {
        const st::math::Vec3 to_anchor = cluster_anchor[node_cluster[i]] - position[i];
        force[i] = force[i] + to_anchor * static_cast<float>(options.cluster_pull);
      }
    }

    // 积分（带阻尼与单步上限）
    for (std::size_t i = 0; i < count; ++i) {
      velocity[i] = (velocity[i] + force[i]) * static_cast<float>(options.damping);
      st::math::Vec3 step = velocity[i] * static_cast<float>(heat);
      const double step_length = static_cast<double>(step.length());
      if (step_length > clamp_max) {
        step = step * static_cast<float>(clamp_max / step_length);
      }
      position[i] = position[i] + step;
    }
  }

  // 归一化到统一尺度：布局算法的绝对尺度没有物理意义，只有相对结构有意义。
  double max_radius = 1.0;
  for (const auto& p : position) {
    max_radius = std::max(max_radius, static_cast<double>(p.length()));
  }
  const double shrink = options.scale / max_radius;
  for (std::size_t i = 0; i < count; ++i) {
    graph.nodes[i].position = position[i] * static_cast<float>(shrink);
  }
}

auto bounding_radius(const Graph& graph) -> double {
  double radius = 1.0;
  for (const auto& node : graph.nodes) {
    radius = std::max(radius, static_cast<double>(node.position.length()));
  }
  return radius;
}

auto framing_distance(double radius, double fov_y_degrees, double margin) -> double {
  const double safe_radius = std::max(radius, 1e-3);
  const double safe_fov = std::min(std::max(fov_y_degrees, 1.0), 179.0);
  const double half = safe_fov * kPi / 360.0;
  return safe_radius / std::tan(half) * std::max(margin, 1.0);
}

auto visibility_stats(const std::vector<Projected>& projected) -> VisibilityStats {
  VisibilityStats stats;
  stats.total = projected.size();
  bool any = false;
  for (const auto& point : projected) {
    if (!point.visible) continue;
    ++stats.visible;
    if (!any) {
      stats.min_x = point.x;
      stats.max_x = point.x;
      stats.min_y = point.y;
      stats.max_y = point.y;
      any = true;
      continue;
    }
    stats.min_x = std::min(stats.min_x, point.x);
    stats.max_x = std::max(stats.max_x, point.x);
    stats.min_y = std::min(stats.min_y, point.y);
    stats.max_y = std::max(stats.max_y, point.y);
  }
  return stats;
}

// ── 相机 ──────────────────────────────────────────────────────────────────────

void Camera::orbit(double delta_yaw, double delta_pitch) {
  yaw += delta_yaw;
  pitch += delta_pitch;
  // 俯仰夹到 ±(π/2 − ε)：越过极点会让画面翻转（用户会瞬间失去方向感）
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

// ── 投影 ──────────────────────────────────────────────────────────────────────

auto project(const Graph& graph, const Camera& camera, const ProjectOptions& options)
    -> std::vector<Projected> {
  std::vector<Projected> out(graph.nodes.size());
  if (graph.nodes.empty()) return out;

  // 相机基向量（球坐标 → 笛卡尔）
  const double cos_yaw = std::cos(camera.yaw);
  const double sin_yaw = std::sin(camera.yaw);
  const double cos_pitch = std::cos(camera.pitch);
  const double sin_pitch = std::sin(camera.pitch);
  const st::math::Vec3 eye{static_cast<float>(camera.distance * cos_pitch * sin_yaw),
                           static_cast<float>(camera.distance * sin_pitch),
                           static_cast<float>(camera.distance * cos_pitch * cos_yaw)};
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
  const double center_x = options.viewport_x + options.viewport_width * 0.5;
  const double center_y = options.viewport_y + options.viewport_height * 0.5;

  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const st::math::Vec3 relative = graph.nodes[i].position - eye;
    const double depth = static_cast<double>(relative.dot(f));  // 相机前方为正
    Projected point;
    point.depth = depth;
    if (depth < options.near_plane) {
      // 近平面之后：不画。若不裁，投影会除出巨大坐标把整屏刷黑。
      point.visible = false;
      out[i] = point;
      continue;
    }
    const double local_x = static_cast<double>(relative.dot(right));
    const double local_y = static_cast<double>(relative.dot(up));
    point.x = center_x + local_x * focal / depth;
    point.y = center_y - local_y * focal / depth;  // 屏幕 y 向下
    point.radius = options.base_radius * graph.nodes[i].weight * focal / depth;
    point.visible = point.radius > 0.25;  // 太小的标记不值得画（也点不中）
    out[i] = point;
  }
  return out;
}

auto pick(const Graph& graph, const std::vector<Projected>& projected, double x, double y,
          double tolerance) -> std::size_t {
  std::size_t best = graph.nodes.size();  // == 未命中
  double best_score = 0.0;
  for (std::size_t i = 0; i < projected.size(); ++i) {
    const auto& point = projected[i];
    if (!point.visible) continue;
    const double dx = point.x - x;
    const double dy = point.y - y;
    const double distance = std::sqrt(dx * dx + dy * dy);
    const double hit_radius = point.radius + tolerance;
    if (distance > hit_radius) continue;
    // 命中多个时取"最近球心"（而不是最近的边）：重叠处点到的应是中心更近的那个
    const double score = hit_radius - distance;
    if (score > best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

auto pick_nearest(const Graph& graph, const Camera& camera, const ProjectOptions& options,
                  double x, double y, double tolerance) -> std::size_t {
  return pick(graph, project(graph, camera, options), x, y, tolerance);
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

// ── 颜色与字符串 ──────────────────────────────────────────────────────────────

auto argb_to_hex(std::uint32_t argb) -> std::string {
  return std::format("0x{:02x}{:02x}{:02x}", (argb >> 16U) & 0xFFU, (argb >> 8U) & 0xFFU,
                     argb & 0xFFU);
}

auto argb_scaled(std::uint32_t argb, double factor) -> std::uint32_t {
  const auto scale_channel = [factor](std::uint32_t channel) -> std::uint32_t {
    const double scaled = static_cast<double>(channel) * factor;
    const auto clamped = scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled);
    return static_cast<std::uint32_t>(clamped);
  };
  const std::uint32_t alpha = (argb >> 24U) & 0xFFU;
  const std::uint32_t red = scale_channel((argb >> 16U) & 0xFFU);
  const std::uint32_t green = scale_channel((argb >> 8U) & 0xFFU);
  const std::uint32_t blue = scale_channel(argb & 0xFFU);
  return (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
}

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

}  // namespace st::ui::graph
