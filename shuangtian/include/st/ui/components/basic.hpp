#pragma once

/// 基础组件：文本 / 标题 / 图标 / 按钮 / 卡片 / 分隔线 / 徽标行。
/// 全部自绘：颜色与尺度一律取自 `Theme`（`apply_theme` 时落进 `style_`），不在组件里硬编码。

#include <functional>
#include <string>
#include <string_view>

#include "st/ui/element.hpp"
#include "st/ui/icon.hpp"

namespace st::ui {

/// 文本标签（单行省略，或 `set_multiline` 折行显示）。
class Text : public Element {
 public:
  explicit Text(std::string content = {});
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Text"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_content(std::string content);
  [[nodiscard]] auto content() const -> const std::string& { return content_; }
  void set_multiline(bool multiline);
  void set_max_lines(std::size_t lines);
  void set_tone(Tone tone);
  void set_font_size(float size);
  void set_weight(FontWeight weight);

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return content_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string content_{};
  bool multiline_{false};
  std::size_t max_lines_{0};
  Tone tone_{Tone::Default};
  float font_size_override_{0.0f};
  FontWeight weight_{FontWeight::Regular};
};

/// 标题（按层级取字号与字重）。
class Heading : public Text {
 public:
  explicit Heading(std::string content = {}, std::uint32_t level = 1);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Heading"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Heading; }
  void set_level(std::uint32_t level);
  [[nodiscard]] auto level() const noexcept -> std::uint32_t { return level_; }
  void apply_theme(const Theme& theme) override;

 private:
  std::uint32_t level_{1};
};

/// 图标。
class IconView : public Element {
 public:
  explicit IconView(std::string name = {}, float size = 18.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Icon"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Icon; }

  void set_icon(std::string name);
  [[nodiscard]] auto icon() const -> const std::string& { return icon_; }
  void set_size(float size);
  void set_tone(Tone tone);
  [[nodiscard]] auto size() const noexcept -> float { return size_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return icon_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;

 private:
  std::string icon_{};
  float size_{18.0f};
  Tone tone_{Tone::Default};
};

/// 按钮（含变体与尺寸；支持前导图标与点击回调）。
class Button : public Element {
 public:
  enum class Variant : std::uint8_t { Primary, Secondary, Ghost, Soft, Danger };
  enum class Size : std::uint8_t { Small, Medium, Large };

  explicit Button(std::string label, Variant variant = Variant::Primary,
                  Size size = Size::Medium);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Button"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Button; }

  void set_label(std::string label);
  [[nodiscard]] auto label() const -> const std::string& { return label_; }
  void set_icon(std::string icon);
  void set_variant(Variant variant);
  void set_size(Size size);
  void set_tone(Tone tone);
  [[nodiscard]] auto variant() const noexcept -> Variant { return variant_; }

  /// **图标盒径**（逻辑 px）——与 `apply_theme` 同一入口写入，供测试/宿主读取。
  ///
  /// 为何是一等读取面：图标尺寸是观感量，需要一个**被测对象自报的值**
  /// （`perceptual_changes.md` §14：不要信旁路探针）。测试直接读它，
  /// 就不必在测试里复刻一遍公式（复刻的公式一旦与实现脱钩就成假护栏）。
  [[nodiscard]] auto icon_box() const noexcept -> float { return icon_size_; }

  /// 当前图标在 `icon_box()` 盒里的**实际墨迹尺寸**（逻辑 px）。
  ///
  /// 排版（`measure`）与绘制（`paint_content`）都必须用它而不是盒宽：盒里
  /// 有透明留白，按盒宽排版会让内容重心偏向文字一侧（见 `measure` 里的推导）。
  [[nodiscard]] auto icon_ink_size() const -> math::Size {
    return Icon::ink_size(icon_, math::Size{icon_size_, icon_size_});
  }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  std::function<void()> on_click{};
  /// 图标在左（默认）或右。
  bool icon_leading{true};

 private:
  std::string label_{};
  std::string icon_{};
  Variant variant_{Variant::Primary};
  Size size_{Size::Medium};
  Tone tone_{Tone::Default};
  /// 图标盒径（由 `apply_theme` 按 Size 档从 `Metrics::icon_size*` 取）。
  float icon_size_{22.0f};
};

/// 卡片容器：`surface` 底 + 描边 + 圆角 + 阴影 + 内边距。
class Card : public Element {
 public:
  explicit Card(float padding = 16.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Card"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  void set_padding(float padding);
  void set_radius(float radius);
  void set_shadow_level(std::uint8_t level);
  void set_elevated(bool elevated);
  void apply_theme(const Theme& theme) override;

 private:
  float padding_{16.0f};
  float radius_override_{0.0f};
  /// 0=无阴影、1=sm、≥2=md（`elevated_` 为真时一律 lg）。
  /// 卡片默认用 **md**：浅底上的卡片需要“真的浮起来”，而 sm 更像一道描边。
  std::uint8_t shadow_level_{2};
  bool elevated_{false};
};

/// 水平分隔线。
class Divider : public Element {
 public:
  explicit Divider(bool vertical = false);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Divider"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Separator; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

 private:
  bool vertical_{false};
};

/// 键值行（左标签右值，用于详情面板）。
class KeyValueRow : public Element {
 public:
  KeyValueRow(std::string key, std::string value);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "KeyValueRow"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_value(std::string value);
  [[nodiscard]] auto value() const -> const std::string& { return value_; }
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return value_; }

 private:
  /// 显示标签（**不是** `Element::key_`）：基类的 key 是稳定逻辑身份（参与自动 id
  /// `Type@key`），把界面文案当身份会让 id 随文案变动而漂移。
  std::string label_{};
  std::string value_{};
};

}  // namespace st::ui
