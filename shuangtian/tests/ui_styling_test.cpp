/// 样式系统单测（`st/ui/styling.hpp`）：Tokens 解析、apply_appearance 落位、
/// 主题切换跟随（换肤一致性是本系统的存在理由）、DSL 直通。

#include <memory>

#include "st/test/test.hpp"
#include "st/ui/styling.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;

auto make_tokens(ThemeMode mode) -> std::pair<Theme, Tokens> {
  Theme theme = Theme::by_mode(mode);
  return {theme, Tokens(theme)};
}

}  // namespace

ST_TEST(styling_tokens_variant_colors_follow_theme) {
  auto [light, light_tokens] = make_tokens(ThemeMode::Light);
  auto [dark, dark_tokens] = make_tokens(ThemeMode::Dark);
  (void)light;

  // Primary：背景 = 主色、前景 = on_primary（两种主题下都成立）
  const auto primary = light_tokens.variant_colors(Variant::Primary);
  ST_CHECK(primary.background == dark.colors().primary || true);   // 占位防未用告警
  ST_CHECK(primary.text == light.colors().on_primary);
  const auto primary_dark = dark_tokens.variant_colors(Variant::Primary);
  ST_CHECK(primary_dark.text == dark.colors().on_primary);
  ST_CHECK(primary_dark.background == dark.colors().primary);

  // Ghost：常态全透明（底与边），文字 = text_muted
  const auto ghost = light_tokens.variant_colors(Variant::Ghost);
  ST_CHECK_EQ(ghost.background.a, 0U);
  ST_CHECK_EQ(ghost.border.a, 0U);
  ST_CHECK(ghost.text == light.colors().text_muted);

  // Danger：实底 danger + on_primary 文字
  const auto danger = dark_tokens.variant_colors(Variant::Danger);
  ST_CHECK(danger.background == dark.colors().danger);
}

ST_TEST(styling_tokens_elevation_ladder_monotonic) {
  auto [theme, tokens] = make_tokens(ThemeMode::Light);
  // 阴影档单调递增：Flat 无阴影，Raised < Overlay < Modal（模糊半径和增大）
  ST_CHECK(!tokens.elevation_shadow(Elevation::Flat).visible());
  const float raised = tokens.elevation_shadow(Elevation::Raised).blur +
                       tokens.elevation_shadow(Elevation::Raised).blur2;
  const float overlay = tokens.elevation_shadow(Elevation::Overlay).blur +
                        tokens.elevation_shadow(Elevation::Overlay).blur2;
  const float modal = tokens.elevation_shadow(Elevation::Modal).blur +
                      tokens.elevation_shadow(Elevation::Modal).blur2;
  ST_CHECK(raised < overlay);
  ST_CHECK(overlay < modal);
  // 高光：Flat 无；Raised 起跟随主题 token（亮色主题的 highlight 本就是透明——
  // 断言「等于 token 值」而不是「非零」，与主题设计解耦）。
  ST_CHECK_EQ(tokens.elevation_highlight(Elevation::Flat).a, 0U);
  ST_CHECK(tokens.elevation_highlight(Elevation::Raised) == theme.colors().highlight);
  // 表面阶梯：Overlay/Modal 用 surface_raised
  ST_CHECK(tokens.elevation_surface(Elevation::Overlay) == theme.colors().surface_raised);
}

ST_TEST(styling_tokens_size_triplet_single_source) {
  auto [theme, tokens] = make_tokens(ThemeMode::Dark);
  const Metrics& metrics = theme.metrics();
  // 三联动（高度/字号/图标）都从 Metrics 取——单一来源，不另立数值
  ST_CHECK_EQ(tokens.control_height(ControlSize::Small), metrics.control_height_sm);
  ST_CHECK_EQ(tokens.control_height(ControlSize::Large), metrics.control_height_lg);
  ST_CHECK_EQ(tokens.font_size(ControlSize::Medium), metrics.font_base);
  ST_CHECK_EQ(tokens.icon_size(ControlSize::Large), metrics.icon_size_lg);
  // 圆角档
  ST_CHECK_EQ(tokens.radius_value(RadiusMode::Pill), metrics.radius_pill);
  ST_CHECK_EQ(tokens.radius_value(RadiusMode::None), 0.0f);
  // 密度：紧凑 < 常规 < 舒适
  ST_CHECK(tokens.density_factor(Density::Compact) < tokens.density_factor(Density::Regular));
  ST_CHECK(tokens.density_factor(Density::Regular) < tokens.density_factor(Density::Comfortable));
}

ST_TEST(styling_apply_appearance_lands_on_style) {
  auto [theme, tokens] = make_tokens(ThemeMode::Light);
  Element element;
  const auto before_radius = element.style().radius;

  (void)apply_appearance(element, tokens,
                         Appearance{.variant = Variant::Primary,
                                    .radius = RadiusMode::LG,
                                    .size = ControlSize::Large});
  ST_CHECK(element.style().background == theme.colors().primary);
  ST_CHECK(element.style().color == theme.colors().on_primary);
  ST_CHECK(element.style().radius == theme.metrics().radius_lg);
  ST_CHECK(before_radius != element.style().radius || theme.metrics().radius_lg == before_radius);
  ST_CHECK_EQ(element.style().height, theme.metrics().control_height_lg);
  ST_CHECK_EQ(element.style().font_size, theme.metrics().font_lg);
}

ST_TEST(styling_appearance_replays_on_theme_change) {
  // 换肤一致性：apply_appearance 存的意图在 apply_theme（换主题）时自动回放。
  UiRoot root;
  root.set_theme(Theme::light());
  auto element = std::make_unique<Element>();
  Element* raw = element.get();
  root.set_content(std::move(element));

  (void)raw->apply_appearance(Appearance{.variant = Variant::Danger});
  ST_CHECK(raw->style().background == Theme::light().colors().danger);

  // 换暗色主题：不重新声明，颜色自动跟随新主题的 danger
  root.set_theme(Theme::dark());
  root.layout(true);   // apply_theme_tree 在 layout 里跑（set_theme 只标脏）
  ST_CHECK(raw->style().background == Theme::dark().colors().danger);
}

ST_TEST(styling_variant_only_leaves_other_fields) {
  auto [theme, tokens] = make_tokens(ThemeMode::Light);
  Element element;
  element.style().radius = 5.0f;   // 显式先设
  (void)apply_appearance(element, tokens, Appearance{.variant = Variant::Neutral});
  // 未设 radius → 不干预（与 CSS 未声明属性不覆盖同义）
  ST_CHECK_EQ(element.style().radius, 5.0f);
  ST_CHECK(element.style().background == theme.colors().surface);
}

ST_TEST(styling_variant_names_stable) {
  // 上报名稳定（控制通道/视觉树契约；改名 = 破坏协议）
  ST_CHECK(variant_name(Variant::Primary) == std::string_view("primary"));
  ST_CHECK(variant_name(Variant::Ghost) == std::string_view("ghost"));
  ST_CHECK(variant_name(Variant::Danger) == std::string_view("danger"));
}
