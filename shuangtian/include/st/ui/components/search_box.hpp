#pragma once

/// @file search_box.hpp
/// @brief `SearchBox`：搜索框（放大镜 + 输入 + 清除 × + 结果计数徽标）。
///
/// ## 与 `Input` 的差别
///
/// `Input` 是**通用**文本输入；搜索框有四个只在搜索语义下存在的部件与行为：
///
/// | 部件 / 行为 | 手搭 `Input` 时容易漏的 |
/// |---|---|
/// | 前置放大镜 | 图标位留了但画不出（`Input::set_icon_prefix` 目前只占位不绘制） |
/// | 清除 × | 只在**有内容**时出现，且它的命中区不能盖住输入区 |
/// | 结果计数（`3 项`） | 位置与输入文本冲突（内容长时会互相覆盖） |
/// | `Esc` 清空、`Enter` 提交 | 搜索框的标准键位，通用输入框不该有 |
///
/// ## 几何
///
/// ```
/// ┌──┬───────────────────────────┬───┬─────┐
/// │🔍│ 输入区（左对齐、可滚动）  │ × │ 3 项│
/// └──┴───────────────────────────┴───┴─────┘
/// ```
///
/// 右到左依次让位：计数值宽 → 清除位（有内容才有）→ 输入区拿到剩下的宽。
///
/// ## 属性面
///
/// `value`（可写）、`placeholder`（可写）、`count`（结果数，`-1` = 不显示）、
/// `clearable`（可写）。
/// 动作面：`clear`（清空并派发）、`submit`（派发）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 搜索框。
class SearchBox : public Element {
 public:
  static constexpr float kHeight{30.0f};        ///< 默认高（px）
  static constexpr float kPaddingX{8.0f};       ///< 横向内边距（px）
  static constexpr float kIconSize{14.0f};      ///< 放大镜边长（px）
  static constexpr float kGap{6.0f};            ///< 部件间距（px）
  static constexpr float kClearArea{18.0f};     ///< 清除 × 命中区边长（px）
  static constexpr float kRadius{6.0f};         ///< 圆角（px）

  SearchBox();
  explicit SearchBox(std::string placeholder);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SearchBox"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  void set_value(std::string value);
  [[nodiscard]] auto value() const noexcept -> const std::string& { return value_; }
  void set_placeholder(std::string placeholder);
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  /// 结果数（`< 0` = 不显示徽标）。
  void set_count(int count);
  [[nodiscard]] auto count() const noexcept -> int { return count_; }
  void set_clearable(bool clearable);
  [[nodiscard]] auto clearable() const noexcept -> bool { return clearable_; }
  /// 清空并派发 `on_change` / `on_clear`。
  void clear();

  // —— 几何（arrange 后有效）——
  [[nodiscard]] auto field_rect() const noexcept -> math::Rect { return field_; }
  [[nodiscard]] auto clear_rect() const noexcept -> math::Rect { return clear_; }
  [[nodiscard]] auto icon_rect() const noexcept -> math::Rect { return icon_; }
  [[nodiscard]] auto count_rect() const noexcept -> math::Rect { return count_box_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return value_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 文本变化（每次输入/清空）。
  std::function<void(std::string_view)> on_change{};
  /// `Enter` 提交。
  std::function<void(std::string_view)> on_submit{};
  /// 点 × 或按 `Esc`。
  std::function<void()> on_clear{};

 private:
  [[nodiscard]] auto caret_x(const RenderContext& context) const -> float;
  auto handle_key(const Event& event) -> bool;
  void insert_text(std::string_view inserted);

  std::string value_{};
  std::string placeholder_{"搜索"};
  int count_{-1};
  bool clearable_{true};
  std::size_t caret_{0};

  mutable math::Rect field_{};
  mutable math::Rect clear_{};
  mutable math::Rect icon_{};
  mutable math::Rect count_box_{};
  mutable bool hover_clear_{false};
  mutable int last_blink_visible_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::SearchBox, "SearchBox")
