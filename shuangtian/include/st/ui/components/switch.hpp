#pragma once

/// @file switch.hpp
/// @brief `Switch`：开关：40×22 胶囊轨道 + 22px 圆形滑块；开=`primary`，关=`surface_sunken`+边框。。
///
/// 从 `toggle.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `toggle.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 开关：40×22 胶囊轨道 + 22px 圆形滑块；开=`primary`，关=`surface_sunken`+边框。
/// 位置随 `context.time_seconds` 以 `metrics.motion_normal`（180ms）插值（缓出）。
class Switch : public Element {
 public:
  explicit Switch(std::string label = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Switch"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Switch; }

  void set_label(std::string label);
  void set_checked(bool value) noexcept;
  void toggle();

  [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }
  [[nodiscard]] auto checked() const noexcept -> bool { return checked_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  /// 开关状态变化回调。
  std::function<void(bool)> on_change{};

 private:
  [[nodiscard]] auto track_rect(const RenderContext& context) const -> math::Rect;
  /// 当前滑块进度 [0,1]（按 `time_seconds` 推演；绘制期间推进动画状态）。
  [[nodiscard]] auto knob_progress(const RenderContext& context) const -> float;

  std::string label_{};
  bool checked_{false};
  bool cycle_consumed_{false};
  mutable double toggle_time_{-1.0};
  mutable double last_time_{0.0};
  mutable bool animation_from_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT_WITH(::st::ui::Switch, "Switch", std::make_unique<::st::ui::Switch>(""))
