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

class UiRoot : public Element::HostFocus {
 public:
  UiRoot();
  ~UiRoot();
  UiRoot(const UiRoot&) = delete;
  auto operator=(const UiRoot&) -> UiRoot& = delete;

  void set_content(std::unique_ptr<Element> content);
  [[nodiscard]] auto content() const noexcept -> Element* { return content_.get(); }
  /// 取出根内容（调用方接管所有权；用于「包一层」重建）。无内容时返回 nullptr。
  /// 注：取出的元素**保留**其 id 与子树（不会被 assign_ids 重写）。
  [[nodiscard]] auto take_content() -> std::unique_ptr<Element>;

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

  // —— 全局快捷键 ——
  //
  // 编辑器形态的 Ctrl+S/Ctrl+W/Ctrl+Tab 需要一个**先于焦点链**的落点：文本组件吞键
  // （只对认识的键返回 true 之外还有已消费的合法场景），事后猜测不可靠。
  // 派发顺序：快捷键表 → 浮层 → 焦点元素 → Tab 焦点环；命中即消费，不再下沉。
  /// 修饰键组合（与 `Event` 同名四位全真才命中；`register_shortcut("s", {true}, …)` = Ctrl+S）。
  struct Shortcut {
    std::string key{};                        ///< 主键（区分大小写归一：比对前统一小写）
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
    bool meta{false};
  };
  /// 注册全局快捷键（后注册者优先）；handler 返回 false 表示放弃消费，继续下沉。
  /// `key` 为空或 handler 为空时不注册（返回 false）。
  [[nodiscard]] auto register_shortcut(const std::string& key, Shortcut mods,
                                       std::function<bool()> handler) -> bool;
  /// 移除全部同名同修饰键的快捷键。
  void unregister_shortcut(const std::string& key, Shortcut mods);
  [[nodiscard]] auto shortcut_count() const noexcept -> std::size_t {
    return shortcuts_.size();
  }

  /// 设置键盘焦点（`nullptr` = 清除焦点）。
  ///
  /// **不可聚焦元素（`focusable() == false`）拒绝接受焦点**：返回 `false` 且焦点不变。
  /// `focusable()` 是「能否持有焦点」的契约（Tab 焦点环按它筛选），无条件赋值会让状态
  /// 分裂——root 焦点指向它、键盘派发给它，而 Tab 环跳过它。
  /// 返回 `true` = 焦点状态已按请求应用（含「本就如此」的幂等情形）；
  /// **调用方应当检查返回值**（控制通道/脚本据此如实报告，而不是静默丢弃焦点请求）。
  auto set_focus(Element* element) -> bool;
  /// `Element::HostFocus` 的实现（子组件隔着 `element.hpp` 请求焦点时用）。
  auto set_keyboard_focus(Element* element) -> bool override { return set_focus(element); }
  /// 当前焦点元素（**调用前会清理悬垂指针**，树里已不在则返回 `nullptr`）。
  [[nodiscard]] auto focused() -> Element*;
  void focus_next(bool backwards = false);

  /// 登记一棵**声明式树的推进回调**（由 `dsl::DeclarativeHost` 构造时自登记、
  /// 析构时落旗标失活）：帧首由 UI 统一推进。
  ///
  /// 为什么需要它：同一个页面可以占**多处树位**（`mount_into` 把声明式树挂到既有元素
  /// 的子位，如标题栏的附属槽）——于是进程里同时存在多棵声明式树，每棵都要在每帧被
  /// “kick”一下才会重组。曾经的做法是**让调用方自己记得**：主循环里写 `host->tick()`；
  /// 漏掉第二棵时，点击命中了、状态也变了，**但面板永远不出现**
  /// （实测：gbcode 菜单与标题栏合并成一行时踩到）。
  ///
  /// 登记后 `tick_declarative_hosts()` 一次推全部——漏不掉，调用方也不必知道有几棵。
  /// 用回调而不是存 `DeclarativeHost*`：`ui_root` 不该反向依赖 `dsl`。
  void register_declarative_host(std::function<bool()> advance);
  [[nodiscard]] auto declarative_host_count() const noexcept -> std::size_t {
    return declarative_hosts_.size();
  }
  /// 推进全部已登记的声明式树（主循环里、`app.tick()` 之前调）。
  /// 回调自己判定“本帧要不要做事”（不脏就返回）——与调用方原先写的
  /// `if (host->dirty()) host->tick()` 同义。
  ///
  /// 返回本次**真的重组了**的树数：调用方据此决定要不要 `request_repaint()`
  /// （帧节拍：重组出了新内容才需要重绘）。
  auto tick_declarative_hosts() -> std::size_t;

  /// 语义树（`tree` 协议；`max_depth == 0` 表示不限）。
  [[nodiscard]] auto semantics(std::uint32_t max_depth = 0) const -> SemanticsNode;
  /// 视觉树（`visual` 协议）。
  [[nodiscard]] auto visual_tree() const -> VisualNode;

  /// 变更计数（每次布局/状态变化自增；控制通道事件与等待逻辑据此判定"已重绘"）。
  [[nodiscard]] auto version() const noexcept -> std::uint64_t { return version_; }
  void bump_version() noexcept { ++version_; }

  /// 登记"这个元素变了"（供 `ui.changed` 事件携带变更 id 清单）。
  ///
  /// 语义边界：登记的是**状态/内容变化**（set/invoke/输入/增删）——不是"重绘"。
  /// 悬浮过渡、光标闪烁这类每帧都在动的视觉变化不该登记（否则事件流会变成逐帧刷屏，
  /// "变更清单"就失去了"哪些元素真的变了"的信噪比）。去重、上限截断（见 kMaxChangedIds）。
  void note_changed(const Element& element);
  /// 取走累积的变更 id 清单（调用后清零；控制通道 `ui.changed` 事件消费）。
  [[nodiscard]] auto take_changed_ids() -> std::vector<ElementId>;

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

  /// 叠加层排布形态：`Stack`（自上而下堆叠，历史行为）或 `FillViewport`
  /// （铺满视口，由 overlay 自己在 bounds 内定位卡片——模态遮罩/命令面板的标准形态）。
  enum class OverlayLayout { Stack, FillViewport };

  /// 叠加层（对话框 / Toast / 菜单）：绘制在内容之上，事件优先命中。
  /// `layout` 选排布形态：`FillViewport` 时 measure 拿到完整视口约束、arrange 全视口矩形，
  /// 组件在 `arrange` 里自行计算遮罩与卡片（不再需要调用方注入视口尺寸）。
  void add_overlay(std::unique_ptr<Element> overlay, OverlayLayout layout = OverlayLayout::Stack);
  /// 某个浮层的排布形态（不在浮层列表中返回 Stack）。
  [[nodiscard]] auto overlay_layout(const Element* overlay) const -> OverlayLayout;
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
  /// 回收"派发或绘制期间摘除的叠加层"（延迟析构的墓场）。
  void reap_overlays();
  /// 逐个绘制叠加层，并在对象之间回收墓场。
  ///
  /// 单独抽出来是因为 **叠加层的 `paint` 可以摘除自己**（`Toast` 到期），
  /// 那时 `remove_overlay` 必须延后释放，而延后就得有人在绘完当前对象之后回收。
  void paint_overlays(const RenderContext& context, raster::Surface& canvas);
  /// 把宿主契约（`this`，作为 `Element::HostFocus`）写到整棵子树（`Element::set_host`）。
  /// 子组件据此请求焦点，同时保持 `element.hpp` 不反向依赖 `ui_root.hpp`。
  void wire_owner(Element& element);
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

  /// 浮层键盘下钻：深度优先（后声明者优先）找**第一个能处理**该键的节点，
  /// 子树都不处理再问 `root` 自身。浮层容器（铺满视口的声明式宿主）自己不认键，
  /// 真正的面板在子树里——见 `dispatch` 的模态分支注释。
  auto dispatch_key_into(Element* root, Event& event) -> bool;

  Theme theme_{};
  std::unique_ptr<Element> content_{};
  std::vector<std::unique_ptr<Element>> overlays_{};
  /// 与 `overlays_` 同序的排布形态。
  std::vector<OverlayLayout> overlay_layouts_{};
  /// 事件分发期间被摘除的叠加层（**延迟析构**）。
  ///
  /// 分发栈里可能还持着该子树内元素的裸指针（`dispatch_to` 沿 `parent()` 链回溯），
  /// 立即释放就是 use-after-free（实测 SIGSEGV）。用 `dispatching_` 标记 +
  /// 分发末尾 `reap_overlays()` 统一回收。
  std::vector<std::unique_ptr<Element>> graveyard_{};
  bool dispatching_{false};
  /// 是否正在绘制叠加层（`paint_overlays` 置位）。
  /// 与 `dispatching_` 并列：摘除发生在**绘制**里时同样要延后释放，
  /// 否则当前对象的 `paint` 栈帧还在用 `this`（use-after-free，ASan 有现场）。
  bool painting_overlays_{false};
  const TextPort* text_port_{nullptr};
  math::Size viewport_{1280.0f, 720.0f};
  Element* focused_{nullptr};
  Element* hovered_{nullptr};
  Element* pressed_{nullptr};
  EventObserver event_observer_{};
  /// 全局快捷键表（后注册优先）。
  struct ShortcutEntry {
    Shortcut mods{};
    std::function<bool()> handler{};
  };
  std::vector<std::pair<std::string, ShortcutEntry>> shortcuts_{};
  /// 已登记的声明式树推进回调（见 `register_declarative_host`）。
  /// 回调返回 true 表示“本帧真的重组了”（用于统计重组次数）。
  std::vector<std::function<bool()>> declarative_hosts_{};
  /// 上限：单帧变更清单超过它即截断——事件流是对"改了什么"的提示，
  /// 不是全量日志；无界清单会把一次批量操作变成巨型事件帧。
  static constexpr std::size_t kMaxChangedIds = 64;
  std::vector<ElementId> changed_ids_{};   ///< 累积的变更元素 id（见 note_changed）
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
