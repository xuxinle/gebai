#pragma once

/// `ThreeView`：把**一份 three.js 场景**渲进界面的一块区域，并支持鼠标交互。
///
/// ## 它与 `SceneView` 的分工
///
/// | | `SceneView` | `ThreeView`（本文件） |
/// |---|---|---|
/// | 内容 | 一个内置形状或一个外部网格 | 任意 three.js 脚本（多物体/程序化几何/层级） |
/// | 相机 | 固定 | 可轨道旋转 / 推拉 / 平移（用户操作） |
/// | 交互 | 无 | 悬浮高亮、点击选中、拖拽旋转、滚轮缩放、右键平移 |
/// | 拾取 | 无 | 屏幕点 → 世界射线 → 与真实三角形求交 |
///
/// 两者并存而不是二合一：`SceneView` 的用途是"界面里放个转着的模型当装饰"，
/// 它不该背相机状态机与拾取；本组件是"把三维当编辑器视口用"。
///
/// ## 相机怎样被操作（Orbit 模型）
///
/// 相机由三个量描述——**目标点**、**距离**、**两个角度（偏航/俯仰）**。
/// 这样做的理由：直接改 `eye` 会积累数值漂移（拖一千次后距离会缓慢变化），
/// 而球坐标天然把"绕目标转"和"靠近/远离"分开，且俯仰可以夹住
/// （否则会翻过头变成倒置视角——那种状态用户无法自行恢复）。
///
/// | 操作 | 行为 |
/// |---|---|
/// | 左键拖拽 | 绕目标点旋转（偏航 × 俯仰，俯仰夹在 ±89°） |
/// | 滚轮 | 推拉距离（按 1.12 的等比缩放，并对数域夹取，避免"越缩越快"） |
/// | 右键/中键拖拽 | 平移目标点（沿相机的右/上方向，位移按当前距离缩放） |
/// | 双击 | 回到初始视角 |
///
/// ## 拾取为什么用 CPU 射线而不是 ID 缓冲
///
/// ID 缓冲要额外一张渲染目标 + 一次回读，而回读在 GPU 腿上是**最贵的单项**
/// （本仓实测 1080p 回读 100ms 量级）。而三维视口的物体数通常是几十个，
/// 一次 CPU 宽相（世界 AABB）+ 窄相（三角形）在微秒级。
/// 更重要的是：CPU 路径**与后端无关**，软件腿与 GPU 腿的拾取结果必然一致
/// （ID 缓冲则要在两条腿各实现一遍，那正是"降级就不是降级"的来源）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/math/collision.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 一次命中的结果（拾取）。
struct ThreeHit {
  /// 命中的物体名（three.js 里的 `mesh.name`；空串表示未命名）。
  std::string name{};
  /// 命中点（世界坐标）。
  math::Vec3 point{};
  /// 命中面法线（世界坐标，**朝射线来向**）。
  math::Vec3 normal{};
  /// 沿射线的距离。
  float distance{0.0f};
};

/// 相机状态（球坐标 + 目标点）。
///
/// 单独成结构是为了让"保存/恢复视角""脚本里设视角"都是纯数据操作。
struct OrbitCameraState {
  math::Vec3 target{0.0f, 0.4f, 0.0f};
  float distance{4.5f};
  /// 偏航（弧度，绕世界 Y 轴；0 = 相机在 +Z 侧）。
  float yaw{0.0f};
  /// 俯仰（弧度，正 = 从上方看）。
  float pitch{0.28f};
  float fov_y_degrees{42.0f};
  float near_z{0.05f};
  float far_z{100.0f};

  /// 由球坐标算出实际相机（`raster::Camera`）。
  [[nodiscard]] auto to_camera() const noexcept -> raster::Camera;

  /// 夹取到合法范围（俯仰 ±89°、距离 > near、fov 1..179）。
  ///
  /// 为什么必须有：俯仰越过 ±90° 会让 `up` 与视线共线，look_at 退化，
  /// 画面会瞬间翻转——而用户拖到那里之后**无法自己拖回来**（方向感全失）。
  void clamp() noexcept;
};

class ThreeView : public Element {
 public:
  ThreeView();
  ~ThreeView() override;
  ThreeView(const ThreeView&) = delete;
  auto operator=(const ThreeView&) -> ThreeView& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ThreeView"; }

  // ── 内容 ──

  /// 装载一份 three.js 脚本（重新求值整个场景，相机保持用户当前视角）。
  ///
  /// 失败时**不清空**已有的场景（否则改坏一行脚本会让画面消失，
  /// 而用户以为程序坏了）；错误留给 `last_error()`，界面角标会显示它。
  auto load_script(std::string_view script, std::string_view source_name = "<inline>") -> bool;

  /// 最近一次装载是否成功的原因（成功时为空）。
  [[nodiscard]] auto last_error() const -> const std::string& { return load_error_; }
  /// 当前场景的物体数。
  [[nodiscard]] auto object_count() const noexcept -> std::size_t { return scene_.objects.size(); }
  /// 已装载的场景（只读；调用方可据此算包围盒、做自己的分析）。
  [[nodiscard]] auto scene() const noexcept -> const raster::ThreeScene& { return scene_; }

  // ── 相机 ──

  [[nodiscard]] auto camera_state() const noexcept -> const OrbitCameraState& { return orbit_; }
  void set_camera_state(const OrbitCameraState& state);
  /// 把相机**框到当前场景的包围盒**（改脚本后调用最方便）。
  ///
  /// 留空（`1.0`）时给 30% 余量；`margin` 是相对包围球半径的倍数。
  void frame_scene(float margin = 1.0f);
  /// 回到上次 `frame_scene`/`set_camera_state` 记下的视角。
  void reset_view();

  // ── 外观 ──

  void set_background(math::Color color) noexcept { background_ = color; }
  /// 悬浮高亮与选中描边用的颜色（默认取主题 accent，这里可覆盖）。
  void set_highlight_color(math::Color color) noexcept { highlight_override_ = color; }
  /// 是否显示左上角的状态角标（物体数/后端/装载错误）。默认开。
  void set_show_hud(bool enabled) noexcept { show_hud_ = enabled; }

  /// 三维后端偏好（`""` = 自动选；`"software"` / `"gpu"` 强制）。默认 `software`。
  ///
  /// 理由见 `backend_pref_` 的说明（GPU 腿在交互式视口里合成失效，
  /// 实测只剩背景色）。设成 `""` 则交回框架的实测选腿。
  void set_backend(std::string_view backend) {
    backend_pref_ = std::string(backend);
    backend_.reset();   // 下一帧按新偏好重建
    mark_dirty();
  }
  [[nodiscard]] auto backend_preference() const -> const std::string& { return backend_pref_; }

  // ── 交互开关（默认全开）──

  /// 是否按时间推进物体自转/振荡（默认 true）。
  ///
  /// 关掉用于“定格看一个姿态”：此时画面不再随时间变，截图可复现。
  void set_animation_enabled(bool enabled) noexcept;
  [[nodiscard]] auto animation_enabled() const noexcept -> bool { return animation_enabled_; }

  /// 主动推进动画时钟并登记损坏区（**宿主每帧调一次**）。
  ///
  /// ## 为何必须由宿主调，而不能在 `paint_content` 里做
  ///
  /// 这是本会话耗时最久的缺陷。`UiRoot` 绘制前会做**视口剔除**：
  /// 与当前裁剪区无交集的元素整块不画。而局部重绘时裁剪区只含损坏区。
  /// 于是形成死锁：
  ///
  ///   视口没被登记损坏 → 被剔除 → `paint_content` 不执行
  ///   → 里面的 `request_animation` / `record_damage` 永不执行
  ///   → 下一帧依旧没 damage → 依旧被剔除
  ///
  /// 症状极具欺骗性：物体数、网格（51.8 万顶点）、矩阵、后端全部正常，
  /// `three_check` 渲染同一份场景也正常，**只有屏幕上什么都没有**。
  ///
  /// 所以损坏登记必须发生在**绘制之外**——由宿主在主循环里每帧调一次。
  /// 返回 true 表示本帧有变化（调用方可据此重绘）。
  auto tick_animation() -> bool;

  void set_orbit_enabled(bool enabled) noexcept { orbit_enabled_ = enabled; }
  void set_zoom_enabled(bool enabled) noexcept { zoom_enabled_ = enabled; }
  void set_pan_enabled(bool enabled) noexcept { pan_enabled_ = enabled; }
  void set_pick_enabled(bool enabled) noexcept { pick_enabled_ = enabled; }

  /// 自然高度（未给定具体尺寸时用它；默认 320）。
  ///
  /// 为何需要：三维视口没有“内容高度”可推（几何在大地上无限铺）。
  /// 而在纵向布局里**不能**吃满 `available_height`——那会把同级的状栏挤成 0 高。
  void set_preferred_height(float height) noexcept {
    preferred_height_ = height > 0.0f ? height : 1.0f;
  }

  /// 是否**吃满可用高度**（默认 false）。
  ///
  /// 典型用法：视口是页面主体（本示例就是）时应吃满剩余空间；
  /// 而“界面里放个预览小窗”则不该（会把同级元素挤掉）。
  void set_fill_height(bool enabled) noexcept { fill_height_ = enabled; }

  // ── 观察结果 ──

  /// 当前悬浮命中的物体（无则空）。
  [[nodiscard]] auto hovered() const noexcept -> const std::optional<ThreeHit>& { return hovered_; }
  /// 当前选中的物体（无则空）。
  [[nodiscard]] auto selected() const noexcept -> const std::optional<ThreeHit>& { return selected_; }
  /// 拾取事件计数器（**用于测试与诊断**：证明"确实发生了拾取"而不是"没画高亮"）。
  [[nodiscard]] auto hover_events() const noexcept -> std::uint64_t { return hover_events_; }
  [[nodiscard]] auto click_events() const noexcept -> std::uint64_t { return click_events_; }

  /// 屏幕坐标（**组件内的逻辑像素**）→ 世界射线。
  ///
  /// 公开它是因为"从屏幕点算射线"是视口的公共语义：调用方（脚本、测试、
  /// 将来的拖拽放置）都会需要它，而它依赖相机与视口尺寸两个私有状态。
  [[nodiscard]] auto ray_from_point(math::Point local) const noexcept -> math::Ray;

  // ── Element ──

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;

  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 只读观测面（`objects`/`backend`/`picked`/`error`/计数等）。
  ///
  /// ⚠ 必须单独声明：控制通道的属性快照只遍历 `property_names()` +
  /// 本方法，写在 `get_property` 里而没列进来就**永远不会被导出**。
  [[nodiscard]] auto readonly_property_names() const
      -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 做一次拾取（`point` 是组件内逻辑坐标）。
  [[nodiscard]] auto pick(math::Point point) const -> std::optional<ThreeHit>;
  /// 当前高亮色（覆盖值优先，否则取主题 accent）。
  [[nodiscard]] auto highlight_color(const RenderContext& context) const noexcept -> math::Color;
  /// 把场景画进 `box`（`paint_content` 与自检共用）。
  void render_into(const RenderContext& context, raster::Surface& canvas, math::Rect box) const;

  raster::ThreeScene scene_{};
  /// 每个物体一份可拾取对象（世界 AABB 预算好，避免每次点击重算）。
  ///
  /// `bounds` 得在**装载时**算：它依赖网格与 world 矩阵，两者装载后都不变。
  /// 每次点击重算就是"点一下遍历几万个顶点"——实测在 2 万三角形的场景上有 3ms 级抖动。
  struct PickCache {
    std::string name{};
    raster::Bounds world_bounds{};
    std::size_t index{0};
  };
  std::vector<PickCache> pick_cache_{};

  OrbitCameraState orbit_{};
  OrbitCameraState home_{};
  bool has_home_{false};

  math::Color background_{0x0E, 0x12, 0x1A, 0xFF};
  std::optional<math::Color> highlight_override_{};
  bool show_hud_{true};
  float preferred_height_{320.0f};
  bool fill_height_{false};

  bool orbit_enabled_{true};
  bool zoom_enabled_{true};
  bool pan_enabled_{true};
  bool pick_enabled_{true};
  bool animation_enabled_{true};
  /// 动画时钟（秒）。由 `paint_content` 用 `RenderContext::time_seconds` 推进；
  /// 渲染与测试都不依赖墙钟，因此可复现。
  mutable double animation_time_{0.0};
  mutable double last_frame_time_{-1.0};
  /// `render_into` 真正执行的次数（诊断用）。
  ///
  /// 存在的理由：动画失效有两种截然不同的原因——“没画”（帧被跳过）与
  /// “画了但时间没走”。没有这个计数就只能靠猜。
  mutable std::uint64_t paint_frames_{0};
  /// 三维后端重建次数（诊断用）：若每帧都在重建，画面会永远停在初始时刻。
  mutable std::uint64_t backend_rebuilds_{0};
  /// 三维后端偏好（`""` = 自动选；`"software"` / `"gpu"` 强制）。
  ///
  /// ## 为何默认 `software`
  ///
  /// 自动选腿会挑实测更快的那条（通常 GPU）。但在**交互式视口**里，GPU 腿把离屏
  /// 纹理合成回画布那一步实测失效：`begin_frame` 的清屏色能看见，而网格完全不可见
  /// （整块只剩背景色）。软件腿逐像素直写画布，行为确定。
  /// 默认**自动选腿**（框架按实测挑更快的那条）。
  /// 曾把它默认成 software 试图绕开 GPU 合成问题，结果连原本正常的场景
  /// 也画不出来了——说明问题不在选腿。
  std::string backend_pref_{};

  std::optional<ThreeHit> hovered_{};
  std::optional<ThreeHit> selected_{};
  std::uint64_t hover_events_{0};
  std::uint64_t click_events_{0};

  std::string load_error_{};
  std::string source_name_{};

  // 拖拽状态（不是"按了哪个键"，而是"这次拖拽在干什么"）
  enum class DragMode : std::uint8_t { None, Orbit, Pan };
  DragMode drag_mode_{DragMode::None};
  math::Point drag_last_{};
  /// 按下时的位置与是否已移动超过阈值（用于区分"点击"与"拖拽"）。
  math::Point press_origin_{};
  bool press_moved_{false};

  /// 三维场景实例（按需创建；尺寸变化时重建）。
  mutable std::unique_ptr<raster::Scene3D> backend_{};
  /// 上一帧尺寸（物理像素），用于判断是否需要重建后端。
  mutable int backend_width_{0};
  mutable int backend_height_{0};
};

}  // namespace st::ui
