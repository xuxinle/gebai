#pragma once

/// 组件库 C（数据展示与反馈）——叠加层组件：模态对话框 `Dialog` 与轻提示 `Toast`。
/// 视觉规格：
/// - `Dialog`：居中卡片（宽 420~520、`radius_xl`、`shadow_lg`、`surface` 底、内边距 20），
///   标题 `font_xl` SemiBold、正文 `font_base`/`text_muted`、底部右对齐按钮行；打开时先铺
///   半透明遮罩（`colors.overlay` 覆盖整个视口）；
/// - `Toast`：`radius_md` + `shadow_md` + 左侧 4px 色条（`tone_color`），高 40、横向内边距 14。
/// 用法：把 `Dialog` / `Toast` 作为 `UiRoot` 的 overlay 挂载（`add_overlay`），
/// `Dialog` 需由调用方注入视口矩形（`set_viewport_rect`）以铺满遮罩并居中卡片。
/// 依赖纪律：文本一律经 `RenderContext::text`（可为空 → `NullTextPort`），不 include text 层。

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 模态对话框：遮罩 + 居中卡片 + 标题/正文 + 底部按钮行。
/// 交互：Esc 与点击卡片外区域触发 `on_dismiss`；按钮点击触发 `on_action(index)`。
class Dialog : public Element {
 public:
  static constexpr float kMinWidth{420.0f};
  static constexpr float kMaxWidth{520.0f};
  static constexpr float kPadding{20.0f};

  explicit Dialog(std::string title = {}, std::string body = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Dialog"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  /// 模态对话框只有在场时才拦截输入：`set_visible(false)` 后不再截住下层内容
  /// （不可见浮层的命中穿透；见 `Element::intercepts_input` 契约）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  void set_title(std::string title);
  [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
  void set_body(std::string body);
  [[nodiscard]] auto body() const noexcept -> const std::string& { return body_; }
  /// 设定底部按钮行（顺序即 `on_action` 的序号；末项为主按钮）。
  void set_actions(std::vector<std::string> actions);
  [[nodiscard]] auto actions() const noexcept -> const std::vector<std::string>& { return actions_; }

  /// 视口矩形（遮罩覆盖范围；由调用方注入）。
  void set_viewport_rect(math::Rect rect);
  [[nodiscard]] auto viewport_rect() const noexcept -> math::Rect { return viewport_; }
  /// 居中卡片矩形（arrange 后有效）。
  [[nodiscard]] auto card_rect() const noexcept -> math::Rect { return card_; }
  /// 第 `index` 个按钮的矩形（arrange 后有效；越界返回空矩形）。
  [[nodiscard]] auto action_rect(std::size_t index) const noexcept -> math::Rect;
  [[nodiscard]] auto action_count() const noexcept -> std::size_t { return actions_.size(); }

  /// 触发关闭（调用 `on_dismiss`；是否真正移除由调用方决定）。
  void dismiss();

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return title_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return body_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 按钮点击回调（参数为按钮序号）。
  std::function<void(std::size_t)> on_action{};
  /// 关闭请求回调（Esc / 点击遮罩）。
  std::function<void()> on_dismiss{};

 private:
  [[nodiscard]] auto body_lines(const RenderContext& context, float width) const
      -> std::vector<std::string>;

  std::string title_{};
  std::string body_{};
  std::vector<std::string> actions_{};
  std::vector<Element*> buttons_{};  ///< 非拥有（生命周期随子节点）
  math::Rect viewport_{};
  math::Rect card_{};
};

/// 轻提示：`surface` 底 + `shadow_md` + 左侧 4px 色条（高 40、横向内边距 14）。
/// 自动消失由帧时间轴驱动（`context.time_seconds`）：不额外起线程/定时器，
/// 无头单帧也能用 `expired()` 判定（详见 `set_auto_dismiss_ms`）。
class Toast : public Element {
 public:
  static constexpr float kHeight{40.0f};
  static constexpr float kPaddingX{14.0f};
  static constexpr float kAccentWidth{4.0f};
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
  void paint(const RenderContext& context, raster::Surface& canvas) const override;
  /// 居中定位：UiRoot 对叠加层的默认排布是顶部左对齐，轻提示习惯上居中（可被覆盖）。
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
