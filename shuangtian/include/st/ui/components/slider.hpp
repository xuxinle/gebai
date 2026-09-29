#pragma once

/// 组件库 A · 连续取值（`DESIGN.md` §4.5）：`Slider`（单值滑杆）。
///
/// 依赖纪律：文本经 `context.text`（空则退化 `NullTextPort`）；颜色/间距/圆角/字号一律取
/// `context.theme` 的 token（`apply_theme` 时落进 `style_`），组件内不硬编码颜色。
///
/// 交互：按下与按住拖动（`MouseDown` 后跟随 `MouseMove`）即时更新取值，并按轨道内缘映射后
/// 夹取到 `[0,1]`；`ArrowLeft`/`ArrowRight` 按 `step` 步进、`Home`/`End` 到两端；
/// 取值变化经 `on_change` 通知（拖拽过程中逐次通知）。
///
/// 控制通道属性面：`value`（浮点字符串，写回即夹取）、`label`（无障碍名称）、`step`。

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 单值滑杆：6px 圆角轨道 + 已选段 `primary` + 16px 圆滑块（`shadow_sm`，拖拽时主色光圈）。
class Slider : public Element {
 public:
  explicit Slider(float value = 0.0f);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Slider"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Slider; }

  /// 设置取值（夹取到 `[0,1]`；`notify` 为 true 时触发 `on_change`）。
  void set_value(float value, bool notify = false);
  [[nodiscard]] auto value() const noexcept -> float { return value_; }

  /// 无障碍名称（`semantics_text`；不绘制）。
  void set_label(std::string label);
  [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }

  /// 键盘步进量（夹取到 `(0,1]`）。
  void set_step(float step);
  [[nodiscard]] auto step() const noexcept -> float { return step_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 取值变化回调。
  std::function<void(float)> on_change{};

 private:
  [[nodiscard]] auto track_rect(const RenderContext& context) const -> math::Rect;
  [[nodiscard]] auto knob_center(const RenderContext& context) const -> math::Point;
  /// 横坐标 → 取值（按滑块中心可移动范围映射，夹取到 `[0,1]`）。
  [[nodiscard]] auto value_at_x(const RenderContext& context, float x) const -> float;
  /// 步进取值（`notify` 控制是否回调）。
  void nudge(float delta, bool notify);

  float value_{0.0f};
  float step_{0.05f};
  std::string label_{};
};

}  // namespace st::ui
