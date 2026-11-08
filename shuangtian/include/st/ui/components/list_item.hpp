#pragma once

/// @file list_item.hpp
/// @brief `ListItem`：列表项：单行标签 + 可选次行说明（仍固定 40px 高，说明用 `font_xs` 小字）。。
///
/// 从 `list.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `list.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// "无选中"的哨兵值（`std::size_t` 的 -1）。
///
/// 为何是 `inline constexpr` 而不是枚举：它直接用在下标运算里（`selected_ != kNoSelection`、
/// `selected_ < children_.size()`）。本头持有它而不是放 `list_view.hpp`：
/// `ListItem` 与 `List` 都要它，而 `List` 已依赖本头（少一个反向依赖）。
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
  /// 前置图标（图标名，取内置图标集；空 = 不画）。
  ///
  /// 为何是项级而不是容器级：同一个列表里不同行的图标天然不同
  ///（大纲的类/函数/变量各一个），放到容器上就得让调用方自己排版。
  /// 图标宽度从文本区里**预先扣掉**（`kIconSlot`），所以带图标与不带图标的
  /// 行文本起点不同——这是有意的：图标是行结构的一部分，不是浮在上面的装饰。
  [[nodiscard]] auto icon() const noexcept -> const std::string& { return icon_; }
  void set_icon(std::string icon);
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

  /// 把容器（`List`）记的排版覆盖镜像到本项（字是项画的，见
  /// `List::apply_theme_to_items` 的说明）。每帧镜像，不累积。
  void apply_container_text_overrides(const Element& container);

  void apply_theme(const Theme& theme) override;

 private:
  std::string label_{};
  std::string subtitle_{};
  std::string icon_{};
  std::size_t index_{0};
  bool selected_{false};
  std::function<void(std::size_t)> on_activate_{};

  /// 标签/副行的**文本宽度缓存**（`mutable`：与 `Tree::row_label_widths_` 同一套做法）。
  ///
  /// 为何需要（2026-10-14，实测）：`List` 不覆写 `measure`，走 `Element` 基类逐子元素
  /// 递归——于是 N 项列表每轮 measure 要调 `TextPort::measure_width` **2N 次**
  ///（label + subtitle）。而文本层的宽度缓存只有 4096 条且满了**整表清空**：
  /// 2048 项时正好 4096 次调用"自己把自己的缓存冲掉"，命中率≈0 → **断崖**
  ///（实测：2000 项 0.187 ms → 2100 项 10.923 ms；5000 项 25.927 ms）。
  ///
  /// 修法是把宽度缓存在**项自己身上**：文案没变就不重量（`set_label`/`set_subtitle`
  /// 会失效）。于是 `List::measure` 的 O(N) 遍历退化成纯内存扫描。
  ///
  /// 为何不缓存"最终宽度"（含 `kTextInset` 与上下限夹取那一堆）：那些依赖 `constraints`
  /// 与 `style_`，逐轮都可能变；缓存的应是**纯文本度量**这一层——它只取决于
  /// `(文案, 字号)`，语义上就是"这段字占多宽"。
  mutable float label_width_{-1.0F};
  mutable float subtitle_width_{-1.0F};
  /// 缓存对应的字号（`Metrics::font_base` / `font_xs`）；主题换档时重算。
  mutable float label_width_font_size_{-1.0F};
  mutable float subtitle_width_font_size_{-1.0F};
};

}  // namespace st::ui

ST_UI_COMPONENT_WITH(::st::ui::ListItem, "ListItem", std::make_unique<::st::ui::ListItem>("", ""))
