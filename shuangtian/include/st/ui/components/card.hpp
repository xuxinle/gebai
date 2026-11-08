#pragma once

/// @file card.hpp
/// @brief `Card`：卡片容器：`surface` 底 + 描边 + 圆角 + 阴影 + 内边距。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 卡片容器：`surface` 底 + 描边 + 圆角 + 阴影 + 内边距。
class Card : public Element {
 public:
  explicit Card(float padding = 16.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Card"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  void set_padding(float padding);
  void set_radius(float radius);
  void set_shadow_level(std::uint8_t level);
  void set_elevated(bool elevated);
  void apply_theme(const Theme& theme) override;

 private:
  float padding_{16.0f};
  float radius_override_{0.0f};
  /// 0=无阴影、1=sm、≥2=md（`elevated_` 为真时一律 lg）。
  /// 卡片默认用 **md**：浅底上的卡片需要“真的浮起来”，而 sm 更像一道描边。
  std::uint8_t shadow_level_{2};
  bool elevated_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Card, "Card")
