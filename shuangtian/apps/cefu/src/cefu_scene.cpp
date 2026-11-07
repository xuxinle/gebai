/// @file cefu_scene.cpp
/// @brief 知识图谱 → three.js 脚本。
///
/// 设计说明见头文件。这里记实现上的关键取舍。

#include "cefu_scene.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace cefu::scene {
namespace {

/// 把三个坐标写进脚本。三位小数足够：
/// 布局尺度约 ±170，0.001 的精度在这个视野下远小于一个像素。
auto vec3(double x, double y, double z) -> std::string {
  return std::format("{:.3f}, {:.3f}, {:.3f}", x, y, z);
}

/// three.js 的颜色字面量（`0xRRGGBB`）。
auto hex_rgb(int r, int g, int b) -> std::string {
  return std::format("0x{:02x}{:02x}{:02x}", r & 0xFF, g & 0xFF, b & 0xFF);
}

/// 转义单引号与反斜杠（节点标签会进脚本字符串字面量）。
///
/// **不转义换行会有实实在在的后果**：一个标题里带换行，整份脚本就语法错，
/// `load_script` 失败 → 视口保持旧场景（框架的"失败不清空"语义），
/// 现象是"改了知识库但图不变"。所以这里宁严勿宽。
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
        // 其余字节原样透传：UTF-8 多字节序列不需要转义（QuickJS 按 UTF-8 解码源串）
        out += c;
        break;
    }
  }
  return out;
}

/// 包围球半径（相机定位用）。没有它就得手写一个"看起来差不多"的距离。
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

}  // namespace

auto node_color(graph::NodeKind kind, const std::string& confidence) -> std::string {
  switch (kind) {
    case graph::NodeKind::Category:
      // 分类：低饱和的暖金，作"册"的骨架色
      return hex_rgb(0xC9, 0xA2, 0x27);
    case graph::NodeKind::Person:
      // 人：青绿（与"谁负责"的语义关联）
      return hex_rgb(0x3F, 0xB9, 0x8C);
    case graph::NodeKind::Scope:
      // 作用域：紫（只在非 global 时才出现）
      return hex_rgb(0x9B, 0x7B, 0xD8);
    case graph::NodeKind::Entry:
      break;
  }
  // 词条按置信度分色——这是图谱的**语义核心**：一眼看出哪些还没被确认。
  if (confidence == "verified") return hex_rgb(0x58, 0xA6, 0xFF);  // 已验证：亮蓝实心
  if (confidence == "stale") return hex_rgb(0x6B, 0x72, 0x80);     // 过期：灰
  // inferred：琥珀——"待确认"要在视觉上刺眼，否则没人去确认
  return hex_rgb(0xF0, 0x9B, 0x3E);
}

auto build_script(const graph::Graph& graph, const Options& options) -> std::string {
  if (graph.nodes.empty()) return build_empty_script(options);

  std::string out;
  out.reserve(graph.nodes.size() * 260 + graph.edges.size() * 130 + 2048);

  const double radius = bounding_radius(graph);
  // 相机距离：包围球半径的 2.6 倍。太近会穿进图里，太远图变成一点。
  const double camera_distance = std::max(3.0, radius * 2.6);

  out += "// 册府知识图谱（本文件由 cefu_scene.cpp 生成，不要手改）\n";
  out += std::format("// {} 个节点 · {} 条边 · 包围半径 {:.1f}\n", graph.nodes.size(),
                     graph.edges.size(), radius);
  out += "const scene = new THREE.Scene();\n";
  out += std::format("scene.background = new THREE.Color({});\n", options.background);
  out += "\n";

  // ── 相机 ──
  out += "const camera = new THREE.PerspectiveCamera(48, 16 / 9, 0.1, 10000);\n";
  out += std::format("camera.position.set({});\n", vec3(0.0, camera_distance * 0.45, camera_distance));
  out += "camera.lookAt(0, 0, 0);\n";
  out += "scene.add(camera);\n\n";

  // ── 灯光 ──
  //
  // 三点布光：够让球体有明暗层次（看不出球是球的图谱等于一堆圆片）。
  out += "scene.add(new THREE.AmbientLight(0x8899bb, 0.62));\n";
  out += "const key = new THREE.DirectionalLight(0xffffff, 2.0);\n";
  out += std::format("key.position.set({});\n", vec3(camera_distance * 0.6, camera_distance * 0.9, camera_distance * 0.7));
  out += "scene.add(key);\n";
  out += "const fill = new THREE.DirectionalLight(0x88bbff, 0.65);\n";
  out += std::format("fill.position.set({});\n", vec3(-camera_distance * 0.8, camera_distance * 0.3, camera_distance * 0.4));
  out += "scene.add(fill);\n";
  out += "const rim = new THREE.DirectionalLight(0xffaa66, 0.75);\n";
  out += std::format("rim.position.set({});\n", vec3(0.0, -camera_distance * 0.5, -camera_distance));
  out += "scene.add(rim);\n\n";

  // ── 边（先画，节点后画会盖住连线端点，视觉更干净）──
  //
  // 用圆柱体而不是 Line：Line 在真三维里永远 1px 宽（不随距离变粗），
  // 远看会消失；圆柱有实际半径，远近感一致。
  out += "// ── 连线 ──\n";
  out += "function addEdge(from, to, color, thickness) {\n";
  out += "  const a = new THREE.Vector3(from.x, from.y, from.z);\n";
  out += "  const b = new THREE.Vector3(to.x, to.y, to.z);\n";
  out += "  const dir = new THREE.Vector3().subVectors(b, a);\n";
  out += "  const len = dir.length();\n";
  out += "  if (len < 1e-6) return null;\n";
  out += "  const geo = new THREE.CylinderGeometry(thickness, thickness, len, 6, 1);\n";
  out += "  const mat = new THREE.MeshBasicMaterial({ color: color, transparent: true, opacity: 0.42 });\n";
  out += "  const mesh = new THREE.Mesh(geo, mat);\n";
  // 圆柱默认沿 +Y；用 quaternion 把它转到 dir 方向。
  out += "  mesh.position.copy(a).add(b).multiplyScalar(0.5);\n";
  out += "  const up = new THREE.Vector3(0, 1, 0);\n";
  out += "  const q = new THREE.Quaternion().setFromUnitVectors(up, dir.clone().normalize());\n";
  out += "  mesh.quaternion.copy(q);\n";
  out += "  return mesh;\n";
  out += "}\n\n";

  // 每种边一个颜色（与图的语义对应）
  const auto edge_color = [](graph::EdgeKind kind) -> std::string {
    switch (kind) {
      case graph::EdgeKind::BelongsTo:
        return "0x8a7a3a";  // 从属：暗金
      case graph::EdgeKind::Links:
        return "0x4a90d0";  // 显式关联：蓝
      case graph::EdgeKind::Supersedes:
        return "0xd05a4a";  // 取代：红（"这条顶掉那条"要看得见）
      case graph::EdgeKind::OwnedBy:
        return "0x3fb98c";  // 负责人：青绿
      case graph::EdgeKind::SharedFact:
        return "0x9b7bd8";  // 共享硬事实：紫（隐含关联，最需要被看见）
      case graph::EdgeKind::ScopedTo:
        return "0x7a8fd8";  // 作用域：蓝紫
    }
    return "0x666666";
  };

  // 节点坐标先写进一个数组，供边引用（避免重复字面量，脚本更小）
  out += "const P = [\n";
  for (const auto& node : graph.nodes) {
    out += std::format("  {{x: {:.3f}, y: {:.3f}, z: {:.3f}}},\n",
                       static_cast<double>(node.position.x), static_cast<double>(node.position.y),
                       static_cast<double>(node.position.z));
  }
  out += "];\n\n";

  std::size_t edge_index = 0;
  for (const auto& edge : graph.edges) {
    if (edge.from >= graph.nodes.size() || edge.to >= graph.nodes.size()) continue;
    // 共享硬事实边略细：它是"推断出来的关联"，不该和显式声明一样重
    const double thickness = edge.kind == graph::EdgeKind::SharedFact ? options.edge_radius * 0.7
                                                                     : options.edge_radius;
    out += std::format(
        "const e{} = addEdge(P[{}], P[{}], {}, {:.4f});\n"
        "if (e{} !== null) {{ e{}.name = 'edge_{}'; scene.add(e{}); }}\n",
        edge_index, edge.from, edge.to, edge_color(edge.kind), thickness, edge_index, edge_index,
        edge_index, edge_index);
    ++edge_index;
  }
  out += "\n";

  // ── 节点 ──
  out += "// ── 节点 ──\n";
  // 共享几何：几百个球共用一份顶点数据，内存与构建时间都省一半以上
  out += std::format("const sphereGeo = new THREE.SphereGeometry({:.4f}, 20, 14);\n",
                     options.node_radius);
  out += "const matCache = {};\n";
  out += "function matFor(color) {\n";
  out += "  if (!matCache[color]) {\n";
  out += "    matCache[color] = new THREE.MeshStandardMaterial({ color: color, metalness: 0.25, roughness: 0.42 });\n";
  out += "  }\n";
  out += "  return matCache[color];\n";
  out += "}\n\n";

  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto& node = graph.nodes[i];
    const auto color = node_color(node.kind, node.confidence);
    // 球体大小按 weight；undefined 的 weight 用 1
    const double scale = std::max(0.5, node.weight);
    out += std::format(
        "const n{} = new THREE.Mesh(sphereGeo, matFor({}));\n"
        "n{}.position.set({});\n"
        "n{}.scale.setScalar({:.3f});\n"
        "n{}.name = '{}';\n"
        "scene.add(n{});\n",
        i, color, i, vec3(static_cast<double>(node.position.x), static_cast<double>(node.position.y),
                          static_cast<double>(node.position.z)),
        i, scale, i, escape_js(node.id), i);
  }
  out += "\n";

  // ── 标签（可选）──
  if (options.labels) {
    out += "// ── 标签 ──\n";
    out += "// 脚本宿主（QuickJS）**没有 Canvas 2D**，所以 three.js 常见的\n";
    out += "// 「Canvas 画字 → 转纹理 → Sprite」这条路走不通。文字由二维叠层\n";
    out += "// （界面层按投影坐标画）负责，三维里只画几何。\n";
    out += "// 详见 cefu_scene.hpp 的注释：这里是刻意的分工，不是没做完。\n";
  }

  out += "\nscene;\n";
  return out;
}

auto build_empty_script(const Options& options) -> std::string {
  std::string out;
  out += "// 册府知识图谱：空图\n";
  out += "const scene = new THREE.Scene();\n";
  out += std::format("scene.background = new THREE.Color({});\n", options.background);
  out += "const camera = new THREE.PerspectiveCamera(48, 16 / 9, 0.1, 1000);\n";
  out += "camera.position.set(0, 1.6, 4.2);\n";
  out += "camera.lookAt(0, 0, 0);\n";
  out += "scene.add(camera);\n";
  out += "scene.add(new THREE.AmbientLight(0x8899bb, 0.8));\n";
  // 空图给一个小小的占位球：完全空白会让人以为"坏了"
  out += "const dot = new THREE.Mesh(new THREE.SphereGeometry(0.16, 16, 12),\n";
  out += "  new THREE.MeshStandardMaterial({color: 0x2a3038, metalness: 0.2, roughness: 0.8}));\n";
  out += "dot.name = 'empty_placeholder';\n";
  out += "scene.add(dot);\n";
  out += "scene;\n";
  return out;
}

}  // namespace cefu::scene
