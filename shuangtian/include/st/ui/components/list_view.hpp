#pragma once

/// @file list_view.hpp
/// @brief `List`：垂直列表容器：`add_item` 追加列表项，选中态与键盘导航由容器统一管理。。
///
/// 从 `list.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `list.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

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
    /// 副行文本（空 = 不画副行）。
    ///
    /// 为何要它：`ListItem` 本来就支持副行（`set_subtitle`），但 `Entry`（数据驱动同步
    /// 用的那个结构）没带这个字段——于是 `dsl::list` 的 `ListItemData.subtitle`
    /// **没有落点、静默丢掉**（实测：文件历史的「作者 · 日期」一行都画不出来，
    /// 而数据面明明给了值）。
    std::string subtitle{};
    /// 前置图标名（空 = 不画）。落点是 `ListItem::set_icon`。
    std::string icon{};
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

  /// 容器层的排版覆盖（DSL `BoxProps::color/size`）下放到每个项。
  /// 字是 `ListItem` 画的，容器只记不画——不下放就是静默不生效。
  /// 在 `apply_theme` 末尾调：镜像完紧跟项的基类 apply_theme 回放进 style_。
  void apply_theme_to_items();
  /// 把容器上的**显式高度**下放为各项行高（`List` 的 `height` 语义 = 单行高）。
  ///
  /// 为何需要：DSL 的 `.height` 应用在 **List 容器**上，而行的实际尺寸由
  /// `ListItem::style_.height` 决定——不写这条下放，`.height` 是"设了没生效"
  ///（实测：大纲想压到 22px，行高恒 40）。
  void apply_row_height_override();
  void apply_theme(const Theme& theme) override;

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

ST_UI_COMPONENT(::st::ui::List, "List")
