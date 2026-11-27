/// 「能力矩阵」页：把 gallery 里**从未有过展位**的那些能力摆出来。
///
/// ## 为什么需要这一页
///
/// 组件页回答"有哪些控件"，却回答不了这几类问题：
///
/// - 三维（`ThreeView` / `SceneView` / `GraphView` / `LabelOverlay`）到底能不能画？
///   不能的话是**本后端不支持**还是**我用错了**？
/// - `SlowContent` 的异步排版在真实界面上是什么观感（骨架 → 内容）？
/// - XML UI / 脚本宿主 / 热重载 / 样式表这些"通路型"能力，怎么验它活着？
///
/// 这些能力有一个共同点：**没有它们也能跑，坏了也看不出来**。所以这一页不只摆出来，
/// 每一项还给出「怎么验」——把"能力存在性"变成可复算的判据。
///
/// ## 三维族的摆法（与文档一致）
///
/// `ThreeView` / `GraphView` / `LabelOverlay` **不在声明式注册表**里（无头环境按名建
/// 三维没有意义），由 C++ 直接 `new` 挂树。本页因此是手搭：这与"页面用手搭还是声明式"
/// 无关，纯粹是被挂的组件决定的。

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pages.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/card.hpp"
#include "st/ui/components/graph_view.hpp"
#include "st/ui/components/heading.hpp"
#include "st/ui/components/key_value_row.hpp"
#include "st/ui/components/label_overlay.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/scene_view.hpp"
#include "st/ui/components/slow_content.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/components/three_view.hpp"
#include "st/ui/graph_layout.hpp"

namespace gallery {
namespace {

using st::math::Insets;
using st::ui::Button;
using st::ui::Card;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Heading;
using st::ui::KeyValueRow;
using st::ui::Panel;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::Tone;

/// 一份最小但"看得出是三维"的脚本：球 + 盒 + 底座 + 一盏方向光。
///
/// 为什么自己写而不读 `assets/models/*.json`：那些是**场景描述**（另一种格式），
/// 而 `ThreeView` 吃的是 three.js 脚本。两者不是一回事——这一页要演示的正是前者之外的
/// 后者（`load_script` 这条通路），所以脚本必须内联在代码里、能一眼看完。
inline constexpr std::string_view kThreeScript = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 16/9, 0.05, 100);
camera.position.set(0, 1.15, 4.6);
camera.lookAt(0, 0.42, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0x8fa4c8, 0.7));
const key = new THREE.DirectionalLight(0xffffff, 2.2);
key.position.set(3.2, 4.6, 3.4);
scene.add(key);

const ball = new THREE.Mesh(new THREE.SphereGeometry(0.62, 32, 24),
  new THREE.MeshStandardMaterial({ color: 0xd08c50, metalness: 0.35, roughness: 0.32 }));
ball.name = 'ball';
ball.position.set(-0.78, 0.7, 0);
scene.add(ball);

const box = new THREE.Mesh(new THREE.BoxGeometry(0.92, 0.92, 0.92),
  new THREE.MeshStandardMaterial({ color: 0x4a86d0, metalness: 0.2, roughness: 0.45 }));
box.name = 'box';
box.position.set(0.82, 0.48, 0);
scene.add(box);

const base = new THREE.Mesh(new THREE.BoxGeometry(3.0, 0.1, 1.5),
  new THREE.MeshStandardMaterial({ color: 0x2b3442, roughness: 0.85 }));
base.name = 'base';
scene.add(base);
scene;
)js";

/// 造一张"能力清单"表（能力名 / 本机可用性 / 怎么验）。
[[nodiscard]] auto make_capability_table() -> std::unique_ptr<Table> {
  std::vector<TableColumn> columns{
      TableColumn{.name = "能力", .width = 168.0f},
      TableColumn{.name = "本机", .width = 96.0f},
      TableColumn{.name = "怎么验（判据）"},
  };
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_id("capability-table");
  table->style().height = 250.0f;
  table->set_row_height(30.0f);
  table->set_zebra(true);

  const bool three_ready = st::ui::SceneView::rendering_ready();
  const auto add = [&table](std::string name, std::string state, std::string how,
                            Table::CellTone tone) {
    table->add_row({std::move(name), std::move(state), std::move(how)});
    table->set_cell_tone(table->row_count() - 1, 1U, tone);
  };
  add("软件光栅器（软渲染腿）", "可用",
      "本页任何一张卡能画出像素就是它；metrics.renderer=software", Table::CellTone::Accent);
  add("三维：SceneView", three_ready ? "可用" : "不可用",
      "下面那张三维卡的画面 + get 它的 shape/spin（只读属性面）",
      three_ready ? Table::CellTone::Accent : Table::CellTone::Danger);
  add("三维：ThreeView", three_ready ? "可用" : "不可用",
      "拖动旋转/滚轮缩放/双击复位；get object_count 与 last_error",
      three_ready ? Table::CellTone::Accent : Table::CellTone::Danger);
  add("关系图：GraphView", three_ready ? "可用" : "不可用",
      "set_graph 后 get visible_node_count / painted_labels（截图一片黑时用它分辨原因）",
      three_ready ? Table::CellTone::Accent : Table::CellTone::Danger);
  add("二维叠层：LabelOverlay", "可用",
      "项目器注入后 painted_labels > 0 即证明「该画的画了」（无需截图）",
      Table::CellTone::Accent);
  add("异步排版：SlowContent", "可用",
      "layout_pending 由 true 转 false、last_layout_ms > 0；期间画骨架占位",
      Table::CellTone::Accent);
  add("XML UI（build_from_xml）", "可用",
      "tests/xml_ui_* 全套；未知标签会进 XmlBuildReport::unknown_tags（不静默）",
      Table::CellTone::Accent);
  add("脚本宿主（QuickJS）", "按需",
      "默认关闭：应用要 --enable-script；hello 的 capabilities 里有 script 才是开着的",
      Table::CellTone::Muted);
  add("热重载（HotReload）", "按需",
      "磁盘优先、内嵌兜底；开发期改 XML/JS 即生效，见 docs/hot-reload.md",
      Table::CellTone::Muted);
  add("原生子窗口（NativeHost）", "看后端",
      "无头退化占位块；真实窗口下 get attached / content_x 判对位（capture 拍不到）",
      Table::CellTone::Muted);
  add("嵌入浏览器（WebView）", "看平台",
      "Windows/WebView2 可用；get state / supported 如实上报",
      Table::CellTone::Muted);
  return table;
}

/// 造一张最小关系图（图 → 三维脚本 → 自动取景全部由 `GraphView` 负责）。
[[nodiscard]] auto make_demo_graph() -> st::ui::graph::Graph {
  using st::ui::graph::Edge;
  using st::ui::graph::Graph;
  using st::ui::graph::Node;
  Graph graph{};
  const auto add_node = [&graph](std::string id, std::string label, std::string kind) {
    Node node{};
    node.id = std::move(id);
    node.label = std::move(label);
    node.kind = std::move(kind);
    graph.nodes.push_back(std::move(node));
    return graph.nodes.size() - 1;
  };
  const std::size_t ui = add_node("ui", "ui 层", "layer");
  const std::size_t raster = add_node("raster", "raster", "layer");
  const std::size_t text = add_node("text", "text", "layer");
  const std::size_t app = add_node("app", "app", "layer");
  const std::size_t control = add_node("control", "control", "layer");
  const std::size_t pkg = add_node("pkg", "pkg", "layer");
  (void)add_node("core", "core", "layer");
  const auto add_edge = [&graph](std::size_t from, std::size_t to, std::uint32_t argb) {
    Edge edge{};
    edge.from = from;
    edge.to = to;
    edge.argb = argb;
    graph.edges.push_back(std::move(edge));
  };
  add_edge(ui, raster, 0xFF7FA8F0U);
  add_edge(ui, text, 0xFF7FA8F0U);
  add_edge(ui, app, 0xFF9BE0A8U);
  add_edge(app, control, 0xFFE8C072U);
  add_edge(app, pkg, 0xFFE8C072U);
  return graph;
}

}  // namespace

/// 构建能力矩阵页。
[[nodiscard]] auto build_capabilities_page(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id("page-capabilities");
  page->style().padding = Insets::all(24.0f);
  page->style().gap = 16.0f;
  page->add_child(std::make_unique<Heading>("能力矩阵", 2));
  auto sub = std::make_unique<Text>(
      "这些能力在 gallery 里此前没有任何展位——它们的共同点是：没有也能跑，坏了也看不出来。"
      "所以每一项都给出「怎么验」的判据，而不是只摆一个样子。");
  sub->set_tone(Tone::Muted);
  page->add_child(std::move(sub));

  // —— 能力清单 ——
  auto table_card = std::make_unique<Card>(16.0f);
  table_card->set_id("card-capability-table");
  table_card->style().direction = FlexDirection::Column;
  table_card->style().gap = 8.0f;
  auto table_title = std::make_unique<Text>("能力 / 本机可用性 / 判据");
  table_title->set_weight(st::ui::FontWeight::SemiBold);
  table_card->add_child(std::move(table_title));
  table_card->add_child(make_capability_table());
  page->add_child(std::move(table_card));

  // —— 三维：SceneView（吃内置形状，无交互）——
  auto scene_card = std::make_unique<Card>(16.0f);
  scene_card->set_id("card-scene-view");
  scene_card->style().direction = FlexDirection::Column;
  scene_card->style().gap = 8.0f;
  auto scene_title = std::make_unique<Text>("SceneView（内置形状 · 自转）");
  scene_title->set_weight(st::ui::FontWeight::SemiBold);
  scene_card->add_child(std::move(scene_title));
  auto scene_hint = std::make_unique<Text>(
      "用途是「界面里放个转着的模型当装饰」，所以它没有相机状态机与拾取——"
      "那两样属于下面的 ThreeView。");
  scene_hint->set_tone(Tone::Faint);
  scene_hint->set_scale(Text::Scale::Xs);
  scene_card->add_child(std::move(scene_hint));
  auto scene_row = std::make_unique<Panel>(FlexDirection::Row);
  scene_row->style().gap = 16.0f;
  scene_row->style().wrap = true;
  {
    auto cube = std::make_unique<st::ui::SceneView>(st::ui::SceneShape::Cube);
    cube->set_id("scene-cube");
    cube->set_preferred_height(180.0f);
    cube->style().width = 260.0f;
    scene_row->add_child(std::move(cube));
    auto sphere = std::make_unique<st::ui::SceneView>(st::ui::SceneShape::Sphere);
    sphere->set_id("scene-sphere");
    sphere->set_preferred_height(180.0f);
    sphere->style().width = 260.0f;
    scene_row->add_child(std::move(sphere));
  }
  scene_card->add_child(std::move(scene_row));
  page->add_child(std::move(scene_card));

  // —— 三维：ThreeView（three.js 脚本 + 轨道相机 + 拾取）——
  auto three_card = std::make_unique<Card>(16.0f);
  three_card->set_id("card-three-view");
  three_card->style().direction = FlexDirection::Column;
  three_card->style().gap = 8.0f;
  auto three_title = std::make_unique<Text>("ThreeView（three.js 脚本 · 轨道相机 · 拾取）");
  three_title->set_weight(st::ui::FontWeight::SemiBold);
  three_card->add_child(std::move(three_title));
  auto three_hint = std::make_unique<Text>(
      "左键拖拽旋转 · 滚轮推拉 · 右键/中键平移 · 双击复位。装载失败**不清空**已有场景"
      "（错误进 last_error），所以改坏一行脚本不会让画面消失。");
  three_hint->set_tone(Tone::Faint);
  three_hint->set_scale(Text::Scale::Xs);
  three_card->add_child(std::move(three_hint));
  auto three = std::make_unique<st::ui::ThreeView>();
  three->set_id("three-view");
  three->style().height = 320.0f;
  three->set_render_scale(0.75f);   // 软件腿逐像素算：半分辨率换来跟手的旋转
  const bool three_ok = three->load_script(kThreeScript, "capabilities-demo.js");
  // 取景由 `ThreeView` 自己在拿到真实尺寸后算（`frame_pending_`，见 three_view.cpp）：
  // 装载发生在 arrange 之前时它只记一个待办，第一次拿到宽高才真正解距离。
  auto* three_ptr = three.get();
  three_card->add_child(std::move(three));
  auto three_state = std::make_unique<Text>(
      three_ok ? std::format("已装载：{} 个物体", three_ptr->object_count())
               : std::format("装载失败：{}", three_ptr->last_error()));
  three_state->set_id("three-view-state");
  three_state->set_tone(three_ok ? Tone::Success : Tone::Danger);
  three_state->set_scale(Text::Scale::Xs);
  auto* three_state_ptr = three_state.get();
  three_card->add_child(std::move(three_state));
  auto three_actions = std::make_unique<Panel>(FlexDirection::Row);
  three_actions->style().gap = 8.0f;
  three_actions->style().wrap = true;
  auto reset_button = std::make_unique<Button>("复位视角", Button::Variant::Ghost,
                                               Button::Size::Small);
  reset_button->set_id("btn-three-reset");
  auto* reset_ptr = reset_button.get();
  three_actions->add_child(std::move(reset_button));
  auto reload_button = std::make_unique<Button>("重载脚本", Button::Variant::Neutral,
                                                Button::Size::Small);
  reload_button->set_id("btn-three-reload");
  auto* reload_ptr = reload_button.get();
  three_actions->add_child(std::move(reload_button));
  three_card->add_child(std::move(three_actions));
  page->add_child(std::move(three_card));

  reset_ptr->on_click = [three_ptr, three_state_ptr, hooks]() {
    three_ptr->reset_view();
    if (hooks.set_status) hooks.set_status("三维视角已复位");
    if (three_state_ptr != nullptr) {
      three_state_ptr->set_content(
          std::format("视角已复位 · 物体 {}", three_ptr->object_count()));
    }
  };
  reload_ptr->on_click = [three_ptr, three_state_ptr, hooks]() {
    const bool ok = three_ptr->load_script(kThreeScript, "capabilities-demo.js");
    if (ok) three_ptr->reset_view();
    if (three_state_ptr != nullptr) {
      three_state_ptr->set_content(ok ? std::format("已重载：{} 个物体", three_ptr->object_count())
                                      : std::format("装载失败：{}", three_ptr->last_error()));
    }
    if (hooks.set_status) hooks.set_status(ok ? "三维脚本已重载" : "三维脚本装载失败");
  };

  // —— 关系图：GraphView（图 → 三维脚本 → 标签层，全自动）——
  auto graph_card = std::make_unique<Card>(16.0f);
  graph_card->set_id("card-graph-view");
  graph_card->style().direction = FlexDirection::Column;
  graph_card->style().gap = 8.0f;
  auto graph_title = std::make_unique<Text>("GraphView（关系图：三维连线 + 屏幕标签）");
  graph_title->set_weight(st::ui::FontWeight::SemiBold);
  graph_card->add_child(std::move(graph_title));
  auto graph_hint = std::make_unique<Text>(
      "几何在三维里画（深度调制/遮挡/透视都由渲染层负责），文字在屏幕叠层——"
      "因为脚本宿主没有 Canvas 2D，用几何体拼汉字不现实。");
  graph_hint->set_tone(Tone::Faint);
  graph_hint->set_scale(Text::Scale::Xs);
  graph_card->add_child(std::move(graph_hint));
  auto graph = std::make_unique<st::ui::GraphView>();
  graph->set_id("graph-view");
  // 不用 set_fill(true)：那个开关的语义是"盖住整个内容区"（自身也绝对定位），
  // 而这里它只是卡片里的一块。绝对定位在流式列里会拿到 0 可用高，
  // 实测卡片高度被压成 100px、整张图不可见——**画面上像"图没加载"**。
  // 显式高度才是这块视口在这个布局里的正确表达。
  graph->style().height = 340.0f;
  st::ui::graph::Graph demo_graph = make_demo_graph();
  st::ui::graph::layout(demo_graph);
  (void)graph->set_graph(demo_graph);
  // ⚠ `set_graph()` **不会**把两个子视图打开：`ThreeView` 与 `LabelOverlay` 在构造里
  // 都是隐藏的（空图时不该占着一块黑）。不调这一行就是“树里参数全对、屏幕上一片
  // 背景色”——`objects`/`visible_nodes`/`labels` 全部正常，只有 `paint_frames` 是 0。
  // 头注释的用法示例里漏了这一步（已记入 docs/BACKLOG.md）。
  graph->set_graph_visible(true);
  auto* graph_ptr = graph.get();
  graph_card->add_child(std::move(graph));
  auto graph_state = std::make_unique<Text>(
      std::format("节点 {} · 边 {} · 可见节点 {} · 已画标签 {}", demo_graph.nodes.size(),
                  demo_graph.edges.size(), graph_ptr->visible_node_count(),
                  graph_ptr->painted_labels()));
  graph_state->set_id("graph-view-state");
  graph_state->set_tone(Tone::Muted);
  graph_state->set_scale(Text::Scale::Xs);
  graph_card->add_child(std::move(graph_state));
  page->add_child(std::move(graph_card));

  // —— 异步排版：SlowContent ——
  //
  // 这里演示的是"壳的契约"而不是真去排一份大文档：内容组件要自己实现
  // `SlowLayoutTarget`（快照 → 后台排 → 采纳），而 gallery 里没有那样的内容组件
  // （`MarkdownView` 有那一套，但装配它需要文本端口，属于 gbcode 的形态）。
  // 所以这一卡**如实说明**，而不是造一个假实现让页面"看起来有"。
  auto slow_card = std::make_unique<Card>(16.0f);
  slow_card->set_id("card-slow-content");
  slow_card->style().direction = FlexDirection::Column;
  slow_card->style().gap = 6.0f;
  auto slow_title = std::make_unique<Text>("SlowContent（异步排版壳）");
  slow_title->set_weight(st::ui::FontWeight::SemiBold);
  slow_card->add_child(std::move(slow_title));
  auto slow_body = std::make_unique<Text>(
      "壳负责调度（同步/异步分档、占位骨架、取消与去抖），内容组件实现 "
      "SlowLayoutTarget（快照 → 后台排版 → 采纳）。\n"
      "真装配要一个实现了该接口的内容组件（gbcode 的 Markdown 预览就是：447KB 的 "
      "DESIGN.md 同步排要 160ms，投后台后主线程单帧 < 1ms）。\n"
      "判据：layout_pending 由 true 转 false、last_layout_ms > 0、期间画骨架占位。");
  slow_body->set_tone(Tone::Muted);
  slow_body->set_scale(Text::Scale::Xs);
  slow_card->add_child(std::move(slow_body));
  slow_card->add_child(std::make_unique<KeyValueRow>(
      "在 gallery 里的形态", "如实说明（不造假实现）；真装配见 apps/gbcode 的 md-preview-shell"));
  page->add_child(std::move(slow_card));

  // —— 通路型能力：怎么验 ——
  auto how_card = std::make_unique<Card>(16.0f);
  how_card->set_id("card-capability-how");
  how_card->style().direction = FlexDirection::Column;
  how_card->style().gap = 4.0f;
  auto how_title = std::make_unique<Text>("通路型能力：一条命令就能验");
  how_title->set_weight(st::ui::FontWeight::SemiBold);
  how_card->add_child(std::move(how_title));
  how_card->add_child(std::make_unique<KeyValueRow>(
      "XML UI", "st test xml_ui——未知标签/属性会进报告，不静默消失"));
  how_card->add_child(std::make_unique<KeyValueRow>(
      "声明式重组", "本页与「声明式」页；tools/gallery_declarative_e2e.py 走完整交互链"));
  how_card->add_child(std::make_unique<KeyValueRow>(
      "控制通道", "tools/events_e2e.py / input_action_e2e.py / visual_assert_e2e.py"));
  how_card->add_child(std::make_unique<KeyValueRow>(
      "热重载", "改 ui/*.xml 或 *.js 保存即生效（磁盘优先、内嵌兜底）"));
  how_card->add_child(std::make_unique<KeyValueRow>(
      "像素回归", "visual_diff 与基线比对；capture_hash 判「变了没有」"));
  page->add_child(std::move(how_card));

  (void)hooks;
  return page;
}

}  // namespace gallery
