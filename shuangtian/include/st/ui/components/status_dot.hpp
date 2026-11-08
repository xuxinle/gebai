#pragma once

/// @file status_dot.hpp
/// @brief `StatusDot`：状态点（语义色小圆 + 可选脉冲 + 可选文字标签）。
///
/// ## 为什么要一个「只有一个点」的组件
///
/// 服务健康、连接状态、任务进度这类信息在界面上就是一个**点**。手写它的问题不在
/// 画圆（一行 `fill_circle`），而在三件容易被各写一遍的事：
///
/// 1. **尺寸与文字基线对齐**——点与标签并排时，点必须居中在文字墨迹上，
///    而不是居中在行盒上（行盒含字体预留空间，直接居中会偏下 1~2px）；
/// 2. **语义色映射**——`online`/`degraded`/`offline` 该取 `success`/`warning`/`danger`，
///    分散写就会出现“同一状态三种颜色”；
/// 3. **脉冲相位**——「正在连接」需要一个呼吸的点，而它的相位必须由时间驱动
///    （可截图回归），不能靠各自 `request_animation` 猜。
///
/// ## 属性面
///
/// `state`（`ok` / `warn` / `error` / `idle` / `busy`，可写）、`label`（可写）、
/// `pulse`（可写）、`dot_size`（可写）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 状态点。
class StatusDot : public Element {
 public:
  /// 状态（语义；颜色由主题解析）。
  enum class State : std::uint8_t {
    Ok,     ///< 正常（`success`）
    Warn,   ///< 降级（`warning`）
    Error,  ///< 故障（`danger`）
    Idle,   ///< 空闲/未知（`text_faint`）
    Busy,   ///< 进行中（`primary`）
  };

  static constexpr float kDotSize{8.0f};            ///< 默认点直径（px）
  static constexpr float kGap{6.0f};                ///< 点与标签的间距（px）
  static constexpr float kPulsePeriodSeconds{1.4f}; ///< 呼吸周期（秒）

  StatusDot();
  explicit StatusDot(std::string label, State state = State::Ok);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "StatusDot"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Image; }

  void set_state(State state);
  [[nodiscard]] auto state() const noexcept -> State { return state_value_; }
  void set_label(std::string label);
  [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }
  /// 是否呼吸（`Busy` 默认自动开；其余默认关）。
  void set_pulse(bool pulse);
  [[nodiscard]] auto pulse() const noexcept -> bool { return pulse_; }
  void set_dot_size(float size);
  [[nodiscard]] auto dot_size() const noexcept -> float { return dot_size_; }

  /// 指定时刻的脉冲半径系数（1.0 = 原尺寸）。
  [[nodiscard]] auto pulse_scale(double time_seconds) const noexcept -> float;
  /// 点的矩形（arrange 后有效）。
  [[nodiscard]] auto dot_rect() const noexcept -> math::Rect { return dot_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  /// 状态短名（稳定；属性面与日志共用）。
  [[nodiscard]] static constexpr auto state_name(State state) noexcept -> std::string_view {
    switch (state) {
      case State::Ok: return "ok";
      case State::Warn: return "warn";
      case State::Error: return "error";
      case State::Idle: return "idle";
      case State::Busy: return "busy";
    }
    return "idle";
  }

 private:
  State state_value_{State::Ok};
  std::string label_{};
  bool pulse_{false};
  float dot_size_{kDotSize};
  math::Color dot_color_{};
  math::Color label_color_{};
  mutable math::Rect dot_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::StatusDot, "StatusDot")
