#pragma once

/// @file progress_ring.hpp
/// @brief `ProgressRing`：环形进度（确定/不确定两态 + 中心文本 + 可选动画）。
///
/// ## 与 `ProgressBar` 的分工
///
/// | 组件 | 适合 |
/// |---|---|
/// | `ProgressBar` | 线性的、与宽度绑定的进度（下载条、步骤条） |
/// | `ProgressRing` | **紧凑空间**里的循环进度（工具栏按钮、卡片角标、上传转圈） |
///
/// ## 三个容易做错的点
///
/// 1. **圆弧从 12 点开始、顺时针**：从 3 点开始（数学角 0）是最常见的错——
///    用户的预期是「钟表」而不是「坐标系」。
/// 2. **不确定态**（不知道还要多久）：不能画一个固定比例的弧（那是在撒谎），
///    要用**转动的短弧**（`indeterminate`）。
/// 3. **轨道与进度分开画**：只有进度弧时，用户看不出"总共多长"，
///    于是 50% 和 80% 看起来像同一条弧。
///
/// ## 几何
///
/// ```
///      ╭──────╮        中心可放：百分比 / 图标 / 自定义文本
///     ╱  ╭──╮  ╲
///    │  │ 72% │  │     ← 轨道（淡）+ 进度弧（主色，从 12 点顺时针）
///     ╲  ╰──╯  ╱
///      ╰──────╯
/// ```
///
/// ## 属性面
///
/// `value`（0..1，可写）、`indeterminate`（可写）、`text`（中心文本，空 = 自动显示百分比）、
/// `thickness`（环宽）、`tone`。只读：`percent`（整数百分比）。

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

/// 环形进度。
class ProgressRing : public Element {
 public:
  static constexpr float kDefaultSize{40.0f};     ///< 默认直径（px）
  static constexpr float kThickness{3.0f};        ///< 默认环宽（px）
  static constexpr float kIndeterminateSpan{0.28f};  ///< 不确定态的弧长（占整圈比例）
  static constexpr float kIndeterminatePeriod{1.1f}; ///< 不确定态转一圈的时长（秒）

  ProgressRing();
  explicit ProgressRing(float value);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ProgressRing"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ProgressBar; }

  /// 进度（夹到 0..1）。
  void set_value(float value);
  [[nodiscard]] auto value() const noexcept -> float { return value_; }
  /// 不确定态（不知道还要多久）：画一段转动的短弧。
  void set_indeterminate(bool indeterminate);
  [[nodiscard]] auto indeterminate() const noexcept -> bool { return indeterminate_; }
  /// 中心文本（空 = 自动显示百分比；`indeterminate` 时恒空）。
  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  void set_thickness(float thickness);
  [[nodiscard]] auto thickness() const noexcept -> float { return thickness_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }
  /// 不确定态当前弧的起始角（弧度；由 `time_seconds` 驱动，可截图回归）。
  [[nodiscard]] auto sweep_start(double time_seconds) const noexcept -> float;
  /// 整数百分比（0..100）。
  [[nodiscard]] auto percent() const noexcept -> int;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

  /// 到达 100% 时触发一次（`set_value` 越过 1.0 时；不会重复触发）。
  std::function<void()> on_complete{};

 private:
  [[nodiscard]] auto ring_rect() const -> math::Rect;

  float value_{0.0f};
  bool indeterminate_{false};
  std::string text_{};
  float thickness_{kThickness};
  Tone tone_{Tone::Primary};
  bool completed_{false};

  math::Color track_color_{};
  math::Color progress_color_{};
  math::Color text_color_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::ProgressRing, "ProgressRing")
