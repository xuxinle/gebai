#pragma once

/// 组件库 A · 窗框（`DESIGN.md` §4.5）：`TitleBar`（自绘标题栏 + 窗口控制按钮）。
///
/// 存在的理由（`CONVENTIONS.md` §10 第 7 条）：窗口装饰**一律自绘**，任何平台都不用系统
/// 标题栏——三平台自带标题栏的字号/高度/圆角/配色各不相同，"一块代码三平台外观一致"会从
/// 窗框处漏掉；而自绘窗框才与 UI 共用同一套设计令牌、同一套 DPI 口径与**同一份无头截图**。
/// 于是平台层只剩下三件事（贴像素、翻译输入、提供窗口控制），窗框归组件库。
///
/// 依赖纪律：颜色/间距/字号一律取 `context.theme` 的 token（`apply_theme` 落进 `style_`），
/// 组件内不硬编码颜色；窗口动作经 `ui::WindowControl` 端口（**ui 层不认识 `shell::Backend`**，
/// 见 `st/ui/window_control.hpp` 的依赖方向说明）。
///
/// 跨平台无差异的三条硬约束（本组件就是它们的落点）：
/// 1. **画面**：无窗口（无头）时像素照画——"内置通道截图"与"实机窗口"必须可比；
/// 2. **判定**：边缘/按钮命中只用 `ui::resize_edge_at` 与自身 bounds，不读任何平台 API；
/// 3. **能力**：宿主不支持窗口控制时**如实拒绝**（`invoke` 返回 false），不静默无效，
///    且不改变绘制结果（不因环境不同而少画一个按钮）。
///
/// 控制通道属性面：`title`。动作面：`minimize` / `maximize`（切换）/ `close`。
/// 便捷接口：`set_title` / `set_icon` / `set_show_controls` / `set_window_control`。

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/window_control.hpp"

namespace st::ui {

/// 自绘标题栏：图标 + 标题 + 右侧最小化/最大化/关闭。
///
/// 交互（与系统标题栏**一致**，这是"自绘不丢手势"的关键）：
/// - 标题区**按下** → `WindowControl::window_begin_move()`（等价于拖系统标题栏：吸附、
///   跨屏拖动等系统手势随之生效——自己算窗口位置会把这些全丢掉）；
/// - 标题区**双击** → 最大化/还原（系统标题栏同款语义）；
/// - **边缘带按下**（`ui::resize_edge_at`，物理像素经 `device_scale` 折算）→
///   `window_begin_resize(edge)`（X11/Wayland 走它；Win32 由 `WM_NCHITTEST` 覆盖，
///   接口返回 false 时**不影响拖动/按钮**，只是这条路不通）；
/// - 三个控制按钮：`hover` 高亮、**关闭按钮 hover 用 danger 色**（系统惯例，防误点）。
///
/// 高度缺省 `metrics.control_height_lg`（40px）：标题栏比正文控件略高一档，
/// 是"窗框"与"工具栏"在视觉上分开的最小代价。
class TitleBar : public Element {
 public:
  explicit TitleBar(std::string title = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TitleBar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 标题文案（标题变长变短**不**改标题栏高度与按钮位置，所以只重绘、不重排——
  /// 这是高频路径：代码编辑器的脏点每敲一个字就变一次）。
  void set_title(std::string title);
  [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
  /// 前置图标名（内置图标集或 `svg:` 前缀的 SVG 源；空 = 不画）。
  void set_icon(std::string icon);
  [[nodiscard]] auto icon() const noexcept -> const std::string& { return icon_; }
  /// 是否显示窗口控制按钮（默认显示；**与宿主能力无关**——画面跨平台一致，
  /// 能力差异只体现在动作返回值上）。
  void set_show_controls(bool show);
  [[nodiscard]] auto show_controls() const noexcept -> bool { return show_controls_; }
  /// 绑定窗口控制端口（宿主提供；为空时按钮与拖动**如实拒绝**）。
  void set_window_control(WindowControl* control) noexcept { control_ = control; }
  /// 双击标题区触发的回调（缺省走 `window_toggle_maximize`）。
  void set_on_double_click(std::function<void()> callback) { on_double_click_ = std::move(callback); }

  // —— 附属槽（窗框是**框架**，不是仅一根标题）——
  //
  // 存在的理由：真实应用的外壳里，菜单入口、主题/DPI/分享等控件本来就长在标题栏这一行
  // （VSCode / 浏览器 / 文件管理器都是这样）。没有槽位时，示例只能另建一条与窗框**并列**
  // 的"顶部栏"——那是两份高度、两套内边距、两个拖拽区；写了 leading 就免了这类重复。
  //
  // 布局：`[标题图标 标题 trailing… ｜ 控制按钮]`——内容槽在标题之后、控制按钮之前，
  // 因此宿主控件不会被控制按钮盖住，且**自动**把控制按钮推到最右。
  // 语义：附属槽 **不参与拖动**（它们是控件，不是拖动区）；标题带仍可拖动/双击。
  auto add_trailing(std::unique_ptr<Element> child) -> Element*;
  /// 标题前（图标与标题之间）的附属内容。
  auto add_leading(std::unique_ptr<Element> child) -> Element*;

  // —— 几何（命中、绘制、单测共用同一份；避免"测试复刻布局公式"的假绿）——
  /// 三个控制按钮的边长（逻辑像素）。
  [[nodiscard]] static constexpr auto control_button_size() noexcept -> float { return 46.0f; }
  /// 标题栏高度（逻辑像素）。
  [[nodiscard]] static constexpr auto bar_height() noexcept -> float { return 40.0f; }
  /// 单个控制按钮的矩形（`index`：0=最小化 1=最大化 2=关闭），绝对坐标。
  [[nodiscard]] auto control_button_rect(std::size_t index) const -> math::Rect;
  /// 控制按钮区（三个按钮加起来的占位）：附属槽排在它左侧，标题带也要避开它。
  [[nodiscard]] auto controls_rect() const -> math::Rect;
  /// 标题条带矩形（`leading` 槽之后 → `trailing` 槽之前；标题**文字**画在这里）。
  [[nodiscard]] auto caption_rect() const -> math::Rect;
  /// 拖动/双击的**命中**区：从窗口左缘到尾部槽之前。
  ///
  /// 比 `caption_rect()` 多出"左边那条空当（图标/内边距/前部槽）"——与系统标题栏的
  /// `HTCAPTION` 同样"整条都算"：用户不会精确地点到文字上，只认文字区会让拖动难用。
  [[nodiscard]] auto drag_rect() const -> math::Rect;
  /// 点是否落在标题栏可拖动区（拖动/双击的命中判据）。
  [[nodiscard]] auto hits_caption(math::Point point) const -> bool;
  /// 标题文字区（`leading` 槽之后 → `trailing` 槽之前；`arrange` 时算定）。
  [[nodiscard]] auto title_rect() const noexcept -> math::Rect { return caption_rect(); }

  /// 载体子元素：**附属槽在前，自持的标题/图标/按钮在后**（与 `ScrollView` 同约定）。
  /// 它们由本组件自己排版（横排一行、标题带居首），不进入调用方的列布局。
  [[nodiscard]] auto content_child_count() const noexcept -> std::size_t override {
    return trailing_.size() + leading_.size();
  }
  /// 附属槽插到自持部件**之前**（自持部件恒在末尾；见 `content_child_count`）。
  auto add_child(std::unique_ptr<Element> child) -> Element* override;
  auto insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* override;

  void apply_theme(const Theme& theme) override;
  /// 单行横排（标题带 + 附属槽 + 控制按钮）：不依赖列布局的父容器。
  void arrange(const RenderContext& context, math::Rect rect) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override { return title_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 按钮 → 图标名（`maximized` 时"最大化"图标换成"还原"图标）。
  [[nodiscard]] auto button_icon(std::size_t index) const -> std::string_view;
  /// 触发窗口动作并如实返回结果（宿主缺失/不支持 → false）。
  auto run_window_action(std::string_view action) -> bool;
  /// 控制按钮个数（最小化 / 最大化 / 关闭）：与实现文件里的 `k_control_count` 必须一致。
  static constexpr std::size_t k_control_button_count{3};
  /// 控制按钮区左缘（`show_controls` 关闭时为窗口右缘）。
  [[nodiscard]] auto controls_left() const noexcept -> float {
    return show_controls_ ? bounds_.right() - control_button_size() *
                                                static_cast<float>(k_control_button_count)
                          : bounds_.right();
  }

  std::string title_{};
  std::string icon_{};
  bool show_controls_{true};
  WindowControl* control_{nullptr};
  std::function<void()> on_double_click_{};
  /// 附属槽（非拥有；同时作为 `Element::children()` 的一部分，见 `content_child_count`）。
  std::vector<Element*> leading_{};
  std::vector<Element*> trailing_{};
  /// 槽位边界（`arrange` 时算定）：标题**文字**起始 x 与 `trailing` 槽区左缘。
  ///
  /// 为什么只存这两个标量而不是两块矩形：矩形还取决于 `show_controls` 等**属性**，
  /// 把矩形一起缓存就会出现"改了属性但未重排 → `caption_rect()` 返回旧值"
  /// （实测：单测正是这么撞出来的）。标量是真实的排版结果，矩形按需算就得一致。
  mutable float title_left_{0.0f};
  mutable float slots_left_{0.0f};
  /// 当前鼠标悬浮的控制按钮序号（`-1` = 无）：按钮级高亮。
  /// 元素级 `hovered_` 只能表达"指针在标题栏里"，而三个按钮各自要高亮不同的那一个。
  mutable int hovered_button_{-1};
  /// 当前按下的控制按钮序号（`-1` = 无）：画出按压态。
  int pressed_button_{-1};
};

}  // namespace st::ui
