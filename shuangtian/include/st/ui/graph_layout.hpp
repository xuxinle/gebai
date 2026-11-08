#pragma once

/// @file graph_layout.hpp
/// @brief 三维知识图谱的**通用计算层**：图模型 + 力导向布局 + 轨道相机 + 透视投影 + 拾取。
///
/// ## 这一层为什么在框架里（而不是某个应用里）
///
/// 它最初是册府（知识库应用）的 `cefu_graph.*`。把领域剥离之后剩下来的东西
/// **一个领域符号都没有**：
///
/// | 组成 | 领域相关吗 |
/// |---|---|
/// | 节点/边/聚簇的数据形状 | 否——任何「一堆带关系的实体」都能填 |
/// | 力导向 + 分簇布局解算 | 否——纯数值迭代 |
/// | 轨道相机 / 透视投影 / 拾取 / 深度排序 | 否——纯几何 |
///
/// 领域相关的只有三件事，全部由调用方**填进数据**里，本层不认识它们：
/// ① 节点怎么分类（`Node::kind` 是自由字符串）；② 用什么颜色（`Node::argb`）；
/// ③ 谁和谁该聚成一簇（`Edge::clustering` + `Node::cluster_rank`）。
///
/// ## 为什么这一层不碰渲染（这条是它能被单测的前提）
///
/// 三维图谱有两类完全不同的工作，混在一起就没法各自验证：
///
/// | 层 | 工作 | 能否单测 |
/// |---|---|---|
/// | **本文件** | 图模型、布局解算、相机、投影、拾取、取景距离 | ✅ 纯函数，可穷举 |
/// | `components/graph_view.hpp` | 把结果画成三维连线 + 屏幕文字 | 只能截图核验 |
///
/// 所以本文件**零依赖 `st::ui`**，只用 `st::math`——布局收敛、投影边界、拾取命中
/// 都能写断言，而不是"看着像对的"。
///
/// ## 为什么布局要预先算一次并缓存
///
/// 力导向是迭代解算（O(迭代 × 节点²)）。300 节点 × 300 迭代 ≈ 2700 万次距离计算，
/// 每帧算一次会直接卡死界面。所以：**布局算一次 → 缓存坐标 → 每帧只做投影与绘制**。
/// 词条集变化时才重算（由调用方按 `Graph::revision` 判断）。
///
/// ## 确定性是硬要求（不是洁癖）
///
/// 布局必须**同输入必得同输出**：① 每次打开图形状都变会让人以为数据在变；
/// ② 截图回归会飘（同一份数据两次截图不同像素，回归无从判定）；
/// ③ 布局类缺陷（节点重叠、簇散开）无法复现就无法修。
/// 所以随机源是**固定种子的 xorshift64\***、初始撒点按下标定序、排序用 `stable_sort`。
///
/// ## 数据里为什么带颜色与权重（而不是让渲染层查表）
///
/// 因为"哪个节点重要、哪个还没确认"是**领域语义**，只有应用知道。放进数据意味着
/// 组件不需要任何领域知识；而查表式（组件按 `kind` 分支）会把领域知识搬进框架，
/// 每加一个领域就要改框架一次——那正是这一层要避免的耦合。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/matrix.hpp"  // Vec3（三维向量；几何头只到二维 Point）

namespace st::ui::graph {

// ── 图模型 ────────────────────────────────────────────────────────────────────

/// 图节点。
///
/// 字段分两类：**结构**（id/label/position，布局与绘制都要）与
/// **观感**（weight/argb/label_weight/plate*，只影响画出来什么样）。
/// 观感放这里的理由是「它是数据不是逻辑」——见文件头最后一段。
struct Node {
  /// 唯一 id（应用自己保证唯一；重复会让连线指向错误的位置）。
  std::string id;
  /// 显示标签（已按需要截断；本层不再加工——截断长度依赖字体与视野，只有组件知道）。
  std::string label;
  /// 节点类型（**自由字符串**）。本层只用它做诊断计数，不解释语义。
  std::string kind;
  /// 类型排名：多个聚簇归属时取**排名最高**的那个（数值大者优先）。
  ///
  /// 为什么需要：一个实体常有多个归属（例如「这条知识属于某册 / 某负责人 / 某作用域」），
  /// 全归则在图上会被拉成三角张力，簇内反而挤。取一个主归属（读的人按它找东西）
  /// 才能得到「同册聚成一团」的形状——这是 Obsidian / Gephi 分组视图的通行做法。
  double cluster_rank{0.0};
  /// 影响三维节点标记的尺寸（枢纽比叶子大）。
  double weight{1.0};

  /// 三维坐标（布局解算结果；单位约等于像素尺度）。
  st::math::Vec3 position{};
  /// 是否孤立（没有任何边）。由调用方在 `build` 时填；布局会把它放到远处。
  bool isolated{false};

  // —— 观感（可选；缺省值保证"不填也画得出来"）——
  /// 主色（ARGB）。文字、圆点、三维标记都用它。
  std::uint32_t argb{0xFFD8DEE9U};
  /// 标签重要度（0~1）：决定字号与避让优先级（重要的先被排下、被避让的是次要的）。
  float label_weight{0.35F};
  /// 是否给标签画底板（深底 + 连线的画面上，无底板的字会被切碎；实测必须默认开）。
  bool label_plate{true};
  /// 底板浓度（0~255）：次要节点的底板更淡，让它在视觉上退后一层。
  unsigned char plate_alpha{0xE6};
};

/// 图边。
struct Edge {
  /// 两端节点下标（越界边被布局与绘制**跳过**，不报错——图是派生物，坏边不该带走进程）。
  std::size_t from{0};
  std::size_t to{0};
  /// 边的语义名（自由字符串；本层只用它做诊断计数）。
  std::string kind;
  /// 颜色（ARGB）。
  std::uint32_t argb{0xFF8593A5U};
  /// 线宽相对系数（1.0 = 标准；推断出的隐含关联用更细的线）。
  double width_scale{1.0};
  /// 这条边是否**表达归属**（参与聚簇推定）。
  ///
  /// 为什么是边上的标志、而不是让本层认 `kind` 的字符串：归属关系是**领域语义**
  /// （册府叫 `belongs_to`/`owned_by`/`scoped_to`，别的应用可能叫 `part_of`）。
  /// 由调用方声明"这条边算归属"，本层就永远不会因为领域改名而失效。
  bool clustering{false};
};

/// 整张图（一次构建，供布局与渲染共用）。
struct Graph {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  /// 数据指纹：变了才需要重算布局（布局缓存的判据）。
  std::uint64_t revision{0};
  /// 孤立节点数（界面直接显示，用于判断"图是不是太稀"）。
  std::size_t isolated_count{0};
  /// 最大连通分量规模（"知识聚成几团"的真实答案）。
  std::size_t largest_cluster{0};

  /// 某类节点的个数（诊断：界面上直接显示「N 条词条 / M 个分类」）。
  [[nodiscard]] auto count_kind(std::string_view kind) const -> std::size_t;
};

/// 补全图的派生统计：`revision`（数据指纹）、`isolated_count`、`largest_cluster`。
///
/// 为什么单独一个函数而不是塞进 `layout`：布局可能被缓存（只算一次），而这些统计
/// 每次重建图都要更新——两者生命周期不同，混在一起必然出现「改了数据但界面上
/// 的孤立节点数还是旧的」。
void finalize(Graph& graph);

// ── 布局 ──────────────────────────────────────────────────────────────────────

/// 布局参数。
struct LayoutOptions {
  std::size_t iterations{220};    ///< 迭代轮数（多 = 更稳，线性变慢）
  double repel{1.0};              ///< 斥力强度
  double attract{0.06};           ///< 引力强度（沿边的弹簧）
  double center_pull{0.012};      ///< 向原点聚拢（防止整体飘散）
  double damping{0.86};           ///< 速度阻尼（小 = 更稳但收敛慢）
  double max_step{14.0};          ///< 单步位移上限（防早期爆炸）
  double scale{170.0};            ///< 布局完成后的整体缩放（贴合视野）

  /// 分簇聚簇的强度（0 = 关，即纯力导向）。
  ///
  /// ## 为何需要它（治本，而不是调线条参数）
  ///
  /// 纯力导向里节点只被「真实关系」约束，于是同组的实体也可能被斥力推到对面。
  /// 三维投影后连线横七竖八——那是**布局**的后果，不是线条画法的错
  /// （册府为此调过四轮线条参数，都没治好）。
  ///
  /// 加簇力之后：同一簇的节点被拉向该簇的锚点，簇之间再互斥。
  /// 于是「同簇聚成一团」，连线变成簇内短段 + 簇间少数跨线。
  double cluster_pull{0.055};
  /// 各簇锚点之间的分布半径（相对 `scale`）——控制「分得多开」。
  ///
  /// 从 0.62 提到 0.92：实测 0.62 时簇内文字仍然拥挤，避让算法把标签推开几十像素，
  /// 造成"字离点很远"的观感。铺开一点后，簇内能用「贴着圆点」的默认位置排下。
  double cluster_spread{0.92};

  /// 固定的随机种子：布局必须可重复，否则每次打开图形状都变（截图回归也会飘）。
  std::uint64_t seed{0x9E3779B97F4A7C15ULL};
};

/// 解算三维布局（原地写入 `node.position`）。确定性：同输入必得同输出。
void layout(Graph& graph, const LayoutOptions& options = {});

/// 包围球半径（布局坐标的最大模长；渲染取景与线宽都按它换算）。
[[nodiscard]] auto bounding_radius(const Graph& graph) -> double;

/// 把整张图恰好装进视野的相机距离。
///
/// 为什么框架要给这个函数：`ThreeView::frame_scene()` 报的是**物理像素**尺寸，
/// 而取景公式按逻辑尺寸算，DPI 缩放会被多算一次——实测册府包围半径 183 的场景
/// 被算出距离 600（合理值约 200），画面只占中间一小块。这里按包围球与 fov
/// 直接算，不经过渲染层的尺寸语义，因此与 DPI 无关。
///
/// `margin` 留白系数（1.12 约等于"四周各留 6%"）。
[[nodiscard]] auto framing_distance(double radius, double fov_y_degrees, double margin = 1.12)
    -> double;

/// 一帧内的可见性统计（诊断：截图里"一片背景色"时用它区分是相机错还是没几何）。
struct VisibilityStats {
  std::size_t total{0};
  std::size_t visible{0};
  /// 屏幕包围盒（逻辑像素；`visible == 0` 时为空）。
  double min_x{0.0};
  double max_x{0.0};
  double min_y{0.0};
  double max_y{0.0};
};

/// 由投影结果算一帧的可见性统计。
[[nodiscard]] auto visibility_stats(const std::vector<struct Projected>& projected)
    -> VisibilityStats;

// ── 相机 ──────────────────────────────────────────────────────────────────────

/// 轨道相机（球坐标）。
///
/// 为什么用球坐标而不是直接给 `eye`：直接改位置会积累数值漂移（拖一千次后距离
/// 会缓慢变化），而球坐标天然把"绕目标转"和"靠近/远离"分开，且俯仰可以夹住
/// （否则会翻过头变成倒置视角——那种状态用户无法自行恢复）。
struct Camera {
  double yaw{0.6};        ///< 方位角（弧度）
  double pitch{0.42};     ///< 俯仰角（弧度，正值从上方看）
  double distance{620.0}; ///< 相机到原点距离
  double fov_degrees{52.0};

  /// 归位（回到默认视角）。
  void reset() { *this = Camera{}; }
  /// 绕目标点旋转（俯仰夹在 ±(π/2 − ε)，偏航归一到 [−π, π]）。
  void orbit(double delta_yaw, double delta_pitch);
  /// 推拉（距离夹取：太近会穿进图内部，太远图变成一个点）。
  void zoom(double factor);
};

// ── 投影与拾取 ────────────────────────────────────────────────────────────────

/// 投影结果（每个节点一份）。
struct Projected {
  double x{0.0};      ///< 屏幕坐标（逻辑像素，已含视口偏移）
  double y{0.0};
  double depth{0.0};  ///< 相机空间 z（越大越远）
  double radius{0.0}; ///< 投影后的标记半径（透视缩放后）
  bool visible{false};///< 视锥/近平面裁剪结果
};

/// 投影参数。
struct ProjectOptions {
  double viewport_x{0.0};
  double viewport_y{0.0};
  double viewport_width{800.0};
  double viewport_height{600.0};
  /// 标记基准半径（未透视缩放前）。
  double base_radius{9.0};
  /// 近平面（小于它的点不画：投影会炸成巨大坐标）。
  double near_plane{24.0};
};

/// 把全部节点投到屏幕。返回与 `graph.nodes` 同序的数组。
[[nodiscard]] auto project(const Graph& graph, const Camera& camera, const ProjectOptions& options)
    -> std::vector<Projected>;

/// 拾取：屏幕点 → 最近的可见节点下标（未命中返回 `graph.nodes.size()`）。
///
/// 只按**屏幕距离 vs 投影半径**判定，不做深度遮挡判断——图谱里**故意**允许
/// 点到被别的节点"挡住"的节点：用户看到的是标签，不是标记表面。
[[nodiscard]] auto pick(const Graph& graph, const std::vector<Projected>& projected, double x,
                        double y, double tolerance = 4.0) -> std::size_t;

/// 拾取：屏幕点 → 节点下标（未命中返回 `nullopt` 语义的空 `optional` 不便内联，
/// 故提供这个便捷包装；找不到时返回 `graph.nodes.size()`）。
[[nodiscard]] auto pick_nearest(const Graph& graph, const Camera& camera,
                                const ProjectOptions& options, double x, double y,
                                double tolerance = 4.0) -> std::size_t;

/// 按深度从远到近排序的下标（画家算法：先画远的）。
[[nodiscard]] auto depth_order(const std::vector<Projected>& projected)
    -> std::vector<std::size_t>;

// ── 颜色与字符串小工具（纯函数，供组件与调用方复用） ─────────────────────────

/// ARGB → three.js 的 `0xrrggbb` 字面量（**丢弃 alpha**：三维材质的不透明由材质决定，
/// 把 alpha 混进颜色会在不支持 alpha 的材质上变成全透明或全亮）。
[[nodiscard]] auto argb_to_hex(std::uint32_t argb) -> std::string;

/// 按比例调暗/调亮（`factor < 1` 变暗；用于"远处自然变暗"这类深度线索）。
[[nodiscard]] auto argb_scaled(std::uint32_t argb, double factor) -> std::uint32_t;

/// 按 UTF-8 **码点**截断（按字节会切出半个汉字）。
///
/// 为什么放在这儿而不是各应用各写一份：它是显示标签的通用需要，且"按字节切"
/// 这个错法在中文语料上必犯——收敛到一处才有唯一正确实现。
[[nodiscard]] auto truncate_label(std::string_view text, std::size_t max_chars) -> std::string;

}  // namespace st::ui::graph
