#pragma once

/// @file scroll_view.hpp
/// @brief `ScrollView`：垂直滚动容器：子节点按列堆叠，超出视口部分被裁剪，右侧自动出现 `ScrollBar`。。
///
/// 从 `scroll.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `scroll.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include "st/ui/component_registry.hpp"
#include "st/ui/components/scroll_bar.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 垂直滚动容器：子节点按列堆叠，超出视口部分被裁剪，右侧自动出现 `ScrollBar`。
/// 滚动不改变子节点尺寸，只平移其 `bounds`（因此会触发重排）。
class ScrollView : public Element {
 public:
  /// 滚轮每格滚动距离的**缺省值**（真实值优先取主题 `Metrics::wheel_step`，
  /// `apply_theme` 同步；此常量仅作离屏/无主题兑底）。
  static constexpr float kStep{48.0f};
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
  /// **可写**属性面：`offset`（夹取到 `[0, max_scroll]`）。
  ///
  /// 为何必须有（2026-10-22，相期实测）：`offset` 一直只在 `get_property` 里，
  /// 于是控制通道 `set` 与脚本 `$('#x').set({offset: …})` 都**静默无效**——
  /// 现象是“设了但没动”，而且不回错（属性名合法但无写路径）。
  /// 应用要“新内容到了自动滚到底”就只能合成滚轮事件绕，那不是应用该干的事。
  ///
  /// 语义：超过 `max_scroll` 夹到底（不是拒绝）；负值夹到 0。
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 动作面：`scroll_to`（参数 = 偏移）/ `scroll_by`（参数 = 增量）/ `scroll_to_end`。
  ///
  /// 为何除了 `set offset` 还要动作：一次性动作（"滚到底"）写成属性太隐。
  /// 而 `scroll_by` 是相对量，属性面表达不了。
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;
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
  void paint_impl(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  /// 滚动条占位区是“可拖拽滚动”，内容区交回给子节点（回 `Arrow` = 没意见）。
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
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

ST_UI_COMPONENT(::st::ui::ScrollView, "ScrollView")
