#pragma once

/// @file toast.hpp
/// @brief `Toast`：轻提示：`surface` 底 + `shadow_md` + 左侧 4px 色条（高 40、横向内边距 14）。。
///
/// 从 `overlay.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `overlay.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 轻提示：`surface` 底 + `shadow_md` + 左侧 4px 色条（高 40、横向内边距 14）。
/// 自动消失由帧时间轴驱动（`context.time_seconds`）：不额外起线程/定时器，
/// 无头单帧也能用 `expired()` 判定（详见 `set_auto_dismiss_ms`）。
class Toast : public Element {
 public:
  static constexpr float kHeight{40.0f};
  static constexpr float kPaddingX{14.0f};
  static constexpr float kAccentWidth{4.0f};
  /// FillViewport 挂载时与视口底边的留白。
  static constexpr float kBottomMargin{32.0f};
  /// 默认自动消失时长（ms）；`0` = 常驻（见 `set_auto_dismiss_ms`）。
  static constexpr double kDefaultDismissMs{2600.0};

  explicit Toast(std::string message = {}, Tone tone = Tone::Default);

  /// 工厂：`Toast::make("已保存", Tone::Success)`。
  [[nodiscard]] static auto make(std::string message, Tone tone) -> std::unique_ptr<Toast>;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Toast"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  void set_message(std::string message);
  [[nodiscard]] auto message() const noexcept -> const std::string& { return message_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  /// 到期推演前置到整组件绘制之前（阴影也不落盘；见实现注释）。
  void paint_impl(const RenderContext& context, raster::Surface& canvas) const override;
  /// 居中定位：UiRoot 对叠加层的默认排布是顶部左对齐，轻提示习惯上居中（可被覆盖）。
  /// `OverlayLayout::FillViewport` 挂载时改为**底部居中**（习惯的 Toast 位置），
  /// 遮罩不绘制（浅层浮层不遮内容）。
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return message_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  // —— 自动消失（时间轴驱动；v0.1.5） ——
  /// 设定自动消失时长（ms）。`0` = 常驻；改动会**重置**起算点（仅当已在计时）。
  void set_auto_dismiss_ms(double milliseconds) noexcept;
  [[nodiscard]] auto auto_dismiss_ms() const noexcept -> double { return auto_dismiss_ms_; }
  /// 是否已到期（未启用自动消失或尚未绘制过则恒 false；绘制路径推演）。
  [[nodiscard]] auto expired() const noexcept -> bool { return expired_; }
  /// 剩余展示时间（ms；未启用/已过期返回 0）。只读推算，不推进状态。
  [[nodiscard]] auto remaining_ms() const noexcept -> double;

  /// 到期回调（自动消失触发）：宿主应在此经 `overlay_remove` 摘除本组件。
  /// 不在绘制回调里直接摘：绘制路径不能在 UiRoot 遍历 overlays 时改动容器
  /// （与 `Select::flush_dismiss` 同一套"延迟摘除"做法）。
  std::function<void()> on_dismiss{};

 private:
  std::string message_{};
  Tone tone_{Tone::Default};
  double auto_dismiss_ms_{0.0};      ///< `0` = 常驻
  mutable double dismiss_at_{-1.0};  ///< 到期时刻（秒；`-1` = 未起算；mutable：绘制是 const 方法
  mutable bool expired_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Toast, "Toast")
