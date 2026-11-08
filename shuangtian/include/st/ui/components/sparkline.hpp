#pragma once

/// @file sparkline.hpp
/// @brief `Sparkline`：迷你趋势线（折线 / 面积 / 柱状三形态，无坐标轴、无标签）。
///
/// ## 为什么不是「画一堆线段」
///
/// 迷你图的价值在于**一眼看出形状**，而这要求几件容易被做错的事：
///
/// 1. **纵轴范围**：必须按数据自身的 min/max 归一（不是 0..1，也不是固定范围），
///    否则「2.1 到 2.2 的波动」会被画成一条直线。全等数据要落在中线（不是贴底）。
/// 2. **不得裁掉峰谷**：线宽吃进边界会把最高点削平——笔画要**内缩半个线宽**。
/// 3. **点太密时的降采样**：1000 个点画在 200px 上，直接连线会画出锯齿噪点，
///    正确做法是**每像素列取 min/max**（保留极值），而不是隔点抽样（会丢峰）。
/// 4. **面积填充要闭合**：路径必须回到底边再闭合，否则填充会跨过整个框。
///
/// ## 三形态
///
/// | 形态 | 用途 |
/// |---|---|
/// | `Line` | 趋势（默认） |
/// | `Area` | 趋势 + 量感（线下方填充淡色） |
/// | `Bar` | 计数/离散量（每点一柱） |
///
/// ## 属性面
///
/// `values`（逗号分隔的数值，可写）、`shape`（line/area/bar）、`min`/`max`（显式范围，
/// 不设则按数据自适应）、`baseline`（是否画零基线）、`stroke`。
/// 只读：`data_min` / `data_max`（自适应出来的范围，便于调用方核对）。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 迷你趋势线。
class Sparkline : public Element {
 public:
  /// 形态。
  enum class Shape : std::uint8_t { Line, Area, Bar };

  static constexpr float kDefaultHeight{28.0f};   ///< 默认高（px）
  static constexpr float kStrokeWidth{1.5f};      ///< 线宽（px）
  static constexpr float kBarGapRatio{0.25f};     ///< 柱间距占柱宽的比例

  Sparkline();
  explicit Sparkline(std::vector<double> values);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Sparkline"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Image; }

  // —— 数据 ——
  void set_values(std::vector<double> values);
  void set_values_csv(std::string_view csv);
  [[nodiscard]] auto values() const noexcept -> const std::vector<double>& { return values_; }
  [[nodiscard]] auto count() const noexcept -> std::size_t { return values_.size(); }
  void add_value(double value);
  void clear_values();

  void set_shape(Shape shape);
  [[nodiscard]] auto shape() const noexcept -> Shape { return shape_; }
  /// 显式纵轴范围（`set_min`/`set_max`；不设则按数据自适应）。
  void set_range(double min_value, double max_value);
  void clear_range();
  [[nodiscard]] auto has_range() const noexcept -> bool { return has_range_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }
  /// 是否画零基线（数据跨零时有意义）。
  void set_baseline(bool baseline);
  [[nodiscard]] auto baseline() const noexcept -> bool { return baseline_; }
  /// 后 N 个点高亮（如「最近 3 个」用主色，其余用淡色；`0` = 不高亮）。
  void set_emphasis_tail(std::size_t count);
  [[nodiscard]] auto emphasis_tail() const noexcept -> std::size_t { return emphasis_tail_; }

  /// 数据自适应范围（`has_range` 为真时返回显式范围）。
  [[nodiscard]] auto data_min() const noexcept -> double { return data_min_; }
  [[nodiscard]] auto data_max() const noexcept -> double { return data_max_; }
  /// 数据范围全等时该点落在**垂直中线**（不是贴底）。
  [[nodiscard]] auto is_flat() const noexcept -> bool { return flat_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

  /// 数值 → 绘制 y（`paint` 与测试共用同一套映射；`rect` 为绘图区）。
  [[nodiscard]] auto value_to_y(double value, math::Rect rect) const -> float;

 private:
  /// 重算自适应范围与 `flat_`。
  void update_range();

  std::vector<double> values_{};
  Shape shape_{Shape::Line};
  Tone tone_{Tone::Primary};
  bool baseline_{false};
  bool has_range_{false};
  double range_min_{0.0};
  double range_max_{1.0};
  std::size_t emphasis_tail_{0};
  float stroke_{kStrokeWidth};

  double data_min_{0.0};
  double data_max_{1.0};
  bool flat_{true};
  math::Color stroke_color_{};
  math::Color fill_color_{};
  math::Color muted_color_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Sparkline, "Sparkline")
