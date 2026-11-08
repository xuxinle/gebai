#pragma once

/// @file select_box.hpp
/// @brief `Select`：下拉选择控件（点击开面板、选中回调）。
///
/// 从 `select.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
///
/// ⚠ 文件名是 `select_box.hpp` 而不是 `select.hpp`：后者是**聚合头**
/// （把 `select_panel.hpp` + 本头一起拉进来）。同名会让聚合头 include 自己
/// ——实测后果是 `Select` 类“莫名消失”（那个头被自己的聚合版本覆盖了）。
///
/// `select.hpp` 仍保留**聚合**，既有引用不必一次改完。

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "st/ui/component_registry.hpp"
#include "st/ui/components/select_panel.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

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

// `Select` 无参可构（选项随后经 `set_options` 填）。
ST_UI_COMPONENT(::st::ui::Select, "Select")
