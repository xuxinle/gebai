#pragma once

/// @file badge.hpp
/// @brief `Badge`：徽标：`tone_soft_color` 胶囊底 + `tone_color` 文本（高 20、横向内边距 8）。。
///
/// 从 `feedback.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `feedback.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 徽标：`tone_soft_color` 胶囊底 + `tone_color` 文本（高 20、横向内边距 8）。
class Badge : public Element {
 public:
  static constexpr float kHeight{20.0f};
  static constexpr float kPaddingX{8.0f};

  explicit Badge(std::string text = {}, Tone tone = Tone::Default);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Badge"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string text_{};
  Tone tone_{Tone::Default};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Badge, "Badge")
