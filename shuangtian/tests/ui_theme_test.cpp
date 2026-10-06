/// 设计令牌的**可验证契约**：对比度、层次、阴影分层。
///
/// 为什么把"好看"写成断言：颜色是唯一无法靠代码审查发现问题的领域——
/// `#94A3B8` 与 `#64748B` 在 diff 里看不出差别，而前者在白底上只有 2.6:1，
/// 11px 的小字直接"看不清"（实测：导航标题与版本号就踩了这个）。
/// 这里用 WCAG 相对亮度把每个**真实使用组合**的下限钉住：
/// 改动调色板时越过下限会立刻红灯，而不是等到有人肉眼发现。
///
/// 口径：正文级文字 4.5:1（WCAG AA）、辅助小字 4.0:1、作为唯一识别手段的控件描边按"肉眼可辨"。

#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "st/math/color.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;

/// 对比度用框架自带的 `math::contrast_ratio`（WCAG 2.1，1:1 ~ 21:1）——
/// 不再另写一份：同一套公式全框架只有一处，测试与运行期自检结果才必然一致。
[[nodiscard]] auto contrast(const Color& foreground, const Color& background) -> double {
  return static_cast<double>(st::math::contrast_ratio(foreground, background));
}

/// 把前景按 alpha 合成到背景上（令牌里的半透明色要先合成再算对比度：
/// 直接拿原始值算会高估——`focus_ring` 就是半透明的）。
[[nodiscard]] auto composite(const Color& foreground, const Color& background) -> Color {
  const double alpha = static_cast<double>(foreground.a) / 255.0;
  const auto mix = [alpha](std::uint8_t top, std::uint8_t bottom) -> std::uint8_t {
    const double value =
        static_cast<double>(top) * alpha + static_cast<double>(bottom) * (1.0 - alpha);
    return static_cast<std::uint8_t>(std::lround(value));
  };
  return Color{mix(foreground.r, background.r), mix(foreground.g, background.g),
               mix(foreground.b, background.b), 255};
}

/// 文字三级在各底色上的**最低**对比度（逐组合断言时只用 ST_CHECK，不用新宏）。
struct TextLadder {
  double text{1.0e9};
  double muted{1.0e9};
  double faint{1.0e9};
};

[[nodiscard]] auto ladder_on(const st::ui::Theme& theme) -> TextLadder {
  const st::ui::Palette& palette = theme.colors();
  TextLadder ladder;
  for (const Color& background : {palette.bg, palette.surface, palette.surface_alt}) {
    ladder.text = std::min(ladder.text, contrast(palette.text, background));
    ladder.muted = std::min(ladder.muted, contrast(palette.text_muted, background));
    ladder.faint = std::min(ladder.faint, contrast(palette.text_faint, background));
  }
  return ladder;
}

}  // namespace

ST_TEST(theme_light_text_contrast_meets_wcag) {
  const TextLadder ladder = ladder_on(st::ui::Theme::light());
  // 正文级 4.5:1；辅助小字（11~12px 的标签/说明）取 4.0:1——
  // 小字对比度不足比大字更难读，不能因为"次要"就放宽到 3:1。
  ST_CHECK(ladder.text >= 4.5);
  ST_CHECK(ladder.muted >= 4.5);
  ST_CHECK(ladder.faint >= 4.0);
}

ST_TEST(theme_dark_text_contrast_meets_wcag) {
  const TextLadder ladder = ladder_on(st::ui::Theme::dark());
  ST_CHECK(ladder.text >= 4.5);
  ST_CHECK(ladder.muted >= 4.5);
  ST_CHECK(ladder.faint >= 4.0);
}

ST_TEST(theme_accent_contrast_meets_wcag) {
  for (const st::ui::Theme& theme : {st::ui::Theme::light(), st::ui::Theme::dark()}) {
    const st::ui::Palette& palette = theme.colors();
    // 语义色常用于"带色文字/图标"，必须能在卡片面上读出来
    ST_CHECK(contrast(palette.primary, palette.surface) >= 4.5);
    ST_CHECK(contrast(palette.danger, palette.surface) >= 4.5);
    ST_CHECK(contrast(palette.success, palette.surface) >= 3.0);
    // 主按钮：`on_primary` 文字在 primary 实底上
    ST_CHECK(contrast(palette.on_primary, palette.primary) >= 4.5);
    // 柔和按钮：主色文字在 primary_soft 底上
    ST_CHECK(contrast(palette.primary, palette.primary_soft) >= 4.5);
  }
}

ST_TEST(theme_borders_are_perceptible) {
  // 输入框描边是"这里可以输入"的重要线索：太淡就等于没有控件。
  // WCAG 1.4.11 的目标是 3:1，但浅色输入框还有自身填充色作为第二线索，
  // 故这里取 1.35 作为"肉眼可辨"的底线（原值 #DDE4EF ≈ 1.28 已偏糊）。
  const st::ui::Theme light = st::ui::Theme::light();
  ST_CHECK(contrast(light.colors().border, light.colors().surface) >= 1.35);
  ST_CHECK(contrast(light.colors().border_strong, light.colors().surface) >= 1.8);

  const st::ui::Theme dark = st::ui::Theme::dark();
  ST_CHECK(contrast(dark.colors().border, dark.colors().surface) >= 1.35);
}

ST_TEST(theme_tonal_ladder_is_ordered) {
  // 文字三级必须**真的分得开**：任意相邻两级至少差 8% 相对对比度，
  // 否则"层次"只是名义上的（改色时最容易把三级挤成一坨）。
  for (const st::ui::Theme& theme : {st::ui::Theme::light(), st::ui::Theme::dark()}) {
    const TextLadder ladder = ladder_on(theme);
    ST_CHECK(ladder.text > ladder.muted);
    ST_CHECK(ladder.muted > ladder.faint);
    ST_CHECK(ladder.muted / ladder.faint >= 1.08);
    ST_CHECK(ladder.text / ladder.muted >= 1.08);
  }
}

ST_TEST(theme_shadow_tiers_are_layered_and_ordered) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow small = st::ui::shadow_sm(theme);
  const st::ui::Shadow medium = st::ui::shadow_md(theme);
  const st::ui::Shadow large = st::ui::shadow_lg(theme);

  for (const st::ui::Shadow* shadow : {&small, &medium, &large}) {
    ST_CHECK(shadow->visible());
    // 两层缺一不可：只有关键层 = 灰边，只有环境层 = 一坨雾
    ST_CHECK(shadow->second_visible());
    // 环境层必须比关键层更"散"，否则两层叠起来只是更脏
    ST_CHECK(shadow->blur2 > shadow->blur);
    ST_CHECK(shadow->offset2_y > shadow->offset_y);
    // 单层不透明度上限：浅底上超过 ~16% 就会显脏
    ST_CHECK(static_cast<int>(shadow->color.a) <= 40);
    ST_CHECK(static_cast<int>(shadow->color2.a) <= 40);
    // 阴影必须是**很暗的低亮度色**且真的带透明度：
    // 允许带偏色（浅色主题的基色是带蓝调的 #0F172A，比纯黑更自然），
    // 但三个通道都必须够暗——实测踩过一个反例：`0x0000008C` 被读成
    // 不透明的纯蓝 `0x00008C`，B=140 直接超限，深色主题的卡片因此一圈蓝光。
    ST_CHECK(shadow->color.r <= 0x40);
    ST_CHECK(shadow->color.g <= 0x40);
    ST_CHECK(shadow->color.b <= 0x40);
    ST_CHECK(st::math::relative_luminance(shadow->color) < 0.06f);
    ST_CHECK(shadow->color.a > 0U && shadow->color.a < 255U);
    ST_CHECK(shadow->color2.a > 0U && shadow->color2.a < 255U);
  }

  // 深色主题同样要过这一关（基色是纯黑，α 必须有意义）
  const st::ui::Theme dark_theme = st::ui::Theme::dark();
  const st::ui::Shadow dark_shadow = st::ui::shadow_md(dark_theme);
  ST_CHECK(dark_shadow.color.r == 0U && dark_shadow.color.g == 0U && dark_shadow.color.b == 0U);
  ST_CHECK(dark_shadow.color.a > 0U && dark_shadow.color.a < 255U);
  ST_CHECK(dark_shadow.color2.a > 0U && dark_shadow.color2.a < 255U);

  // 三档必须**单调递增**（sm < md < lg）：否则"提升层级"在视觉上是反的
  ST_CHECK(small.blur < medium.blur);
  ST_CHECK(medium.blur < large.blur);
  ST_CHECK(small.blur2 < medium.blur2);
  ST_CHECK(medium.blur2 < large.blur2);
  ST_CHECK(static_cast<int>(small.color.a) <= static_cast<int>(medium.color.a));
  ST_CHECK(static_cast<int>(medium.color.a) <= static_cast<int>(large.color.a));
}

ST_TEST(theme_focus_ring_is_visible_on_surfaces) {
  // 焦点环是键盘可达性的唯一线索：合成到各底色后必须真的看得见
  for (const st::ui::Theme& theme : {st::ui::Theme::light(), st::ui::Theme::dark()}) {
    const st::ui::Palette& palette = theme.colors();
    for (const Color& background : {palette.bg, palette.surface, palette.surface_alt}) {
      ST_CHECK(contrast(composite(palette.focus_ring, background), background) >= 1.35);
    }
  }
}

ST_TEST(theme_surface_ladder_is_monotone_and_distinguishable) {
  // 亚克力的"层次"全部靠底色阶梯表达（没有品牌色块、没有粗描边），因此
  // **相邻档必须真的分得开**。实测踩过：亮色 bg→surface 只差 1.063，卡片像"贴"在
  // 背景上而不是浮在上面，而当时的测试只检查了"有值"、没检查"差得出来"。
  const auto luminance = [](const Color& color) {
    return static_cast<double>(st::math::relative_luminance(color));
  };
  const st::ui::Theme light = st::ui::Theme::light();
  const st::ui::Palette& lp = light.colors();
  // 浅色：背景（灰）→ 面板（近白）→ 抬升面（白），阶梯逐级向上
  ST_CHECK(luminance(lp.bg) < luminance(lp.surface));
  ST_CHECK(luminance(lp.bg) < luminance(lp.surface_alt));
  ST_CHECK(luminance(lp.surface_alt) < luminance(lp.surface));
  // 凹槽要**比背景暗到看得见**。这里取 1.08 而不是 1.05：
  // `1.05` 太松了——实测 逆向验证 时发现 `#E0E0E6`（旧值，凹陷感几乎为零）
  // 也有 1.077，照样能过，于是这条断言根本拦不住"把凹槽调回贴平"。
  ST_CHECK(luminance(lp.surface_sunken) < luminance(lp.bg));
  ST_CHECK(contrast(lp.surface, lp.bg) >= 1.08);
  ST_CHECK(contrast(lp.surface, lp.surface_alt) >= 1.05);
  ST_CHECK(contrast(lp.surface_sunken, lp.bg) >= 1.08);

  const st::ui::Theme dark = st::ui::Theme::dark();
  const st::ui::Palette& dp = dark.colors();
  // 深色：背景（近黑）→ 面板 → 抬升面（更亮），同为逐级向上
  ST_CHECK(luminance(dp.bg) < luminance(dp.surface));
  ST_CHECK(luminance(dp.surface) < luminance(dp.surface_alt));
  ST_CHECK(luminance(dp.surface_alt) <= luminance(dp.surface_raised));
  // **凹槽在两个模式下都更暗**：语义是"凹进去"，不随亮暗反转。
  // 旧深色主题的 `surface_sunken` 比 `surface` 更亮（1.040），语义是反的。
  ST_CHECK(luminance(dp.surface_sunken) < luminance(dp.surface));
  ST_CHECK(contrast(dp.surface, dp.bg) >= 1.08);
  ST_CHECK(contrast(dp.surface_alt, dp.surface) >= 1.08);
  ST_CHECK(contrast(dp.surface_sunken, dp.surface) >= 1.10);
}

ST_TEST(theme_elevated_surface_is_never_darker_than_panel) {
  // 弹层压在面板上：深色下阴影几乎不可见，若抬升面不比面板亮，弹层会"陷"进去。
  // 浅色下两者同为白系（抬升靠阴影表达），所以断言是"不更暗"而不是"更亮"。
  for (const st::ui::Theme& theme : {st::ui::Theme::light(), st::ui::Theme::dark()}) {
    const st::ui::Palette& palette = theme.colors();
    const double panel = static_cast<double>(st::math::relative_luminance(palette.surface));
    const double raised = static_cast<double>(st::math::relative_luminance(palette.surface_raised));
    ST_CHECK(raised >= panel);
    if (theme.mode() == st::ui::ThemeMode::Dark) ST_CHECK(raised > panel);
  }
}

ST_TEST(theme_border_subtle_is_lighter_than_border_but_visible) {
  // 分隔线的第三档：必须在 `bg` 上看得见（它就是列表行/区段的分隔依据），
  // 又必须比 `border` 轻（否则"同组内"与"组之间"的区分就没了）。
  for (const st::ui::Theme& theme : {st::ui::Theme::light(), st::ui::Theme::dark()}) {
    const st::ui::Palette& palette = theme.colors();
    for (const Color& background : {palette.surface, palette.surface_alt}) {
      ST_CHECK(contrast(palette.border_subtle, background) >= 1.10);
    }
    const double subtle =
        static_cast<double>(st::math::relative_luminance(palette.border_subtle));
    const double normal = static_cast<double>(st::math::relative_luminance(palette.border));
    if (theme.mode() == st::ui::ThemeMode::Light) {
      ST_CHECK(subtle > normal);   // 浅色：更淡 = 更亮
    } else {
      ST_CHECK(subtle < normal);   // 深色：更淡 = 更暗
    }
  }
}

ST_TEST(theme_glass_edge_is_visible_only_where_it_helps) {
  // 亚克力的"玻璃边缘"（顶部内高光）只用在一个方向：**深底上提亮**。
  // 浅底已经接近纯白，叠白光不会可见——歌白浅色档用的是一条**暗色发丝线**，
  // 而那正是 `border` 已经承担的职责，不该重复一层。
  const st::ui::Theme light_theme = st::ui::Theme::light();
  ST_CHECK(light_theme.colors().highlight.a == 0U);

  const st::ui::Theme dark_theme = st::ui::Theme::dark();
  const Color& edge = dark_theme.colors().highlight;
  ST_CHECK(edge.a > 0U);
  // 深色的边缘光必须是**弱白光**：比面板亮，但对比度很低（它是"受光"不是"描边"）
  ST_CHECK(st::math::relative_luminance(edge) >
           st::math::relative_luminance(dark_theme.colors().surface));
  ST_CHECK(contrast(edge, dark_theme.colors().surface_raised) < 2.0);
  ST_CHECK(contrast(edge, dark_theme.colors().surface_raised) > 1.05);
}

ST_TEST(theme_shadow_tuning_knobs_scale_without_touching_defaults) {
  // 强度/扩散旋钮是给自定义主题用的；**默认值下结果必须与旧值逐位相同**，
  // 否则"加了两个旋钮"就变成了"偷偷改了所有主题的阴影"。
  const st::ui::Theme base = st::ui::Theme::light();
  const st::ui::Shadow md = st::ui::shadow_md(base);
  st::ui::Theme tweaked = base;
  tweaked.metrics().shadow_strength = 2.0f;
  const st::ui::Shadow strong = st::ui::shadow_md(tweaked);
  ST_CHECK(strong.color.a > md.color.a);
  ST_CHECK(strong.blur == md.blur);   // 强度只改浓淡，不改扩散

  st::ui::Theme spread = base;
  spread.metrics().shadow_spread = 2.0f;
  const st::ui::Shadow wide = st::ui::shadow_md(spread);
  ST_CHECK(wide.blur > md.blur);
  ST_CHECK(wide.color.a == md.color.a);  // 扩散只改模糊，不改浓淡

  // 非正值被忽略（宁可保留默认，也不要静默把阴影变成 0）
  st::ui::Theme broken = base;
  broken.metrics().shadow_strength = -1.0f;
  broken.metrics().shadow_spread = 0.0f;
  const st::ui::Shadow fallback = st::ui::shadow_md(broken);
  ST_CHECK(fallback.color.a == md.color.a);
  ST_CHECK(fallback.blur == md.blur);
}

ST_TEST(theme_name_survives_construction) {
  // 主题名是控制通道 `theme` 的上报字段（"我现在用的是哪份主题"）。
  // `dark()` 曾经漏设名字，于是切到暗色后仍上报 "light"——用户以为没切过去。
  ST_CHECK(st::ui::Theme::light().name() == "light");
  ST_CHECK(st::ui::Theme::dark().name() == "dark");
}

ST_TEST(theme_metrics_scale_is_consistent) {
  // 尺度令牌：间距与圆角都要**成阶梯**，否则布局会出现"差 1px"的脏边
  const st::ui::Metrics metrics = st::ui::Theme::light().metrics();
  ST_CHECK(metrics.space_xs < metrics.space_sm);
  ST_CHECK(metrics.space_sm < metrics.space_md);
  ST_CHECK(metrics.space_md < metrics.space_lg);
  ST_CHECK(metrics.space_lg < metrics.space_xl);
  ST_CHECK(metrics.radius_sm < metrics.radius_md);
  ST_CHECK(metrics.radius_md < metrics.radius_lg);
  ST_CHECK(metrics.radius_lg < metrics.radius_xl);
  ST_CHECK(metrics.font_xs < metrics.font_sm);
  ST_CHECK(metrics.font_sm < metrics.font_base);
  ST_CHECK(metrics.font_base < metrics.font_lg);
  ST_CHECK(metrics.font_lg < metrics.font_xl);
  // 控件高度随字号走：小号控件必须比中号矮，否则视觉层级混乱
  ST_CHECK(metrics.control_height_sm < metrics.control_height);
  ST_CHECK(metrics.control_height < metrics.control_height_lg);
  // 行高必须大于 1（小于 1 会裁字），且标题行高比正文紧
  ST_CHECK(metrics.line_height_body > 1.0f);
  ST_CHECK(metrics.line_height_heading > 1.0f);
  ST_CHECK(metrics.line_height_heading < metrics.line_height_body);
}
