#pragma once

/// 组件库 A · 下拉单选（`DESIGN.md` §4.5）：`Select` + `SelectPanel`。
///
/// 依赖纪律：文本经 `context.text`（空则退化 `NullTextPort`）；颜色/间距/圆角/字号一律取
/// `context.theme` 的 token（`apply_theme` 时落进 `style_`），组件内不硬编码颜色。
///
/// 展开机制（关键）：`Select` **不直接持有 `UiRoot`** —— 展开时创建 `SelectPanel` 并经
/// `overlay_host(std::unique_ptr<Element>)` 交给调用方挂载（典型实现
/// `root.add_overlay(std::move(panel))`）。收起时若设置了 `overlay_remove(Element*)`
/// （典型实现 `root.remove_overlay(panel)`）则面板由其摘除；**摘除不在事件分发内执行**
/// （延迟到布局/绘制期），以免在回调中析构正在处理事件的面板。
/// 面板自身立即 `set_visible(false)`，因此即使调用方不摘除，也已经从绘制/命中/语义树中消失。
///
/// 控制通道属性面：`value`（选中值，可写=按值选中；`none` 清空）、`active`（序号或 `none`）、
/// `label`（选中项文本，可写=按文本选中）、`options`（`|` 分隔，可写=整表替换 value=label）。

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 下拉选项：`value` 为表单/控制通道值，`label` 为显示文本（空则显示 `value`）。
struct SelectOption {
  std::string value{};
  std::string label{};
};

/// 下拉面板：`surface` + `shadow_md` + `radius_md`，选项行高 32（hover `surface_alt` / 选中 `primary_soft`）。
///
/// 由 `Select` 创建并经 `overlay_host` 交出，调用方负责其生命周期。面板按 `anchor`
/// （绝对坐标 = 控件左下角）自行定位：叠加层容器给它的布局槽位只用于尺寸，原点由锚点决定。
class SelectPanel : public Element {
 public:
  SelectPanel(std::vector<SelectOption> options, std::optional<std::size_t> selected);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SelectPanel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::List; }

  /// 锚点（绝对坐标：控件左下角）。
  void set_anchor(math::Point anchor);
  /// 面板宽度（通常与控件等宽）。
  void set_panel_width(float width);
  void set_selected(std::optional<std::size_t> index);

  /// 选项行矩形（命中/测试用；越界返回空矩形）。
  [[nodiscard]] auto option_rect(std::size_t index) const -> math::Rect;
  [[nodiscard]] auto option_count() const noexcept -> std::size_t { return options_.size(); }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  /// 选项被选中（点击）。回调返回后本对象可能已被摘除，实现内已先 `set_visible(false)`。
  std::function<void(std::size_t)> on_pick{};
  /// 请求收起（`Esc`）。
  std::function<void()> on_dismiss{};

 private:
  /// 指针命中的选项行（-1 无）。
  [[nodiscard]] auto index_at(math::Point point) const -> int;

  std::vector<SelectOption> options_{};
  std::optional<std::size_t> selected_{};
  math::Point anchor_{};
  float panel_width_{0.0f};
  int hover_index_{-1};
};

/// 下拉单选控件：与 `Input` 同高（`metrics.control_height`），右侧自绘 `chevron-down`。
class Select : public Element {
 public:
  Select();
  ~Select() override;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Select"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Select; }

  /// 追加选项（`label` 为空时显示 `value`）。
  void add_option(std::string value, std::string label = {});
  /// 整表替换（会收起已展开的面板）。
  void set_options(std::vector<SelectOption> options);
  void clear_options();
  [[nodiscard]] auto options() const noexcept -> const std::vector<SelectOption>& { return options_; }
  [[nodiscard]] auto option_count() const noexcept -> std::size_t { return options_.size(); }

  /// 空选中态显示（`text_faint` 色）。
  void set_placeholder(std::string text);
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }

  /// 设置选中项（`nullopt` 清空；`notify` 为 true 时触发 `on_change`）。
  void set_selected_index(std::optional<std::size_t> index, bool notify = false);
  [[nodiscard]] auto selected_index() const noexcept -> std::optional<std::size_t> {
    return selected_;
  }
  /// 选中值（无选中返回空视图）。
  [[nodiscard]] auto selected_value() const -> std::string_view;
  /// 选中项显示文本（无选中返回空视图）。
  [[nodiscard]] auto selected_label() const -> std::string_view;

  [[nodiscard]] auto open() const noexcept -> bool { return open_; }
  /// 展开/收起（展开需已设置 `overlay_host` 且存在选项）。
  void set_open(bool value);
  /// 控件盒矩形（面板锚点/命中/测试用）。
  [[nodiscard]] auto control_rect() const noexcept -> math::Rect { return bounds_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 选中项变化回调（实参指向组件内部存储，仅回调期间有效）。
  std::function<void(std::string_view)> on_change{};

  /// 面板挂载回调（**必需**，否则无法展开）：调用方通常写 `[&root](std::unique_ptr<Element> p) { root.add_overlay(std::move(p)); }`。
  std::function<void(std::unique_ptr<Element>)> overlay_host{};
  /// 面板摘除回调（可选）：调用方通常写 `[&root](Element* p) { root.remove_overlay(p); }`。
  std::function<void(Element*)> overlay_remove{};

 private:
  void open_panel();
  void handle_pick(std::size_t index);
  void move_selection(int delta);
  /// 执行延迟摘除（布局/绘制期调用）。
  void flush_dismiss() const;
  [[nodiscard]] auto display_label(std::optional<std::size_t> index) const -> std::string_view;
  [[nodiscard]] auto index_of(std::string_view text) const -> std::optional<std::size_t>;

  std::vector<SelectOption> options_{};
  std::string placeholder_{};
  std::optional<std::size_t> selected_{};
  math::Point anchor_hint_{};
  float control_width_{0.0f};
  // 面板生命周期簿记：摘除延迟到布局/绘制期，故在 const 绘制路径上也会被推进。
  mutable SelectPanel* panel_{nullptr};
  mutable bool dismiss_pending_{false};
  bool open_{false};
  bool cycle_consumed_{false};  ///< 同一按压周期内 down/up/click 只切换一次
};

}  // namespace st::ui
