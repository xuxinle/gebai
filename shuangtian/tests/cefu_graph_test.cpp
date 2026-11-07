/// @file cefu_graph_test.cpp
/// @brief 册府三维图谱内核：图构建 / 布局 / 投影 / 拾取。
///
/// 这一层是纯算法，所以每条断言都盯**具体的错**，而不是"跑通就行"：
/// - 布局不确定 → 每次打开图形状都变、截图回归必飘；
/// - 投影不裁近平面 → 整屏发黑（除零出巨大坐标）；
/// - 拾取按错优先级 → 点到被挡住的节点；
/// - 标签按字节截断 → 切出半个汉字。

#include <cmath>
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
using cefu::graph::Camera;
using cefu::graph::EdgeKind;
using cefu::graph::Graph;
using cefu::graph::NodeKind;
using cefu::graph::ProjectOptions;

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

auto count_edges(const Graph& g, EdgeKind kind) -> std::size_t {
  std::size_t n = 0;
  for (const auto& e : g.edges) {
    if (e.kind == kind) ++n;
  }
  return n;
}

auto count_nodes(const Graph& g, NodeKind kind) -> std::size_t {
  std::size_t n = 0;
  for (const auto& node : g.nodes) {
    if (node.kind == kind) ++n;
  }
  return n;
}

/// 到原点距离（布局尺度的判据）。
///
/// 先把 float 转成 double 再乘：直接 `p.x * p.x` 是 float 乘法，
/// 与 `double` 相加会触发 `-Werror=double-promotion`（本仓库就是严格门禁）。
auto radius_of(const st::math::Vec3& p) -> double {
  const auto x = static_cast<double>(p.x);
  const auto y = static_cast<double>(p.y);
  const auto z = static_cast<double>(p.z);
  return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

ST_TEST(cefu_graph_empty_build) {
  const auto g = cefu::graph::build({});
  ST_CHECK_EQ(g.nodes.size(), std::size_t{0});
  ST_CHECK_EQ(g.edges.size(), std::size_t{0});
  ST_CHECK_EQ(g.entry_count, std::size_t{0});
  ST_CHECK_EQ(g.largest_cluster, std::size_t{0});
}

ST_TEST(cefu_graph_category_node_deduped) {
  std::vector<Entry> entries{
      make_entry("e1", "写入延迟高", "运维"),
      make_entry("e2", "写入放大", "运维"),
      make_entry("e3", "配额找谁", "运维"),
  };
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_nodes(g, NodeKind::Entry), std::size_t{3});
  // 三条同册 → 只应有一个分类节点（不建就会变成三个同名节点叠在一起）
  ST_CHECK_EQ(count_nodes(g, NodeKind::Category), std::size_t{1});
  ST_CHECK_EQ(count_edges(g, EdgeKind::BelongsTo), std::size_t{3});
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
  ST_CHECK_EQ(count_edges(g, EdgeKind::Links), std::size_t{2});
}

ST_TEST(cefu_graph_self_link_ignored) {
  std::vector<Entry> entries{make_entry("e1", "自己链自己", "运维")};
  entries[0].links.push_back("e1");
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, EdgeKind::Links), std::size_t{0});
}

ST_TEST(cefu_graph_supersedes_edge) {
  std::vector<Entry> entries{
      make_entry("old", "旧做法", "运维"),
      make_entry("new", "新做法", "运维"),
  };
  entries[1].supersedes = "old";
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, EdgeKind::Supersedes), std::size_t{1});
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
  ST_CHECK_EQ(count_edges(g, EdgeKind::SharedFact), std::size_t{2});
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
  ST_CHECK_EQ(count_edges(g, EdgeKind::SharedFact), std::size_t{0});
}

ST_TEST(cefu_graph_shared_fact_needs_two_owners) {
  std::vector<Entry> entries{make_entry("e1", "只有我", "运维")};
  entries[0].facts.push_back("端口=8428");
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(count_edges(g, EdgeKind::SharedFact), std::size_t{0});
}

ST_TEST(cefu_graph_global_scope_not_a_node) {
  std::vector<Entry> entries{
      make_entry("e1", "公共知识", "运维"),
      make_entry("e2", "本群规矩", "运维"),
  };
  entries[1].scope = R"({"kind":"chat","id":"oc_abc"})";
  const auto g = cefu::graph::build(entries);
  // global 是缺省：建成节点等于给图加一个巨大枢纽，把真正的结构完全遮住
  ST_CHECK_EQ(count_nodes(g, NodeKind::Scope), std::size_t{1});
  ST_CHECK_EQ(count_edges(g, EdgeKind::ScopedTo), std::size_t{1});
}

ST_TEST(cefu_graph_isolated_entry_counted) {
  std::vector<Entry> entries{
      make_entry("e1", "甲", "运维"),
      make_entry("e2", "乙", ""),  // 无分类无负责人 → 孤立
  };
  const auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(g.isolated_entries, std::size_t{1});
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

ST_TEST(cefu_graph_layout_deterministic) {
  std::vector<Entry> entries;
  for (int i = 0; i < 12; ++i) {
    entries.push_back(make_entry("e" + std::to_string(i), "词条" + std::to_string(i), "运维"));
  }
  auto g1 = cefu::graph::build(entries);
  auto g2 = cefu::graph::build(entries);
  cefu::graph::layout(g1);
  cefu::graph::layout(g2);

  // 布局必须确定：否则每次打开图形状都变，截图回归也永远飘
  for (std::size_t i = 0; i < g1.nodes.size(); ++i) {
    ST_CHECK_EQ(g1.nodes[i].position.x, g2.nodes[i].position.x);
    ST_CHECK_EQ(g1.nodes[i].position.y, g2.nodes[i].position.y);
    ST_CHECK_EQ(g1.nodes[i].position.z, g2.nodes[i].position.z);
  }
}

ST_TEST(cefu_graph_layout_bounded) {
  std::vector<Entry> entries;
  for (int i = 0; i < 40; ++i) {
    entries.push_back(
        make_entry("e" + std::to_string(i), "词条" + std::to_string(i), "册" + std::to_string(i % 4)));
  }
  auto g = cefu::graph::build(entries);
  cefu::graph::layout(g);
  for (const auto& node : g.nodes) {
    // 归一化到 scale 附近：飞到千里之外说明力发散
    ST_CHECK(radius_of(node.position) < 900.0);
    ST_CHECK(std::isfinite(static_cast<double>(node.position.x)));
    ST_CHECK(std::isfinite(static_cast<double>(node.position.y)));
    ST_CHECK(std::isfinite(static_cast<double>(node.position.z)));
  }
}

ST_TEST(cefu_graph_layout_separates_nodes) {
  std::vector<Entry> entries{
      make_entry("e1", "甲", "册甲"),
      make_entry("e2", "乙", "册乙"),
  };
  auto g = cefu::graph::build(entries);
  cefu::graph::layout(g);
  ST_CHECK(g.nodes[0].position.x != g.nodes[1].position.x ||
           g.nodes[0].position.y != g.nodes[1].position.y);
}

ST_TEST(cefu_graph_project_clips_near_plane) {
  std::vector<Entry> entries{make_entry("e1", "甲", "")};  // 不分类 → 只有 1 个节点
  auto g = cefu::graph::build(entries);
  cefu::graph::layout(g);
  ST_CHECK_EQ(g.nodes.size(), std::size_t{1});  // 前提：真的只有词条节点
  Camera camera;
  camera.distance = 150.0;
  ProjectOptions opts;
  opts.near_plane = 100000.0;  // 人为极大 → 确保全部裁掉
  const auto projected = cefu::graph::project(g, camera, opts);
  ST_CHECK_EQ(projected.size(), std::size_t{1});
  ST_CHECK(!projected[0].visible);

  ProjectOptions normal;
  const auto visible = cefu::graph::project(g, camera, normal);
  ST_CHECK(visible[0].visible);
}

ST_TEST(cefu_graph_project_centers_single_node) {
  std::vector<Entry> entries{make_entry("e1", "居中", "")};  // 不分类 → 单节点
  auto g = cefu::graph::build(entries);
  cefu::graph::layout(g);
  Camera camera;
  ProjectOptions opts;
  opts.viewport_width = 800.0;
  opts.viewport_height = 600.0;
  const auto projected = cefu::graph::project(g, camera, opts);
  ST_CHECK(projected[0].visible);
  // 单节点在布局后会被归一化到 scale 距离（不是原点——归一化按最大半径缩），
  // 所以不断言“恰好在正中”，只断言“在视口内且没跑飞”。
  ST_CHECK(projected[0].x > 0.0);
  ST_CHECK(projected[0].x < 800.0);
  ST_CHECK(projected[0].y > 0.0);
  ST_CHECK(projected[0].y < 600.0);
  ST_CHECK(projected[0].radius > 0.0);
}

ST_TEST(cefu_graph_camera_pitch_clamped) {
  Camera camera;
  for (int i = 0; i < 100; ++i) camera.orbit(0.0, 0.5);
  // 越过 ±π/2 会让画面翻转，用户瞬间失去方向感
  ST_CHECK(camera.pitch < 1.58);
  ST_CHECK(camera.pitch > 1.50);
}

ST_TEST(cefu_graph_camera_zoom_limited) {
  Camera camera;
  for (int i = 0; i < 100; ++i) camera.zoom(0.8);
  ST_CHECK(camera.distance >= 150.0);
  for (int i = 0; i < 200; ++i) camera.zoom(1.2);
  ST_CHECK(camera.distance <= 2400.0);
}

ST_TEST(cefu_graph_pick_hits_nearest_center) {
  std::vector<Entry> entries{
      make_entry("e1", "甲", ""),  // 不分类：避免分类节点干扰下标断言
      make_entry("e2", "乙", ""),
  };
  auto g = cefu::graph::build(entries);
  ST_CHECK_EQ(g.nodes.size(), std::size_t{2});  // 前提：两个词条节点，无元节点
  // 手工摆位置，避免依赖布局细节
  g.nodes[0].position = st::math::Vec3{0.0F, 0.0F, 0.0F};
  g.nodes[1].position = st::math::Vec3{200.0F, 0.0F, 0.0F};
  Camera camera;
  camera.yaw = 0.0;
  camera.pitch = 0.0;
  ProjectOptions opts;
  opts.viewport_width = 800.0;
  opts.viewport_height = 600.0;
  const auto projected = cefu::graph::project(g, camera, opts);

  ST_CHECK_EQ(cefu::graph::pick(g, projected, projected[0].x, projected[0].y), std::size_t{0});
  ST_CHECK_EQ(cefu::graph::pick(g, projected, projected[1].x, projected[1].y), std::size_t{1});
  // 远处空白 → 未命中（返回 nodes.size()）
  ST_CHECK_EQ(cefu::graph::pick(g, projected, 5.0, 5.0), g.nodes.size());
}

ST_TEST(cefu_graph_depth_order_far_first) {
  std::vector<cefu::graph::Projected> projected(3);
  projected[0].depth = 100.0;
  projected[1].depth = 300.0;  // 最远
  projected[2].depth = 200.0;
  const auto order = cefu::graph::depth_order(projected);
  ST_CHECK_EQ(order.size(), std::size_t{3});
  // 画家算法：先画远的 → 深 300 的那个排第一
  ST_CHECK_EQ(order[0], std::size_t{1});
  ST_CHECK_EQ(order[2], std::size_t{0});
}

ST_TEST(cefu_graph_label_truncates_on_codepoints) {
  std::vector<Entry> entries{make_entry("e1", "一二三四五六七八九十", "册")};
  BuildOptions opts;
  opts.max_label_chars = 5;
  const auto g = cefu::graph::build(entries, opts);
  const auto& label = g.nodes[0].label;
  // 5 个汉字（每字 3 字节）+ 省略号 3 字节 = 18 字节
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
