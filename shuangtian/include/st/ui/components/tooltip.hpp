#pragma once

/// @file tooltip.hpp
/// @brief `Tooltip`：提示气泡：`set_target_rect` 后自动定位到目标上方居中；`set_active` 控制显隐。。
///
/// 从 `feedback.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `feedback.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 提示气泡：`set_target_rect` 后自动定位到目标上方居中；`set_active` 控制显隐。
class Tooltip : public Element {
 public:
  static constexpr float kPaddingX{8.0f};
  static constexpr float kPaddingY{4.0f};
  static constexpr float kGap{6.0f};     ///< 与目标的间距（px）
  static constexpr float kRadius{8.0f};  ///< 圆角（px）

  explicit Tooltip(std::string text = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Tooltip"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Tooltip; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 目标矩形（屏幕坐标）：arrange 时据此把气泡放到上方居中（空间不足则放下方）。
  void set_target_rect(math::Rect rect);
  [[nodiscard]] auto target_rect() const noexcept -> math::Rect { return target_; }
  /// **视口尺寸**（用于把气泡夹在屏幕内）。
  ///
  /// 为何不能从 `arrange` 的入参推：`OverlayLayout::Stack` 传的是气泡自身尺寸的矩形，
  /// 不是窗口——拿它当视口会把 x 钉死在 0（气泡跑到窗口左缘，看着像“没显示”）。
  void set_viewport(math::Size size);
  [[nodiscard]] auto viewport() const noexcept -> math::Size { return viewport_; }
  void set_active(bool active);
  [[nodiscard]] auto active() const noexcept -> bool { return active_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_impl(const RenderContext& context, raster::Surface& canvas) const override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return text_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string text_{};
  math::Rect target_{};
  /// 视口尺寸（0 = 未知，退化为只夹左上）。见 `set_viewport`。
  math::Size viewport_{};
  bool active_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Tooltip, "Tooltip")
