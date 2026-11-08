#pragma once

/// @file banner.hpp
/// @brief `Banner`：通知条（左色条 + 图标 + 标题/说明 + 可选关闭与动作）。
///
/// ## 与 `Toast` / `Tooltip` 的分工
///
/// | 组件 | 出现方式 | 生命周期 | 典型场景 |
/// |---|---|---|---|
/// | `Banner` | **在流内**（占据布局位置） | 由数据决定，用户可关 | 页面顶部的版本更新提示、权限申请、离线警告 |
/// | `Toast` | 浮在内容之上 | 自动消失 | 操作结果（已保存/复制成功） |
/// | `Tooltip` | 悬停跟随 | 交互驱动 | 控件说明 |
///
/// 关键差别在**是否占位**：需要用户一直看见的提示不能用 Toast（会消失），
/// 而 Banner 是内容流的一部分——它把「页面往下推」，用户绕不过它。
///
/// ## 视觉
///
/// 四档语义色调（`Info` / `Success` / `Warning` / `Danger`），每档给：
/// 左缘一道 3px 色条、`tone_soft_color` 底、`tone_color` 图标与标题。
/// 关闭 × 与动作按钮都是可选项（`set_closable` / `set_action`）。
///
/// ## 属性面
///
/// `title`、`description`、`tone`、`icon`、`action`、`closable` 全部可读写；
/// 只读：`visible`（关闭后为 false，但仍占位——**不**自动从父级摘掉自己）。
/// 动作面：`dismiss`（同点关闭）、`activate`（同点动作）。

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

/// 通知条。
class Banner : public Element {
 public:
  static constexpr float kAccentWidth{3.0f};     ///< 左缘色条宽（px）
  static constexpr float kPaddingX{12.0f};       ///< 横向内边距（px）
  static constexpr float kPaddingY{10.0f};       ///< 纵向内边距（px）
  static constexpr float kIconSize{16.0f};       ///< 图标边长（px）
  static constexpr float kGap{8.0f};             ///< 图标/文字/按钮之间的间距（px）
  static constexpr float kCloseArea{18.0f};      ///< 关闭 × 命中区边长（px）

  Banner();
  explicit Banner(std::string title, Tone tone = Tone::Primary);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Banner"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 数据 ——
  void set_title(std::string title);
  [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
  void set_description(std::string description);
  [[nodiscard]] auto description() const noexcept -> const std::string& { return description_; }
  /// 语义色调（`Info` 档 = `Primary`；霜天的 `Tone` 没有单独的 info，主色就是“信息蓝”）。
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }
  /// 图标名（空 = 按色调取默认图标）。
  void set_icon(std::string icon_name);
  [[nodiscard]] auto icon() const noexcept -> const std::string& { return icon_; }
  /// 动作按钮文案（空 = 不显示）。
  void set_action(std::string label);
  [[nodiscard]] auto action() const noexcept -> const std::string& { return action_; }
  /// 是否显示关闭 ×。
  void set_closable(bool closable);
  [[nodiscard]] auto closable() const noexcept -> bool { return closable_; }
  /// 关闭（派发 `on_dismiss`，并把自身设为不可见；**不从父级移除**——
  /// 「谁负责摘掉它」是宿主的决定，组件不掌握自己的所有权）。
  void dismiss();

  // —— 几何（arrange 后有效）——
  [[nodiscard]] auto close_rect() const noexcept -> math::Rect { return close_; }
  [[nodiscard]] auto action_rect() const noexcept -> math::Rect { return action_box_; }

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

  /// 点 ×。
  std::function<void()> on_dismiss{};
  /// 点动作按钮。
  std::function<void()> on_action_click{};

 private:
  /// 该色调的默认图标（用户没指定 `icon` 时用）。
  [[nodiscard]] auto default_icon() const -> std::string_view;
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::string title_{};
  std::string description_{};
  std::string icon_{};
  std::string action_{};
  /// 四档语义色调（`Primary` 充当 Info）。
  Tone tone_{Tone::Primary};
  bool closable_{false};

  mutable math::Rect close_{};
  mutable math::Rect action_box_{};
  mutable bool hover_close_{false};
  mutable bool hover_action_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT_WITH(::st::ui::Banner, "Banner", std::make_unique<::st::ui::Banner>(""))
