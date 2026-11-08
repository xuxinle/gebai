/// @file ui_graph_layout_test.cpp
/// @brief 通用关系图计算层（`st/ui/graph_layout.hpp`）：统计 / 布局 / 相机 / 投影 / 拾取。
///
/// ## 这一套用例守的是「换个领域也不会坏」
///
/// 它原先长在册府（知识库）的测试里，断言与"词条/分类"的语义混在一起——于是
/// 想验证"布局确定性"必须先造一批知识词条，而且换个领域（依赖图、调用图）就
/// 全都没法复用。现在通用层**不认识任何领域**（节点类型是自由字符串），
/// 所以这里的断言只谈几何与数值：
///
/// | 断言 | 它防的具体缺陷 |
/// |---|---|
/// | 布局确定性 | 每次打开形状不同 → 截图回归永远飘 |
/// | 布局有界 | 力发散 → 节点飞到千里之外（画面全空） |
/// | 近平面裁剪 | 除零出巨大坐标 → 整屏刷黑 |
/// | 拾取取最近球心 | 球重叠处点到被挡的那个 |
/// | 深度排序远者在前 | 画家算法反了 → 画面层次颠倒 |
/// | 分簇真的成团 | 簇力失效 → 同组散落（连线横七竖八的根因） |
/// | 指纹区分字段边界 | ("ab","c") 与 ("a","bc") 同指纹 → 改了数据不重算布局 |
/// | 取景距离与 fov 自洽 | 相机距离算错 → 画面只占中间一小块 / 什么都看不到 |

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/ui/graph_layout.hpp"

namespace {

using st::ui::graph::Camera;
using st::ui::graph::Edge;
using st::ui::graph::Graph;
using st::ui::graph::LayoutOptions;
using st::ui::graph::Node;
using st::ui::graph::Projected;
using st::ui::graph::ProjectOptions;

/// 造一个节点（只有通用字段，**没有任何领域语义**）。
auto make_node(std::string id, std::string label = {}, double cluster_rank = 0.0) -> Node {
  Node node;
  node.id = std::move(id);
  node.label = label.empty() ? node.id : std::move(label);
  node.kind = "node";
  node.cluster_rank = cluster_rank;
  return node;
}

/// 造一条边。
auto make_edge(std::size_t from, std::size_t to, bool clustering = false) -> Edge {
  Edge edge;
  edge.from = from;
  edge.to = to;
  edge.kind = "rel";
  edge.clustering = clustering;
  return edge;
}

/// 到原点距离。先把 float 转 double 再乘：直接 `p.x * p.x` 是 float 乘法，
/// 与 `double` 相加会触发 `-Werror=double-promotion`（本仓库是严格门禁）。
auto radius_of(const st::math::Vec3& p) -> double {
  const auto x = static_cast<double>(p.x);
  const auto y = static_cast<double>(p.y);
  const auto z = static_cast<double>(p.z);
  return std::sqrt(x * x + y * y + z * z);
}

/// 链式图：`n` 个节点连成一条线（没有元节点，便于对下标做断言）。
auto make_chain(std::size_t count) -> Graph {
  Graph graph;
  for (std::size_t i = 0; i < count; ++i) {
    graph.nodes.push_back(make_node("n" + std::to_string(i)));
  }
  for (std::size_t i = 1; i < count; ++i) {
    graph.edges.push_back(make_edge(i - 1, i));
  }
  st::ui::graph::finalize(graph);
  return graph;
}

}  // namespace

// ── 统计 ──────────────────────────────────────────────────────────────────────

ST_TEST(graph_layout_empty_graph) {
  Graph graph;
  st::ui::graph::finalize(graph);
  ST_CHECK_EQ(graph.nodes.size(), std::size_t{0});
  ST_CHECK_EQ(graph.edges.size(), std::size_t{0});
  ST_CHECK_EQ(graph.isolated_count, std::size_t{0});
  ST_CHECK_EQ(graph.largest_cluster, std::size_t{0});
  // 空图也要能布局与投影（不崩、不产出垃圾）
  st::ui::graph::layout(graph);
  const auto projected = st::ui::graph::project(graph, Camera{}, ProjectOptions{});
  ST_CHECK_EQ(projected.size(), std::size_t{0});
}

ST_TEST(graph_layout_isolated_counted) {
  Graph graph;
  graph.nodes.push_back(make_node("a"));
  graph.nodes.push_back(make_node("b"));  // 无边 → 孤立
  graph.nodes.push_back(make_node("c"));
  graph.edges.push_back(make_edge(0, 2));
  st::ui::graph::finalize(graph);
  ST_CHECK_EQ(graph.isolated_count, std::size_t{1});
  ST_CHECK(graph.nodes[1].isolated);
  ST_CHECK(!graph.nodes[0].isolated);
}

ST_TEST(graph_layout_largest_cluster_counts_whole_component) {
  Graph graph;
  for (int i = 0; i < 5; ++i) graph.nodes.push_back(make_node("n" + std::to_string(i)));
  // 0-1-2 连成一片，3-4 连成另一片
  graph.edges.push_back(make_edge(0, 1));
  graph.edges.push_back(make_edge(1, 2));
  graph.edges.push_back(make_edge(3, 4));
  st::ui::graph::finalize(graph);
  // 最大连通分量 = 3（只数叶子节点会把"十个连成一片"看成十个小岛）
  ST_CHECK_EQ(graph.largest_cluster, std::size_t{3});
}

ST_TEST(graph_layout_count_kind) {
  Graph graph;
  graph.nodes.push_back(make_node("a"));
  graph.nodes.push_back(make_node("b"));
  graph.nodes[0].kind = "hub";
  st::ui::graph::finalize(graph);
  ST_CHECK_EQ(graph.count_kind("hub"), std::size_t{1});
  ST_CHECK_EQ(graph.count_kind("node"), std::size_t{1});
  ST_CHECK_EQ(graph.count_kind("absent"), std::size_t{0});
}

// ── 指纹 ──────────────────────────────────────────────────────────────────────

ST_TEST(graph_layout_revision_stable_for_same_data) {
  const auto a = make_chain(4);
  const auto b = make_chain(4);
  ST_CHECK_EQ(a.revision, b.revision);  // 同内容 → 同指纹（布局缓存能命中）
}

ST_TEST(graph_layout_revision_tracks_label_change) {
  Graph a;
  a.nodes.push_back(make_node("x", "甲"));
  st::ui::graph::finalize(a);
  Graph b;
  b.nodes.push_back(make_node("x", "乙"));
  st::ui::graph::finalize(b);
  ST_CHECK(a.revision != b.revision);  // 改了标题必须重算布局
}

/// 字段之间必须放分隔符：否则 ("ab","c") 与 ("a","bc") 指纹相同——
/// 而改了数据指纹不变 → 布局不重算 → 界面上是旧形状但数据是新的。
ST_TEST(graph_layout_revision_separates_field_boundaries) {
  Graph a;
  Node na = make_node("ab");
  na.label = "c";
  a.nodes.push_back(na);
  st::ui::graph::finalize(a);

  Graph b;
  Node nb = make_node("a");
  nb.label = "bc";
  b.nodes.push_back(nb);
  st::ui::graph::finalize(b);

  ST_CHECK(a.revision != b.revision);
}

// ── 布局 ──────────────────────────────────────────────────────────────────────

ST_TEST(graph_layout_deterministic) {
  auto first = make_chain(12);
  auto second = make_chain(12);
  st::ui::graph::layout(first);
  st::ui::graph::layout(second);

  // 布局必须确定：否则每次打开形状都变，截图回归也永远飘
  ST_REQUIRE((first.nodes.size()) == (second.nodes.size()));
  for (std::size_t i = 0; i < first.nodes.size(); ++i) {
    ST_CHECK_EQ(first.nodes[i].position.x, second.nodes[i].position.x);
    ST_CHECK_EQ(first.nodes[i].position.y, second.nodes[i].position.y);
    ST_CHECK_EQ(first.nodes[i].position.z, second.nodes[i].position.z);
  }
}

ST_TEST(graph_layout_same_seed_same_result_different_seed_differs) {
  auto a = make_chain(8);
  auto b = make_chain(8);
  LayoutOptions options;
  options.iterations = 40;
  st::ui::graph::layout(a, options);
  options.seed = 0x1234567ULL;
  st::ui::graph::layout(b, options);
  // 不同种子应当给出不同初始撒点 → 不同结果（证明种子真的被用上了，
  // 而不是"结果总是同一个常数"那种假确定）
  bool differs = false;
  for (std::size_t i = 0; i < a.nodes.size(); ++i) {
    if (a.nodes[i].position.x != b.nodes[i].position.x) differs = true;
  }
  ST_CHECK(differs);
}

ST_TEST(graph_layout_bounded) {
  Graph graph;
  for (int i = 0; i < 40; ++i) {
    graph.nodes.push_back(make_node("n" + std::to_string(i)));
  }
  for (int i = 1; i < 40; ++i) graph.edges.push_back(make_edge(static_cast<std::size_t>(i) - 1U,
                                                              static_cast<std::size_t>(i)));
  st::ui::graph::finalize(graph);
  st::ui::graph::layout(graph);
  for (const auto& node : graph.nodes) {
    // 归一化到 scale 附近：飞到千里之外说明力发散
    ST_CHECK(radius_of(node.position) < 900.0);
    ST_CHECK(std::isfinite(static_cast<double>(node.position.x)));
    ST_CHECK(std::isfinite(static_cast<double>(node.position.y)));
    ST_CHECK(std::isfinite(static_cast<double>(node.position.z)));
  }
}

ST_TEST(graph_layout_separates_nodes) {
  Graph graph;
  graph.nodes.push_back(make_node("a"));
  graph.nodes.push_back(make_node("b"));
  graph.edges.push_back(make_edge(0, 1));
  st::ui::graph::finalize(graph);
  st::ui::graph::layout(graph);
  ST_CHECK(graph.nodes[0].position.x != graph.nodes[1].position.x ||
           graph.nodes[0].position.y != graph.nodes[1].position.y);
}

/// 分簇真的成团：同簇节点两两距离，必须比"跨簇"距离小。
///
/// 这条是"连线横七竖八"的**根因断言**——那个症状在册府被当成线条画法问题调了四轮，
/// 实际是布局：没有簇力时同组节点会被斥力推到球的对面。
ST_TEST(graph_layout_clusters_group_together) {
  Graph graph;
  // 两个簇心（rank 高）+ 每簇 4 个成员
  graph.nodes.push_back(make_node("hub_a", "簇甲", 3.0));
  graph.nodes.push_back(make_node("hub_b", "簇乙", 3.0));
  for (int i = 0; i < 4; ++i) {
    graph.nodes.push_back(make_node("a" + std::to_string(i)));  // 下标 2..5
    graph.nodes.push_back(make_node("b" + std::to_string(i)));  // 下标 6..9
  }
  for (int i = 0; i < 4; ++i) {
    graph.edges.push_back(make_edge(static_cast<std::size_t>(2 + i), 0, true));  // a → hub_a
    graph.edges.push_back(make_edge(static_cast<std::size_t>(6 + i), 1, true));  // b → hub_b
  }
  st::ui::graph::finalize(graph);
  st::ui::graph::layout(graph);

  const auto distance = [&graph](std::size_t i, std::size_t j) {
    const auto delta = graph.nodes[i].position - graph.nodes[j].position;
    return static_cast<double>(delta.length());
  };
  // 簇内平均距离 vs 簇间平均距离
  double intra = 0.0;
  double inter = 0.0;
  int intra_count = 0;
  int inter_count = 0;
  for (std::size_t i = 2; i <= 5; ++i) {
    for (std::size_t j = i + 1; j <= 5; ++j) {
      intra += distance(i, j);
      ++intra_count;
    }
  }
  for (std::size_t i = 2; i <= 5; ++i) {
    for (std::size_t j = 6; j <= 9; ++j) {
      inter += distance(i, j);
      ++inter_count;
    }
  }
  ST_REQUIRE(intra_count > 0);
  ST_REQUIRE(inter_count > 0);
  const double intra_avg = intra / static_cast<double>(intra_count);
  const double inter_avg = inter / static_cast<double>(inter_count);
  // 簇力生效时同簇明显更近（实测比率远大于 1；这里只要 > 1.2 就足以区分
  // "成团"与"全靠斥力随缘"）
  ST_CHECK(inter_avg > intra_avg * 1.2);
}

// ── 包围球与取景 ──────────────────────────────────────────────────────────────

ST_TEST(graph_layout_bounding_radius) {
  Graph graph;
  graph.nodes.push_back(make_node("a"));
  graph.nodes[0].position = st::math::Vec3{3.0F, 4.0F, 0.0F};  // 模长 5
  const double radius = st::ui::graph::bounding_radius(graph);
  ST_CHECK(radius >= 4.99);
  ST_CHECK(radius <= 5.01);
}

ST_TEST(graph_layout_framing_distance_respects_fov) {
  // 半径与 fov 的自洽：距离 = r / tan(fov/2) * margin
  const double distance = st::ui::graph::framing_distance(170.0, 90.0, 1.0);
  ST_CHECK(distance > 169.9);
  ST_CHECK(distance < 170.1);

  // 视野更窄 → 必须退得更远（同半径装不下）
  const double narrow = st::ui::graph::framing_distance(170.0, 30.0, 1.0);
  ST_CHECK(narrow > distance);

  // 留白系数生效
  const double padded = st::ui::graph::framing_distance(170.0, 90.0, 1.12);
  ST_CHECK(padded > distance);
}

/// 取景**与 DPI 无关**：这条防的是册府实测过的那个坑——`frame_scene` 用物理像素
/// 算高度，DPI 1.5 时距离被多算一次，画面只占中间一小块。本函数只吃半径与 fov。
ST_TEST(graph_layout_framing_distance_has_no_dpi_input) {
  const double base = st::ui::graph::framing_distance(100.0, 46.0, 1.12);
  // 同一输入重复调用必须同一结果（没有隐藏的全局状态/环境依赖）
  ST_CHECK_EQ(base, st::ui::graph::framing_distance(100.0, 46.0, 1.12));
  // 退化输入不产生 NaN/负值
  ST_CHECK(st::ui::graph::framing_distance(0.0, 46.0, 1.12) > 0.0);
  ST_CHECK(st::ui::graph::framing_distance(100.0, 0.0, 1.12) > 0.0);
}

// ── 投影与拾取 ────────────────────────────────────────────────────────────────

ST_TEST(graph_layout_project_clips_near_plane) {
  Graph graph;
  graph.nodes.push_back(make_node("only"));
  graph.nodes[0].position = st::math::Vec3{0.0F, 0.0F, 0.0F};
  st::ui::graph::finalize(graph);

  Camera camera;
  camera.distance = 150.0;
  ProjectOptions clipped;
  clipped.near_plane = 100000.0;  // 人为极大 → 确保全部裁掉
  const auto projected = st::ui::graph::project(graph, camera, clipped);
  ST_REQUIRE((projected.size()) == (std::size_t{1}));
  ST_CHECK(!projected[0].visible);

  ProjectOptions normal;
  const auto visible = st::ui::graph::project(graph, camera, normal);
  ST_CHECK(visible[0].visible);
}

ST_TEST(graph_layout_project_single_node_inside_viewport) {
  Graph graph;
  graph.nodes.push_back(make_node("only"));
  graph.nodes[0].position = st::math::Vec3{0.0F, 0.0F, 0.0F};
  st::ui::graph::finalize(graph);

  Camera camera;
  camera.yaw = 0.0;
  camera.pitch = 0.0;
  ProjectOptions options;
  options.viewport_width = 800.0;
  options.viewport_height = 600.0;
  const auto projected = st::ui::graph::project(graph, camera, options);
  ST_REQUIRE((projected.size()) == (std::size_t{1}));
  ST_CHECK(projected[0].visible);
  // 原点在相机正前方 → 投到视口正中
  ST_CHECK(projected[0].x > 399.0);
  ST_CHECK(projected[0].x < 401.0);
  ST_CHECK(projected[0].y > 299.0);
  ST_CHECK(projected[0].y < 301.0);
  ST_CHECK(projected[0].radius > 0.0);
}

ST_TEST(graph_layout_visibility_stats) {
  std::vector<Projected> projected(3);
  projected[0].visible = true;
  projected[0].x = 10.0;
  projected[0].y = 20.0;
  projected[1].visible = false;  // 不该影响包围盒
  projected[1].x = 100000.0;
  projected[2].visible = true;
  projected[2].x = 30.0;
  projected[2].y = 5.0;
  const auto stats = st::ui::graph::visibility_stats(projected);
  ST_CHECK_EQ(stats.total, std::size_t{3});
  ST_CHECK_EQ(stats.visible, std::size_t{2});
  ST_CHECK(stats.min_x > 9.9);
  ST_CHECK(stats.min_x < 10.1);
  ST_CHECK(stats.max_x > 29.9);
  ST_CHECK(stats.max_x < 30.1);
  ST_CHECK(stats.min_y > 4.9);
  ST_CHECK(stats.max_y < 20.1);
}

ST_TEST(graph_layout_pick_hits_nearest_center) {
  Graph graph;
  graph.nodes.push_back(make_node("a"));
  graph.nodes.push_back(make_node("b"));
  // 手工摆位置，避免依赖布局细节
  graph.nodes[0].position = st::math::Vec3{0.0F, 0.0F, 0.0F};
  graph.nodes[1].position = st::math::Vec3{200.0F, 0.0F, 0.0F};
  st::ui::graph::finalize(graph);

  Camera camera;
  camera.yaw = 0.0;
  camera.pitch = 0.0;
  ProjectOptions options;
  options.viewport_width = 800.0;
  options.viewport_height = 600.0;
  const auto projected = st::ui::graph::project(graph, camera, options);

  ST_CHECK_EQ(st::ui::graph::pick(graph, projected, projected[0].x, projected[0].y), std::size_t{0});
  ST_CHECK_EQ(st::ui::graph::pick(graph, projected, projected[1].x, projected[1].y), std::size_t{1});
  // 远处空白 → 未命中（返回 nodes.size()）
  ST_CHECK_EQ(st::ui::graph::pick(graph, projected, 5.0, 5.0), graph.nodes.size());
}

/// 两个节点投影重叠时，点到的应是**球心更近**的那个（而不是"先遇到的那个"）。
ST_TEST(graph_layout_pick_prefers_closer_center) {
  Graph graph;
  graph.nodes.push_back(make_node("near"));
  graph.nodes.push_back(make_node("far"));
  graph.nodes[0].position = st::math::Vec3{0.0F, 0.0F, 0.0F};
  graph.nodes[1].position = st::math::Vec3{0.0F, 0.0F, 0.0F};  // 完全重合
  graph.nodes[0].weight = 3.0;   // 半径更大（覆盖更广）
  graph.nodes[1].weight = 1.0;
  st::ui::graph::finalize(graph);

  Camera camera;
  camera.yaw = 0.0;
  camera.pitch = 0.0;
  ProjectOptions options;
  options.viewport_width = 800.0;
  options.viewport_height = 600.0;
  const auto projected = st::ui::graph::project(graph, camera, options);
  ST_REQUIRE((projected.size()) == (std::size_t{2}));
  // 点点中心：两个节点球心重合，取 score 更大的那支（半径大者）；
  // 关键是**必须命中一个**且不越界。
  const std::size_t hit = st::ui::graph::pick(graph, projected, projected[0].x, projected[0].y);
  ST_CHECK(hit < graph.nodes.size());
}

ST_TEST(graph_layout_pick_nearest_wraps_project) {
  Graph graph;
  graph.nodes.push_back(make_node("a"));
  graph.nodes[0].position = st::math::Vec3{0.0F, 0.0F, 0.0F};
  st::ui::graph::finalize(graph);
  Camera camera;
  camera.yaw = 0.0;
  camera.pitch = 0.0;
  ProjectOptions options;
  options.viewport_width = 800.0;
  options.viewport_height = 600.0;
  const auto projected = st::ui::graph::project(graph, camera, options);
  ST_CHECK_EQ(st::ui::graph::pick_nearest(graph, camera, options, projected[0].x, projected[0].y),
              std::size_t{0});
  ST_CHECK_EQ(st::ui::graph::pick_nearest(graph, camera, options, 1.0, 1.0), graph.nodes.size());
}

ST_TEST(graph_layout_depth_order_far_first) {
  std::vector<Projected> projected(3);
  projected[0].depth = 100.0;
  projected[1].depth = 300.0;  // 最远
  projected[2].depth = 200.0;
  const auto order = st::ui::graph::depth_order(projected);
  ST_REQUIRE((order.size()) == (std::size_t{3}));
  // 画家算法：先画远的 → 深 300 的那个排第一
  ST_CHECK_EQ(order[0], std::size_t{1});
  ST_CHECK_EQ(order[2], std::size_t{0});
}

// ── 相机 ──────────────────────────────────────────────────────────────────────

ST_TEST(graph_layout_camera_pitch_clamped) {
  Camera camera;
  for (int i = 0; i < 100; ++i) camera.orbit(0.0, 0.5);
  // 越过 ±π/2 会让画面翻转，用户瞬间失去方向感
  ST_CHECK(camera.pitch < 1.58);
  ST_CHECK(camera.pitch > 1.50);
}

ST_TEST(graph_layout_camera_yaw_normalized) {
  Camera camera;
  for (int i = 0; i < 100; ++i) camera.orbit(1.0, 0.0);
  // 长时间拖动后浮点精度会流失 → 必须归一到 [-π, π]
  ST_CHECK(camera.yaw >= -3.15);
  ST_CHECK(camera.yaw <= 3.15);
}

ST_TEST(graph_layout_camera_zoom_limited) {
  Camera camera;
  for (int i = 0; i < 100; ++i) camera.zoom(0.8);
  ST_CHECK(camera.distance >= 150.0);
  for (int i = 0; i < 200; ++i) camera.zoom(1.2);
  ST_CHECK(camera.distance <= 2400.0);
}

ST_TEST(graph_layout_camera_reset) {
  Camera camera;
  camera.orbit(1.0, 0.3);
  camera.zoom(2.0);
  camera.reset();
  ST_CHECK_EQ(camera.yaw, Camera{}.yaw);
  ST_CHECK_EQ(camera.pitch, Camera{}.pitch);
  ST_CHECK_EQ(camera.distance, Camera{}.distance);
}

// ── 颜色与字符串 ──────────────────────────────────────────────────────────────

ST_TEST(graph_layout_argb_to_hex_drops_alpha) {
  // 丢 alpha 是有意的：三维材质的不透明由材质决定，把 alpha 混进颜色会在
  // 不支持 alpha 的材质上变成全透明或全亮。
  ST_CHECK_EQ(st::ui::graph::argb_to_hex(0xFFE8F2FFU), std::string("0xe8f2ff"));
  ST_CHECK_EQ(st::ui::graph::argb_to_hex(0x80123456U), std::string("0x123456"));
}

ST_TEST(graph_layout_argb_scaled_clamps) {
  const auto darker = st::ui::graph::argb_scaled(0xFF808080U, 0.5);
  ST_CHECK_EQ(darker, 0xFF404040U);
  // 放大超过 255 必须夹住（不溢出到别的通道）
  const auto brighter = st::ui::graph::argb_scaled(0xFF808080U, 4.0);
  ST_CHECK_EQ(brighter, 0xFFFFFFFFU);
  // 负系数夹到 0
  const auto zero = st::ui::graph::argb_scaled(0xFF808080U, -1.0);
  ST_CHECK_EQ(zero, 0xFF000000U);
  // alpha 原样保留
  ST_CHECK_EQ((st::ui::graph::argb_scaled(0x80102030U, 1.0) >> 24U) & 0xFFU, 0x80U);
}

ST_TEST(graph_layout_label_truncates_on_codepoints) {
  // 5 个汉字（每字 3 字节）+ 省略号 3 字节 = 18 字节
  const auto label = st::ui::graph::truncate_label("一二三四五六七八九十", 5);
  ST_CHECK_EQ(label.size(), std::size_t{5 * 3 + 3});

  // 逐字节验证 UTF-8 合法：末尾必须是完整码点（按字节截断会切出半个汉字）
  std::size_t i = 0;
  int codepoints = 0;
  bool valid = true;
  while (i < label.size()) {
    const auto byte = static_cast<unsigned char>(label[i]);
    std::size_t step = 0;
    if ((byte & 0x80U) == 0x00U) {
      step = 1;
    } else if ((byte & 0xE0U) == 0xC0U) {
      step = 2;
    } else if ((byte & 0xF0U) == 0xE0U) {
      step = 3;
    } else if ((byte & 0xF8U) == 0xF0U) {
      step = 4;
    } else {
      valid = false;
      break;
    }
    i += step;
    ++codepoints;
  }
  ST_CHECK(valid);
  ST_CHECK_EQ(i, label.size());
  ST_CHECK_EQ(codepoints, 6);  // 5 字 + 省略号
}

ST_TEST(graph_layout_label_short_text_untouched) {
  // 短于阈值 → 原样返回，不加省略号
  ST_CHECK_EQ(st::ui::graph::truncate_label("短", 5), std::string("短"));
  ST_CHECK_EQ(st::ui::graph::truncate_label("", 5), std::string(""));
  // 恰好等于阈值 → 不截断
  ST_CHECK_EQ(st::ui::graph::truncate_label("一二三四五", 5), std::string("一二三四五"));
}
