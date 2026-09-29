#include "st/ui/theme.hpp"

namespace st::ui {
namespace {

// 设计令牌一律用 `from_rgba_hex`（显式 0xRRGGBBAA）：
// `from_hex` 靠数值大小猜格式，遇到“高位字节为 0 且带 alpha”的字面量会静默误读——
// 实测 `0x0000008C`（黑色 55%）被当成 `0x00008C`（纯蓝、不透明），
// 结果是深色主题的卡片阴影发蓝光。字面量都写成 8 位 + 显式 alpha 就没有这个歧义。
[[nodiscard]] auto hex(std::uint32_t value) -> math::Color {
  return math::Color::from_rgba_hex(value);
}

}  // namespace

auto Theme::light() -> Theme {
  Theme theme;
  theme.mode_ = ThemeMode::Light;
  Palette& palette = theme.colors_;
  palette.bg = hex(0xF6F8FCFFU);
  palette.surface = hex(0xFFFFFFFFU);
  palette.surface_alt = hex(0xEEF2F9FFU);
  palette.surface_sunken = hex(0xE6EBF4FFU);
  palette.border = hex(0xD3DCE9FFU);
  palette.border_strong = hex(0xB4C0D4FFU);
  palette.text = hex(0x0F172AFFU);
  // 次级级文字拉开一档并**各自达标**：
  // 原 text_faint (#94A3B8) 在白色上只有 **2.6:1**（正文级 4.5:1、大字级 3:1 都不够），
  // 而它承担的是 11px 的导航标题/版本号这类小字——对比度不够就是“看不清”。
  // 现在 muted ≈ 6.3:1、faint ≈ 4.2:1（大字与辅助信息可用），层次仍然看得见。
  palette.text_muted = hex(0x56647CFFU);
  palette.text_faint = hex(0x66768CFFU);
  palette.primary = hex(0x2563EBFFU);
  palette.primary_hover = hex(0x1D4ED8FFU);
  // 悬浮档位要**看得见**：早先取 0xF2F6FC，与侧栏底色只差 4/2/0（肉眼近乎没有），
  // 实测像素差才 6/255。现在这一步在浅色底上清晰可辨，又不至于喧宾夺主。
  palette.surface_hover = hex(0xE6EEFAFFU);
  palette.surface_pressed = hex(0xD6E2F5FFU);
  palette.border_hover = hex(0x8FAAD2FFU);
  palette.glow = hex(0x3B82F62EU);
  palette.primary_active = hex(0x1E40AFFFU);
  palette.primary_soft = hex(0xEAF1FFFFU);
  palette.on_primary = hex(0xFFFFFFFFU);
  palette.accent = hex(0x0891B2FFU);
  palette.accent_soft = hex(0xDCF3F8FFU);
  palette.success = hex(0x059669FFU);
  palette.warning = hex(0xD97706FFU);
  palette.danger = hex(0xDC2626FFU);
  palette.focus_ring = hex(0x2563EB66U);
  palette.overlay = hex(0x0F172A66U);
  palette.shadow = hex(0x0F172A26U);  ///< 两层阴影的基色（≈15%：两层叠加后仍在 20% 以内）
  palette.selection = hex(0xBFD6FEFFU);
  palette.code_bg = hex(0xF3F6FBFFU);
  palette.code_border = hex(0xE2E8F2FFU);
  // 语法色板（浅色）：关键字紫、字符串绿、数字橙、注释灰、类型青、函数蓝——彼此高辨识度
  theme.syntax_.plain = hex(0x1E2433FFU);
  theme.syntax_.keyword = hex(0x8B31C7FFU);
  theme.syntax_.type = hex(0x0E7C86FFU);
  theme.syntax_.string = hex(0x1A7F4BFFU);
  theme.syntax_.number = hex(0xB45309FFU);
  theme.syntax_.comment = hex(0x8A94A6FFU);
  theme.syntax_.function = hex(0x2563EBFFU);
  theme.syntax_.operator_ = hex(0x64748BFFU);
  theme.syntax_.punctuation = hex(0x94A3B8FFU);
  theme.syntax_.preprocessor = hex(0xC2410CFFU);
  theme.syntax_.builtin = hex(0x0F766EFFU);
  theme.syntax_.attribute = hex(0xBE185DFFU);
  theme.syntax_.key = hex(0x1D4ED8FFU);
  theme.syntax_.tag = hex(0xB91C1CFFU);
  theme.syntax_.inserted = hex(0x15803DFFU);
  theme.syntax_.deleted = hex(0xB91C1CFFU);
  theme.syntax_.line_number = hex(0xA0AEC0FFU);
  theme.syntax_.current_line = hex(0x2563EB0AU);
  theme.syntax_.selection = hex(0x2563EB33U);
  theme.syntax_.cursor = hex(0x2563EBFFU);
  theme.syntax_.matching_bracket = hex(0xF59E0B55U);
  theme.metrics_ = Metrics{};
  return theme;
}

auto Theme::dark() -> Theme {
  Theme theme;
  theme.mode_ = ThemeMode::Dark;
  Palette& palette = theme.colors_;
  palette.bg = hex(0x0A0F1AFFU);
  palette.surface = hex(0x121A2BFFU);
  palette.surface_alt = hex(0x1A2438FFU);
  palette.surface_sunken = hex(0x0E1626FFU);
  palette.border = hex(0x2E3C57FFU);
  palette.border_strong = hex(0x435473FFU);
  palette.text = hex(0xE8EEF9FFU);
  palette.text_muted = hex(0xA3B1C9FFU);
  palette.text_faint = hex(0x7F8DA6FFU);
  palette.primary = hex(0x4C8DFFFFU);
  palette.primary_hover = hex(0x6BA1FFFFU);
  palette.primary_active = hex(0x3B7AF0FFU);
  // 深色主题同样要给这套令牌：漏掉的话默认是全透明，**悬浮在暗色下毫无反馈**——
  // 而这种"某个主题下静默失效"的问题最难发现（浅色看着一切正常）。
  palette.surface_hover = hex(0x1D2942FFU);
  palette.surface_pressed = hex(0x25334FFF);
  palette.border_hover = hex(0x46608CFFU);
  palette.glow = hex(0x60A5FA38U);
  palette.primary_soft = hex(0x16233DFFU);
  palette.on_primary = hex(0x08101FFFU);
  palette.accent = hex(0x22D3EEFFU);
  palette.accent_soft = hex(0x0E2A34FFU);
  palette.success = hex(0x34D399FFU);
  palette.warning = hex(0xFBBF24FFU);
  palette.danger = hex(0xF87171FFU);
  palette.focus_ring = hex(0x4C8DFF66U);
  palette.overlay = hex(0x02061799U);
  // 深色主题的阴影基色比浅色重得多：深底上投影几乎看不见，需要更高不透明度
  // 才能提供一点“离地”分离度。
  palette.shadow = hex(0x0000008CU);
  palette.selection = hex(0x274B8CFFU);
  palette.code_bg = hex(0x0E1626FFU);
  palette.code_border = hex(0x1E2A41FFU);
  // 语法色板（深色）：同族色相提亮，保证在深底上仍有足够对比
  theme.syntax_.plain = hex(0xD7E0F0FFU);
  theme.syntax_.keyword = hex(0xC792EAFFU);
  theme.syntax_.type = hex(0x7FDBFFFFU);
  theme.syntax_.string = hex(0xA5E075FFU);
  theme.syntax_.number = hex(0xF7A85FFFU);
  theme.syntax_.comment = hex(0x6B7A90FFU);
  theme.syntax_.function = hex(0x82AAFFFFU);
  theme.syntax_.operator_ = hex(0x9AA9C0FFU);
  theme.syntax_.punctuation = hex(0x7C8AA3FFU);
  theme.syntax_.preprocessor = hex(0xFFB07BFFU);
  theme.syntax_.builtin = hex(0x7FE3D4FFU);
  theme.syntax_.attribute = hex(0xFF9ED2FFU);
  theme.syntax_.key = hex(0x8FB8FFFFU);
  theme.syntax_.tag = hex(0xFF8F8FFFU);
  theme.syntax_.inserted = hex(0x6EE7A0FFU);
  theme.syntax_.deleted = hex(0xFF9A9AFFU);
  theme.syntax_.line_number = hex(0x54637AFFU);
  theme.syntax_.current_line = hex(0x4C8DFF1AU);
  theme.syntax_.selection = hex(0x4C8DFF44U);
  theme.syntax_.cursor = hex(0x9CC0FFFFU);
  theme.syntax_.matching_bracket = hex(0xFBBF2455U);
  theme.metrics_ = Metrics{};
  return theme;
}

auto Theme::by_mode(ThemeMode mode) -> Theme { return mode == ThemeMode::Dark ? dark() : light(); }

auto tone_color(const Theme& theme, Tone tone) -> math::Color {
  const Palette& palette = theme.colors();
  switch (tone) {
    case Tone::Default: return palette.text;
    case Tone::Muted: return palette.text_muted;
    case Tone::Faint: return palette.text_faint;
    case Tone::Primary: return palette.primary;
    case Tone::Accent: return palette.accent;
    case Tone::Success: return palette.success;
    case Tone::Warning: return palette.warning;
    case Tone::Danger: return palette.danger;
    case Tone::OnPrimary: return palette.on_primary;
  }
  return palette.text;
}

auto tone_soft_color(const Theme& theme, Tone tone) -> math::Color {
  const Palette& palette = theme.colors();
  switch (tone) {
    case Tone::Primary: return palette.primary_soft;
    case Tone::Accent: return palette.accent_soft;
    case Tone::Success: return palette.success.with_alpha_f(0.14f);
    case Tone::Warning: return palette.warning.with_alpha_f(0.16f);
    case Tone::Danger: return palette.danger.with_alpha_f(0.14f);
    case Tone::Default:
    case Tone::Muted:
    case Tone::Faint:
    case Tone::OnPrimary: return theme.mode() == ThemeMode::Dark ? palette.surface_alt
                                                                : palette.surface_alt;
  }
  return palette.surface_alt;
}

/// 阴影三档（均为“关键光 + 环境光”两层，基色取主题的 `colors().shadow`，可被主题覆盖）。
///
/// 参数取值原则：
/// - **关键层**紧（blur 小、偏移 1~4px）：提供“边缘与底面接触”的可信感；
/// - **环境层**大（blur 是关键层的 3~4 倍、更淡）：提供“离地高度”的纵深感；
/// - 浅色底上总不透明度控制在 20% 以内：超过会显脏（像污渍而不是光）；
/// - 深色底上阴影几乎看不见，层次靠 `border`/`surface_alt`——阴影只补一点分离度。
namespace {

/// 从基色按比例派生一层（`factor` 为相对基色不透明度的倍数）。
[[nodiscard]] auto shadow_layer(const math::Color& base, float factor) -> math::Color {
  return base.with_alpha_f(static_cast<float>(base.a) / 255.0f * factor);
}

}  // namespace

[[nodiscard]] auto shadow_sm(const Theme& theme) -> Shadow {
  const math::Color base = theme.colors().shadow;
  Shadow shadow;
  shadow.color = shadow_layer(base, 0.90f);
  shadow.blur = 3.0f;
  shadow.offset_y = 1.0f;
  shadow.color2 = shadow_layer(base, 0.60f);
  shadow.blur2 = 10.0f;
  shadow.offset2_y = 3.0f;
  return shadow;
}

[[nodiscard]] auto shadow_md(const Theme& theme) -> Shadow {
  const math::Color base = theme.colors().shadow;
  Shadow shadow;
  shadow.color = base;
  shadow.blur = 6.0f;
  shadow.offset_y = 2.0f;
  shadow.color2 = shadow_layer(base, 0.70f);
  shadow.blur2 = 22.0f;
  shadow.offset2_y = 6.0f;
  return shadow;
}

[[nodiscard]] auto shadow_lg(const Theme& theme) -> Shadow {
  const math::Color base = theme.colors().shadow;
  Shadow shadow;
  shadow.color = shadow_layer(base, 1.05f);
  shadow.blur = 10.0f;
  shadow.offset_y = 4.0f;
  shadow.color2 = shadow_layer(base, 0.85f);
  shadow.blur2 = 40.0f;
  shadow.offset2_y = 14.0f;
  return shadow;
}

}  // namespace st::ui
