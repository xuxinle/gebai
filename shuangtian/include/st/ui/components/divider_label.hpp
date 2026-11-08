#pragma once

/// @file divider_label.hpp
/// @brief `DividerLabel`：带文字的分隔线（分隔线 + 居中小字标题 + 可选右侧动作）。
///
/// ## 与 `Divider` 的分工
///
/// | 组件 | 用途 |
/// |---|---|
/// | `Divider` | 纯分隔（同组内的弱分隔） |
/// | `DividerLabel` | **分区分组**：告诉用户"下面是另一个区块"（如「高级设置」「历史记录」「其它」） |
///
/// ## 为什么值得单独一个组件
///
/// 手搭「线 + 文字 + 线」时，最容易错的不是画线，而是**线的两端与文字之间的留白分配**：
///
/// - 文字要**垂直居中于线**（不是居中于行盒——行盒含字体预留空间，会偏下 1~2px）；
/// - 线要**贴文字左右两侧留出固定间隙**（间隙不等会显得文字偏心）；
/// - 文字宽度不能吃掉整行（长标题时线要能缩到 0 而不是溢出）；
/// - 可选右侧动作（如「查看全部」）要贴右缘，且不与线重叠。
///
/// 这些一次写对，就不用每个页面各调一遍。
///
/// ## 几何（三档对齐）
///
/// ```
/// ──────  区块标题  ──────────────────  查看全部 →
///    ↑        ↑              ↑              ↑
///  左线段   文字         右线段         右侧动作
/// ```
///
/// `Align` 决定文字落在哪个位置：`Left`（文字贴左，右线段长）、`Center`（两段等长）、
/// `Right`（文字贴右，左线段长）。
///
/// ## 属性面
///
/// `text`（可写）、`align`（left/center/right，可写）、`action`（右侧动作文案，可写）。
/// 动作面：`activate`（等同点右侧动作）。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 带文字的分隔线。
class DividerLabel : public Element {
 public:
  static constexpr float kLineThickness{0.5f};   ///< 线厚（与 `Divider` 同口径）
  static constexpr float kGap{10.0f};            ///< 线与文字之间的间隙（px）
  static constexpr float kActionGap{8.0f};       ///< 右侧动作与线之间的间隙（px）
  static constexpr float kHeight{22.0f};         ///< 默认高（px）

  /// 文字对齐档。
  enum class Align : std::uint8_t { Left, Center, Right };

  DividerLabel();
  explicit DividerLabel(std::string text);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "DividerLabel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Separator; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  void set_align(Align align);
  [[nodiscard]] auto align() const noexcept -> Align { return align_; }
  /// 右侧动作文案（空 = 不显示）。
  void set_action(std::string label);
  [[nodiscard]] auto action() const noexcept -> const std::string& { return action_; }
  /// 是否画线（`false` = 只当小标题用）。
  void set_line_visible(bool visible);
  [[nodiscard]] auto line_visible() const noexcept -> bool { return line_visible_; }

  // —— 几何（arrange 后有效）——
  [[nodiscard]] auto left_line_rect() const noexcept -> math::Rect { return left_line_; }
  [[nodiscard]] auto right_line_rect() const noexcept -> math::Rect { return right_line_; }
  [[nodiscard]] auto text_rect() const noexcept -> math::Rect { return text_box_; }
  [[nodiscard]] auto action_rect() const noexcept -> math::Rect { return action_box_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 点右侧动作。
  std::function<void()> on_action{};

 private:
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::string text_{};
  std::string action_{};
  Align align_{Align::Left};
  bool line_visible_{true};
  float font_size_{12.0f};
  float action_font_{12.0f};

  mutable math::Rect left_line_{};
  mutable math::Rect right_line_{};
  mutable math::Rect text_box_{};
  mutable math::Rect action_box_{};
  mutable bool hover_action_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::DividerLabel, "DividerLabel")
