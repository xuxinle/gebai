#pragma once

/// 组件库 B（数据展示与反馈）——垂直列表 `List` 与列表项 `ListItem`。
/// 视觉规格：列表项高 40px、圆角 `radius_sm`；hover 底色 `surface_alt`；
/// 选中底色 `primary_soft` + 左侧 2px `primary` 指示条（文字转 `primary`）；
/// 交互规格：点击派发 `on_select(index)`，键盘 ↑/↓ 移动选中项、Home/End 首尾。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 无选中项哨兵值。
inline constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);

/// 列表项：单行标签 + 可选次行说明（仍固定 40px 高，说明用 `font_xs` 小字）。
class ListItem : public Element {
 public:
  static constexpr float kHeight{40.0f};
  static constexpr float kIndicatorWidth{2.0f};

  /// `label` 主文本；`subtitle` 为空时不绘制次行。
  explicit ListItem(std::string label, std::string subtitle = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ListItem"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ListItem; }

  [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }
  void set_label(std::string label);
  [[nodiscard]] auto subtitle() const noexcept -> const std::string& { return subtitle_; }
  void set_subtitle(std::string subtitle);
  [[nodiscard]] auto selected() const noexcept -> bool { return selected_; }
  void set_selected(bool value);
  /// 在所属 `List` 中的序号（由 `List` 维护）。
  [[nodiscard]] auto index() const noexcept -> std::size_t { return index_; }
  void set_index(std::size_t index) noexcept { index_ = index; }
  /// 激活回调（由 `List` 注入；参数为项序号）。
  void set_on_activate(std::function<void(std::size_t)> callback) {
    on_activate_ = std::move(callback);
  }

  /// 激活本项（触发 `on_activate`）。
  ///
  /// **必须走 `activate()`**：这是"可激活元素"的统一入口——鼠标点击（`on_event`）、
  /// 协议 `invoke(click)`、脚本 `ui_invoke` 都落到这里。若把逻辑写在 `on_event` 里而不覆盖
  /// `activate()`，真实点击有效、`invoke(click)` 却静默无效（实测踩到）。
  void activate() override;

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

 private:
  std::string label_{};
  std::string subtitle_{};
  std::size_t index_{0};
  bool selected_{false};
  std::function<void(std::size_t)> on_activate_{};
};

/// 垂直列表容器：`add_item` 追加列表项，选中态与键盘导航由容器统一管理。
class List : public Element {
 public:
  List();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "List"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::List; }

  /// 追加一项（返回非拥有指针，生命周期随本容器）。
  auto add_item(std::string label, std::string subtitle = {}) -> ListItem*;
  /// 清空全部项（选中态一并复位，不派发 `on_select`）。
  ///
  /// 有它才能"按最新数据重建列表"——过滤/排序/刷新这类场景无法只靠追加完成
  /// （写一个真实应用时发现的缺口：列表只能加不能减）。
  void clear_items();
  [[nodiscard]] auto item_count() const noexcept -> std::size_t;
  [[nodiscard]] auto item(std::size_t index) const noexcept -> ListItem*;
  /// 当前选中序号（无选中为 `kNoSelection`）。
  [[nodiscard]] auto selected_index() const noexcept -> std::size_t { return selected_; }
  /// 选中指定项（越界即清空选中）；`notify` 为 true 时派发 `on_select`。
  void select(std::size_t index, bool notify = true);
  void set_on_select(std::function<void(std::size_t)> callback) { on_select_ = std::move(callback); }
  /// 项间距（px，默认 2）。
  void set_item_gap(float gap) noexcept {
    style_.gap = gap > 0.0f ? gap : 0.0f;
    mark_layout_dirty();
  }

  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

 private:
  std::size_t selected_{kNoSelection};
  std::function<void(std::size_t)> on_select_{};
};

}  // namespace st::ui
