#pragma once

/// @file rating.hpp
/// @brief `Rating`：评分（星标 / 只读 / 半星 / 键盘可调）。
///
/// ## 为什么不用「几个 Icon 拼一行」
///
/// 评分有四个**只有它才需要**的语义，拼图做不出来：
///
/// 1. **半星**：`3.5` 要画成「三颗满 + 一颗半」。半星要么裁剪一个星，要么
///    并用两种颜色——手拼时几乎一定退化成「四舍五入成 4 颗」。
/// 2. **悬停预览**：鼠标移到第 4 颗时，前三颗应**预览为选中**（还没点），
///    移开后回到实际值。这是评分的核心手感，靠 `hover` 状态 + 每星命中区。
/// 3. **键盘可调**：`←`/`→` 增减一档、`Home`/`End` 到两端（分数也是表单控件）。
/// 4. **点击已选中的星**：很多产品语义是「再点一次清零」（`allow_clear`）。
///
/// ## 几何
///
/// ```
/// ★ ★ ★ ★ ☆        ← 每颗星一个命中区，间距统一
/// 3.5 / 5（可选显示数值）
/// ```
///
/// ## 属性面
///
/// `value`（当前值，可写；支持半档）、`max`（星数，可写）、`readonly`（可写）、
/// `allow_half`（可写）、`allow_clear`（可写）、`show_value`（可写）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 评分。
class Rating : public Element {
 public:
  static constexpr float kStarSize{18.0f};     ///< 单颗星边长（px）
  static constexpr float kStarGap{2.0f};       ///< 星间距（px）
  static constexpr float kValueGap{6.0f};      ///< 数值文本与星的间距（px）

  Rating();
  explicit Rating(int max_stars);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Rating"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Slider; }

  /// 当前值（夹在 `0..max`；`allow_half` 为真时可落半档）。
  void set_value(float value);
  [[nodiscard]] auto value() const noexcept -> float { return value_; }
  /// 星数（1..10）。
  void set_max(int max_stars);
  [[nodiscard]] auto max_stars() const noexcept -> int { return max_; }
  /// 只读：不可交互（不显示悬停预览、不响应点击/键盘）。
  void set_readonly(bool readonly);
  [[nodiscard]] auto readonly() const noexcept -> bool { return readonly_; }
  /// 允许半星（点击左半边 = 半档）。
  void set_allow_half(bool allow);
  [[nodiscard]] auto allow_half() const noexcept -> bool { return allow_half_; }
  /// 再次点击当前值可清零。
  void set_allow_clear(bool allow);
  [[nodiscard]] auto allow_clear() const noexcept -> bool { return allow_clear_; }
  /// 星后显示数值（如 `3.5`）。
  void set_show_value(bool show);
  [[nodiscard]] auto show_value() const noexcept -> bool { return show_value_; }
  void set_star_size(float size);
  [[nodiscard]] auto star_size() const noexcept -> float { return star_size_; }

  /// 第 `index` 颗星（0 基）的矩形（arrange 后有效；越界为空）。
  [[nodiscard]] auto star_rect(std::size_t index) const -> math::Rect;
  /// 当前**显示**值（悬停时是预览值；`readonly` 时恒等于 `value_`）。
  [[nodiscard]] auto displayed_value() const noexcept -> float;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 值变化（实参为新值）。
  std::function<void(float)> on_change{};

 private:
  /// 坐标 → 目标值（含半星判定）；不在任何星上返回 `-1`。
  [[nodiscard]] auto value_at(math::Point point) const -> float;
  void commit(float value);

  float value_{0.0f};
  int max_{5};
  bool readonly_{false};
  bool allow_half_{false};
  bool allow_clear_{true};
  bool show_value_{false};
  float star_size_{kStarSize};
  float hover_value_{-1.0f};

  mutable std::vector<math::Rect> star_boxes_{};
  mutable math::Rect value_box_{};
  math::Color filled_color_{};
  math::Color empty_color_{};
  math::Color text_color_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Rating, "Rating")
