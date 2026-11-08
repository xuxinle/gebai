#pragma once

/// @file pagination.hpp
/// @brief `Pagination`：页码条（`‹ 1 … 4 5 6 … 20 ›`；中段按 `sibling_count` 折叠）。
///
/// ## 为什么需要它
///
/// 表格分页此前在每个宿主里各写一套「上一页/下一页 + 当前页文本」（gallery 的数据页、
/// 文件对话框的列表、gbcode 的问题列表）。页码条是**有确定几何约定**的控件：
/// 页码窗口、省略号占位、首末页常驻、上下页在两端禁用——分散实现必然在
/// 「第 1 页时上页该不该禁用」「页码多于 7 个怎么折」这类问题上各自为政。
///
/// ## 折叠规则（与主流产品一致，可复现）
///
/// 记当前页 `p`、总页数 `n`、邻居数 `s`（默认 1）：
///
/// - `n <= 2*s + 5` → **全部页码直出**（不再折叠：折出来反而更难读）；
/// - 否则窗口固定 `2*s + 1` 个页码，`lo = clamp(p - s, 1, n - 2*s)`：
///   **靠边时向中间撑开**而不是被夹窄（否则翻到首页时页码条突然变短、按钮位置集体
///   跳一次，连点两下很容易点错）；
/// - 窗口左端 `> 2` 才插左 `…`；右端 `< n-1` 才插右 `…`。
///   （空缺只有 1 个位置时不插 `…`：`…` 与真页码同宽，画 `…` 是白丢一个可点目标。）
///
/// ## 属性面
///
/// `page`（当前页，从 1 起，可写）、`page_count`（总页数，可写）、
/// `sibling_count`（当前页左右各显示几个，可写）、`count`/`items`（实际渲染项，只读）。
/// 动作面：`goto`（参数 = 页码）、`next`、`prev`、`first`、`last`。
///
/// 页数变化时当前页自动夹取到 `[1, page_count]`，并**不**替调用方派发 `on_change`
/// ——数据变化不是用户翻页（与 `Tabs::sync_tabs` 同一口径）。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 页码条：上一页 / 页码窗口（含省略号）/ 下一页。
class Pagination : public Element {
 public:
  static constexpr float kHeight{28.0f};       ///< 控件高（px）
  static constexpr float kCellSize{28.0f};     ///< 单个页码格边长（px）
  static constexpr float kGap{4.0f};           ///< 格间距（px）
  static constexpr float kRadius{6.0f};        ///< 格圆角（px）

  Pagination();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Pagination"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Button; }

  // —— 数据 ——
  /// 设置当前页（从 1 起；夹取到 `[1, page_count]`）。`notify` 为真时派发 `on_change`。
  void set_page(int page, bool notify = false);
  [[nodiscard]] auto page() const noexcept -> int { return page_; }
  /// 设置总页数（`< 1` 按 1 处理；当前页随之夹取）。
  void set_page_count(int count);
  [[nodiscard]] auto page_count() const noexcept -> int { return page_count_; }
  /// 当前页左右各显示几个页码（≥0；默认 1）。
  void set_sibling_count(int siblings);
  [[nodiscard]] auto sibling_count() const noexcept -> int { return sibling_count_; }
  /// 是否显示首/末页（默认 `true`；`false` 时窗口贴边不加常驻页）。
  void set_show_edges(bool value);

  /// 实际渲染的项（`0` = 省略号占位，正数 = 页码）。折叠规则见文件头。
  [[nodiscard]] auto items() const -> std::vector<int>;
  /// 第 `slot` 项的命中矩形（arrange 后有效；越界为空）。
  [[nodiscard]] auto slot_rect(std::size_t slot) const -> math::Rect;
  /// 「上一页 / 下一页」按钮矩形（arrange 后有效）。
  [[nodiscard]] auto prev_rect() const -> math::Rect;
  [[nodiscard]] auto next_rect() const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 用户翻页回调（实参为新页码）。
  std::function<void(int)> on_change{};

 private:
  /// 重算落位（`items()` 的几何唯一来源）。
  void relayout() const;
  /// 点落在哪个页码上（0 = 没有；省略号不算）。
  [[nodiscard]] auto page_at(math::Point point) const -> int;
  [[nodiscard]] auto slot_at(math::Point point) const -> int;  ///< 项序号；-1 = 没有
  /// 页码格内文字可用的宽度（`measure` 用；决定控件宽度）。
  [[nodiscard]] static auto cell_count(int page_count, int page, int siblings,
                                       bool show_edges) -> int;

  int page_{1};
  int page_count_{1};
  int sibling_count_{1};
  bool show_edges_{true};

  mutable std::vector<int> items_{};
  mutable std::vector<math::Rect> slots_{};
  mutable math::Rect prev_{};
  mutable math::Rect next_{};
  mutable int hover_slot_{-1};   ///< 悬停项序号（-1 无）
  mutable bool hover_prev_{false};
  mutable bool hover_next_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Pagination, "Pagination")
