/// @file cefu_scene.cpp
/// @brief 知识图谱 → three.js 脚本。
///
/// 设计说明见头文件。这里只记实现上的关键取舍。

#include "cefu_scene.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace cefu::scene {
namespace {

/// 三位小数：布局尺度约 ±170，0.001 在这个视野下远小于一个像素。
auto vec3(double x, double y, double z) -> std::string {
  return std::format("{:.3f}, {:.3f}, {:.3f}", x, y, z);
}

auto hex_rgb(int r, int g, int b) -> std::string {
  return std::format("0x{:02x}{:02x}{:02x}", r & 0xFF, g & 0xFF, b & 0xFF);
}

/// 转义 JS 字符串字面量。
///
/// **不只是洁癖**：标题里一个换行就会让整份脚本语法错，
/// `load_script` 失败后视口**保持旧场景**（框架的"失败不清空"语义），
/// 现象是"改了知识库但图不变"——很难联想到是标题里的字符。
auto escape_js(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '\'':
        out += "\\'";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

auto bounding_radius(const graph::Graph& graph) -> double {
  double max_radius = 1.0;
  for (const auto& node : graph.nodes) {
    const auto x = static_cast<double>(node.position.x);
    const auto y = static_cast<double>(node.position.y);
    const auto z = static_cast<double>(node.position.z);
    max_radius = std::max(max_radius, std::sqrt(x * x + y * y + z * z));
  }
  return max_radius;
}

/// 边色（按语义）。颜色偏离而都亮：连线是背景结构，但仍要能区分。
auto edge_color(graph::EdgeKind kind) -> std::string {
  switch (kind) {
    case graph::EdgeKind::BelongsTo:
      return hex_rgb(0xB8, 0x94, 0x48);  // 从属：金
    case graph::EdgeKind::Links:
      return hex_rgb(0x4E, 0x96, 0xE0);  // 显式关联：蓝
    case graph::EdgeKind::Supersedes:
      return hex_rgb(0xD0, 0x5C, 0x4C);  // 取代：红
    case graph::EdgeKind::OwnedBy:
      return hex_rgb(0x42, 0xB8, 0x8C);  // 负责人：绿
    case graph::EdgeKind::SharedFact:
      return hex_rgb(0x9A, 0x78, 0xD0);  // 共享硬事实：紫
    case graph::EdgeKind::ScopedTo:
      return hex_rgb(0x74, 0x84, 0xC8);  // 作用域：蓝紫
  }
  return hex_rgb(0x64, 0x74, 0x8B);
}

}  // namespace

auto node_color(graph::NodeKind kind, const std::string& confidence) -> std::string {
  switch (kind) {
    case graph::NodeKind::Category:
      return hex_rgb(0xE8, 0xB5, 0x4B);  // 册：暖金（枢纽）
    case graph::NodeKind::Person:
      return hex_rgb(0x4E, 0xD4, 0xA0);  // 人：薄荷绿
    case graph::NodeKind::Scope:
      return hex_rgb(0xB4, 0x8C, 0xF0);  // 作用域：紫
    case graph::NodeKind::Entry:
      break;
  }
  // 词条按置信度分色——图谱的**语义核心**：一眼看出哪些还没被确认。
  if (confidence == "verified") return hex_rgb(0x5E, 0xAE, 0xFF);  // 已验证：亮蓝
  if (confidence == "stale") return hex_rgb(0x5A, 0x60, 0x6C);     // 过期：灰
  return hex_rgb(0xF5, 0xA6, 0x3E);                                 // 待确认：琥珀
}

auto build_script(const graph::Graph& graph, const Options& options) -> std::string {
  if (graph.nodes.empty()) return build_empty_script(options);

  std::string out;
  out.reserve(graph.nodes.size() * 420 + graph.edges.size() * 160 + 8192);

  const double radius = bounding_radius(graph);
  const double edge_radius = radius * options.edge_radius_ratio;
  const double camera_distance = std::max(3.0, radius * 1.25);

  out += "// 册府知识图谱（本文件由 cefu_scene.cpp 生成，不要手改）\n";
  out += std::format("// {} 个节点 · {} 条边 · 包围半径 {:.1f}\n", graph.nodes.size(),
                     graph.edges.size(), radius);
  out += "const scene = new THREE.Scene();\n";
  out += std::format("scene.background = new THREE.Color({});\n", options.background);
  out += "\n";

  // ── 相机 ──
  out += "const camera = new THREE.PerspectiveCamera(46, 16 / 9, 0.1, 20000);\n";
  // 斜上方俯视：纯正视会把三维压成二维，看不出层级
  out += std::format("camera.position.set({});\n",
                     vec3(camera_distance * 0.30, camera_distance * 0.40, camera_distance * 0.87));
  out += "camera.lookAt(0, 0, 0);\n";
  out += "scene.add(camera);\n\n";

  // ── 灯光 ──
  //
  // 三点布光 + 一个很弱的点光。环境光给得高（0.9）是因为暗背景上
  // 全靠环境光把暗部托出来；主光负责高光与体积感。
  out += "scene.add(new THREE.AmbientLight(0x8899bb, 0.95));\n";
  out += "const key = new THREE.DirectionalLight(0xffffff, 2.4);\n";
  out += std::format("key.position.set({});\n",
                     vec3(camera_distance * 0.7, camera_distance * 1.1, camera_distance * 0.8));
  out += "scene.add(key);\n";
  out += "const fill = new THREE.DirectionalLight(0x7FA8FF, 1.0);\n";
  out += std::format("fill.position.set({});\n",
                     vec3(-camera_distance * 0.9, camera_distance * 0.2, camera_distance * 0.5));
  out += "scene.add(fill);\n";
  out += "const rim = new THREE.DirectionalLight(0xFFC58A, 1.1);\n";
  out += std::format("rim.position.set({});\n",
                     vec3(-camera_distance * 0.2, -camera_distance * 0.6, -camera_distance));
  out += "scene.add(rim);\n\n";

  // ── 连线：**在三维里画**（真正的三维线条）──
  //
  // ## 为何必须回到三维（这一版的核心修正）
  //
  // 我曾在二维叠层里“自己算投影 + 画直线”，理由是透视投影保持直线性、
  // 位置等价。**位置确实等价，但三维语义丢了**：
  // - 深度调制（远的线该更细更淡）——二维版靠屏幕长度猜，完全不对；
  // - 遮挡（线该被近处的字挡住）——二维叠层里做不到；
  // - 转视角时的层次——两套投影各算一遍，错位与滞后无法避免。
  //
  // 而且 three.js 自己就会做这些事：给一段几何、一个材质，
  // 深度缓冲、透视、光照全由渲染器管。人为在 CPU 上重做一遍只会更差。
  out += "function addEdge(from, to, color, thickness) {\n";
  out += "  const a = new THREE.Vector3(from.x, from.y, from.z);\n";
  out += "  const b = new THREE.Vector3(to.x, to.y, to.z);\n";
  out += "  const dir = new THREE.Vector3().subVectors(b, a);\n";
  out += "  const full = dir.length();\n";
  out += "  if (full < 1e-6) return null;\n";
  out += "  dir.multiplyScalar(1.0 / full);\n";
  out += "  const geo = new THREE.CylinderGeometry(thickness, thickness, full, 14, 1);\n";
  out += "  const mat = new THREE.MeshBasicMaterial({ color: color });\n";
  out += "  const mesh = new THREE.Mesh(geo, mat);\n";
  out += "  mesh.position.set((a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5);\n";
  out += "  const up = new THREE.Vector3(0, 1, 0);\n";
  out += "  mesh.quaternion.copy(new THREE.Quaternion().setFromUnitVectors(up, dir));\n";
  out += "  return mesh;\n";
  out += "}\n\n";

  // 坐标数组
  out += "const P = [\n";
  for (const auto& node : graph.nodes) {
    out += std::format("  {{x: {:.3f}, y: {:.3f}, z: {:.3f}}},\n",
                       static_cast<double>(node.position.x), static_cast<double>(node.position.y),
                       static_cast<double>(node.position.z));
  }
  out += "];\n\n";

  // 逐条边：颜色由边语义决定（这些颜色偏离但都亮，在深底上看得见）
  {
    std::size_t edge_index = 0;
    for (const auto& edge : graph.edges) {
      if (edge.from >= graph.nodes.size() || edge.to >= graph.nodes.size()) continue;
      const bool inferred = edge.kind == graph::EdgeKind::SharedFact;
      const double thickness = inferred ? edge_radius * 0.75 : edge_radius;
      out += std::format("const e{} = addEdge(P[{}], P[{}], {}, {:.4f});\n"
                         "if (e{} !== null) {{ e{}.name = 'edge_{}'; scene.add(e{}); }}\n",
                         edge_index, edge.from, edge.to, edge_color(edge.kind), thickness,
                         edge_index, edge_index, edge_index, edge_index);
      ++edge_index;
    }
  }
  out += "\n";

  // ── 节点：**不画球**（只画连线，文字由二维叠层画）──
  //
  // 为何去掉球（用户直说“球都去掉”）：
  // 球只能表达“这里有个点”，而图谱要表达的是“这里是什么”。
  // 文字已经在同一位置说了是什么，再叠一个球只是噪声——
  // 而且球的光晕会盖住文字、加重密集区的拥挤。
  //
  // 保留的部分：**连线**。关系是三维才能表达好的东西，
  // 而且没有连线就看不出结构（只剩一堆散落的字）。
  //
  // 每个节点留一个空 Group 当锚点：让场景物体数与节点数一致（便于核对），
  // 也给将来的拾取留个落点。
  out += "// ── 节点锚点（无几何，仅用于计数与拾取）──\n";
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto& node = graph.nodes[i];
    // 用 Group 而不是 Object3D：`three.cjs` 里 Object3D 是基类、可构造，
    // 但 Group 更保险（它在任何版本都存在且可 `scene.add`）。
    out += std::format("const a{} = new THREE.Group();\n"
                       "a{}.position.set({});\n"
                       "a{}.name = '{}';\n"
                       "scene.add(a{});\n",
                       i, i,
                       vec3(static_cast<double>(node.position.x),
                            static_cast<double>(node.position.y),
                            static_cast<double>(node.position.z)),
                       i, escape_js(node.id), i);
  }
  out += "\n";

  // 标签说明：脚本宿主无 Canvas 2D，Sprite 文字做不了。
  // 这不是没做完——三维里只画几何，文字由界面层的图例承担。
  out += "// 注：脚本宿主（QuickJS）没有 Canvas 2D，three.js 常规的\n";
  out += "// 「Canvas 画字 → 纹理 → Sprite」走不通。文字信息由界面图例承担。\n";

  out += "\nscene;\n";
  return out;
}

auto build_empty_script(const Options& options) -> std::string {
  std::string out;
  out += "// 册府知识图谱：空图\n";
  out += "const scene = new THREE.Scene();\n";
  out += std::format("scene.background = new THREE.Color({});\n", options.background);
  out += "const camera = new THREE.PerspectiveCamera(46, 16 / 9, 0.1, 1000);\n";
  out += "camera.position.set(0, 1.4, 3.6);\n";
  out += "camera.lookAt(0, 0, 0);\n";
  out += "scene.add(camera);\n";
  out += "scene.add(new THREE.AmbientLight(0x8899bb, 1.0));\n";
  out += "const dot = new THREE.Mesh(new THREE.SphereGeometry(1.0, 20, 14),\n";
  out += "  new THREE.MeshStandardMaterial({color: 0x232830, metalness: 0.3, roughness: 0.7}));\n";
  out += "dot.scale.setScalar(0.28);\n";
  out += "dot.name = 'empty_placeholder';\n";
  out += "scene.add(dot);\n";
  out += "scene;\n";
  return out;
}

}  // namespace cefu::scene
