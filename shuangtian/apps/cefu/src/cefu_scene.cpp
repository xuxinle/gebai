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

  // 坐标先存成数组：供节点锚点复用
  out += "const P = [\n";
  for (const auto& node : graph.nodes) {
    out += std::format("  {{x: {:.3f}, y: {:.3f}, z: {:.3f}}},\n",
                       static_cast<double>(node.position.x), static_cast<double>(node.position.y),
                       static_cast<double>(node.position.z));
  }
  out += "];\n\n";

  // ── 连线：**不在三维层画**（已改到 LabelOverlay）──
  //
  // 为何改掉（换掉一版的关键）：三维画的是节点中心的投影点，
  // 而文字被避让算法挪到了别处（右侧 4px、重叠时上下让出几十像素）。
  // 两套坐标对不上，于是线永远“悬空”，调粗细/颜色/段数都治不好。
  //
  // 现在线由 LabelOverlay 画：它知道每个文字框的**精确屏幕矩形**，
  // 从框边缘连出，二维画线还能抗锯齿。
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
