#pragma once

/// @file breadcrumb.hpp
/// @brief `Breadcrumb`：面包屑导航（`首页 › 分组 › 当前页`；末段是当前位置，主色不可点）。
///
/// ## 为什么把它抽成组件（此前只有一份私有实现）
///
/// 分段路径的绘制与命中此前**只存在于 `FileDialog` 内部**（`file_dialog.cpp` 的
/// `crumbs()` / `crumb_rect()` / `crumb_widths_`）：想在别的界面里摆一条路径导航，
/// 只能照抄那段几何——而「段宽缓存 + 分隔符占位 + 末段不可点 + 超宽折叠」这四件事
/// 任何一处抄漏，表现都是「点不准」或「末段还能点」。
///
/// 抽出来后同一份几何被三处共用（对话框、页面头、设置页）。
///
/// ## 视觉与交互
///
/// - 分段之间是**自绘 chevron 图标**（不是文本 `›`：文本分隔符的字面宽度随字体变，
///   而几何要与命中一致，用图标才能给出确定的占位宽）。
/// - 末段是当前位置：`text` 色、`font_weight Medium`、**不可点**（默认；
///   `set_last_clickable(true)` 可放开，用于「刷新当前页」这类场景）。
/// - 中间段 `text_muted`，悬停转 `text` 并显示手型光标。
/// - 总宽超出可用宽时**折叠中段**：保留首段与末尾若干段，中间插入 `…` 占位
///   （最左是根、最右是当前页——这两端是导航时最常点的）。
///
/// ## 属性面
///
/// `segments`（`|` 分隔的段标签，可写）、`current`（末段标签，只读）、
/// `count`（段数，只读）、`separator`（分隔符图标名，可写）。
/// 动作面：`goto_crumb`（参数 = 段序号或业务值）、`back`（上溯一层）。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 面包屑导航：分段路径 + 分隔符 + 「末段即当前位置」语义。
class Breadcrumb : public Element {
 public:
  /// 一段路径。
  struct Segment {
    std::string label{};
    /// 业务值（空 = 回落为 `label`）。回调按它回传——**不要用段序号做身份**：
    /// 路径一变，同一个序号就指向另一段（与 `List` 的 key 同一条理由）。
    std::string value{};
    /// 可选前导图标名（`Icon` 表里的名字，如 `home` / `folder`）。
    std::string icon{};
  };

  static constexpr float kHeight{26.0f};          ///< 控件高（px）
  static constexpr float kSegmentPaddingX{6.0f};  ///< 单段左右内边距（px）
  static constexpr float kSeparatorGap{4.0f};     ///< 段与分隔符之间的间距（px）
  static constexpr float kSeparatorGlyph{12.0f};  ///< 分隔符图标边长（px）
  static constexpr float kIconGap{4.0f};          ///< 段内图标与文字的间距（px）
  static constexpr float kIconSize{14.0f};        ///< 段内图标边长（px）
  static constexpr float kEllipsisWidth{20.0f};   ///< 折叠时 `…` 占位宽（px）

  Breadcrumb();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Breadcrumb"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Link; }

  // —— 数据 ——
  void set_segments(std::vector<Segment> segments);
  void add_segment(std::string label, std::string value = {});
  void clear_segments();
  [[nodiscard]] auto segments() const noexcept -> const std::vector<Segment>& { return segments_; }
  [[nodiscard]] auto segment_count() const noexcept -> std::size_t { return segments_.size(); }
  /// 第 `index` 段标签（越界为空）。
  [[nodiscard]] auto segment_label(std::size_t index) const -> std::string_view;
  /// 末段标签（无段时为空）——即「当前位置」。
  [[nodiscard]] auto current() const -> std::string_view;

  /// 末段是否可点（默认 `false`：它表示「你在这里」）。
  void set_last_clickable(bool value);
  [[nodiscard]] auto last_clickable() const noexcept -> bool { return last_clickable_; }
  /// 分隔符图标名（默认 `chevron-right`）。
  void set_separator(std::string icon_name);
  [[nodiscard]] auto separator() const noexcept -> const std::string& { return separator_; }

  // —— 几何（arrange 后有效）——
  /// 第 `index` 段的命中矩形；被折叠或越界时返回空矩形。
  [[nodiscard]] auto segment_rect(std::size_t index) const -> math::Rect;
  /// 折叠占位 `…` 的矩形（未折叠时为空）。
  [[nodiscard]] auto ellipsis_rect() const noexcept -> math::Rect { return ellipsis_; }
  /// 折叠时仍在场的段序号（升序；未折叠时为空——用它区分「全显示」与「折叠后恰好全显示」）。
  [[nodiscard]] auto visible_indices() const -> std::vector<std::size_t>;

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

  /// 点击某一段（实参为段序号）。末段在 `last_clickable` 为假时不触发。
  std::function<void(std::size_t)> on_click{};
  /// 按业务值回调（推荐：段序号会随路径变化而漂移）。
  std::function<void(std::string_view)> on_navigate{};

 private:
  /// 一段的落位结果。
  struct Placed {
    std::size_t index{0};
    math::Rect rect{};
  };

  /// 重算各段宽度缓存（依赖文本端口，只能在有 `RenderContext` 的地方做）。
  void rebuild_widths(const RenderContext& context) const;
  /// 按当前 `bounds_` 与宽度缓存计算落位（折叠几何的唯一来源：绘制/命中/属性共用）。
  void relayout() const;
  /// 分隔符占位宽（图标 + 两侧间距）。
  [[nodiscard]] static constexpr auto separator_slot() noexcept -> float {
    return kSeparatorGlyph + kSeparatorGap * 2.0f;
  }
  /// 点落在哪一段（-1 = 没落在任何段上）。
  [[nodiscard]] auto segment_at(math::Point point) const -> std::optional<std::size_t>;
  /// 该段此刻是否可激活（末段默认不可点——它是「当前位置」）。
  ///
  /// 单独立这个方法而不是在调用点重复判：**“能不能点”与“点了干什么”必须是同一个判据**。
  /// 分散写时命中与光标会分岔（实测：未段不可点但鼠标仍变成手型）。
  [[nodiscard]] auto activatable(std::size_t index) const noexcept -> bool;
  void activate_segment(std::size_t index);

  std::vector<Segment> segments_{};
  /// 各段文本宽（不含内边距/图标），与 `segments_` 同序。
  mutable std::vector<float> text_widths_{};
  /// 各段总宽（含图标与内边距），与 `segments_` 同序。
  mutable std::vector<float> segment_widths_{};
  mutable std::vector<Placed> placed_{};
  mutable math::Rect ellipsis_{};
  mutable bool collapsed_{false};
  mutable int hover_index_{-1};

  std::string separator_{"chevron-right"};
  bool last_clickable_{false};
  /// 段宽缓存是否有效（**mutable**：`rebuild_widths` 是 const 路径——
  /// `measure`/`arrange`/`paint` 都要能按需重建，而它们都不能改成员）。
  mutable bool widths_valid_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Breadcrumb, "Breadcrumb")
