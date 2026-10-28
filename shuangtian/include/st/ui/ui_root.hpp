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
#include "st/ui/input_coalescer.hpp"
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

  /// 帧内合并的**入口**：宿主在它的帧循环里收到 `MouseMove` 时先调它。
  ///
  /// 返回 `true` = 已被收下（宿主**不要**再 `dispatch` 这个事件）；
  /// `false` = 不该合并（非移动事件，或正在拖拽）——宿主照常 `dispatch`。
  ///
  /// ⚠ 这是**宿主显式选择**，不是 `dispatch` 的自动行为：`dispatch` 的三类调用者里
  /// 只有真窗口帧循环有"帧"的概念（测试与控制通道期望"调一次就派发一次"）。
  /// 早期版本把合并塞进 `dispatch`，直接让整套单测在菜单用例处
  /// 越界崩溃（`opened[0]` 在派发被推迟后为空）——那是**改动引入的真回归**。
  auto feed_pointer_move(Event& event) -> bool;
  /// 帧末把本帧合并的指针移动派发出去（配合 `feed_pointer_move`）。
  ///
  /// 返回本帧实际派发的移动次数（`1 + 被合并掉的次数`；无移动返回 0）。
  /// 宿主在**画完一帧之后**调它——顺序反了就等于"没合并"。
  auto flush_coalesced_moves() -> std::size_t;
  /// 本帧被合并掉的移动次数（诊断/测试用；不含首次）。
  [[nodiscard]] auto coalesced_move_count() const noexcept -> std::size_t;

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
  /// 指针移动专用命中：与 `hit_test` 同构，只在浮层那一层改用
  /// `Element::hit_test_pointer_move`（屏障只拦点击、放行移动）。
  /// 见 `Element::hit_test_pointer_move` 的说明与实测。
  [[nodiscard]] auto hit_test_pointer_move(math::Point point) -> Element*;

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
  /// 注册表的**只读快照**（枚举面，2026-10-16；BACKLOG「快捷键注册表无可枚举面」）。
  ///
  /// 返回**值拷贝**（不暴露 vector 内部、不暴露 handler、不持有内部引用）：
  /// 快捷键表是根级资源，帮助卡/文档生成/协议层要能枚举它，但**不得**经枚举面
  /// 改到注册表或触发 handler——枚举面只读，写入仍只有 register/unregister 两个口。
  ///
  /// 字段口径（与 `register_shortcut` 的存储一致，帮助卡从它生成）：
  /// - `name`：**登记名**（注册时传的 key，已小写归一）——派发时与归一后的
  ///   `event.key` 比对的就是它。同键位不同修饰键的多条注册同名共存
  ///   （如 f12 家族四条），它们靠 `mods` 区分。
  /// - `mods`：修饰键组合（含 `mods.key`；与登记名同值冗余存偅，枚举时两者都给）。
  ///
  /// 顺序：**注册顺序**（与 `shortcut_count()` 同源同序，不重排——派发是后注册优先，
  /// 枚举按注册序让调用方既能对上调试输出，也能自己反转出派发优先序）。
  struct ShortcutInfo {
    std::string name{};   ///< 登记名（小写归一后的注册键）
    Shortcut mods{};      ///< 修饰键组合（mods.key 与 name 同值）
  };
  [[nodiscard]] auto shortcuts() const -> std::vector<ShortcutInfo>;

  /// 设置键盘焦点（`nullptr` = 清除焦点）。
  ///
  /// **不可聚焦元素（`focusable() == false`）拒绝接受焦点**：返回 `false` 且焦点不变。
  /// `focusable()` 是「能否持有焦点」的契约（Tab 焦点环按它筛选），无条件赋值会让状态
  /// 分裂——root 焦点指向它、键盘派发给它，而 Tab 环跳过它。
  /// 返回 `true` = 焦点状态已按请求应用（含「本就如此」的幂等情形）；
  /// **调用方应当检查返回值**（控制通道/脚本据此如实报告，而不是静默丢弃焦点请求）。
  auto set_focus(Element* element) -> bool;
  /// 清空悬停状态（`hovered_` 置空 + 旧目标标脏重绘）。
  ///
  /// 用途：窗口恢复/内容替换等“鼠标没动但界面要重置交互态”的场合——
  /// 否则隐藏窗口期间残留的 hover（如关闭按钮红色高亮）会带进下一次显示。
  /// 鼠标一旦真的移动，`dispatch(MouseMove)` 会按新位置重新 hover，不受影响。
  void clear_hover();
  /// `Element::HostFocus` 的实现（子组件隔着 `element.hpp` 请求焦点时用）。
  auto set_keyboard_focus(Element* element) -> bool override { return set_focus(element); }
  /// 当前**焦点环是否应该画出来**（键盘导航语义；组件画环前问它）。
  ///
  /// 为何要区分“有焦点”与“该画环”：鼠标按下可聚焦元素**也会给焦点**
  /// （`MouseDown` 分支），而点一下就在按钮上留一圈边框是不对的 ——
  /// Windows / VSCode 的惯例都是「**焦点环只在键盘导航时出现**」。
  /// 实测：活动栏按钮点一下就带一圈边框（用户报“不要点击时显示边框”）。
  ///
  /// 规则：键盘事件（KeyDown/TextInput，含 Tab 焦点环）置真；鼠标按下置假。
  /// **不改变焦点本身**（键盘派发仍归焦点元素）——只影响“要不要画出来”。
  [[nodiscard]] auto focus_ring_visible() const -> bool override {
    return focus_ring_visible_;
  }
  /// 焦点环可见性变了：把**焦点元素**标脏重绘（环是它画的）。
  void on_focus_ring_changed() override {
    if (focused_ != nullptr) focused_->mark_dirty();
  }
  /// 样式系统的主题真值源（`Element::apply_appearance` 立即落位用）。
  [[nodiscard]] auto host_theme() const -> const Theme* override { return &theme_; }
  /// 焦点环可见性（**只在键盘导航时为真**；见 `focus_ring_visible()` 的说明）。
  /// 写入方只有 `UiRoot` 的事件分支——写的时候顺带把焦点元素标脏。
  void set_focus_ring_visible(bool value) {
    if (focus_ring_visible_ == value) return;
    focus_ring_visible_ = value;
    on_focus_ring_changed();
  }
  /// `Element::HostFocus` 的剪贴板实现：转发给 provider（`Application` 装的）。
  /// 没装 provider 时如实返回空串（headless 测试里粘贴路径自然降级，不会抢错）。
  [[nodiscard]] auto host_clipboard_text() -> std::string override {
    return clipboard_provider_ ? clipboard_provider_() : std::string{};
  }
  /// 写侧（2026-10-12 补齐，与读侧对称）：转发给写 provider；没装时如实返回 false。
  auto host_set_clipboard_text(std::string_view text) -> bool override {
    return clipboard_write_provider_ ? clipboard_write_provider_(text) : false;
  }
  /// 装剪贴板 provider（`Application` 在拿到 backend 后接上；无头后端可不装）。
  void set_clipboard_provider(std::function<std::string()> provider) {
    clipboard_provider_ = std::move(provider);
  }
  /// 装剪贴板**写** provider（同上；读/写分开装配——两者失败语义不同，
  /// 读失败降级空串、写失败要如实上报 false）。
  void set_clipboard_write_provider(std::function<bool(std::string_view)> provider) {
    clipboard_write_provider_ = std::move(provider);
  }
  /// 当前焦点元素（**调用前会清理悬垂指针**，树里已不在则返回 `nullptr`）。
  [[nodiscard]] auto focused() -> Element*;
  void focus_next(bool backwards = false);

  /// 悬停提示（气泡）的当前状态——自动化/测试的**可断言面**。
  [[nodiscard]] auto hint_text() const -> std::string { return hint_text_cache_; }
  [[nodiscard]] auto hint_visible() const noexcept -> bool { return hint_visible_; }
  /// 悬停延时（毫秒；0 = 立刻弹）。默认 450ms——与系统惯例同量级：
  /// 太短会在“鼠标扫过一排按钮”时闪出一堆气泡，太长又让人以为没提示。
  /// 实例级覆盖（测试用小延时；应用可以按平台习惯调）。
  void set_hint_delay_ms(int delay_ms) { hint_delay_ms_ = std::max(0, delay_ms); }
  /// 提示计时的推进次数与最后一次 delta（**诊断面**：
  /// “延时到底有没有生效”只能看这两个数——它们直接回答
  /// “推进了几次、每次多少毫秒”，而“气泡什么时候出现”是间接观测量）。
  [[nodiscard]] auto hint_update_calls() const noexcept -> std::uint64_t;
  [[nodiscard]] auto hint_last_delta_ms() const noexcept -> double;
  /// 诊断：`set_hint_target` 的调用/失联/换目标次数（定位“提示不出”用）。
  [[nodiscard]] auto hint_target_calls() const noexcept -> std::uint64_t;
  [[nodiscard]] auto hint_target_null_count() const noexcept -> std::uint64_t;
  [[nodiscard]] auto hint_target_changed_count() const noexcept -> std::uint64_t;
  /// 诊断：`update_hint` 的两个早退分支命中次数（定位“计时不走”用）。
  [[nodiscard]] auto hint_bail_prune_count() const noexcept -> std::uint64_t;
  [[nodiscard]] auto hint_bail_start_count() const noexcept -> std::uint64_t;
  /// 诊断：当前提示起点的单调钟值（0 = 未立）。
  [[nodiscard]] auto hint_start_ns() const noexcept -> std::int64_t { return hint_start_ns_; }
  [[nodiscard]] auto hint_bail_live_count() const noexcept -> std::uint64_t;
  /// 诊断：按 id 也找不到目标（真没了）的次数。
  [[nodiscard]] auto hint_bail_gone_count() const noexcept -> std::uint64_t;
  /// 当前已累计的悬停毫秒数（诊断面：与延时对比即知“等够没有”）。
  [[nodiscard]] auto hint_elapsed_ms() const noexcept -> double { return hint_elapsed_ms_; }
  [[nodiscard]] auto hint_delay_ms() const noexcept -> int { return hint_delay_ms_; }

  /// 当前**焦点环是否应该画出来**（键盘导航语义）。
  ///
  /// 为何要区分“有焦点”与“该画环”：鼠标按下可聚焦元素**也会给焦点**
  /// （`MouseDown` 分支），而点一下就在按钮上留一圈边框是不对的 ——
  /// Windows / VSCode 的惯例都是「**焦点环只在键盘导航时出现**」。
  /// 实测：活动栏按钮点一下就带一圈边框（用户报“不要点击时显示边框”）。
  ///
  /// 规则：键盘事件（KeyDown/TextInput，含 Tab 焦点环）置真；鼠标按下置假。
  /// **不改变焦点本身**（键盘派发仍归焦点元素）——只影响“要不要画出来”。
  /// 写入方只有 `UiRoot` 的事件分支（定义在 `HostFocus` 实现那一段里）。

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

  void refresh_cursor(math::Point point);
  /// 当前生效的指针形态（控制通道只读属性 / 测试断言 / 拖拽锁定判断）。
  [[nodiscard]] auto cursor() const noexcept -> MouseCursor { return cursor_; }
  /// 指针变化时的出口（宿主注入为 `Backend::set_cursor` 的转发）。
  ///
  /// 为何用回调而不是让 UiRoot 持有 `shell::Backend*`：`ui` 层不依赖 `shell` 层
  ///（依赖方向是 shell → ui）。用回调把这件事留在装配处（`app`），
  /// UI 层只知道“光标变了要通知一声”。
  ///
  /// 不注入时（单测/无头）什么也不发生——与 `text_port_` 同一姿态。
  std::function<void(MouseCursor)> cursor_sink{};
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
  /// 共享悬停提示气泡（**只有一个**：悬停的对象每次至多一个，
  /// 每个元素自建气泡会让几百个图标按钮带几百个隐藏子元素）。
  /// 所有权在 `overlays_`（经 `add_overlay`）；这里只留**裸指针**观察。
  ///
  /// 为何不留 `unique_ptr` 成员：“插进叠加层就交出了所有权”，留一个
  /// `unique_ptr` 就会变成**双重持有**（析构两次）。裸指针 + `prune` 校验
  /// 与 `focused_`/`hovered_` 同一套契约（都是“树上的观察者”）。
  /// 类型用基类指针：本头文件不必知道 `Tooltip` 是谁（少一个包含依赖），
  /// 而它只承担“显示/隐藏、给文本与目标矩形”这几个动作。
  Element* hint_bubble_raw_{nullptr};
  Element* hint_target_{nullptr};       ///< 当前悬停的、带提示的元素
  /// 目标的 **id**：声明式界面每帧重组会**销毁重建**元素，裸指针随时可能悬垂
  ///（`hint_target_->hint()` 读到空串 → 当成“没提示”直接 hide，计时跟着清零）。
  /// 存 id 就能在需要时从树上重新定位——id 是声明式稳定不变量，指针不是。
  std::string hint_target_id_{};
  /// 按 id 取当前目标（悬垂时返回 nullptr）。
  [[nodiscard]] auto live_hint_target() -> Element*;
  std::string hint_text_cache_{};       ///< 当前气泡文本（可断言面）
  double hint_elapsed_ms_{0.0};         ///< 悬停已持续多久（与延时比）
  bool hint_visible_{false};
  /// 悬停提示的出现延时（毫秒）。
  /// 450 → **280**（用户 2026-10-22：“悬浮提示出来的太慢了”）：
  /// 450ms 是桌面平台的“保险值”（防止扫过一排图标时连弹），但实测本应用的
  /// 提示是**共享气泡**（同时只会有一个）+ 鼠标真停在控件上才计时，
  /// 所以不需要那么保守。280ms 仍在“不是一碰就弹”的区间，但明显地跟手。
  /// 调低到 <150ms 会开始出现“划过就闪”的干扰。
  int hint_delay_ms_{280};
  /// 悬停开始的**单调钟**时间戳（纳秒，`st::time::now_ns` 同源）。
  ///
  /// 用它**直接算已悬停时长**，而不是每帧累加间隔——后者在丢帧/卡顿时会漂移
  ///（实测同一控件三次 322/389/627ms），且空闲不重绘时根本不走。见 `update_hint`。
  std::int64_t hint_start_ns_{0};
  /// 指针“失联”（落在无 hint 节点上）的起始时刻；0 = 未失联。
  ///
  /// 用于**去抖**：云桌面指针会在控件间隙里抖动，严格“离开即收”会让气泡闪。
  /// 只有持续失联 ≥ `kHintLeaveGraceMs` 才真收起。见 `set_hint_target`。
  std::int64_t hint_lost_ns_{0};
  /// 气泡**刚显示**的时刻（秒，单调钟）；0 = 未显示。
  ///
  /// 用于“最短保持期”（见 `kHintMinVisibleMs`）：云桌面下 `hovered` 很快回落，
  /// 不粘滞一下气泡根本看不清。
  std::int64_t hint_shown_ns_{0};
  /// 上一次推进提示时看到的 `time_seconds_`（算帧间隔；不引入新时钟）。
  double hint_time_seconds_{0.0};
  /// 悬停刚发生、下一帧**先立基准再计时**（见 `set_hint_target`）。
  bool hint_base_pending_{false};

  /// 悬停对象变了：重开计时（没延时就直接弹）。
  void set_hint_target(Element* target);
  /// 推进提示计时（每帧/每次事件调用）；到点就插气泡并摆位。
  void update_hint(double delta_ms);
  /// 收起提示（移开/点击/按键/切页都调）。
  void hide_hint();

  /// 按 `point` 重新解析该显示的指针形态并下发给后端。
  ///
  /// 解析规则（**逐级向上回退**，这是本机制的核心）：
  /// 1. `hit_test_pointer_move` 找到最内层命中的元素；
  /// 2. 沿 `parent` 链向上，第一个 `cursor_at(point)` 返回非 Arrow 的就是它；
  /// 3. 都没有 → Arrow。
  ///
  /// 为何要回退而不是只看命中元素：`cursor_at` 返回 Arrow 有两种含义——
  /// “我这一块就要箭头”与“我这里没意见”。若不回退，一个没声明光标的容器
  /// 会把它里面的按钮（Hand）盖成箭头。回退让**只有最具体的那层**说了算。
  ///
  /// 只在**变化时**才调后端：`SetCursor` 是每次鼠标移动都跑的路径，
  /// 无脑调用会把消息泵的 CPU 白烧掉。
  /// 焦点环是否可见（**默认真**：没有过任何鼠标交互时按“该画”算）。
  ///
  /// 为何默认真：这与浏览器 `:focus-visible` 的启发式一致——**程序化聚焦**
  /// （代码 `set_focus`、初始聚焦）按“最近一次输入是键盘（或还没输过）”处理；
  /// 只有**鼠标按下**才把它按下去。默认假会让所有“程序化聚焦后断言环在”的
  /// 既有回归（滑块/开关/下拉/标签…）全部变红，也会让离屏/测试渲染的观感改变。
  bool focus_ring_visible_{true};
  Element* hovered_{nullptr};
  Element* pressed_{nullptr};
  /// 当前生效的指针形态（上次下发给后端的值）。
  MouseCursor cursor_{MouseCursor::Arrow};
  /// 本次按下时的指针位置（与 `pressed_` 同生命周期）。
  ///
  /// 用途：区分「点击」与「拖动后的释放」——后端在 `LBUTTONUP` 后无条件补发 `Click`
  ///（见 `platform_win32.cpp`），而 `Click` 分支的兜底 `activate()` 会把拖动过的松手
  /// 误当成激活（实测：拖 SplitView 手柄松手 → `activate()` → 比例归位 0.5，
  /// 表现为“面板拖动后回弹到一半”）。阈值判定见 `dispatch` 的 `Click` 分支。
  math::Point pressed_at_{};
  /// 「最近一次按放」是否拖动过（`MouseUp` 时结算，`Click` 分支消费后复位）。
  ///
  /// 为什么不直接在 Click 里比对 `pressed_at_`：后端补发的 Click 到达时 `pressed_`
  /// 已清空；而**只有 Click、没有 down/up**的派发（控制通道 `input.mouse{kind:"click"}`、
  /// 单测夹具）里 `pressed_at_` 是无效默认值——直接比对会把普通点击误判成拖动（实测踩到）。
  bool last_release_dragged_{false};
  /// 「拖动过」的位移阈值（逻辑 px）：超过它就不再把这次松手当点击。
  ///
  /// 取手柄命中区的同一量级（SplitView 手柄 8px）：太小会把手抖误判成拖动
  ///（拖选/拖拽都会中招），太大则小幅拖动仍触发激活。命中区的一半是实测最佳平衡。
  static constexpr float kDragThreshold{4.0f};
  /// 帧内指针移动的合并器（见 `input_coalescer.hpp`）。
  ///
  /// 只在**未按下**时使用（拖拽期间每次移动都要派发——那是连续几何，合并会断笔）。
  InputCoalescer moves_{};
  EventObserver event_observer_{};
  /// 全局快捷键表（后注册优先）。
  struct ShortcutEntry {
    Shortcut mods{};
    std::function<bool()> handler{};
  };
  std::vector<std::pair<std::string, ShortcutEntry>> shortcuts_{};
  /// 剪贴板 provider（`set_clipboard_provider` 装入；空 = 无剪贴板能力）。
  std::function<std::string()> clipboard_provider_{};
  /// 剪贴板**写** provider（`set_clipboard_write_provider` 装入；空 = 无写能力）。
  std::function<bool(std::string_view)> clipboard_write_provider_{};
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
