#pragma once

/// @file divider.hpp
/// @brief `Divider`：水平分隔线。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 水平分隔线。
class Divider : public Element {
 public:
  /// 线厚（逻辑 px）。**允许亚像素**：`0.5f` 在 2 倍 DPI 下正好一个物理像素，
  /// 1 倍下会被光栅器按覆盖率画成一条淡线。比 `metrics.border_width`（1.0）轻一档，
  /// 因为分割线是“同组内的弱分隔”，不该与控件边界同重（用户 2026-10-21 要求）。
  static constexpr float kLineThickness{0.5f};

  explicit Divider(bool vertical = false);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Divider"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Separator; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

 private:
  bool vertical_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Divider, "Divider")
