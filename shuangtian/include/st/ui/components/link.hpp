#pragma once

/// @file link.hpp
/// @brief `Link`：文字链接（下划线 / 悬停 / 外部跳转标记 / 点击派发 URL）。
///
/// ## 与 `Button` 的分工
///
/// `Button` 是**动作**（提交、删除）；`Link` 是**跳转**（去别处 / 打开浏览器）。
/// 二者在视觉与语义上都不同：链接是文字、有下划线、光标是手型、语义角色是 `Role::Link`。
///
/// ## 五个必须做对的点
///
/// 1. **下划线位置**：贴**墨迹底**（不是行盒底）——按行盒画会离文字太远，
///    看起来像一条独立的分隔线。
/// 2. **下划线不覆盖下伸部**：`g`/`p`/`y` 的下伸部分不能穿过下划线，
///    所以要用字体的 `underline_position` 而不是「墨迹底」。
/// 3. **外部链接标记**：`↗` 要跟着文字、不能吃掉文字宽（否则点击区变了）。
/// 4. **截断时下划线跟着截断**：文字用 `…` 截断后，下划线不能还是原宽。
/// 5. **悬停**：颜色转主色 + 下划线加粗一档（不是只变色——弱视用户看不出）。
///
/// ## 几何
///
/// ```
/// 查看文档 ↗
/// ─────────     ← 下划线贴墨迹底（不是行盒底）
/// ```
///
/// ## 属性面
///
/// `text`（可写）、`url`（可写）、`external`（可写：显示 `↗`）、`tone`（可写）、
/// `underline`（可写）。
/// 动作面：`activate` / `click`（派发 `on_click`）。

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 文字链接。
class Link : public Element {
 public:
  static constexpr float kExternalGap{3.0f};    ///< `↗` 与文字的间距（px）
  static constexpr float kExternalSize{11.0f};  ///< `↗` 的边长（px）

  Link();
  explicit Link(std::string text);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Link"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Link; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 目标地址（点击时经 `on_click` 交给宿主——本组件**不自己打开浏览器**，
  /// 那是宿主的职责与权限，组件只负责把意图说清楚）。
  void set_url(std::string url);
  [[nodiscard]] auto url() const noexcept -> const std::string& { return url_; }
  /// 外部链接：显示 `↗` 标记。
  void set_external(bool external);
  [[nodiscard]] auto external() const noexcept -> bool { return external_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }
  void set_underline(bool underline);
  [[nodiscard]] auto underline() const noexcept -> bool { return underline_; }

  /// 文字区块（arrange 后有效；不含 `↗`）。
  [[nodiscard]] auto text_rect() const noexcept -> math::Rect { return text_box_; }
  /// 下划线的矩形（`underline()` 为假或文字为空时为空）。
  [[nodiscard]] auto underline_rect() const noexcept -> math::Rect { return underline_box_; }
  /// `↗` 标记的矩形（非外部链接时为空）。
  [[nodiscard]] auto external_rect() const noexcept -> math::Rect { return external_box_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return url_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 点击（实参为 `url`；未设 `url` 时为空串）。
  std::function<void(std::string_view)> on_click{};

 private:
  void relayout(const RenderContext& context, math::Rect rect) const;

  std::string text_{};
  std::string url_{};
  bool external_{false};
  bool underline_{true};
  Tone tone_{Tone::Primary};
  float font_size_{14.0f};

  mutable math::Rect text_box_{};
  mutable math::Rect underline_box_{};
  mutable math::Rect external_box_{};
  mutable std::string shown_{};
  mutable bool hover_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Link, "Link")
