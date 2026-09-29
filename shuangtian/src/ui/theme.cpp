#include "st/ui/theme.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto hex(std::uint32_t value) -> math::Color { return math::Color::from_hex(value); }

}  // namespace

auto Theme::light() -> Theme {
  Theme theme;
  theme.mode_ = ThemeMode::Light;
  Palette& palette = theme.colors_;
  palette.bg = hex(0xF6F8FCFFU);
  palette.surface = hex(0xFFFFFFFFU);
  palette.surface_alt = hex(0xEEF2F9FFU);
  palette.surface_sunken = hex(0xE6EBF4FFU);
  palette.border = hex(0xDDE4EFFFU);
  palette.border_strong = hex(0xC3CEDFFFU);
  palette.text = hex(0x0F172AFFU);
  palette.text_muted = hex(0x64748BFFU);
  palette.text_faint = hex(0x94A3B8FFU);
  palette.primary = hex(0x2563EBFFU);
  palette.primary_hover = hex(0x1D4ED8FFU);
  palette.primary_active = hex(0x1E40AFFFU);
  palette.primary_soft = hex(0xE4ECFEFFU);
  palette.on_primary = hex(0xFFFFFFFFU);
  palette.accent = hex(0x0891B2FFU);
  palette.accent_soft = hex(0xDCF3F8FFU);
  palette.success = hex(0x059669FFU);
  palette.warning = hex(0xD97706FFU);
  palette.danger = hex(0xDC2626FFU);
  palette.focus_ring = hex(0x2563EB66U);
  palette.overlay = hex(0x0F172A66U);
  palette.shadow = hex(0x0F172A1FU);
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
  palette.border = hex(0x26324AFFU);
  palette.border_strong = hex(0x33425FFFU);
  palette.text = hex(0xE8EEF9FFU);
  palette.text_muted = hex(0x94A3BDffU);
  palette.text_faint = hex(0x64748BFFU);
  palette.primary = hex(0x4C8DFFFFU);
  palette.primary_hover = hex(0x6BA1FFFFU);
  palette.primary_active = hex(0x3B7AF0FFU);
  palette.primary_soft = hex(0x16233DFFU);
  palette.on_primary = hex(0x08101FFFU);
  palette.accent = hex(0x22D3EEFFU);
  palette.accent_soft = hex(0x0E2A34FFU);
  palette.success = hex(0x34D399FFU);
  palette.warning = hex(0xFBBF24FFU);
  palette.danger = hex(0xF87171FFU);
  palette.focus_ring = hex(0x4C8DFF66U);
  palette.overlay = hex(0x02061799U);
  palette.shadow = hex(0x00000059U);
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

auto shadow_sm(const Theme& theme) -> Shadow {
  Shadow shadow;
  shadow.color = theme.colors().shadow;
  shadow.blur = 2.0f;
  shadow.offset_y = 1.0f;
  return shadow;
}

auto shadow_md(const Theme& theme) -> Shadow {
  Shadow shadow;
  shadow.color = theme.colors().shadow;
  shadow.blur = 8.0f;
  shadow.offset_y = 2.0f;
  return shadow;
}

auto shadow_lg(const Theme& theme) -> Shadow {
  Shadow shadow;
  shadow.color = theme.colors().shadow;
  shadow.blur = 24.0f;
  shadow.offset_y = 6.0f;
  return shadow;
}

}  // namespace st::ui
