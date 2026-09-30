#pragma once

/// UI 树根：布局 → 绘制 → 事件分发 → 焦点 → 语义/视觉快照（控制通道的数据源）。
/// 无头模式与窗口模式走同一条路径：`layout()` + `paint(canvas)`，差异只在 shell 后端。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/element.hpp"
#include "st/ui/selector.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

class UiRoot {
 public:
  UiRoot();
  ~UiRoot();
  UiRoot(const UiRoot&) = delete;
  auto operator=(const UiRoot&) -> UiRoot& = delete;

  void set_content(std::unique_ptr<Element> content);
  [[nodiscard]] auto content() const noexcept -> Element* { return content_.get(); }

  [[nodiscard]] auto theme() noexcept -> Theme& { return theme_; }
  [[nodiscard]] auto theme() const noexcept -> const Theme& { return theme_; }
  void set_theme(Theme theme);

  /// 文本绘制端口（非拥有；为空时使用 `NullTextPort`）。
  void set_text_port(const TextPort* port);
  [[nodiscard]] auto text_port() const noexcept -> const TextPort* { return text_port_; }

  void set_viewport(math::Size size);
  [[nodiscard]] auto viewport() const noexcept -> math::Size { return viewport_; }

  /// 全树布局（脏时才重算）。
  void layout(bool force = false);
  /// 全树绘制（含遮罩裁剪与叠加层）。
  void paint(raster::Surface& canvas);

  /// 设定本帧的时间戳（秒）。
  ///
  /// 时间轴必须由应用推进：早先 `time_seconds_` **从未被赋值**（恒为 0），
  /// 于是所有基于时间的动画在真实应用里都走"静态帧直接落位"分支——
  /// 即**动画完全不播**（单元测试因为自己构造 RenderContext 而看不出来）。
  void set_time(double seconds) noexcept { time_seconds_ = seconds; }
  [[nodiscard]] auto time() const noexcept -> double { return time_seconds_; }

  /// 事件分发（命中测试 → 捕获链 → 冒泡；焦点/悬停状态随之更新）。
  [[nodiscard]] auto dispatch(Event& event) -> bool;

  /// 事件观察者：每次事件分发到具体元素后回调（事件引用 + 命中元素）。
  ///
  /// 用途是让脚本宿主把 UI 事件桥接给 JS，而**不需要 UiRoot 认识 ScriptHost**
  /// （保持 ui 内核与脚本层的单向依赖）。未设置时零开销。
  using EventObserver = std::function<void(const Event&, Element&)>;
  void set_event_observer(EventObserver observer) { event_observer_ = std::move(observer); }

  /// 手动通知事件观察者。
  ///
  /// 用途：协议 `invoke(click)` 这类**合成事件**不经过真实输入管线，但语义上就是一次点击，
  /// 脚本绑定必须能看到（否则"用 invoke 触发按钮、脚本却收不到"会变成难查的行为差异）。
  void notify_event_observer(const Event& event, Element& target) {
    if (event_observer_) event_observer_(event, target);
  }

  [[nodiscard]] auto find(std::string_view id) -> Element*;
  [[nodiscard]] auto query(const Selector& selector, std::size_t limit = 0) -> std::vector<Element*>;
  [[nodiscard]] auto hit_test(math::Point point) -> Element*;

  void set_focus(Element* element);
  /// 当前焦点元素（**调用前会清理悬垂指针**，树里已不在则返回 `nullptr`）。
  [[nodiscard]] auto focused() -> Element*;
  void focus_next(bool backwards = false);

  /// 语义树（`tree` 协议；`max_depth == 0` 表示不限）。
  [[nodiscard]] auto semantics(std::uint32_t max_depth = 0) const -> SemanticsNode;
  /// 视觉树（`visual` 协议）。
  [[nodiscard]] auto visual_tree() const -> VisualNode;

  /// 变更计数（每次布局/状态变化自增；控制通道事件与等待逻辑据此判定"已重绘"）。
  [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }
  void bump_version() noexcept { ++version_; }

  void mark_dirty_all();
  [[nodiscard]] auto dirty() const noexcept -> bool { return needs_frame(); }
  /// 帧是否需要渲染：布局脏 / 有损坏区待重绘 / 动画续帧。
  ///
  /// 与旧语义的差别：动画续帧不再要求"重新布局"（只重画动画元素那块），
  /// 元素级损坏区也计入（增量重绘的输入）。
  [[nodiscard]] auto needs_frame() const noexcept -> bool;
  /// 上一帧**实际绘制**的元素数（视口剔除后）。诊断与控制通道用。
  [[nodiscard]] auto painted_elements() const noexcept -> std::uint64_t {
    return painted_elements_;
  }
  /// 上一帧重绘的区域（逻辑像素；全量时=整视口）。诊断与控制通道用。
  [[nodiscard]] auto dirty_rect() const noexcept -> math::IntRect { return dirty_rect_; }
  /// 上一帧是否走了**区域重绘**（增量）路径。
  [[nodiscard]] auto last_frame_partial() const noexcept -> bool { return last_frame_partial_; }
  void clear_dirty() noexcept;

  /// 增量重绘：绘制一帧（自动选择全量/局部），返回是否走了局部路径。
  ///
  /// - 全量：清整屏 → 绘制整树（GPU 画布恒走这条，语义与旧版完全一致）
  /// - 局部（软件画布 + 损坏区可控）：清损坏区 → 推裁剪 → 绘制整树
  ///   （越界元素由 `Element::paint` 的裁剪剔除自动跳过，z 序天然正确）
  ///
  /// 决策与消费都在这里：帧首收集元素上报的损坏区；本帧新产生的（绘制期间的
  /// `request_animation`）留给下一帧。
  auto paint_frame(raster::Surface& canvas) -> bool;

  /// 叠加层（对话框 / Toast / 菜单）：绘制在内容之上，事件优先命中。
  void add_overlay(std::unique_ptr<Element> overlay);
  [[nodiscard]] auto overlay_count() const noexcept -> std::size_t { return overlays_.size(); }
  [[nodiscard]] auto overlay_at(std::size_t index) const noexcept -> Element*;
  void remove_overlay(Element* overlay);
  void clear_overlays();

  [[nodiscard]] auto render_context() const -> RenderContext;

  /// 收集 content/overlays 根元素上报的损坏区（帧首；含"外扩未知→整帧"标记）。
  void collect_tree_damage();
  /// 树里是否有元素需要重新布局（根元素上的汇总标记；O(1)）。
  [[nodiscard]] auto tree_layout_dirty() const noexcept -> bool;

 private:
  void assign_ids(Element& element, const std::string& prefix);
  void layout_subtree(Element& element, math::Rect rect);
  void paint_subtree(const RenderContext& context, Element& element, raster::Surface& canvas);
  /// 绘制后汇总"还有元素在过渡中"（决定要不要再给一帧）。
  void collect_animation_requests();
  [[nodiscard]] auto hit_test_subtree(Element& element, math::Point point) -> Element*;
  [[nodiscard]] auto dispatch_to(Element& element, Event& event) -> bool;
  /// 清掉已不在树上的 `focused_` / `hovered_` / `pressed_`。
  ///
  /// 存在的理由：它们是裸指针，而元素能被移除并销毁（列表按数据刷新、页面替换）。
  /// 元件从树上摘下时无法通知到这里（`Element` 没有 root 反指），因此采用
  /// "**用前校验**"：只做**指针比较**、不触碰那块内存——树上找不到就说明它已经死了，
  /// 置空而不是继续持有（继续持有的话，下一次解引用就是未定义行为）。
  void prune_stale_pointers();
  void collect_focus_order(Element& element, std::vector<Element*>& order);
  void update_hover(Element* target);

  Theme theme_{};
  std::unique_ptr<Element> content_{};
  std::vector<std::unique_ptr<Element>> overlays_{};
  const TextPort* text_port_{nullptr};
  math::Size viewport_{1280.0f, 720.0f};
  Element* focused_{nullptr};
  Element* hovered_{nullptr};
  Element* pressed_{nullptr};
  EventObserver event_observer_{};
  std::uint64_t version_{1};
  /// 需要重新布局（mark_dirty_all 置位；布局跑过后清）。
  bool dirty_{true};
  std::uint64_t painted_elements_{0};
  /// 本次绘制后是否还有元素在过渡中（由 `collect_animation_requests` 填）。
  bool animation_pending_{false};
  /// 上一帧之后仍需下一帧（动画续帧；clear_dirty 时从 animation_pending_ 滚动）。
  bool followup_{false};
  /// 累积损坏区（逻辑像素）与"整帧"标记（帧首消费，绘制期间新记录的留给下一帧）。
  math::Rect pending_damage_{};
  bool pending_full_{true};
  /// 上一帧重绘区域/是否局部（诊断用）。
  math::IntRect dirty_rect_{};
  bool last_frame_partial_{false};
  double time_seconds_{0.0};
};

}  // namespace st::ui
