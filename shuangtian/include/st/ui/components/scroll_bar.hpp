#pragma once

/// @file scroll_bar.hpp
/// @brief `ScrollBar`：垂直滚动条：轨道 + 圆角 pill 滑块（宽 8px，hover/拖拽时 10px）。。
///
/// 从 `scroll.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `scroll.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 垂直滚动条：轨道 + 圆角 pill 滑块（宽 8px，hover/拖拽时 10px）。
/// 几何由 `set_scroll_geometry` 注入（内容高 / 视口高 / 偏移），滚动条本身不持有内容。
class ScrollBar : public Element {
 public:
  static constexpr float kBarWidth{8.0f};        ///< 常态宽度（px）
  static constexpr float kBarWidthHover{10.0f};  ///< hover / 拖拽宽度（px）
  static constexpr float kMinThumbLength{18.0f}; ///< 滑块最小长度（px，避免内容极长时不可抓）

  ScrollBar();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ScrollBar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ScrollBar; }

  /// 设定滚动几何（单位 px）：`content_height` 为内容总高、`viewport_height` 为可视高、`offset` 为当前偏移。
  /// 偏移自动夹取到 `[0, content_height - viewport_height]`。
  void set_scroll_geometry(float content_height, float viewport_height, float offset) noexcept;

  [[nodiscard]] auto offset() const noexcept -> float { return offset_; }
  /// 最大可滚动距离（内容高 ≤ 视口高时为 0）。
  [[nodiscard]] auto max_offset() const noexcept -> float;
  /// 滑块长度占轨道长的比例（视口/内容，夹取到 [0.08, 1]）。
  [[nodiscard]] auto thumb_ratio() const noexcept -> float;
  /// 滑块矩形（绝对坐标；未 arrange 时为空）。hover/拖拽时按加宽后的宽度返回。
  [[nodiscard]] auto thumb_rect() const noexcept -> math::Rect;
  [[nodiscard]] auto dragging() const noexcept -> bool { return dragging_; }
  [[nodiscard]] auto hovered_width() const noexcept -> float {
    return (hovered_ || dragging_) ? kBarWidthHover : kBarWidth;
  }

  /// 拖拽/点击轨道时回调（参数为目标偏移 px）。
  void set_on_scroll(std::function<void(float)> callback) { on_scroll_ = std::move(callback); }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  /// 整个滚动条都是“可拖拽滚动”的（轨道与滑块同一意图）。
  ///
  /// 为何必须显式声明：不声明时落到父元素，在 `ScrollView` 里就是箭头（无害），
  /// 但在**代码编辑器自绘**的场景下实打实是 I-beam（见 `MouseCursor::Scroll` 的说明）。
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override {
    (void)point;
    return MouseCursor::Scroll;
  }
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override;

 private:
  /// 按指针 y 计算偏移（拖拽：保留抓取点相对滑块顶部的距离）。
  void apply_pointer(float pointer_y);
  [[nodiscard]] auto thumb_length() const noexcept -> float;

  float content_height_{0.0f};
  float viewport_height_{0.0f};
  float offset_{0.0f};
  float grab_{0.0f};
  bool dragging_{false};
  std::function<void(float)> on_scroll_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::ScrollBar, "ScrollBar")
