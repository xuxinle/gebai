#pragma once

/// @file number_input.hpp
/// @brief `NumberInput`：数值输入框（文本编辑 + 上下步进 + 区间夹取）。
///
/// ## 为什么不直接用 `Input` + 调用方自己 `stod`
///
/// 数值输入有一批**只在数值语义下才存在**的行为，分散到调用方就会各写一遍：
///
/// | 行为 | 如果交给调用方 |
/// |---|---|
/// | 非法输入（`abc`、`1.2.3`、`--5`） | 每次 `on_change` 里都要 try 一遍，写漏就崩/静默归零 |
/// | 区间夹取（`min`/`max`） | 数量字段允许输负数、百分比允许输 300% |
/// | 步进（↑↓、滚轮、加减按钮） | 步长、按住加速、边界处停住，各写一遍三处不一致 |
/// | 步进后的**显示精度** | `0.1 + 0.2` 显示出 `0.30000000000000004` |
///
/// 所以本组件把「值」与「编辑中的文本」**分开保存**：
/// `value()` 永远是合法数值（已夹取），`text()` 是用户正在敲的原文。
/// 只有在文本能被完整解析成数值时才同步两者——于是「敲到一半的 `-`」不会把值打成 0。
///
/// ## 几何
///
/// ```
/// ┌──────────────────────────────┬───┐
/// │  -12.5                      │ ▲ │   ← 步进按钮列（宽 16）
/// │                             │ ▼ │
/// └──────────────────────────────┴───┘
/// ```
///
/// 步进按钮在右缘固定 16px 宽，上下各占一半——比分列在左右两侧更省宽，
/// 也贴近「数值」这个语义（上下调大小）。
///
/// ## 属性面
///
/// `value`（数值；写入即夹取并格式化显示）、`text`（编辑中的原文，读写）、
/// `min` / `max` / `step` / `precision`（小数位数，`-1` = 自动）、`disabled`。

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

/// 数值输入框。
class NumberInput : public Element {
 public:
  static constexpr float kHeight{30.0f};          ///< 默认高（px）
  static constexpr float kPaddingX{8.0f};         ///< 文本左内边距（px）
  static constexpr float kStepperWidth{16.0f};    ///< 步进按钮列宽（px）
  static constexpr float kRadius{6.0f};           ///< 圆角（px）

  NumberInput();
  explicit NumberInput(double value);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "NumberInput"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  // —— 值与区间 ——
  /// 当前值（**永远合法**：已按 `min`/`max` 夹取、已按 `precision` 舍入）。
  [[nodiscard]] auto value() const noexcept -> double { return value_; }
  /// 设置值：夹取 + 舍入 + 同步显示文本 + 派发 `on_change`（值真变了才派发）。
  void set_value(double value);
  /// 编辑中的**原文**（可能与 `value()` 不一致：用户敲到一半的 `-` 或 `1.`）。
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  void set_text(std::string text);

  void set_min(double min);
  void set_max(double max);
  /// 让区间**无界**（默认就是无界）。
  void clear_range();
  [[nodiscard]] auto min_value() const noexcept -> double { return min_; }
  [[nodiscard]] auto max_value() const noexcept -> double { return max_; }
  [[nodiscard]] auto has_range() const noexcept -> bool { return has_min_ || has_max_; }

  /// 步长（`<= 0` 视为 1；`↑`/`↓`、滚轮、加减按钮都按它走）。
  void set_step(double step);
  [[nodiscard]] auto step() const noexcept -> double { return step_; }
  /// 小数位数（`-1` = 按值自适应：整数不带小数点，小数按最短往返表示）。
  void set_precision(int digits);
  [[nodiscard]] auto precision() const noexcept -> int { return precision_; }
  void set_suffix(std::string suffix);
  [[nodiscard]] auto suffix() const noexcept -> const std::string& { return suffix_; }

  /// 按步长增减（自动夹取）。
  void step_up();
  void step_down();

  // —— 命中几何（arrange 后有效）——
  [[nodiscard]] auto up_rect() const noexcept -> math::Rect { return up_; }
  [[nodiscard]] auto down_rect() const noexcept -> math::Rect { return down_; }
  [[nodiscard]] auto field_rect() const noexcept -> math::Rect { return field_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 值变化（实参为夹取后的新值）。on_change 在 set_value 真改变值时同步派发。
  std::function<void(double)> on_change{};
  /// 提交（Enter / 失焦 / commit 动作）后的原文。
  std::function<void(std::string_view)> on_commit{};

 private:
  /// 解析 → 夹取 → 舍入。非法输入返回 `nullopt`（**不改**当前值）。
  [[nodiscard]] auto parse(const std::string& text) const -> std::optional<double>;
  /// 数值 → 显示文本（按 `precision` 或最短往返）。
  [[nodiscard]] auto format(double value) const -> std::string;
  /// 同步显示文本 = `format(value_)`；只有文本真变了才标脏。
  void sync_text_from_value();
  void notify_change();
  /// 提交：把当前原文解析成值（非法则回滚到上一个合法值的文本）。
  void commit();
  [[nodiscard]] auto caret_x(const RenderContext& context) const -> float;
  auto handle_key(const Event& event) -> bool;
  void insert_text(std::string_view inserted);

  double value_{0.0};
  std::string text_{"0"};
  std::string suffix_{};
  double min_{0.0};
  double max_{0.0};
  bool has_min_{false};
  bool has_max_{false};
  double step_{1.0};
  int precision_{-1};
  std::size_t caret_{1};

  mutable math::Rect field_{};
  mutable math::Rect up_{};
  mutable math::Rect down_{};
  mutable bool hover_up_{false};
  mutable bool hover_down_{false};
  mutable int last_blink_visible_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::NumberInput, "NumberInput")
