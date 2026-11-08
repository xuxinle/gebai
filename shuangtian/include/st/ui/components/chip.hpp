#pragma once

/// @file chip.hpp
/// @brief `Chip`：标签片：高 26 胶囊，可选前导圆点与关闭 ×（16px 命中区）。。
///
/// 从 `feedback.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `feedback.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 标签片：高 26 胶囊，可选前导圆点与关闭 ×（16px 命中区）。
class Chip : public Element {
 public:
  static constexpr float kHeight{26.0f};
  static constexpr float kPaddingX{12.0f};
  static constexpr float kDotSize{6.0f};      ///< 前导圆点直径（px）
  static constexpr float kGap{6.0f};          ///< 元素间距（px）
  static constexpr float kCloseArea{16.0f};   ///< 关闭命中区边长（px）
  static constexpr float kCloseGlyph{12.0f};  ///< 关闭图标边长（px）

  explicit Chip(std::string text = {}, bool closable = false);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Chip"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Button; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 前导圆点（与文本同色调）。
  void set_dot(bool dot);
  [[nodiscard]] auto dot() const noexcept -> bool { return dot_; }
  void set_closable(bool closable);
  [[nodiscard]] auto closable() const noexcept -> bool { return closable_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  /// 关闭 × 的命中区（arrange 后有效）。
  [[nodiscard]] auto close_area() const noexcept -> math::Rect;
  /// 文本绘制区（arrange 后有效）。
  [[nodiscard]] auto text_area() const noexcept -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  std::function<void()> on_close{};
  std::function<void()> on_click{};

 private:
  std::string text_{};
  Tone tone_{Tone::Default};
  bool dot_{false};
  bool closable_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Chip, "Chip")
