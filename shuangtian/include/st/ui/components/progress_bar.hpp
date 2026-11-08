#pragma once

/// @file progress_bar.hpp
/// @brief `ProgressBar`：进度条：圆角轨道 + 渐变填充；`value` 取值 `[0,1]`，语义值输出百分比字符串。。
///
/// 从 `feedback.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `feedback.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 进度条：圆角轨道 + 渐变填充；`value` 取值 `[0,1]`，语义值输出百分比字符串。
class ProgressBar : public Element {
 public:
  static constexpr float kTrackHeight{6.0f};  ///< 轨道高（px）

  explicit ProgressBar(float value = 0.0f);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ProgressBar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ProgressBar; }

  /// 设定进度（自动夹取到 `[0,1]`）。
  void set_value(float value);
  [[nodiscard]] auto value() const noexcept -> float { return value_; }
  /// 色调（默认 `Primary` → `primary`→`primary_hover` 渐变；其他色调为同色系渐变）。
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  float value_{0.0f};
  Tone tone_{Tone::Primary};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::ProgressBar, "ProgressBar")
