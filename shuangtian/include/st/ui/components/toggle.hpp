#pragma once

/// 组件库 A · 开关类控件（`DESIGN.md` §4.5）：`Checkbox`（复选框）、`Radio`（单选框）、`Switch`（开关）。
///
/// 依赖纪律：文本经 `context.text`（空则退化 `NullTextPort`）；外观只取 `context.theme` 的 token。
/// 三者共用同一交互语义：`MouseDown` 记录按压 → 首次 `MouseUp`/`Click` 触发一次切换
/// （同一按压周期内去重，避免 down+up+click 三帧导致重复切换），`activate()`（Enter/Space/控制通道
/// `invoke`）同样触发切换，变更经 `on_change` 通知。
///
/// 控制通道属性面：三者均暴露 `checked`（同义 `value`，布尔字符串）与 `label`（同义 `text`）；
/// `set checked=false` 等写入不触发 `on_change`（`toggle()`/交互才通知），避免控制通道回环。

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 复选框：18×18 圆角方框（圆角取 `min(metrics.radius_sm, 5)`），选中态填充 `primary` 并描白色对勾。
class Checkbox : public Element {
 public:
  explicit Checkbox(std::string label = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Checkbox"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Checkbox; }

  void set_label(std::string label);
  void set_checked(bool value) noexcept;
  /// 反转并通知 `on_change`。
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

  /// 选中状态变化回调。
  std::function<void(bool)> on_change{};

 protected:
  /// 方框/圆形指示器的矩形（左对齐、竖向居中）。
  [[nodiscard]] auto indicator_rect(const RenderContext& context) const -> math::Rect;

 private:
  std::string label_{};
  bool checked_{false};
  bool cycle_consumed_{false};
};

/// 单选框：圆形外圈 + 选中内圆点。同组互斥由调用方用 `set_checked` 管理（本组件不做分组）。
class Radio : public Element {
 public:
  explicit Radio(std::string label = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Radio"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Radio; }

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

  /// 选中状态变化回调。
  std::function<void(bool)> on_change{};

 private:
  [[nodiscard]] auto indicator_rect(const RenderContext& context) const -> math::Rect;

  std::string label_{};
  bool checked_{false};
  bool cycle_consumed_{false};
};

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
