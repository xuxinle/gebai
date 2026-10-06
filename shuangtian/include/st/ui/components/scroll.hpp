#pragma once

/// 组件库 B（数据展示与反馈）——滚动容器 `ScrollView` 与垂直滚动条 `ScrollBar`。
/// 视觉规格：滚动条宽 8px（hover 加宽到 10px）、滑块为圆角 pill（`border_strong`，hover/拖拽 `text_faint`）；
/// 交互规格：滚轮每格 48px、PageUp/PageDown 按视口高翻页、Home/End 首尾、滑块可拖拽；
/// 内容超出视口时以 `push_clip_rect` / `push_clip_rounded_rect` 裁剪子节点。

#include <functional>
#include <vector>
#include <optional>
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
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
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

  /// 载体子元素：**调用方声明的内容在前，自持滚动条在末尾**（不计数）。
  ///
  /// 声明式的「位置对齐 + 末尾裁剪」只看前面这些，不会动我们的滚动条——
  /// 否则它会把自己的滚动条当成「上一帧多声明的残留」移除掉，
  /// 而本组件持有的 `bar_` 裸指针随即悬垂（实测：声明式里 ScrollView 与 List
  /// 两个分支互切 → `ScrollView::arrange` 在 `bar_->arrange` 上段错误）。
  [[nodiscard]] auto content_child_count() const noexcept -> std::size_t override {
    return children_.empty() ? 0 : children_.size() - 1;   // 滚动条恒在末尾
  }

  /// 内容子元素插到滚动条**之前**（滚动条恒在末尾；见 `content_child_count`）。
  auto add_child(std::unique_ptr<Element> child) -> Element* override;
  auto insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* override;

  /// 滚到底部（等价 `scroll_to(1e9)`，但**语义明确**）。
  ///
  /// 注意时机：`max_scroll()` 取的是**上一次布局**的 `content_height_`。
  /// 刚追加内容就调，会被**旧上限夹住**、停在中间（实测：终端跟随输出时尾部两行看不到）。
  /// 要在内容变化后的**下一帧**再调（那时几何已经是新的）。
  void scroll_to_end() { scroll_to(1.0e9f); }
  /// 是否已在底部（容差 1px）。判定“要不要自动跟随”用它，别与 `max_scroll()` 手算。
  [[nodiscard]] auto at_end() const noexcept -> bool {
    return scroll_offset() >= max_scroll() - 1.0f;
  }
  [[nodiscard]] auto scroll_offset() const noexcept -> float { return offset_; }

  // —— 列表类通用视口契约（与 Tree/List 同口径）——
  // 滚动容器是**唯一真正持有视口**的组件，所以 `first_visible` 这类量的权威来源在这。
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
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
  /// **跟随模式**：每次布局后自动停在底部（日志/终端/输出窗的语义）。
  ///
  /// 为何必须在 `arrange` 里做而不能让调用方“先滚一下”（2026-10-06，实测）：
  /// `max_scroll()` 取的是**上一次布局**的 `content_height_`。调用方在追加内容后
  /// 立即 `scroll_to_end()`，会被**旧上限夹住**（例：内容追加到 331.8、而上限还是
  /// 175.6，就停在了 175.6）；等布局把内容量成新值并重新夹取时，它已经是一个
  /// 合法偏移，没有人知道它本该在底部——于是**每次都差新追加的那几行**
  /// （表现为滚动条永远差两行到底，尾部看不到）。
  /// 开启这个模式后，“在底部”是**持续意图**而非一次性动作：布局算完新几何、
  /// 归一化偏移后立即跟到底，无论追加了多少。
  ///
  /// 用户往上翻时应关掉（否则每来一行输出就把人拉回底部）；
  /// 典型做法是在 `set_on_scroll` 回调里关。
  void set_follow_end(bool follow) noexcept { follow_end_ = follow; }
  [[nodiscard]] auto follow_end() const noexcept -> bool { return follow_end_; }
  [[nodiscard]] auto scroll_bar() noexcept -> ScrollBar* { return bar_; }
  [[nodiscard]] auto scroll_bar() const noexcept -> const ScrollBar* { return bar_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint(const RenderContext& context, raster::Surface& canvas) const override;
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
  bool follow_end_{false};
  std::function<void(float)> on_scroll_{};
};

}  // namespace st::ui
