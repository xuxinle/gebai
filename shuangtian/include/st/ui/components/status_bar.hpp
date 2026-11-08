#pragma once

/// @file status_bar.hpp
/// @brief `StatusBar`：状态栏（左区若干段 + 右区若干段，段可带图标/色调/点击）。
///
/// ## 为什么需要它
///
/// 状态栏看起来是「一行小字」，但它有一组**只有它才成立**的规则：
///
/// 1. **左右分流**：左区贴左、右区贴右，中间留空（不是均匀分布）。
///    窗口变窄时该先压缩**中间那段空白**，而不是压左右两侧的段。
/// 2. **段间分隔**：段与段之间用竖线或固定间距隔开，且间距要**统一**
///    （各页面自己写就一定会出现 6px 与 8px 混用）。
/// 3. **固定高度、内容垂直居中**：状态栏是「框」，不该随内容行高跳动。
/// 4. **段可点击**（如「行 12, 列 5」点开跳转对话框），命中区要按段算，
///    而不是整栏可点。
///
/// ## 几何
///
/// ```
/// ┌──────────────────────────────────────────────────────────────┐
/// │ ● 就绪 │ 3 个问题                          UTF-8 │ C++ │ 12:34 │
/// └───────┴──────────────────────────────────────────────────────┘
///   左区（左对齐）                     右区（右对齐，从右往左排）
/// ```
///
/// ## 用法
///
/// ```cpp
/// auto bar = std::make_unique<StatusBar>();
/// bar->add_left({"就绪", Tone::Success, "circle"});   // 段
/// bar->add_right({"UTF-8"});
/// bar->set_segment_click_handler([](std::string_view id) { ... });
/// ```
///
/// ## 属性面
///
/// `left` / `right`（`|` 分隔的段文本，可写）、`height`（只读，当前高）。
/// 动作面：`click`（参数为段 id）。

#include <cstddef>
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

/// 状态栏。
class StatusBar : public Element {
 public:
  static constexpr float kHeight{24.0f};        ///< 默认高（px）
  static constexpr float kPaddingX{10.0f};      ///< 横向内边距（px）
  static constexpr float kSegmentGap{10.0f};    ///< 段间距（px）
  static constexpr float kSeparatorGap{5.0f};   ///< 分隔竖线两侧的间距（px）
  static constexpr float kIconSize{10.0f};      ///< 图标边长（px）
  static constexpr float kIconGap{5.0f};        ///< 图标与文本间距（px）

  /// 段（一条状态信息）。
  struct Segment {
    std::string text{};         ///< 显示文本
    std::string icon{};         ///< 图标名（空 = 无图标）
    Tone tone{Tone::Muted};     ///< 色调
    std::string id{};           ///< 稳定标识（点击回调与属性面用；空 = 用 text）
    bool clickable{false};      ///< 是否可点（悬停高亮 + 手型）
  };

  StatusBar();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "StatusBar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 段 ——
  void add_left(Segment segment);
  void add_right(Segment segment);
  void set_left(std::vector<Segment> segments);
  void set_right(std::vector<Segment> segments);
  void clear();
  [[nodiscard]] auto left_count() const noexcept -> std::size_t { return left_.size(); }
  [[nodiscard]] auto right_count() const noexcept -> std::size_t { return right_.size(); }
  [[nodiscard]] auto left_segments() const noexcept -> const std::vector<Segment>& { return left_; }
  [[nodiscard]] auto right_segments() const noexcept -> const std::vector<Segment>& { return right_; }

  /// 段之间是否画竖分隔线（默认 `false`：只用间距，状态栏通常不需要竖线）。
  void set_separators(bool separators);
  [[nodiscard]] auto separators() const noexcept -> bool { return separators_; }

  // —— 几何（arrange 后有效）——
  /// 左区第 `index` 段的矩形（越界为空）。
  [[nodiscard]] auto left_rect(std::size_t index) const -> math::Rect;
  /// 右区第 `index` 段的矩形（越界为空）。
  [[nodiscard]] auto right_rect(std::size_t index) const -> math::Rect;
  /// 中间空白区的宽度（左右区之间的空隙；窄窗口时会被压到 0）。
  [[nodiscard]] auto middle_gap() const noexcept -> float { return middle_gap_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 点击可点段（实参为段 `id`，为空则回落到 `text`）。
  std::function<void(std::string_view)> on_segment_click{};

 private:
  [[nodiscard]] static auto id_of(const Segment& segment) -> std::string_view;
  [[nodiscard]] auto segment_width(const RenderContext& context, const Segment& segment) const
      -> float;
  /// 段的公共落位（左区 `from_left=true` 从 x 往右排；右区从右往左排）。
  void relayout(const RenderContext& context, math::Rect rect) const;
  /// 命中段的索引与区（返回 `{-1,false}` 表示没命中）。
  void hit_segment(math::Point point, bool& is_left, std::size_t& index) const;

  std::vector<Segment> left_{};
  std::vector<Segment> right_{};
  bool separators_{false};
  float font_size_{12.0f};

  mutable std::vector<math::Rect> left_boxes_{};
  mutable std::vector<math::Rect> right_boxes_{};
  mutable float middle_gap_{0.0f};
  mutable int hover_left_{-1};
  mutable int hover_right_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::StatusBar, "StatusBar")
