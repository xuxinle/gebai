#pragma once

/// 组件库 · 主题选择器（`ThemePicker`）：内置主题库**色卡墙**，模态弹窗形态。
///
/// 定位：把「**运行时切主题**」做成框架通用能力——`ThemePicker` 作为窗口级组件
/// 由 `TitleBar` 默认自带（标题栏右上角调色板按钮，与窗口控制按钮同组同规格，
/// 默认开启，`show_controls` 同一开关控制）。点击弹**模态色卡墙**：
///
/// - 半透明遮罩铺满视口（`colors.overlay`），点击遮罩 / Esc / 选定即收起；
/// - 居中卡片：10 套主题各一张三段拼色样品卡（主题底色 + 主色粗带 + 主色洗色），
///   中文名在卡下——选主题像挑涂料样品，而不是背英文名读下拉列表；
/// - 当前主题：主色描边 + 洗底 + 打勾；hover：亮面；点击即选。
///
/// ## 自模态（为什么不走 `Select` 的 overlay 注入惯例）
///
/// 标题栏按钮拿不到应用装配层——ThemePicker 的弹/收经 **宿主契约**
/// （`Element::host()->host_add_overlay/host_remove_overlay`，UiRoot 实现）：
/// 未上树/宿主没接时 `open()` 如实返回 false（不抛、不静默）。挂载形态
/// `FillViewport`（遮罩铺满、组件自己居中卡片）。
///
/// ## 切主题的唯一真切换点
///
/// 选定 → `host()->host_apply_theme(name)`（UiRoot→`Application::set_theme_name`
/// 管线：合成主题/覆盖叠加/换调色一致）；无宿主时回落 `on_change`（宿主自接）。
///
/// ## 属性/动作面（控制通道与自动化）
///
/// - `value`（当前主题名，可写=程序化选中**不触发回调**）、`options`（只读枚举）、
///   `open`（弹窗开/关）；
/// - 动作 `change`（**等同用户选择**）/ `open` / `close` / `toggle`。

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {

/// 主题模态色卡墙：遮罩 + 居中卡片，内含 5×2 主题色卡网格。
/// 由 `ThemePicker` 创建并经宿主契约挂为 FillViewport 浮层；所有权在 UiRoot。
class ThemePanel : public Element {
 public:
  ThemePanel(std::vector<std::string> theme_names, std::string current);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ThemePanel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  /// 模态：在场即拦截输入（遮罩吃掉对下层的点击；见 `Element::intercepts_input`）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  void set_current(const std::string& name);
  /// 卡片矩形（命中/测试用；arrange 后有效）。
  [[nodiscard]] auto card_rect(std::size_t index) const -> math::Rect;
  [[nodiscard]] auto card_count() const noexcept -> std::size_t { return names_.size(); }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  /// 主题被选中（点击/Enter）。回调返回后本对象可能已被摘除，实现内已先隐藏。
  std::function<void(std::string_view)> on_pick{};
  /// 请求收起（Esc / 点遮罩）。
  std::function<void()> on_dismiss{};

 private:
  /// 网格列数（10 套主题 = 5×2）。
  [[nodiscard]] auto columns() const -> std::size_t;
  /// 居中卡片矩形（arrange 算定后缓存）。
  [[nodiscard]] auto center_card() const -> math::Rect;

  std::vector<std::string> names_{};   ///< 主题英文名（id）
  std::optional<std::size_t> selected_{};
  std::size_t hover_index_{static_cast<std::size_t>(-1)};
  /// 按下/结算簿记（点击穿透与“开模态的 Click 尾巴”两坑都靠它）：
  /// - `pressed_inside_`：是否收到过本面板的 MouseDown（点遮罩按下算 true；
  ///   false = Click 没有先导 down——那是标题栏开模态那一下的尾巴，不许关）；
  /// - `pressed_on_card_`：按下时落在哪张卡（-1 = 卡外/遮罩）；**仅当
  ///   pressed_inside_ 为 true 时有效**。
  bool pressed_inside_{false};
  std::size_t pressed_on_card_{static_cast<std::size_t>(-1)};
  math::Rect card_area_{};             ///< 网格区（居中卡片内、标题下方）
};

/// 主题选择按钮：通常由 `TitleBar` 默认创建（标题栏命令按钮形态）；
/// 也可独立摆放（图标按钮，点击弹模态色卡墙）。
class ThemePicker : public Element {
 public:
  ThemePicker();
  ~ThemePicker() override;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ThemePicker"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Select; }

  /// 用内置主题库重设选项（含当前选中）。默认构造已填好，装配层可选调用。
  void populate_builtins(std::string_view current);

  /// 设置当前主题（无匹配项则保持原状；不触发回调——状态同步用）。
  void set_current(std::string_view name);

  /// 主题变更通知（用户点了某张色卡）。**选中即通知**（无论是否有宿主）：
  /// 真切换由宿主契约完成（`host_apply_theme`，无宿主时需宿主在此自接），
  /// 持久化等副作用也在这里做。形参指向内部存储，仅回调期间有效。
  std::function<void(std::string_view)> on_change{};

  /// 打开/关闭模态色卡墙。经宿主契约挂 FillViewport 浮层；
  /// 未上树/宿主未接时 open 如实返回 false。
  auto open() -> bool;
  void close();
  [[nodiscard]] auto is_open() const noexcept -> bool { return open_; }

  /// 当前选中主题名（无选中为空）。
  [[nodiscard]] auto current() const -> std::string_view { return current_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 内部选中变化（用户点击/`change` 动作）→ 换主题（宿主契约优先）。
  void handle_pick(std::string_view name);

  std::vector<std::string> names_{};          ///< 内置主题英文名清单
  std::string current_{"light"};
  bool open_{false};
  // 面板生命周期簿记：观察指针 + 摘除延迟到绘制期（const 路径也要推进，
  // 同 Select 的 dismiss_pending_/panel_ 惯例）。
  mutable ThemePanel* panel_{nullptr};
  mutable bool dismiss_pending_{false};

  /// 摘除延迟推进（const 绘制路径上也会被调）。
  void flush_dismiss() const;
};

}  // namespace st::ui