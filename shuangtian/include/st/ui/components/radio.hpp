#pragma once

/// @file radio.hpp
/// @brief `Radio`：单选框：圆形外圈 + 选中内圆点。同组互斥由调用方用 `set_checked` 管理（本组件不做分组）。。
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

}  // namespace st::ui

ST_UI_COMPONENT_WITH(::st::ui::Radio, "Radio", std::make_unique<::st::ui::Radio>(""))
