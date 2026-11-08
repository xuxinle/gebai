#pragma once

/// @file skeleton.hpp
/// @brief `Skeleton`：骨架屏占位（矩形 / 圆形 / 文本行三种形态，脉冲由时间驱动）。
///
/// ## 为什么它是独立组件而不是「若干个灰色方块」
///
/// 骨架屏的价值全在**形状与真实内容同构**：三行文字的高度/间距要和真文本一致，
/// 头像用圆、卡片用圆角矩形。手摆一堆 `Panel` + 背景色做不到这件事：
/// 行高、行间距、圆角、脉冲节奏会各自为政，切换时内容会「跳动」。
///
/// ## 三种形态
///
/// | 形态 | 用途 | 几何 |
/// |---|---|---|
/// | `Rect` | 图片位、卡片、封面 | 单块圆角矩形（`lines=1`）或若干块 |
/// | `Circle` | 头像位 | 单块正圆 |
/// | `Text` | 段落占位 | `lines` 行，行高 = 字号 × 行距，末行按 `last_line_ratio` 缩短 |
///
/// ## 脉冲
///
/// 呼吸式明暗循环（`kPulsePeriodSeconds` 一个来回），相位**完全由
/// `RenderContext::time_seconds` 决定**——于是同一时刻的像素是确定的，可截图回归。
///
/// ## 属性面
///
/// `lines`（行数）、`shape`（rect/circle/text）、`last_line_ratio`、
/// `pulse`（是否呼吸）、`radius`。只读：`height`（当前排版高度）。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 骨架屏占位。
class Skeleton : public Element {
 public:
  /// 占位形态。
  enum class Shape : std::uint8_t { Rect, Circle, Text };

  static constexpr float kLineHeight{14.0f};        ///< 文本行的行高（px）
  static constexpr float kLineGap{10.0f};           ///< 行间距（px）
  static constexpr float kDefaultRadius{6.0f};      ///< 圆角（px）
  static constexpr float kPulsePeriodSeconds{1.6f}; ///< 一次呼吸的周期（秒）
  static constexpr float kPulseDepth{0.35f};        ///< 明暗幅度（0 = 不动）

  Skeleton();
  /// 建一个 `lines` 行的文本骨架。
  explicit Skeleton(std::size_t lines);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Skeleton"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::None; }

  void set_shape(Shape shape);
  [[nodiscard]] auto shape() const noexcept -> Shape { return shape_; }
  /// 行数（`Rect`/`Circle` 强制为 1）。
  void set_lines(std::size_t lines);
  [[nodiscard]] auto lines() const noexcept -> std::size_t { return lines_; }
  /// 末行占满宽的比例（默认 `0.6`：真实段落的最后一行通常不满）。
  void set_last_line_ratio(float ratio);
  [[nodiscard]] auto last_line_ratio() const noexcept -> float { return last_line_ratio_; }
  /// 是否呼吸（默认 `true`；静态场景可关掉）。
  void set_pulse(bool pulse);
  [[nodiscard]] auto pulse() const noexcept -> bool { return pulse_; }
  /// 圆角（`Circle` 忽略）。
  void set_radius(float radius);

  /// 指定时刻的明暗系数（`1.0` = 原色，`1 - kPulseDepth` = 最暗）。
  [[nodiscard]] auto pulse_factor(double time_seconds) const noexcept -> float;
  /// 第 `index` 块的矩形（arrange 后有效；越界为空）。
  [[nodiscard]] auto block_rect(std::size_t index) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

 private:
  void relayout() const;

  Shape shape_{Shape::Text};
  std::size_t lines_{3};
  float last_line_ratio_{0.6f};
  bool pulse_{true};
  float radius_{kDefaultRadius};
  math::Color base_color_{};
  math::Color block_color_{};

  mutable std::vector<math::Rect> blocks_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Skeleton, "Skeleton")
