#pragma once

/// @file graph_view.hpp
/// @brief `GraphView`：把一张**关系图**画成可转视角的三维图（连线在三维、文字在屏幕）。
///
/// ## 它由什么组成（三个已知的部分 + 一层胶水）
///
/// | 组成 | 职责 |
/// |---|---|
/// | `graph_layout`（`st/ui/graph_layout.hpp`） | 布局、投影、拾取——纯计算 |
/// | `ThreeView` | 三维连线（深度缓冲、光照、真透视） |
/// | `LabelOverlay` | 屏幕文字标签 + 避让（三维里没有文字，见下） |
/// | **本组件** | 把三者接起来：图 → 三维脚本、按包围球取景、每帧相机同步、属性/动作面 |
///
/// 这一层原先在册府（知识库应用）的 `main.cpp` 里，约 400 行胶水。搬到框架里
/// 的理由不是"少写代码"，而是那 400 行里的每一段都是**踩出来的结论**，而任何
/// 想画关系图的应用都会再踩一遍：
///
/// - 为什么文字不在三维里画（脚本宿主无 Canvas 2D，Sprite 路线不通）；
/// - 为什么自动取景不能调 `frame_scene`（它按物理像素算，DPI 被多算一次）；
/// - 为什么标签层必须关掉输入拦截（否则旋转/缩放全失效）；
/// - 为什么相机要每帧同步给标签层（否则"几何转了、字不转"）；
/// - 为什么视口要绝对定位且由 C++ 挂（XML 排流会把它压成 0 尺寸）。
///
/// ## 三维画结构、二维画文字（这条分工是不得已，也是最优）
///
/// three.js 跑在 QuickJS 里，而脚本宿主**没有 Canvas 2D**——常规的
/// 「Canvas 画字 → 转纹理 → Sprite」走不通，用几何体拼汉字更不现实。
/// 所以：**三维只画连线（+ 可选标记），文字交给屏幕上的标签层**，两层叠在同一块
/// 区域。附带好处是文字永远面向观察者、不受视角倾斜影响（三维软件里标签的通行做法）。
///
/// ## 连线为什么在三维里画（而不是二维叠层）
///
/// 曾在二维层"自己算投影 + 画直线"，理由是透视保持直线性、位置等价。**位置确实
/// 等价，但三维语义全丢**：深度调制（远的线该更细更淡）、遮挡（线该被近处的字挡住）、
/// 转视角时的层次——两套投影各算一遍，错位与滞后无法避免。而 three.js 给段几何就会
/// 自己做这些事。
///
/// ## 用法
///
/// ```cpp
/// auto view = std::make_unique<st::ui::GraphView>();
/// view->set_fill(true);                       // 吃满可用区域（它没有自然尺寸）
/// host->add_child(std::move(view));
/// st::ui::graph::Graph graph = build_my_graph();
/// st::ui::graph::layout(graph);
/// view->set_graph(graph);                     // 布局 → 三维脚本 → 自动取景
/// ```
///
/// 应用只负责**填数据**（谁是节点、谁连谁、什么颜色、谁该聚成一簇），
/// 不认识布局与投影。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/matrix.hpp"
#include "st/ui/components/label_overlay.hpp"
#include "st/ui/components/three_view.hpp"
#include "st/ui/element.hpp"
#include "st/ui/graph_layout.hpp"

namespace st::ui {

/// 一次节点点击（应用据此跳转到详情）。
struct GraphPick {
  /// 被点中节点的 id / 标签（来自 `graph::Node`）。
  std::string id;
  std::string label;
  /// 节点下标（对应 `set_graph` 时的 `graph.nodes` 顺序）。
  std::size_t index{0};
};

/// 外观选项（尺寸、配色、取景留白都在这里，不散落在拼接脚本的地方）。
struct GraphStyle {
  /// 三维背景（十六进制 `0xRRGGBB`）。
  std::string background{"0x0B0E14"};
  /// 连线半径**相对布局包围半径**的比例。
  ///
  /// 为什么是比例而不是绝对值：布局坐标归一化到 `LayoutOptions::scale`（默认 170），
  /// 而 three.js 的世界尺度完全由调用方定。写死绝对值会与坐标尺度脱节——册府实测：
  /// 坐标 ±145 配半径 0.11，球比场景小 1300 倍，截图上**一片背景色**。
  ///
  /// 缺省 0.0028 是**GPU 腿**时代的调参：0.0055 是软件光栅器没有抗锯齿时的值
  /// （细圆柱会碎成断续脏点，只能用粗几何换"看着连续"）。换回软件腿时要重新调大。
  double edge_radius_ratio{0.0028};
  /// 是否画节点标记（小球）。默认关。
  ///
  /// 为何默认关（用户原话："球都去掉"）：球只能表达"这里有个点"，而图谱要表达的是
  /// "这里是什么"——文字已经在同一位置说了是什么，再叠一个球只是噪声，而且球的
  /// 光晕会盖住文字、加重密集区的拥挤。
  bool node_markers{false};
  /// 节点标记半径（相对包围半径；仅 `node_markers` 为真时用）。
  double node_marker_ratio{0.004};
  /// 取景留白系数（1.12 ≈ 四周各留 6%）。
  double framing_margin{1.12};
  /// 相机垂直视野（度）。与 `graph_layout::framing_distance` 用同一个值。
  double fov_degrees{46.0};
  /// 关掉物体自转。图谱的结构本身就是信息，自己动会干扰"看清哪里连哪里"，
  /// 所以缺省关。
  bool animate{false};
};

/// 关系图视图。
///
/// ## 它自己持有相机（而不是让 ThreeView 决定）
///
/// 因为标签层要用**同一台相机**投影：`ThreeView::project_to_screen` 是唯一知道
/// 真实相机的地方，所以每帧把投影器指向它（见 `sync_labels`）。若本组件另存一套
/// 相机再去算投影，就会退化成"两套计算"，转镜头后文字与几何必然逐渐错位——
/// 册府为此踩过四轮。
class GraphView : public Element {
 public:
  GraphView();
  ~GraphView() override;
  GraphView(const GraphView&) = delete;
  auto operator=(const GraphView&) = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "GraphView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // ── 内容 ──

  /// 换图（**已在调用方布局过**；未经布局的节点坐标全为 0，会缩成一点）。
  ///
  /// 内部顺序：生成三维脚本 → 交给视口 → 按包围球取景 → 用同一台相机把节点
  /// 转成标签交给标签层。返回 false = 三维脚本装载失败（错误在 `last_error()`），
  /// 此时**标签层保持旧内容**（与视口"失败不清空"同一语义）。
  auto set_graph(const graph::Graph& graph) -> bool;

  /// 当前图（只读；应用可据此做统计或另画一套）。
  [[nodiscard]] auto graph() const -> const graph::Graph& { return graph_; }

  /// 外观（改完调用 `refresh_style()` 生效）。
  ///
  /// ⚠ 名字叫 `graph_style()` 而不是 `style()`：后者是 `Element::style()`（盒模型），
  /// 同名会把基类接口藏起来，外部的 `view->style().grow = true` 会静默编不过——
  /// 而这正是人们第一件想做的事。
  [[nodiscard]] auto graph_style() -> GraphStyle& { return graph_style_; }
  [[nodiscard]] auto graph_style() const -> const GraphStyle& { return graph_style_; }
  /// 把外观重新应用到两个子视图（背景色/自转/线宽比例）。
  void refresh_style();

  // ── 显隐与相机（脚本/控制通道用）──

  /// 显示/隐藏整张图（两个子视图一起，避免"只隐藏了连线"）。
  void set_graph_visible(bool visible);
  [[nodiscard]] auto graph_visible() const -> bool;
  /// 回到初始视角。
  void reset_camera();
  /// 取/设相机（`yaw`/`pitch`/`distance` 之外的状态原样保留）。
  [[nodiscard]] auto camera_state() const -> OrbitCameraState;
  void set_camera_state(OrbitCameraState state);
  /// 按当前图的包围球自动取景（换图后 `set_graph` 已自动调过）。
  void frame_graph();

  /// 三维后端（`""` = 自动；`"software"` / `"gpu"` 强制）。
  ///
  /// 缺省给 `"gpu"`：册府实测 GPU 腿比软件腿快 **57 倍**
  /// （3533×1870 全分辨率旋转 5ms vs 285ms），因为三维纹理零拷贝合成到 GPU 主画布。
  void set_backend(std::string_view backend);
  [[nodiscard]] auto backend_preference() const -> std::string;
  void set_render_scale(float scale);
  [[nodiscard]] auto render_scale() const -> float;

  /// 吃满可用区域（宽度与高度都跟随父容器）。
  ///
  /// ## 为什么必须有这个开关（四次试错的结论）
  ///
  /// 视口没有"内容尺寸"可推，所以它在纵向布局里报自己的自然高度（320）。想让它
  /// 铺满主体区域只能显式告诉它，而**自己算尺寸的每条路都不通**（册府实测）：
  /// ① `left+top+right+bottom` → 高 320（right/bottom 需"自身宽"参与，而它是 auto）；
  /// ② 取父 `bounds()` → 拿到 0（布局时机未算完）；③ `kAuto` → 塌成 0；
  /// ④ 取窗口全尺寸 → 盖满了但内容没画（全白）。
  void set_fill(bool fill);

  /// 绘制阶段每秒刷新的可见节点数（本帧）。
  mutable std::size_t visible_nodes_{0};
  /// 本组件 `paint_content` 真正执行的帧数（诊断）。
  mutable std::uint64_t paint_frames_{0};

  /// 选中/悬浮回调（点中节点）。
  std::function<void(const GraphPick&)> on_pick{};

  // ── 诊断（截图"一片背景色"时用它区分是相机错、没几何，还是被裁掉）──

  /// 最近一次装载错误（空 = 成功）。
  [[nodiscard]] auto last_error() const -> std::string { return last_error_; }
  /// 本帧可见节点数 / 总数。
  [[nodiscard]] auto visible_node_count() const -> std::size_t { return visible_nodes_; }
  /// 三维场景里的物体数（连线 + 锚点）。
  [[nodiscard]] auto object_count() const -> std::size_t;
  /// 标签层实际画出的文字数（证明"该画的画了"，而不是靠看截图）。
  [[nodiscard]] auto painted_labels() const -> std::size_t;
  /// 标签层实际画出的连线数（当前版本连线在三维里，故通常为 0）。
  [[nodiscard]] auto painted_edges() const -> std::size_t;
  /// 本组件**真正进入绘制**的帧数。
  ///
  /// 为何要有它："屏幕上什么都没有"有两种截然不同的原因——
  /// ① **没画**（帧被跳过 / 被视口剔除 / 元素不可见）；
  /// ② **画了但内容不对**（相机太远、场景没装载、几何在视野外）。
  /// 没有这个计数就只能靠猜（与 `ThreeView::paint_frames_` 同一用意）。
  [[nodiscard]] auto paint_frames() const noexcept -> std::uint64_t { return paint_frames_; }

  // ── Element 接口 ──

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void apply_theme(const Theme& theme) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 每帧同步（相机 → 标签投影器）。**宿主每帧调一次**。
  ///
  /// ## 为何必须由宿主调（而不是在 `paint_content` 里做）
  ///
  /// 与 `ThreeView::tick_animation` 同一条理由：`UiRoot` 绘制前会做视口剔除，
  /// 与裁剪区无交集的元素整块不画。若同步只发生在 `paint_content` 里，那么
  /// "没登记损坏 → 被剔除 → 不执行 → 更不会登记损坏"就形成死锁，
  /// 症状是"一切都正常，只有屏幕上不对"。
  ///
  /// 返回 true = 本帧有变化（调用方可据此重绘）。
  auto tick() -> bool;

 private:
  /// 生成 three.js 脚本（图 → 场景）。
  [[nodiscard]] auto build_scene_script(const graph::Graph& graph) const -> std::string;
  /// 空图脚本（还没有数据时给一句提示，而不是一片黑）。
  [[nodiscard]] auto build_empty_script() const -> std::string;
  /// 把节点转成标签交给标签层（用视口当前相机投影，不自己算）。
  void sync_labels();
  /// 相机变了才重建投影器并重绘（静止时零开销）。
  auto sync_camera() -> bool;

  graph::Graph graph_{};
  GraphStyle graph_style_{};
  std::string last_error_{};

  std::unique_ptr<ThreeView> view_{};
  std::unique_ptr<LabelOverlay> labels_{};
  /// 挂树后的裸指针（所有权在 `Element::children_` 里；**不要**再用 `view_`/`labels_`
  /// ——它们已 move 出去，是空悬）。
  ThreeView* view_owner_{nullptr};
  LabelOverlay* labels_owner_{nullptr};
  bool has_graph_{false};
  bool syncing_{false};

  /// 上一帧相机（只在变化时重建投影器）。
  float last_yaw_{1.0e30F};
  float last_pitch_{1.0e30F};
  float last_distance_{1.0e30F};
};

}  // namespace st::ui

// ── 本组件**不进声明式注册表** ─────────────────────────────────────────
//
// 它是三维族：构造本身不报错，但声明式创建（`dsl::make_element`）在无头环境
// 没有意义，而且 gallery 对账要求"注册了就得上卡或写豁免"。它由 C++ 直接
// `new` 并挂树，不走 `make_element`。
//
// 需要它参与声明式创建时：加 `ST_UI_COMPONENT` + `.cpp` 里 `ST_UI_COMPONENT_IMPL`，
// 并在 `gallery_exhibits.hpp` 补一张卡（或写一条豁免理由）。
