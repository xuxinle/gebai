/// @file graph_view.cpp
/// @brief `GraphView`：图 → 三维脚本 → 视口 + 标签层（含自动取景与每帧相机同步）。
///
/// 设计说明见头文件。这里记**实现上的关键取舍**，每条都对应一次实测。

#include "st/ui/components/graph_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {
namespace {

/// 三位小数：布局尺度约 ±170，0.001 在这个视野下远小于一个像素。
auto vec3_literal(const math::Vec3& value) -> std::string {
  return std::format("{:.3f}, {:.3f}, {:.3f}", static_cast<double>(value.x),
                     static_cast<double>(value.y), static_cast<double>(value.z));
}

/// 转义 JS 字符串字面量。
///
/// **不只是洁癖**：标题里一个换行就会让整份脚本语法错，`load_script` 失败后视口
/// **保持旧场景**（框架"失败不清空"语义），现象是"改了数据但图不变"——
/// 很难联想到是某个标签里的字符。
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

}  // namespace

GraphView::GraphView() : view_(std::make_unique<ThreeView>()), labels_(std::make_unique<LabelOverlay>()) {
  // ── 视口 ──
  view_->set_id("graph-viewport");
  view_->set_background(math::Color{0x0B, 0x0E, 0x14, 0xFF});
  view_->set_backend("gpu");           // 见头文件：GPU 腿比软件腿快 57 倍
  view_->set_render_scale(1.0F);       // 全分辨率：GPU 腿 5ms/次旋转，降分辨率是白牺牲画质
  view_->set_animation_enabled(graph_style_.animate);
  view_->set_show_hud(false);
  // 绝对定位：**不参与父的 flex 分配**，铺满本组件区域、叠在一起。
  view_->style().absolute = true;
  view_->style().left = 0.0F;
  view_->style().top = 0.0F;
  view_->style().width = kAuto;
  view_->style().height = kAuto;
  view_->set_visible(false);

  // ── 标签层 ──
  labels_->set_id("graph-labels");
  labels_->style().absolute = true;
  labels_->style().left = 0.0F;
  labels_->style().top = 0.0F;
  labels_->style().width = kAuto;
  labels_->style().height = kAuto;
  labels_->set_visible(false);
  // **必须关掉输入拦截**（册府那轮的 bug 根源）：标签层铺满整个区域、又在视口之上，
  // 默认 `intercepts_input()==true` 会把鼠标事件全吃掉——旋转/缩放直接失效。
  // 而它自己没有需要交互的东西，本来就该点击穿透。
  labels_->set_intercepts_input(false);

  add_child(std::unique_ptr<Element>(view_.release()));
  add_child(std::unique_ptr<Element>(labels_.release()));
  // `add_child` 返回裸指针且所有权已转移：收回来备后续读写用
  //（不能再用 `view_.get()`——它已被 release，是空悬）。
  view_owner_ = dynamic_cast<ThreeView*>(child_at(0));
  labels_owner_ = dynamic_cast<LabelOverlay*>(child_at(1));
}

GraphView::~GraphView() = default;

// ── 内容 ──────────────────────────────────────────────────────────────────────

auto GraphView::set_graph(const graph::Graph& graph) -> bool {
  graph_ = graph;
  has_graph_ = !graph_.nodes.empty();

  // ① 三维脚本：连线（+ 可选标记）交给渲染器
  const std::string script = has_graph_ ? build_scene_script(graph_) : build_empty_script();
  const bool loaded = view_owner_->load_script(script, "<graph-view>");
  if (!loaded) {
    // 失败不清空标签层：宁可显示上一版（并报错），也不要变成一片空白
    last_error_ = view_owner_->last_error();
    st::log::warn("[GraphView] 三维场景装载失败：{}", last_error_);
    return false;
  }
  last_error_.clear();

  // ② 自动取景：按**图自己的包围球**算距离，不用 `frame_scene`
  //    （它按物理像素算 fov 对应的高度，DPI 会被多算一次——册府实测距离大了 3 倍）
  frame_graph();

  // ③ 标签：用视口当前相机投影（唯一知道真实相机的地方）
  sync_labels();
  mark_dirty();
  return true;
}

void GraphView::refresh_style() {
  view_owner_->set_background(math::Color::from_hex(
      static_cast<std::uint32_t>(std::strtoul(graph_style_.background.c_str(), nullptr, 0))));
  view_owner_->set_animation_enabled(graph_style_.animate);
  mark_dirty();
}

void GraphView::set_graph_visible(bool visible) {
  // **三处都要设**：两个子视图 + 本元素自己。
  //
  // 为何不能只设子视图（实测踩到）：`visible` 是**逐元素**的，父元素仍然可见时
  // 元素树/控制通道会报“可见”，而屏幕上也真的还会剩一块背景区（本元素不画但占位）。
  // 而调用方要的语义是“整张图不在了”——包括它在树上的可见状态。
  set_visible(visible);
  view_owner_->set_visible(visible);
  labels_owner_->set_visible(visible);
  if (visible) {
    // 隐藏期间相机可能变过（窗口缩放），重新开时把标签投影器同步到当前相机。
    sync_labels();
  }
  mark_dirty();
}

auto GraphView::graph_visible() const -> bool { return visible(); }

void GraphView::frame_graph() {
  if (!has_graph_) return;
  const double radius = graph::bounding_radius(graph_);
  auto camera = view_owner_->camera_state();
  camera.fov_y_degrees = static_cast<float>(graph_style_.fov_degrees);
  camera.distance = static_cast<float>(
      graph::framing_distance(radius, graph_style_.fov_degrees, graph_style_.framing_margin));
  view_owner_->set_camera_state(camera);
}

void GraphView::reset_camera() {
  view_owner_->reset_view();
  frame_graph();
  sync_labels();
  mark_dirty();
}

auto GraphView::camera_state() const -> OrbitCameraState {
  return view_owner_->camera_state();
}

void GraphView::set_camera_state(OrbitCameraState state) {
  view_owner_->set_camera_state(state);
  sync_labels();
  mark_dirty();
}

void GraphView::set_backend(std::string_view backend) { view_owner_->set_backend(backend); }
auto GraphView::backend_preference() const -> std::string { return view_owner_->backend_preference(); }
void GraphView::set_render_scale(float scale) { view_owner_->set_render_scale(scale); }
auto GraphView::render_scale() const -> float { return view_owner_->render_scale(); }

void GraphView::set_fill(bool fill) {
  view_owner_->set_fill_height(fill);
  if (fill) {
    // ## 铺满 = 两条都要（实测踩到）
    //
    // ① **自身也绝对定位**。图视图在很多应用里是"盖住整个内容区的覆盖层"，
    //    而它通常被挂在 XML 树里的某个容器下。若只让子视图铺满而不让本元素
    //    脱离 flex 流，本元素会先被父的排流分配成 **0 尺寸**（`grow` 已被兄弟吃满），
    //    于是子视图的"铺满"铺在空区域上——现象是截图一片背景色，
    //    而 `objects`/`labels`/`visible_nodes` 全部正常（它们只依赖相机与数据）。
    //    册府改造后就是这样：树里参数全对，画面全白。
    // ② 子视图铺满本元素的内容盒（宽度 `fill_width` + 高度 `set_fill_height`）。
    style_.absolute = true;
    style_.left = 0.0F;
    style_.top = 0.0F;
    view_owner_->style().width = kAuto;
    view_owner_->style().height = kAuto;
  }
  mark_layout_dirty();
}

// ── 脚本生成 ──────────────────────────────────────────────────────────────────

auto GraphView::build_scene_script(const graph::Graph& graph) const -> std::string {
  if (graph.nodes.empty()) return build_empty_script();

  std::string out;
  out.reserve(graph.nodes.size() * 260 + graph.edges.size() * 220 + 8192);

  const double radius = graph::bounding_radius(graph);
  const double edge_radius = std::max(radius * graph_style_.edge_radius_ratio, 1.0e-5);
  const double camera_distance = graph::framing_distance(radius, graph_style_.fov_degrees, 1.0);

  out += "// 关系图（本文件由 GraphView 生成，不要手改）\n";
  out += std::format("// {} 个节点 · {} 条边 · 包围半径 {:.1f}\n", graph.nodes.size(),
                     graph.edges.size(), radius);
  out += "const scene = new THREE.Scene();\n";
  out += std::format("scene.background = new THREE.Color({});\n\n", graph_style_.background);

  // ── 相机 ──
  out += std::format("const camera = new THREE.PerspectiveCamera({:.1f}, 16 / 9, 0.1, 20000);\n",
                     graph_style_.fov_degrees);
  // 斜上方俯视：纯正视会把三维压成二维，看不出层级
  out += std::format("camera.position.set({});\n",
                     vec3_literal(math::Vec3{static_cast<float>(camera_distance * 0.30),
                                             static_cast<float>(camera_distance * 0.40),
                                             static_cast<float>(camera_distance * 0.87)}));
  out += "camera.lookAt(0, 0, 0);\n";
  out += "scene.add(camera);\n\n";

  // ── 灯光 ──
  // 三点布光 + 高环境光：暗背景上全靠环境光把暗部托出来，主光负责高光与体积感。
  out += "scene.add(new THREE.AmbientLight(0x8899bb, 0.95));\n";
  out += "const key = new THREE.DirectionalLight(0xffffff, 2.4);\n";
  out += std::format("key.position.set({});\n",
                     vec3_literal(math::Vec3{static_cast<float>(camera_distance * 0.7),
                                             static_cast<float>(camera_distance * 1.1),
                                             static_cast<float>(camera_distance * 0.8)}));
  out += "scene.add(key);\n";
  out += "const fill = new THREE.DirectionalLight(0x7FA8FF, 1.0);\n";
  out += std::format("fill.position.set({});\n",
                     vec3_literal(math::Vec3{static_cast<float>(-camera_distance * 0.9),
                                             static_cast<float>(camera_distance * 0.2),
                                             static_cast<float>(camera_distance * 0.5)}));
  out += "scene.add(fill);\n";
  out += "const rim = new THREE.DirectionalLight(0xFFC58A, 1.1);\n";
  out += std::format("rim.position.set({});\n",
                     vec3_literal(math::Vec3{static_cast<float>(-camera_distance * 0.2),
                                             static_cast<float>(-camera_distance * 0.6),
                                             static_cast<float>(-camera_distance)}));
  out += "scene.add(rim);\n\n";

  // ── 连线：**在三维里画**（真正的三维线条）──
  // 给一段圆柱几何，深度缓冲/透视/遮挡/光照全由渲染器管——在 CPU 上重做一遍只会更差。
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

  // 坐标数组（边与标记都按下标引用）
  out += "const P = [\n";
  for (const auto& node : graph.nodes) {
    out += std::format("  {{x: {:.3f}, y: {:.3f}, z: {:.3f}}},\n",
                       static_cast<double>(node.position.x),
                       static_cast<double>(node.position.y),
                       static_cast<double>(node.position.z));
  }
  out += "];\n\n";

  {
    std::size_t edge_index = 0;
    for (const auto& edge : graph.edges) {
      // 越界边跳过：图是派生物，一条坏边不该带走整个画面
      if (edge.from >= graph.nodes.size() || edge.to >= graph.nodes.size()) continue;
      const double thickness = edge_radius * std::max(edge.width_scale, 0.05);
      out += std::format("const e{} = addEdge(P[{}], P[{}], {}, {:.5f});\n"
                         "if (e{} !== null) {{ e{}.name = 'edge_{}'; scene.add(e{}); }}\n",
                         edge_index, edge.from, edge.to, graph::argb_to_hex(edge.argb), thickness,
                         edge_index, edge_index, edge_index, edge_index);
      ++edge_index;
    }
  }
  out += "\n";

  // ── 节点锚点：每个节点一个空 Group ──
  //
  // 为什么留空 Group：① 场景物体数与节点数一致，便于核对（"几何少了"这类问题
  // 一眼可判）；② 给将来的空间拾取留落点。真正的可视化信息是文字（标签层）。
  out += "// ── 节点锚点（无几何，仅用于计数与拾取）──\n";
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const auto& node = graph.nodes[i];
    out += std::format("const a{} = new THREE.Group();\n"
                       "a{}.position.set({});\n"
                       "a{}.name = '{}';\n"
                       "scene.add(a{});\n",
                       i, i, vec3_literal(node.position), i, escape_js(node.id), i);
  }
  out += "\n";

  // ── 可选：节点标记 ──
  // 默认不画（见 `GraphStyle::node_markers` 的说明：球只是噪声，文字才是信息）。
  if (graph_style_.node_markers) {
    const double marker = std::max(radius * graph_style_.node_marker_ratio, 1.0e-5);
    out += "// ── 节点标记（可选）──\n";
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
      const auto& node = graph.nodes[i];
      out += std::format(
          "const m{} = new THREE.Mesh(new THREE.SphereGeometry({:.5f}, 16, 12),\n"
          "  new THREE.MeshStandardMaterial({{color: {}, metalness: 0.15, roughness: 0.55}}));\n"
          "m{}.position.set({});\n"
          "m{}.name = 'node_{}';\n"
          "scene.add(m{});\n",
          i, marker * std::max(node.weight, 0.2), graph::argb_to_hex(node.argb), i,
          vec3_literal(node.position), i, i, i);
    }
    out += "\n";
  }

  out += "// 注：脚本宿主（QuickJS）没有 Canvas 2D，three.js 常规的\n";
  out += "// 「Canvas 画字 → 纹理 → Sprite」走不通。文字由界面层的标签叠层承担。\n";
  out += "\nscene;\n";
  return out;
}

auto GraphView::build_empty_script() const -> std::string {
  std::string out;
  out += "// 关系图：空图\n";
  out += "const scene = new THREE.Scene();\n";
  out += std::format("scene.background = new THREE.Color({});\n", graph_style_.background);
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

// ── 标签同步 ──────────────────────────────────────────────────────────────────

void GraphView::sync_labels() {
  // `syncing_` 防重入：`set_labels` 会让标签层标脏，标脏可能触发一次不必要的
  // 同步链（在宿主 pump 里表现为重复重建）。这一层不重入，一次到底。
  if (syncing_) return;
  syncing_ = true;

  ThreeView* view = view_owner_;
  labels_owner_->set_projector([view](float x, float y, float z) {
    const auto projected = view->project_to_screen(math::Vec3{x, y, z});
    return std::make_pair(std::make_pair(projected.point.x, projected.point.y), projected.depth);
  });

  std::vector<LabelAnchor> anchors;
  anchors.reserve(graph_.nodes.size());
  for (const auto& node : graph_.nodes) {
    LabelAnchor anchor;
    anchor.id = node.id;
    anchor.x = node.position.x;
    anchor.y = node.position.y;
    anchor.z = node.position.z;
    anchor.text = node.label;
    anchor.weight = node.label_weight;
    anchor.argb = node.argb;
    anchor.plate = node.label_plate;
    anchor.plate_alpha = node.plate_alpha;
    anchors.push_back(std::move(anchor));
  }
  labels_owner_->set_labels(std::move(anchors));
  // 连线**不在这里画**：真实的三维线在场景脚本里，交给渲染器。
  // 二维叠层画线必须自己算投影，于是深度调制、遮挡、旋转后的层次全丢，而且两套投影必然错位。
  labels_owner_->set_edges({});
  syncing_ = false;
}

auto GraphView::sync_camera() -> bool {
  const auto live = view_owner_->camera_state();
  if (live.yaw == last_yaw_ && live.pitch == last_pitch_ && live.distance == last_distance_) {
    return false;  // 静止时零开销
  }
  last_yaw_ = live.yaw;
  last_pitch_ = live.pitch;
  last_distance_ = live.distance;
  // 投影器每次调用都读**当前**相机与视口尺寸，所以窗口缩放、拖拽旋转都自动正确。
  ThreeView* view = view_owner_;
  labels_owner_->set_projector([view](float x, float y, float z) {
    const auto projected = view->project_to_screen(math::Vec3{x, y, z});
    return std::make_pair(std::make_pair(projected.point.x, projected.point.y), projected.depth);
  });
  labels_owner_->mark_dirty();
  return true;
}

auto GraphView::tick() -> bool {
  if (!graph_visible() || labels_owner_ == nullptr) return false;
  // ① 三维侧：动画推进（图谱通常关自转，但视口需要知道"本帧要不要重画"）
  bool changed = view_owner_->tick_animation();
  // ② 相机 → 标签投影器（拖拽/滚轮改的是视口内部相机，标签层拿不到通知）
  changed = sync_camera() || changed;
  return changed;
}

// ── Element 接口 ──────────────────────────────────────────────────────────────

void GraphView::measure(const RenderContext& context, const Constraints& constraints) {
  // 视口没有"内容尺寸"可推（几何在大地上无限铺），所以：
  // - 宽度取可用宽（`fill_width` 在无界时退回 fallback）；
  // - 高度默认给一个合理值（与 ThreeView 的 `preferred_height` 同源），
  //   `set_fill(true)` 时吃满父给的可用高度。
  //
  // 注意**不能用 `max_height`**：列布局里它可能是 `kUnbounded`（1e9），
  // 拿去当帧缓冲尺寸会 `std::bad_alloc`（ThreeView 为同一个坑踩过）。
  const float width = style_.has_explicit_width()
                          ? style_.width
                          : (constraints.max_width < kUnbounded ? constraints.max_width : 480.0F);
  float height = 320.0F;
  if (style_.has_explicit_height()) {
    height = style_.height;
  } else if (constraints.available_height > 0.0F && constraints.available_height < kUnbounded) {
    height = constraints.available_height;
  } else if (constraints.max_height > 0.0F && constraints.max_height < kUnbounded) {
    height = constraints.max_height;
  }
  measured_ = math::Size{width > 0.0F ? width : 480.0F, height > 0.0F ? height : 320.0F};
  (void)context;
}

void GraphView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  // 两个子视图都是**绝对定位**，铺满本组件的内容盒。
  const math::Rect content = content_box();
  for (std::size_t index = 0; index < child_count(); ++index) {
    Element* child = child_at(index);
    if (child == nullptr) continue;
    child->arrange(context, content);
  }
  layout_dirty_ = false;
}

void GraphView::apply_theme(const Theme& theme) {
  Element::apply_theme(theme);
  // 背景色跟随显式设置（图谱通常要深底），不跟随主题的表面色——
  // 主题的浅底会让连线与文字全部失去对比度。
  (void)theme;
}

void GraphView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  (void)canvas;
  // 内容全由两个子视图画（它们是绝对定位的子元素，在自己的 paint 里落笔）。
  // 本组件自身只负责"区域与布局"，不画任何东西——这一层空着是对的。
  //
  // 可见节点数顺带记一笔：绘制阶段是唯一每帧都跑的地方，而它正是"截图一片背景色"
  // 时用来区分「相机错」与「没几何」的那个数。用 `graph_layout` 的投影算，
  // 与标签层走同一条路径（不是另一套公式）。
  if (!has_graph_) {
    visible_nodes_ = 0;
    return;
  }
  graph::Camera probe;
  const auto live = view_owner_->camera_state();
  probe.yaw = static_cast<double>(live.yaw);
  probe.pitch = static_cast<double>(live.pitch);
  probe.distance = static_cast<double>(live.distance);
  probe.fov_degrees = static_cast<double>(live.fov_y_degrees);
  graph::ProjectOptions options;
  options.viewport_x = 0.0;
  options.viewport_y = 0.0;
  options.viewport_width = static_cast<double>(bounds_.width);
  options.viewport_height = static_cast<double>(bounds_.height);
  const auto stats = graph::visibility_stats(graph::project(graph_, probe, options));
  visible_nodes_ = stats.visible;
  // 绘制计数（诊断）：与 `ThreeView::paint_frames` 同一用意——区分
  // 「没画」（帧被跳过/被剔除）与「画了但内容不对」（相机/几何）。
  ++paint_frames_;
}

auto GraphView::semantics_text() const -> std::string {
  return std::format("关系图：{} 节点 / {} 边", graph_.nodes.size(), graph_.edges.size());
}

// ── 属性/动作面 ───────────────────────────────────────────────────────────────

auto GraphView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "nodes") return std::to_string(graph_.nodes.size());
  if (name == "edges") return std::to_string(graph_.edges.size());
  if (name == "visible_nodes") return std::to_string(visible_nodes_);
  if (name == "paint_frames") return std::to_string(paint_frames_);
  if (name == "objects") return std::to_string(object_count());
  if (name == "labels") return std::to_string(painted_labels());
  if (name == "error") return last_error_;
  if (name == "backend") return backend_preference();
  if (name == "render_scale") return std::format("{:.3f}", static_cast<double>(render_scale()));
  if (name == "camera") {
    const auto camera = view_owner_->camera_state();
    return std::format("yaw={:.4f} pitch={:.4f} distance={:.2f}",
                       static_cast<double>(camera.yaw), static_cast<double>(camera.pitch),
                       static_cast<double>(camera.distance));
  }
  return Element::get_property(name);
}

auto GraphView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "backend") {
    set_backend(value);
    return true;
  }
  if (name == "render_scale") {
    set_render_scale(static_cast<float>(std::strtod(std::string(value).c_str(), nullptr)));
    return true;
  }
  return Element::set_property(name, value);
}

auto GraphView::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.emplace_back("backend");
  names.emplace_back("render_scale");
  return names;
}

auto GraphView::readonly_property_names() const -> std::vector<std::string_view> {
  // ⚠ 必须单独声明：控制通道的属性快照只遍历 `property_names()` + 本方法，
  // 写在 `get_property` 里而没列进来就**永远不会被导出**。
  auto names = Element::readonly_property_names();
  names.emplace_back("nodes");
  names.emplace_back("edges");
  names.emplace_back("visible_nodes");
  names.emplace_back("paint_frames");
  names.emplace_back("objects");
  names.emplace_back("labels");
  names.emplace_back("error");
  names.emplace_back("camera");
  return names;
}

auto GraphView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "reset_view" || action == "reset_camera") {
    reset_camera();
    return true;
  }
  if (action == "show") {
    set_graph_visible(true);
    return true;
  }
  if (action == "hide") {
    set_graph_visible(false);
    return true;
  }
  if (action == "frame") {
    frame_graph();
    sync_labels();
    mark_dirty();
    return true;
  }
  if (action == "select" && !argument.empty()) {
    // 按 id 挑一个节点并回报（供脚本/自动化核验）。找不到 = false（如实上报）。
    for (std::size_t index = 0; index < graph_.nodes.size(); ++index) {
      if (graph_.nodes[index].id != argument) continue;
      if (on_pick) on_pick(GraphPick{graph_.nodes[index].id, graph_.nodes[index].label, index});
      return true;
    }
    return false;
  }
  return Element::invoke_action(action, argument);
}

// ── 诊断 ──────────────────────────────────────────────────────────────────────

auto GraphView::object_count() const -> std::size_t { return view_owner_->object_count(); }
auto GraphView::painted_labels() const -> std::size_t { return labels_owner_->painted_labels(); }
auto GraphView::painted_edges() const -> std::size_t { return labels_owner_->painted_edges(); }

}  // namespace st::ui
