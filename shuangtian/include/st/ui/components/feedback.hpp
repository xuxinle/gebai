#pragma once

/// 组件库 C（数据展示与反馈）——进度与状态类组件：`ProgressBar` / `Spinner` / `Badge` /
/// `Chip` / `Avatar` / `Tooltip`。
/// 视觉规格（全部取主题 token，无硬编码色）：
/// - `ProgressBar`：6px 圆角轨道（`surface_sunken`）+ 填充（`primary` → `primary_hover` 线性渐变）；
/// - `Spinner`：18px 直径、2px 描边、3/4 圆弧（`primary`），旋转由 `RenderContext::time_seconds`
///   驱动（1.2s/圈），动画过程无内部状态；
/// - `Badge`：胶囊（高 20、横向内边距 8、`radius_pill`），`tone_soft_color` 底 + `tone_color` 字；
/// - `Chip`：高 26 胶囊，可选前导圆点与关闭 ×（16px 命中区）；
/// - `Avatar`：圆形，`primary_soft` 底 + `primary` 首码点（大小 28 / 36 / 48）；
/// - `Tooltip`：`surface` 底 + `shadow_md` + 8px 圆角 + `font_xs` 文本，位于目标矩形上方居中。
/// 依赖纪律：一经 `RenderContext::text` 取文本能力（可为空 → `NullTextPort`），不 include text 层。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 进度条：圆角轨道 + 渐变填充；`value` 取值 `[0,1]`，语义值输出百分比字符串。
class ProgressBar : public Element {
 public:
  static constexpr float kTrackHeight{6.0f};  ///< 轨道高（px）

  explicit ProgressBar(float value = 0.0f);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ProgressBar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ProgressBar; }

  /// 设定进度（自动夹取到 `[0,1]`）。
  void set_value(float value);
  [[nodiscard]] auto value() const noexcept -> float { return value_; }
  /// 色调（默认 `Primary` → `primary`→`primary_hover` 渐变；其他色调为同色系渐变）。
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  float value_{0.0f};
  Tone tone_{Tone::Primary};
};

/// 旋转指示器：3/4 圆弧，相位完全由 `RenderContext::time_seconds` 决定（可截屏回归）。
class Spinner : public Element {
 public:
  static constexpr float kDiameter{18.0f};        ///< 默认直径（px）
  static constexpr float kStroke{2.0f};           ///< 描边宽（px）
  static constexpr float kPeriodSeconds{1.2f};    ///< 一圈耗时（秒）
  static constexpr float kSweepDegrees{270.0f};   ///< 圆弧张角（度）

  explicit Spinner(float diameter = kDiameter);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Spinner"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::ProgressBar; }

  void set_diameter(float diameter);
  [[nodiscard]] auto diameter() const noexcept -> float { return diameter_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  /// 指定时刻的圆弧起始角（度，0° 指向 +x 轴、顺时针为正）。
  [[nodiscard]] auto phase_degrees(double time_seconds) const noexcept -> float;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  float diameter_{kDiameter};
  Tone tone_{Tone::Primary};
};

/// 徽标：`tone_soft_color` 胶囊底 + `tone_color` 文本（高 20、横向内边距 8）。
class Badge : public Element {
 public:
  static constexpr float kHeight{20.0f};
  static constexpr float kPaddingX{8.0f};

  explicit Badge(std::string text = {}, Tone tone = Tone::Default);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Badge"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string text_{};
  Tone tone_{Tone::Default};
};

/// 标签片：高 26 胶囊，可选前导圆点与关闭 ×（16px 命中区）。
class Chip : public Element {
 public:
  static constexpr float kHeight{26.0f};
  static constexpr float kPaddingX{12.0f};
  static constexpr float kDotSize{6.0f};      ///< 前导圆点直径（px）
  static constexpr float kGap{6.0f};          ///< 元素间距（px）
  static constexpr float kCloseArea{16.0f};   ///< 关闭命中区边长（px）
  static constexpr float kCloseGlyph{12.0f};  ///< 关闭图标边长（px）

  explicit Chip(std::string text = {}, bool closable = false);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Chip"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Button; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 前导圆点（与文本同色调）。
  void set_dot(bool dot);
  [[nodiscard]] auto dot() const noexcept -> bool { return dot_; }
  void set_closable(bool closable);
  [[nodiscard]] auto closable() const noexcept -> bool { return closable_; }
  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }

  /// 关闭 × 的命中区（arrange 后有效）。
  [[nodiscard]] auto close_area() const noexcept -> math::Rect;
  /// 文本绘制区（arrange 后有效）。
  [[nodiscard]] auto text_area() const noexcept -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  std::function<void()> on_close{};
  std::function<void()> on_click{};

 private:
  std::string text_{};
  Tone tone_{Tone::Default};
  bool dot_{false};
  bool closable_{false};
};

/// 头像：圆形底色 `primary_soft` + `primary` 首码点（28 / 36 / 48 三档）。
class Avatar : public Element {
 public:
  static constexpr float kSmall{28.0f};
  static constexpr float kMedium{36.0f};
  static constexpr float kLarge{48.0f};

  explicit Avatar(std::string name = {}, float size = kMedium);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Avatar"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Image; }

  void set_name(std::string name);
  [[nodiscard]] auto name() const noexcept -> const std::string& { return name_; }
  void set_size(float size);
  [[nodiscard]] auto size() const noexcept -> float { return size_; }
  /// 用于绘制的首字母（UTF-8 首码点；ASCII 字母大写化）。
  [[nodiscard]] auto initials() const -> std::string;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return initials(); }
  [[nodiscard]] auto semantics_value() const -> std::string override { return name_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string name_{};
  float size_{kMedium};
};

/// 提示气泡：`set_target_rect` 后自动定位到目标上方居中；`set_active` 控制显隐。
class Tooltip : public Element {
 public:
  static constexpr float kPaddingX{8.0f};
  static constexpr float kPaddingY{4.0f};
  static constexpr float kGap{6.0f};     ///< 与目标的间距（px）
  static constexpr float kRadius{8.0f};  ///< 圆角（px）

  explicit Tooltip(std::string text = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Tooltip"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Tooltip; }

  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 目标矩形（屏幕坐标）：arrange 时据此把气泡放到上方居中（空间不足则放下方）。
  void set_target_rect(math::Rect rect);
  [[nodiscard]] auto target_rect() const noexcept -> math::Rect { return target_; }
  void set_active(bool active);
  [[nodiscard]] auto active() const noexcept -> bool { return active_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint(const RenderContext& context, raster::Canvas& canvas) const override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return text_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return text_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string text_{};
  math::Rect target_{};
  bool active_{false};
};

}  // namespace st::ui
