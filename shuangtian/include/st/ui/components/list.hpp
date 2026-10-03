#pragma once

/// 组件库 B（数据展示与反馈）——垂直列表 `List` 与列表项 `ListItem`。
/// 视觉规格：列表项高 40px、圆角 `radius_sm`；hover 底色 `surface_alt`；
/// 选中底色 `primary_soft` + 左侧 2px `primary` 指示条（文字转 `primary`）；
/// 交互规格：点击派发 `on_select(index)`，键盘 ↑/↓ 移动选中项、Home/End 首尾。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
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
///
/// **刷新数据请用 `sync_items`（而不是 `clear_items` + 逐个 `add_item`）**：
/// 前者按 key 复用已有子元素，元素 id 与选中态对没变的数据都保持不变；
/// 后者把索引与选中态全部重置，外部持有的 id 会失效（详见 `sync_items` 注释）。
class List : public Element {
 public:
  List();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "List"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::List; }

  /// 追加一项（返回非拥有指针，生命周期随本容器）。
  auto add_item(std::string label, std::string subtitle = {}) -> ListItem*;
  /// 清空全部项（选中态一并复位，不派发 `on_select`）。
  ///
  /// 有它才能"按最新数据重建列表"。但**重建会重置索引与选中态**，外部持有的元素 id 也随
  /// 索引位移而失效；需要"数据变了但同一项仍是同一项"时用 `sync_items`。
  void clear_items();

  /// 一条列表数据（`sync_items` 用）：`key` 是业务身份，同 key 视为同一项。
  struct Entry {
    std::string key{};
    std::string label{};
    std::function<void()> on_activate{};  ///< 空则用 `set_on_select` 的默认行为
  };

  /// 按 key 同步数据：**保留同 key 项的 id 与选中态**，只重建真正变化的部分。
  ///
  /// 为什么它是"刷新数据"的首选：`clear_items` + 逐个 `add_item` 会把索引推倒重来，
  /// 于是"刷新后原来看中的那一项变成了另一条数据"（外部按 id 引用时尤其致命），
  /// 选中态也会静默丢失。`sync_items` 用 key 做对齐：
  ///
  /// - 新增的 key → 追加新项（id 取 `key`，与位置无关）；
  /// - 保留的 key → 更新文案、**沿用同一个子元素与同一个 id**；
  /// - 消失的 key → 移除该项；
  /// - 顺序变化 → 按新顺序重排，id 不变（因为 id 来自 key 而非索引）。
  ///
  /// 选中态跟随**被选中的那个 key** 走，而不是跟着索引走。
  void sync_items(const std::vector<Entry>& entries);
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

  // —— 列表类通用视口契约（与 `Tree` 同口径，见 tree.hpp 的说明）——
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 视口能整行容纳的项数（至少 1）。
  [[nodiscard]] auto visible_item_count() const noexcept -> std::size_t;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

 private:
  std::size_t selected_{kNoSelection};
  std::function<void(std::size_t)> on_select_{};

  /// 给列表项接上"激活即选中本容器"的链路（`add_item` 与 `sync_items` 共用一处）。
  ///
  /// 抽出来是必要的：`sync_items` 新建的项若漏掉这一步，会表现为"点击没反应"，
  /// 而 `add_item` 建的项一切正常（实测踩到，测试当场揭出）。
  void bind_item(ListItem& node, std::size_t index);
};

}  // namespace st::ui
