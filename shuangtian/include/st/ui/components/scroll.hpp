#pragma once

/// 组件库 B（数据展示与反馈）——滚动容器 `ScrollView` 与垂直滚动条 `ScrollBar`。
/// 视觉规格：滚动条宽 8px（hover 加宽到 10px）、滑块为圆角 pill（`border_strong`，hover/拖拽 `text_faint`）；
/// 交互规格：滚轮每格 48px、PageUp/PageDown 按视口高翻页、Home/End 首尾、滑块可拖拽；
/// 内容超出视口时以 `push_clip_rect` / `push_clip_rounded_rect` 裁剪子节点。

#include <functional>
#include <string>
#include <string_view>

#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

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
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
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

/// 垂直滚动容器：子节点按列堆叠，超出视口部分被裁剪，右侧自动出现 `ScrollBar`。
/// 滚动不改变子节点尺寸，只平移其 `bounds`（因此会触发重排）。
class ScrollView : public Element {
 public:
  static constexpr float kStep{48.0f};  ///< 滚轮每格滚动距离（px）
  static constexpr float kPageRatio{0.9f};
  static constexpr float kBarGap{4.0f};  ///< 内容区与滚动条之间的间隙（px）

  ScrollView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ScrollView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  [[nodiscard]] auto scroll_offset() const noexcept -> float { return offset_; }
  /// 滚动到指定偏移（夹取到 `[0, max_scroll]`）；会重排子节点。
  void scroll_to(float offset);
  void scroll_by(float delta) { scroll_to(offset_ + delta); }
  /// 内容总高（含 padding 与间隙）。
  [[nodiscard]] auto content_height() const noexcept -> float { return content_height_; }
  /// 可视区高（arrange 后有效）。
  [[nodiscard]] auto view_height() const noexcept -> float { return view_height_; }
  [[nodiscard]] auto max_scroll() const noexcept -> float;
  void set_scroll_step(float step) noexcept { step_ = step > 0.0f ? step : kStep; }
  [[nodiscard]] auto scroll_step() const noexcept -> float { return step_; }
  /// 是否保留滚动条占位（默认 true；关闭后内容可用满宽）。
  void set_show_scrollbar(bool show) noexcept;
  [[nodiscard]] auto show_scrollbar() const noexcept -> bool { return show_bar_; }
  void set_on_scroll(std::function<void(float)> callback) { on_scroll_ = std::move(callback); }
  [[nodiscard]] auto scroll_bar() noexcept -> ScrollBar* { return bar_; }
  [[nodiscard]] auto scroll_bar() const noexcept -> const ScrollBar* { return bar_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

 private:
  /// 内容子节点（排除滚动条本身）。
  [[nodiscard]] auto content_extent(const Element& child) const noexcept -> float;
  [[nodiscard]] auto reserved_width() const noexcept -> float;
  void recompute_content_height() noexcept;

  ScrollBar* bar_{nullptr};
  float content_height_{0.0f};
  float view_height_{0.0f};
  float offset_{0.0f};
  float step_{kStep};
  bool show_bar_{true};
  std::function<void(float)> on_scroll_{};
};

}  // namespace st::ui
