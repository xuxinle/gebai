#pragma once

/// @file kbd.hpp
/// @brief `Kbd`：键帽（单个按键或组合键序列，如 `Ctrl` `Shift` `S`）。
///
/// ## 为什么键帽要是一个组件
///
/// 「把 `Ctrl+Shift+S` 画成三个键帽」看起来是小事，但它有几条**必须一致**的规矩：
///
/// 1. **平台修饰键名**：同一份文档在 Windows 上是 `Ctrl`、macOS 上是 `⌘`。
///    分散写必然出现一处 `Ctrl` 一处 `Cmd`。
/// 2. **符号 vs 文字**：`⌘`/`⇧`/`⌥` 这些符号比文字窄得多，键帽宽要按内容量。
/// 3. **键帽的视觉语言**：等宽小字 + 浅底 + 细描边 + 圆角，且**基线对齐**
///    （与正文并排时不能下沉）。
/// 4. **分隔符**：多个键帽之间用 `+` 还是空白，是排版决定，不该散落在调用处。
///
/// ## 几何
///
/// ```
/// ┌───┐ ┌───┐ ┌───┐
/// │Ctrl│ │ ⇧ │ │ S │        ← 键帽：内边距 + 最小宽 + 圆角
/// └───┘ └───┘ └───┘
/// ```
///
/// ## 用法
///
/// ```cpp
/// auto kbd = std::make_unique<Kbd>();
/// kbd->set_keys({"Ctrl", "Shift", "S"});     // 或 set_shortcut("Ctrl+Shift+S")
/// kbd->set_separator(Kbd::Separator::Plus);  // 键帽之间是否画 `+`
/// ```
///
/// ## 属性面
///
/// `keys`（`+` 分隔，可写）、`separator`（none/plus，可写）、`platform`（auto/windows/mac/linux，
/// 可写；影响修饰键的显示名）。

#include <cstddef>
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

/// 键帽（单键或组合键）。
class Kbd : public Element {
 public:
  static constexpr float kHeight{18.0f};        ///< 键帽高（px）
  static constexpr float kPaddingX{5.0f};       ///< 键帽横向内边距（px）
  static constexpr float kMinWidth{16.0f};      ///< 键帽最小宽（px）
  static constexpr float kCapGap{3.0f};         ///< 键帽间距（px）
  static constexpr float kRadius{4.0f};         ///< 键帽圆角（px）

  /// 键帽之间的分隔。
  enum class Separator : std::uint8_t { None, Plus };
  /// 平台（决定修饰键显示名）。
  enum class Platform : std::uint8_t { Auto, Windows, Mac, Linux };

  Kbd();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Kbd"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  // —— 按键 ——
  void set_keys(std::vector<std::string> keys);
  /// 从 `Ctrl+Shift+S` 这种写法设置（自动按 `+` 拆）。
  void set_shortcut(std::string_view shortcut);
  [[nodiscard]] auto keys() const noexcept -> const std::vector<std::string>& { return keys_; }
  /// **显示用**的键名（已按平台替换修饰键）。
  [[nodiscard]] auto display_keys() const -> std::vector<std::string>;

  void set_separator(Separator separator);
  [[nodiscard]] auto separator() const noexcept -> Separator { return separator_; }
  void set_platform(Platform platform);
  [[nodiscard]] auto platform() const noexcept -> Platform { return platform_; }

  /// 第 `index` 个键帽的矩形（arrange 后有效；越界为空）。
  [[nodiscard]] auto cap_rect(std::size_t index) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  /// 平台修饰键名 → 该平台的显示符号/文字。
  [[nodiscard]] static auto platform_label(std::string_view modifier, Platform platform)
      -> std::string;

 private:
  /// 重算键帽矩形（`measure` 与 `arrange` 共用）。
  void layout_caps(const RenderContext& context, math::Rect rect) const;

  std::vector<std::string> keys_{};
  Separator separator_{Separator::None};
  Platform platform_{Platform::Auto};
  float font_size_{11.0f};

  math::Color cap_fill_{};
  math::Color cap_border_{};
  math::Color cap_text_{};
  mutable std::vector<math::Rect> caps_{};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Kbd, "Kbd")
