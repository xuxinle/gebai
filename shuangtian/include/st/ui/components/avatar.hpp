#pragma once

/// @file avatar.hpp
/// @brief `Avatar`：头像：圆形底色 `primary_soft` + `primary` 首码点（28 / 36 / 48 三档）。。
///
/// 从 `feedback.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `feedback.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 头像：圆形底色 `primary_soft` + `primary` 首码点（28 / 36 / 48 三档）。
class Avatar : public Element {
 public:
  static constexpr float kSmall{28.0f};
  static constexpr float kMedium{36.0f};
  static constexpr float kLarge{48.0f};

  explicit Avatar(std::string name = {}, float size = kMedium);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Avatar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Image; }

  void set_name(std::string name);
  [[nodiscard]] auto name() const noexcept -> const std::string& { return name_; }
  void set_size(float size);
  [[nodiscard]] auto size() const noexcept -> float { return size_; }
  /// 用于绘制的首字母（UTF-8 首码点；ASCII 字母大写化）。
  [[nodiscard]] auto initials() const -> std::string;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return initials(); }
  [[nodiscard]] auto semantics_value() const -> std::string override { return name_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string name_{};
  float size_{kMedium};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Avatar, "Avatar")
