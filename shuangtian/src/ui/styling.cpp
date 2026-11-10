/// 样式系统实现（`st/ui/styling.hpp`）：Tokens 解析 + apply_appearance 落位。

#include "st/ui/styling.hpp"

#include "st/ui/element.hpp"

namespace st::ui {
namespace {

/// 色调 → 色（Palette 版：Tokens 快照后不再持有 Theme，这里按同一映射表解析）。
[[nodiscard]] auto palette_tone(const Palette& colors, Tone tone) -> math::Color {
  switch (tone) {
    case Tone::Default: return colors.text;
    case Tone::Muted: return colors.text_muted;
    case Tone::Faint: return colors.text_faint;
    case Tone::Primary: return colors.primary;
    case Tone::Accent: return colors.accent;
    case Tone::Success: return colors.success;
    case Tone::Warning: return colors.warning;
    case Tone::Danger: return colors.danger;
    case Tone::OnPrimary: return colors.on_primary;
  }
  return colors.text;
}

/// 主按钮的“主色 → 强调色”双色渐变；`accent` 与 `primary` 同色（或缺alpha）时返回 `nullopt`。
///
/// 为何要判“不同才画”：缺省主题里两者相等，此时铺一个两端同色的渐变
/// 只是白白多一遍渐变光栅化（零视觉收益、纯耗损）。拉成函数是因为这条规则
/// 现在**只有一个出处**，后续换角度/加色标只需要改这里。
[[nodiscard]] auto gradient_primary_accent(const Palette& colors)
    -> std::optional<raster::Gradient> {
  if (colors.accent.a == 0U) return std::nullopt;
  if (colors.accent.r == colors.primary.r && colors.accent.g == colors.primary.g &&
      colors.accent.b == colors.primary.b) {
    return std::nullopt;
  }
  // 几何是 0~1 归一化（由 `paint_box` 映射到实际 box）——见 `VariantColors` 字段注释。
  return raster::Gradient::linear(math::Point{0.0f, 0.0f}, math::Point{1.0f, 1.0f},
                                  {{0.0f, colors.primary}, {1.0f, colors.accent}});
}

}  // namespace

// —— Tokens ——

Tokens::Tokens(const Theme& theme)
    : palette_(theme.colors()), metrics_(theme.metrics()) {
  // 阴影三档快照（shadow_sm/md/lg 是 Theme 的既有 token 组合函数）。
  shadow_sm_ = ::st::ui::shadow_sm(theme);
  shadow_md_ = ::st::ui::shadow_md(theme);
  shadow_lg_ = ::st::ui::shadow_lg(theme);
}

auto Tokens::variant_colors(Variant variant) const -> VariantColors {
  const Palette& colors = palette_;
  switch (variant) {
    case Variant::Plain:
      return {.background = math::Color{0, 0, 0, 0},
              .border = math::Color{0, 0, 0, 0},
              .text = colors.text,
              .background_hover = colors.surface_hover,
              .background_pressed = colors.surface_pressed};
    case Variant::Neutral:
      return {.background = colors.surface,
              .border = colors.border,
              .text = colors.text,
              .background_hover = colors.surface_hover,
              .background_pressed = colors.surface_pressed};
    case Variant::Primary:
      // 实底主色：hover/pressed 用主色自身的明暗档（不是中性表面——
      // 主色按钮的交互态必须仍「是主色」，变灰会读成「禁用」）。
      //
      // **双色渐变**：仅当主题把 `accent` 定成与 `primary` 不同的颜色时才有
      //（缺省两者相等 → 不画渐变，零开销回退到纯色）。歌白用 `--user-grad`
      // 渐变主色正是这个意图；而给所有卡片都上渐变会让界面失去层次
      //（实测：面板全渐变后，内容区与背景分不开）。
      //
      // 为何做在词表里而不是按钮里（2026-11-10）：这是**品牌语义**，
      // 与“主色用哪个蓝”同层——所有用主色的地方必须一致。
      return {.background = colors.primary,
              .border = colors.primary,
              .text = colors.on_primary,
              .background_hover = colors.primary_hover,
              .background_pressed = colors.primary_active,
              .background_gradient = gradient_primary_accent(colors)};
    case Variant::Accent:
      // 描边式强调：底保持中性（实底 accent 在两种主题下都太吵）。
      return {.background = colors.surface,
              .border = colors.accent,
              .text = palette_tone(palette_, Tone::Accent),
              .background_hover = colors.surface_hover,
              .background_pressed = colors.surface_pressed};
    case Variant::Success:
      return {.background = colors.success,
              .border = colors.success,
              .text = colors.on_primary,
              .background_hover = colors.success,
              .background_pressed = colors.success};
    case Variant::Warning:
      return {.background = colors.warning,
              .border = colors.warning,
              .text = colors.on_primary,
              .background_hover = colors.warning,
              .background_pressed = colors.warning};
    case Variant::Danger:
      return {.background = colors.danger,
              .border = colors.danger,
              .text = colors.on_primary,
              .background_hover = colors.danger,
              .background_pressed = colors.danger};
    case Variant::Ghost:
      // 幽灵：常态全透明（描边也透明——有边会读成 Neutral），
      // hover 才浮出中性表面（工具栏按钮的「安静默认」）。
      return {.background = math::Color{0, 0, 0, 0},
              .border = math::Color{0, 0, 0, 0},
              .text = colors.text_muted,
              .background_hover = colors.surface_hover,
              .background_pressed = colors.surface_pressed};
    case Variant::Soft:
      // 柔和：主色**淡底** + 主色文字。工具栏的选中态用它（实底会与主操作抢眼）。
      return {.background = colors.primary_soft,
              .border = math::Color{0, 0, 0, 0},
              .text = colors.primary,
              .background_hover = colors.primary_soft,
              .background_pressed = colors.primary_soft};
  }
  return variant_colors(Variant::Neutral);
}

auto Tokens::elevation_surface(Elevation level) const -> math::Color {
  const Palette& colors = palette_;
  switch (level) {
    case Elevation::Flat: return colors.surface;
    case Elevation::Raised: return colors.surface;
    case Elevation::Overlay: return colors.surface_raised;
    case Elevation::Modal: return colors.surface_raised;
  }
  return colors.surface;
}

auto Tokens::elevation_shadow(Elevation level) const -> Shadow {
  switch (level) {
    case Elevation::Flat: return Shadow{};
    case Elevation::Raised: return shadow_sm_;
    case Elevation::Overlay: return shadow_md_;
    case Elevation::Modal: return shadow_lg_;
  }
  return Shadow{};
}

auto Tokens::elevation_highlight(Elevation level) const -> math::Color {
  // ≥1 才有「玻璃上缘」：贴底元素画高光会在同面内容之间出现亮线（脏）。
  return level >= Elevation::Raised ? palette_.highlight : math::Color{0, 0, 0, 0};
}

auto Tokens::radius_value(RadiusMode mode) const -> float {
  const Metrics& metrics = metrics_;
  switch (mode) {
    case RadiusMode::None: return 0.0f;
    case RadiusMode::SM: return metrics.radius_sm;
    case RadiusMode::MD: return metrics.radius_md;
    case RadiusMode::LG: return metrics.radius_lg;
    case RadiusMode::XL: return metrics.radius_xl;
    case RadiusMode::Pill: return metrics.radius_pill;
  }
  return metrics.radius_md;
}

auto Tokens::control_height(ControlSize size) const -> float {
  const Metrics& metrics = metrics_;
  switch (size) {
    case ControlSize::Small: return metrics.control_height_sm;
    case ControlSize::Medium: return metrics.control_height;
    case ControlSize::Large: return metrics.control_height_lg;
  }
  return metrics.control_height;
}

auto Tokens::font_size(ControlSize size) const -> float {
  const Metrics& metrics = metrics_;
  switch (size) {
    case ControlSize::Small: return metrics.font_sm;
    case ControlSize::Medium: return metrics.font_base;
    case ControlSize::Large: return metrics.font_lg;
  }
  return metrics.font_base;
}

auto Tokens::icon_size(ControlSize size) const -> float {
  const Metrics& metrics = metrics_;
  switch (size) {
    case ControlSize::Small: return metrics.icon_size_sm;
    case ControlSize::Medium: return metrics.icon_size;
    case ControlSize::Large: return metrics.icon_size_lg;
  }
  return metrics.icon_size;
}

auto Tokens::density_factor(Density density) const -> float {
  switch (density) {
    case Density::Compact: return 0.85f;
    case Density::Regular: return 1.0f;
    case Density::Comfortable: return 1.15f;
  }
  return 1.0f;
}

// —— apply_appearance ——

auto apply_appearance(Element& element, const Tokens& tokens, const Appearance& appearance)
    -> bool {
  bool changed = false;
  Style& style = element.style();

  if (appearance.variant.has_value()) {
    const Tokens::VariantColors colors = tokens.variant_colors(*appearance.variant);
    // ⚠ **必须走成对的两个入口**（不是直接写 `style.background`）：
    // `paint_box` 的优先级是「渐变 > 纯色」，只写纯色会把组件先铺上的渐变
    // （Button 的 Primary 双色底）留成残影——详见 `Style::set_solid_background`。
    //
    // 反过来的情形（本行）同样必须成对：`VariantColors` 带渐变时
    // **必须铺渐变**，否则“声明回放”会把组件刚画对的渐变清回纯色——
    // 这正是本期词表统一后暴露的一处回归（实测：Primary 按钮变纯色，
    // 因为本函数排在 `refresh_theme` 第二步）。
    if (colors.background_gradient.has_value()) {
      style.set_gradient_background(*colors.background_gradient, colors.background);
    } else {
      style.set_solid_background(colors.background);
    }
    style.border_color = colors.border;
    style.color = colors.text;
    changed = true;
  }
  if (appearance.elevation.has_value()) {
    const Elevation level = *appearance.elevation;
    style.set_solid_background(tokens.elevation_surface(level));
    style.shadow = tokens.elevation_shadow(level);
    style.top_highlight = tokens.elevation_highlight(level);
    changed = true;
  }
  if (appearance.radius.has_value()) {
    const float value = tokens.radius_value(*appearance.radius);
    if (style.radius != value) {
      style.radius = value;
      changed = true;
    }
  }
  if (appearance.size.has_value()) {
    const float height = tokens.control_height(*appearance.size);
    if (!style.has_explicit_height() || style.height != height) {
      style.height = height;
      changed = true;
    }
    const float font = tokens.font_size(*appearance.size);
    if (style.font_size != font) {
      style.font_size = font;
      changed = true;
    }
  }
  if (appearance.density.has_value()) {
    const float factor = tokens.density_factor(*appearance.density);
    const Metrics& metrics = tokens.metrics();
    const float pad = metrics.space_sm * factor;
    style.padding = math::Insets{pad, pad, pad, pad};
    changed = true;
  }
  if (appearance.text_tone.has_value()) {
    element.set_text_tone(*appearance.text_tone);
    changed = true;
  }
  if (changed) element.mark_dirty();
  return changed;
}

}  // namespace st::ui
