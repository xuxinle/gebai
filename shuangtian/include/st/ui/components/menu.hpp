#pragma once

/// 菜单组件（`DESIGN.md` §4.5）：`MenuBar`（顶部菜单栏）与 `ContextMenu`（右键上下文菜单）。
///
/// 数据是声明式的（`Menu`/`MenuItem` 嵌套结构），视觉是自绘的（不产生子 Element）：
/// 面板 = `surface` 底 + `shadow_md` + `radius_md`，条目行高 32（与列表项 40 同视觉语言），
/// hover `surface_alt`、键盘选中 `primary_soft`，选中态在面板打开期间由键盘导航维护。
///
/// 挂载模型（要点）：
/// - `MenuBar` 只画"标题条"；点开某个标题时由**调用方**把 `open_menu(index)` 返回的
///   下拉面板以 `UiRoot::add_overlay(panel, OverlayLayout::Stack)` 挂上（面板定位到
///   标题下方已由 `MenuBar` 计算好，面板 `bounds` 即最终位置）。这样面板天然盖在
///   内容之上、键盘事件先于焦点元素（浮层派发），且 MenuBar 无需持有 UiRoot 反指。
/// - `ContextMenu::make(anchor, items)` 工厂构造后以 `OverlayLayout::FillViewport`
///   挂载：面板矩形外不拦截命中（`intercepts_input`/`hit_test` 双口径），点击面板外
///   或按 Esc 触发 `on_close`（由调用方摘除 overlay）。
///
/// 依赖纪律：文本经 `context.text`（空则 `NullTextPort`）；颜色/字号/圆角一律取
/// `context.theme` token；勾选标记与展开三角用 canvas 路径自绘（不依赖字体图标）。

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 菜单条目（可递归：`children` 非空表示有子菜单；本版子面板展开一层）。
struct MenuItem {
  std::string id{};
  std::string label{};
  bool separator{false};  ///< 分隔线（其余字段忽略）
  bool checked{false};    ///< 左侧画勾
  std::vector<MenuItem> children{};
};

/// 菜单栏的顶级菜单（标题 + 条目）。
struct Menu {
  std::string id{};
  std::string label{};
  std::vector<MenuItem> items{};
};

/// 下拉/弹出面板共享的条目绘制与键盘导航（自绘；高 32）。
class MenuPanel : public Element {
 public:
  static constexpr float kItemHeight{32.0f};
  static constexpr float kItemPaddingX{12.0f};
  static constexpr float kMinPanelWidth{160.0f};
  static constexpr float kMaxPanelWidth{280.0f};

  explicit MenuPanel(std::vector<MenuItem> items);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MenuPanel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Menu; }

  /// 面板定位（锚定在菜单标题正下方；由 `ContextMenu`/`MenuBar` 计算后调用）。
  void set_anchor(math::Rect anchor_rect);
  [[nodiscard]] auto anchor_rect() const noexcept -> math::Rect { return anchor_; }

  [[nodiscard]] auto item_count() const noexcept -> std::size_t;
  [[nodiscard]] auto item_id(std::size_t index) const -> std::string_view;
  /// 键盘导航选中序号（无选中为 `kNoIndex`）。
  [[nodiscard]] auto highlighted() const noexcept -> std::size_t { return highlighted_; }
  /// 高亮移动（跳过分隔线；越界夹取；`notify` 触发 hover 视觉刷新）。
  void set_highlighted(std::size_t index);

  /// 条目行矩形（命中/测试；`arrange` 后有效）。
  [[nodiscard]] auto item_rect(std::size_t index) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  /// 条目激活（点击或 Enter）：参数为条目在面板中的序号。
  std::function<void(std::size_t)> on_activate{};
  /// 关闭请求（Esc / 点击面板外 / 激活条目后）：面板应被摘除，调用方决定。
  std::function<void()> on_close{};

  /// 无选中哨兵。
  static constexpr std::size_t kNoIndex{static_cast<std::size_t>(-1)};

 private:
  [[nodiscard]] auto item_at_point(math::Point point) const -> std::size_t;

  std::vector<MenuItem> items_{};
  math::Rect anchor_{};
  std::size_t highlighted_{kNoIndex};
  float item_height_{kItemHeight};
};

/// 顶部菜单栏：水平排布顶级标题（高 32、hover `surface_alt`、点击打开下拉）。
///
/// 打开模型：`on_open_menu(index)` 回调里调用方取 `make_panel(index)` 造面板、
/// `add_overlay` 挂载；面板自身的 `on_activate`/`on_close` 由调用方接线。
/// 键盘：聚焦后 ←→ 切换标题、Enter/ArrowDown 打开当前标题的面板。
class MenuBar : public Element {
 public:
  static constexpr float kBarHeight{32.0f};
  static constexpr float kTitlePaddingX{12.0f};

  MenuBar();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MenuBar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Menu; }

  /// 整表替换。
  void set_menus(const std::vector<Menu>& menus);
  [[nodiscard]] auto menu_count() const noexcept -> std::size_t { return menus_.size(); }
  [[nodiscard]] auto menu_label(std::size_t index) const -> std::string_view;
  [[nodiscard]] auto menu_id(std::size_t index) const -> std::string_view;
  /// 顶级标题矩形（命中/锚定；arrange 后有效）。
  [[nodiscard]] auto title_rect(std::size_t index) const -> math::Rect;
  /// 当前打开的菜单序号（无打开为 `kNoIndex`；由 `set_open_index` 维护）。
  [[nodiscard]] auto open_index() const noexcept -> std::size_t { return open_index_; }
  void set_open_index(std::size_t index);

  /// 为第 `index` 个菜单造下拉面板（已锚定到标题下方、接线 `on_activate`/`on_close`）。
  /// 激活转发到 `on_action(menu_id, item_id)`；面板打开期间 `open_index_` 由本类维护
  /// （面板 on_close 时自动清 `open_index_`，摘除 overlay 仍由调用方完成）。
  [[nodiscard]] auto make_panel(std::size_t index) -> std::unique_ptr<MenuPanel>;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  /// 菜单动作回调（顶级菜单 id + 条目 id；分隔线不会激活）。
  std::function<void(const std::string&, const std::string&)> on_action{};
  /// 标题被点击/键盘打开（调用方在此 `make_panel` + `add_overlay`）。
  std::function<void(std::size_t)> on_open_menu{};

  static constexpr std::size_t kNoIndex{static_cast<std::size_t>(-1)};

 private:
  std::vector<Menu> menus_{};
  std::vector<float> title_widths_{};  ///< 布局缓存（measure 期算好）
  std::size_t open_index_{kNoIndex};
  int hover_index_{-1};
};

/// 右键上下文菜单：`FillViewport` overlay 形态，面板锚定到屏幕坐标。
///
/// 拦截语义（dismiss barrier，与 Flutter modal barrier / Web overlay-backdrop 同款）：
/// - 鼠标按下/点击：全视口拦截——面板内走条目激活，面板外触发 `on_close` 并消费
///   （框架无事件捕获机制，只有拦下这次点击才能可靠地「点外关闭」）；
/// - 鼠标移动：面板外穿透（`on_event` 返回 false，下层 hover 照带）；
/// - Esc → `on_close`；↑↓/Enter 转面板键盘导航；
/// - 定位：优先锚点右下展开；越出视口时向左/上翻转（仍越界则夹入）。
class ContextMenu : public Element {
 public:
  /// 关闭请求（Esc / 面板外点击 / 条目激活后）：由调用方摘除 overlay。
  std::function<void()> on_close{};

  /// 工厂：`anchor` 为期望的面板左上角（通常是鼠标位置）。
  [[nodiscard]] static auto make(math::Point anchor, std::vector<MenuItem> items)
      -> std::unique_ptr<ContextMenu>;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ContextMenu"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Menu; }

  /// 面板内条目面板（非拥有指针，生命周期随本元素）。
  [[nodiscard]] auto panel() noexcept -> MenuPanel* { return panel_; }
  [[nodiscard]] auto panel() const noexcept -> const MenuPanel* { return panel_; }

  /// dismiss barrier：全 bounds 拦截（面板外点击要能到这儿才能转关闭，见类注释）。

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  /// 构造公开（`make` 工厂仍提供，两者等价）。
  ContextMenu(math::Point anchor, std::vector<MenuItem> items);

  math::Point anchor_{};
  MenuPanel* panel_{nullptr};             ///< 非拥有（指向 owned_panel_）
  std::unique_ptr<MenuPanel> owned_panel_{};  ///< 拥有（不走 add_child，免 Flex 挪位）
};

}  // namespace st::ui
