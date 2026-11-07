#pragma once

/// @file cefu_graph.hpp
/// @brief 册府知识图谱：图构建 + 三维力导向布局 + 轨道相机 + 透视投影 + 拾取。
///
/// ## 为什么这一层不碰渲染
///
/// 三维图谱有两类完全不同的工作，混在一起就没法各自验证：
///
/// | 层 | 工作 | 能否单测 |
/// |---|---|---|
/// | **本文件** | 图构建、布局解算、相机、投影、拾取 | ✅ 纯函数，可穷举 |
/// | `graph3d` 组件 | 把投影结果画成球体/连线/文字 | 只能截图核验 |
///
/// 所以这一层零依赖 `st::ui`，只用 `cefu::Entry` 与 `st::math`——
/// 布局收敛、投影边界、拾取命中都能写断言，而不是"看着像对的"。
///
/// ## 为什么布局要预先算一次并缓存
///
/// 力导向是迭代解算（O(迭代 × 节点²)）。300 节点 × 300 迭代 ≈ 2700 万次距离计算，
/// 每帧算一次会直接卡死界面。所以：**布局算一次 → 缓存坐标 → 每帧只做投影与绘制**。
/// 词条集变化时才重算（由调用方按 `revision` 判断）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cefu_index.hpp"
#include "st/math/matrix.hpp"  // Vec3（三维向量；几何头只到二维 Point）

namespace cefu::graph {

/// 节点类型。**顺序即绘制层次**（小的先画、盖在下面更"远"）。
enum class NodeKind : std::uint8_t {
  Category = 0,  ///< 分类册（一个册一个节点）
  Person = 1,    ///< 负责人
  Scope = 2,     ///< 作用域（persona / chat）
  Entry = 3,     ///< 词条本体
};

/// 边的语义。颜色与线型按它区分。
enum class EdgeKind : std::uint8_t {
  BelongsTo = 0,  ///< 词条 → 所属分类
  Links = 1,      ///< 词条 → 词条（`links` 字段显式声明）
  Supersedes = 2, ///< 新词条 → 被它取代的旧词条
  OwnedBy = 3,    ///< 词条 → 负责人
  SharedFact = 4, ///< 两条词条共享同一条硬事实（同 IP/端口/阈值）——**隐含的关联**
  ScopedTo = 5,   ///< 词条 → 作用域
};

/// 图节点。
struct Node {
  std::string id;      ///< 唯一 id（`cat:运维` / `person:张三` / `entry:e_xxx`）
  std::string label;   ///< 显示标签（截断后的短标题）
  NodeKind kind{NodeKind::Entry};
  std::string entry_id;  ///< 若本节点代表词条，指向它（点击跳转用）
  std::string confidence;  ///< verified / inferred / stale（仅词条节点有意义）
  double weight{1.0};      ///< 影响球体半径（分类/人比词条大）

  /// 三维坐标（布局解算结果；单位约等于像素尺度）。
  st::math::Vec3 position{};
  /// 是否已参与布局（孤立节点会被放到远处，见 `layout`）。
  bool isolated{false};
};

/// 图边。
struct Edge {
  std::size_t from{0};  ///< 节点下标
  std::size_t to{0};
  EdgeKind kind{EdgeKind::BelongsTo};
};

/// 整张图（一次构建，供布局与渲染共用）。
struct Graph {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  /// 词条集指纹（标题+分类+更新时间），变了才需要重算布局。
  std::uint64_t revision{0};
  /// 统计（界面上直接显示，用于判断"图是不是太稀/太密"）。
  std::size_t entry_count{0};
  std::size_t category_count{0};
  std::size_t person_count{0};
  std::size_t isolated_entries{0};
  std::size_t largest_cluster{0};
};

/// 构建选项（阈值都在这里，不散落在实现里）。
struct BuildOptions {
  /// 标签最大字数（超出截断加省略号）——太长会把画面糊满。
  std::size_t max_label_chars{14};
  /// 共享硬事实建边的最小词条数：只有 1 条时"共享"没有意义。
  std::size_t min_shared_fact_degree{2};
  /// 是否把分类/人/作用域也建成节点（false = 只有词条，用于"最干净"档）。
  bool include_meta_nodes{true};
};

/// 从索引构建图。**纯派生**，不写库、不改索引。
[[nodiscard]] auto build(const std::vector<Entry>& entries, const BuildOptions& options = {})
    -> Graph;

/// 布局参数。
struct LayoutOptions {
  std::size_t iterations{220};    ///< 迭代轮数（多 = 更稳，线性变慢）
  double repel{1.0};              ///< 斥力强度
  double attract{0.06};           ///< 引力强度（沿边的弹簧）
  double center_pull{0.012};      ///< 向原点聚拢（防止整体飘散）
  double damping{0.86};           ///< 速度阻尼（小 = 更稳但收敛慢）
  double max_step{14.0};          ///< 单步位移上限（防早期爆炸）
  double scale{170.0};            ///< 布局完成后的整体缩放（贴合视野）
  /// 固定的随机种子：布局必须可重复，否则每次打开图形状都变（截图回归也会飘）。
  std::uint64_t seed{0x9E3779B97F4A7C15ULL};
};

/// 解算三维布局（原地写入 `node.position`）。确定性：同输入必得同输出。
void layout(Graph& graph, const LayoutOptions& options = {});

/// 轨道相机（球坐标）。
struct Camera {
  double yaw{0.6};      ///< 方位角（弧度）
  double pitch{0.42};   ///< 俯仰角（弧度，正值从上方看）
  double distance{620.0}; ///< 相机到原点距离
  double fov_degrees{52.0};

  /// 归位（回到默认视角）。
  void reset() { *this = Camera{}; }
  void orbit(double delta_yaw, double delta_pitch);
  void zoom(double factor);
};

/// 投影结果（每个节点一份）。
struct Projected {
  double x{0.0};          ///< 屏幕坐标（逻辑像素，已含视口偏移）
  double y{0.0};
  double depth{0.0};      ///< 相机空间 z（越大越远）
  double radius{0.0};     ///< 投影后的球体半径（透视缩放后）
  bool visible{false};    ///< 视锥/近平面裁剪结果
};

/// 投影参数。
struct ProjectOptions {
  double viewport_x{0.0};
  double viewport_y{0.0};
  double viewport_width{800.0};
  double viewport_height{600.0};
  /// 球体基准半径（未透视缩放前）。
  double base_radius{9.0};
  /// 近平面（小于它的点不画：投影会炸成巨大坐标）。
  double near_plane{24.0};
};

/// 把全部节点投到屏幕。返回与 `graph.nodes` 同序的数组。
[[nodiscard]] auto project(const Graph& graph, const Camera& camera, const ProjectOptions& options)
    -> std::vector<Projected>;

/// 拾取：屏幕点 → 最近的可见节点下标（没有命中返回 `nullopt` 语义的 `nodes.size()`）。
///
/// 只按**屏幕距离 vs 投影半径**判定，不做深度遮挡判断——图谱里**故意**允许
/// 点到被别的节点"挡住"的节点：用户看到的是标签，不是球体表面。
[[nodiscard]] auto pick(const Graph& graph, const std::vector<Projected>& projected, double x,
                        double y, double tolerance = 4.0) -> std::size_t;

/// 按深度从远到近排序的下标（画家算法：先画远的）。
[[nodiscard]] auto depth_order(const std::vector<Projected>& projected) -> std::vector<std::size_t>;

}  // namespace cefu::graph
