#pragma once

/// @file empty_state.hpp
/// @brief `EmptyState`：空态占位（图标 + 标题 + 说明 + 可选动作），四档尺寸。
///
/// ## 为什么需要它
///
/// 「什么都没有」是最常被漏做的界面。没有空态时用户的读法是「坏了/没加载完」，
/// 而不是「确实没有」——差别全在这块占位上：一句**为什么空**和**下一步做什么**。
///
/// 此前各页面各写一套（垂直居中的 Icon + Text + Button 组合），三处不一致：
/// 图标大小、说明色调、动作按钮是否在、垂直留白比例。抽成一个组件后，
/// 「空态长什么样」只有一份定义。
///
/// ## 视觉（四档）
///
/// | 尺寸 | 图标 | 标题 | 说明 | 纵向留白 |
/// |---|---|---|---|---|
/// | `Small` | 24 | `font_sm` | `font_xs` | 16 |
/// | `Medium`（默认） | 36 | `font_base` | `font_sm` | 28 |
/// | `Large` | 52 | `font_lg` | `font_base` | 44 |
/// | `Hero` | 72 | `font_xl` | `font_base` | 64 |
///
/// ## 属性面
///
/// `title`（可写）、`description`（可写）、`icon`（可写）、`size`（可写）、
/// `action`（主按钮文案，可写；空 = 不显示按钮）。
/// 动作面：`activate` / `click`（等同点主按钮）。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 空态占位。
class EmptyState : public Element {
 public:
  /// 四档尺寸（图标/字号/留白联动）。
  enum class Size : std::uint8_t { Small, Medium, Large, Hero };

  EmptyState();
  explicit EmptyState(std::string title, std::string description = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "EmptyState"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 数据 ——
  void set_title(std::string title);
  [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
  void set_description(std::string description);
  [[nodiscard]] auto description() const noexcept -> const std::string& { return description_; }
  /// 图标名（`Icon` 表里的名字；空 = 不画图标）。
  void set_icon(std::string icon_name);
  [[nodiscard]] auto icon() const noexcept -> const std::string& { return icon_; }
  /// 主动作按钮文案（空 = 不显示按钮）。
  void set_action(std::string label);
  [[nodiscard]] auto action() const noexcept -> const std::string& { return action_; }
  void set_size(Size size);
  [[nodiscard]] auto size() const noexcept -> Size { return size_; }

  // —— 几何（arrange 后有效）——
  /// 主按钮矩形（无按钮时为空）。
  [[nodiscard]] auto action_rect() const noexcept -> math::Rect { return action_box_; }
  /// 图标矩形（无图标时为空）。
  [[nodiscard]] auto icon_rect() const noexcept -> math::Rect { return icon_box_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return title_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 主按钮点击。
  std::function<void()> on_action{};

 private:
  /// 当前档位的图标边长。
  [[nodiscard]] auto icon_size() const noexcept -> float;
  /// 当前档位的标题字号。
  [[nodiscard]] auto title_font_size() const -> float;
  /// 当前档位的说明字号。
  [[nodiscard]] auto description_font_size() const -> float;
  /// 当前档位的纵向留白。
  [[nodiscard]] auto vertical_padding() const noexcept -> float;
  /// 按当前尺寸重算各块落位（`measure` 与 `arrange` 共用同一份几何）。
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::string title_{};
  std::string description_{};
  std::string icon_{"folder-open"};
  std::string action_{};
  Size size_{Size::Medium};

  float title_font_{15.0f};
  float description_font_{14.0f};
  mutable math::Rect icon_box_{};
  mutable math::Rect title_box_{};
  mutable math::Rect description_box_{};
  mutable math::Rect action_box_{};
  mutable bool hover_action_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::EmptyState, "EmptyState")
