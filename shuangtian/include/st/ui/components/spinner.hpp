#pragma once

/// @file spinner.hpp
/// @brief `Spinner`：旋转指示器：3/4 圆弧，相位完全由 `RenderContext::time_seconds` 决定（可截屏回归）。。
///
/// 从 `feedback.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `feedback.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 旋转指示器：3/4 圆弧，相位完全由 `RenderContext::time_seconds` 决定（可截屏回归）。
class Spinner : public Element {
 public:
  static constexpr float kDiameter{18.0f};        ///< 默认直径（px）
  static constexpr float kStroke{2.0f};           ///< 描边宽（px）
  static constexpr float kPeriodSeconds{1.2f};    ///< 一圈耗时（秒）
  static constexpr float kSweepDegrees{270.0f};   ///< 圆弧张角（度）

  explicit Spinner(float diameter = kDiameter);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Spinner"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ProgressBar; }

  void set_diameter(float diameter);
  [[nodiscard]] auto diameter() const noexcept -> float { return diameter_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  /// 指定时刻的圆弧起始角（度，0° 指向 +x 轴、顺时针为正）。
  [[nodiscard]] auto phase_degrees(double time_seconds) const noexcept -> float;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  float diameter_{kDiameter};
  Tone tone_{Tone::Primary};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Spinner, "Spinner")
