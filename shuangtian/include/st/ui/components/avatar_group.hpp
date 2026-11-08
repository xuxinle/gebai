#pragma once

/// @file avatar_group.hpp
/// @brief `AvatarGroup`：头像组（重叠若干头像 + 「+N」溢出徽标）。
///
/// ## 为什么需要它
///
/// 「协作者」「在线成员」这类信息在界面上就是一叠头像。手搭的问题在于
/// **重叠几何与溢出规则**：每个头像要叠多少像素、叠到第几个就该收成 `+N`、
/// 收起来之后 `+N` 用多大的字——三处各写一遍就会在「4 个成员」与「9 个成员」
/// 之间长得完全不一样（有的溢出、有的撑满整行）。
///
/// ## 几何规则（写死可预测）
///
/// - 头像直径由 `set_avatar_size` 给定（默认 28px）；
/// - 相邻头像重叠 `kOverlapRatio`（默认 0.28 = 直径的 28%）；
/// - **溢出规则**：可用宽放不下全部时，保留前 N 个 + 一个 `+M` 徽标；
///   `N` 的算法是「让 N 个头像 + 1 个徽标刚好放得下」——不是固定砍到 3 个；
/// - `+M` 徽标的底色取 `surface_alt`、文字 `text_muted`（它是“还有更多”，不该抢眼）。
///
/// ## 属性面
///
/// `members`（`|` 分隔的名字清单，可写）、`max`（最多显示几个，`0` = 按宽度自动）、
/// `size`（头像直径）、`count`（成员总数，只读）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 头像组。
class AvatarGroup : public Element {
 public:
  static constexpr float kAvatarSize{28.0f};       ///< 默认头像直径（px）
  static constexpr float kOverlapRatio{0.28f};     ///< 相邻头像重叠比例

  AvatarGroup();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "AvatarGroup"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Image; }

  // —— 数据 ——
  void set_members(std::vector<std::string> names);
  void add_member(std::string name);
  void clear_members();
  [[nodiscard]] auto members() const noexcept -> const std::vector<std::string>& { return names_; }
  [[nodiscard]] auto count() const noexcept -> std::size_t { return names_.size(); }

  /// 最多显示几个头像（`0` = 按可用宽自动；默认 `0`）。
  void set_max_visible(std::size_t max_visible);
  [[nodiscard]] auto max_visible() const noexcept -> std::size_t { return max_visible_; }
  void set_avatar_size(float size);
  [[nodiscard]] auto avatar_size() const noexcept -> float { return avatar_size_; }

  /// 实际画出来的头像数（不含 `+N` 徽标）。
  [[nodiscard]] auto visible_count() const noexcept -> std::size_t { return shown_count_; }
  /// 溢出数（`0` = 没有溢出徽标）。
  [[nodiscard]] auto overflow_count() const noexcept -> std::size_t { return overflow_; }
  /// 第 `index` 个头像的矩形（arrange 后有效；越界为空）。
  [[nodiscard]] auto avatar_rect(std::size_t index) const -> math::Rect;
  /// `+N` 徽标矩形（无溢出时为空）。
  [[nodiscard]] auto overflow_rect() const noexcept -> math::Rect { return overflow_box_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

  /// 点头像（参数为成员名）。
  std::function<void(std::string_view)> on_member_click{};

 private:
  /// 首字（与 `Avatar` 同口径：跳过前导空白后的第一个码点，小写转大写）。
  [[nodiscard]] static auto initials_of(std::string_view name) -> std::string;
  /// 按当前宽度决定显示几个（写 `visible_` / `overflow_`）。
  void resolve_visible_count(float available_width) const;
  void relayout(float available_width) const;

  std::vector<std::string> names_{};
  std::size_t max_visible_{0};
  float avatar_size_{kAvatarSize};
  math::Color ring_color_{};
  math::Color fallback_fill_{};

  mutable std::size_t shown_count_{0};
  mutable std::size_t overflow_{0};
  mutable std::vector<math::Rect> rects_{};
  mutable math::Rect overflow_box_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::AvatarGroup, "AvatarGroup")
