#pragma once

/// @file icon_view.hpp
/// @brief `IconView`：图标。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/icon.hpp"

namespace st::ui {

/// 图标。
class IconView : public Element {
 public:
  explicit IconView(std::string name = {}, float size = 18.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Icon"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Icon; }

  void set_icon(std::string name);
  [[nodiscard]] auto icon() const -> const std::string& { return icon_; }
  void set_size(float size);
  void set_tone(Tone tone);
  [[nodiscard]] auto size() const noexcept -> float { return size_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return icon_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  // ⚠ `property_names()` 必须与 `get/set_property` **对称**：
  //
  // `ui::apply_properties` 的判据是“属性名在元素自己声明的清单里”（那是为了
  // 消除“读得到、改不了”的分裂）。只实现 get/set 而不报名字，就意味着
  // **这条路永远命不中它**——`set {icon: …}` 会静默什么都不做。
  // 实测：`<Icon name="search"/>` 走 XML 建树时报“值没被接受”，根因就在这里。
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string icon_{};
  float size_{18.0f};
  Tone tone_{Tone::Default};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::IconView, "Icon")
