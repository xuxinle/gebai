#pragma once

/// @file radio_group.hpp
/// @brief `RadioGroup`：单选组（一组选项、互斥选中、方向与键盘导航）。
///
/// ## 为什么 `Radio` 之外还需要它
///
/// `Radio` 的文档明说「同组互斥由调用方用 `set_checked` 管理（本组件不做分组）」。
/// 于是每个用到单选的地方都要自己写这三件事，而它们**三处写法各不相同**：
///
/// 1. **互斥**：选中一个要把同组其余全 `set_checked(false)`——漏一个就出现两个选中；
/// 2. **键盘导航**：单选的语义是 `↑`/`↓`（或 `←`/`→`）**移动并选中**，`Home`/`End`
///    到首尾。手搭时几乎没人做，于是键盘用户根本选不了；
/// 3. **索引到值的映射**：`index` 与「选项文本」是两套身份，重排后按索引取到别的选项。
///
/// 本组件持有**选项清单**（文本 + 值），内部维护真实 `Radio` 子元素，
/// 上述三件事一次写对。
///
/// ## 几何
///
/// - `Vertical`（默认）：每项一行，间距 `kGap`；
/// - `Horizontal`：一行排开，超宽按 `wrap` 折行。
///
/// ## 属性面
///
/// `items`（`|` 分隔，可写）、`value`（当前值，可写）、`index`（可写）、
/// `direction`（vertical/horizontal，可写）、`disabled`。
/// 动作面：`select`（参数为值或索引）。
///
/// ## 关于「值与索引」
///
/// `value` 是**稳定身份**（重排不变），`index` 只是位置。属性面两者都给，
/// 但回调只报值——调用方按值分支才不会在重排后错位。

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

/// 单选组的一个选项。
struct RadioOption {
  std::string label{};   ///< 显示文本
  std::string value{};   ///< 稳定值（空 = 用 label 充当）
  bool enabled{true};    ///< 该项是否可选
};

/// 单选组。
class RadioGroup : public Element {
 public:
  static constexpr float kGap{8.0f};      ///< 项间距（px）
  static constexpr float kRowGap{6.0f};   ///< 折行时的行间距（px）

  /// 排列方向。
  enum class Direction : std::uint8_t { Vertical, Horizontal };

  RadioGroup();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "RadioGroup"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 选项 ——
  void set_options(std::vector<RadioOption> options);
  void set_labels(const std::vector<std::string>& labels);
  [[nodiscard]] auto options() const noexcept -> const std::vector<RadioOption>& { return options_; }
  [[nodiscard]] auto count() const noexcept -> std::size_t { return options_.size(); }

  // —— 选中 ——
  /// 当前值（未选中为空串）。
  [[nodiscard]] auto value() const -> const std::string&;
  /// 按值选中（找不到则不动并返回 `false`）。
  auto set_value(std::string_view value) -> bool;
  /// 当前索引（未选中为 `-1`）。
  [[nodiscard]] auto index() const noexcept -> int { return index_; }
  /// 按索引选中（越界返回 `false`）。**编程路径**：即使 `disabled` 也能设。
  auto set_index(int index) -> bool;
  /// 清空选中（`-1`）。
  void clear_selection();

  void set_direction(Direction direction);
  [[nodiscard]] auto direction() const noexcept -> Direction { return direction_; }
  void set_wrap(bool wrap);
  [[nodiscard]] auto wrap() const noexcept -> bool { return wrap_; }

  /// 第 `index` 个选项的矩形（arrange 后有效；越界为空）。
  [[nodiscard]] auto option_rect(std::size_t index) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 选中项变化（实参为值；清空时为空串）。
  std::function<void(std::string_view)> on_change{};

 private:
  /// 选项的稳定值（`value` 为空时回落 `label`）。
  [[nodiscard]] auto value_of(const RadioOption& option) const -> std::string_view;
  /// 键盘前进/后退一档（跳过 `enabled=false` 的项）。
  void move_selection(int delta);
  void relayout(const RenderContext& context, math::Rect rect) const;
  /// 第 `index` 项是否需要折到下一行（仅水平 + wrap）。
  [[nodiscard]] auto item_width(const RenderContext& context, std::size_t index) const -> float;

  std::vector<RadioOption> options_{};
  int index_{-1};
  Direction direction_{Direction::Vertical};
  bool wrap_{true};
  float item_height_{22.0f};
  float font_size_{14.0f};

  mutable std::vector<math::Rect> rects_{};
  mutable int hover_index_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::RadioGroup, "RadioGroup")
