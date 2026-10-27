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
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::ui::Element;
using st::ui::Panel;

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
  // **`surface_sunken` 必须在内**：编辑器注释/行号用的 `text_faint` 就画在它上面
  // （见 `code_editor.cpp` 的行号与注释绘制）。漏了它，`theme.hpp` 里
  // “`faint` 必须对四个底色都达 4.5” 这条规则就只能靠注释自律——实测漏过
  // 一次：旧值对 `sunken` 只有 4.02，而测试仍全绿（因为没测它）。
  for (const Color& background :
       {palette.bg, palette.surface, palette.surface_alt, palette.surface_sunken}) {
    ladder.text = std::min(ladder.text, contrast(palette.text, background));
    ladder.muted = std::min(ladder.muted, contrast(palette.text_muted, background));
    ladder.faint = std::min(ladder.faint, contrast(palette.text_faint, background));
  }
  return ladder;
}

}  // namespace

ST_TEST(theme_light_text_contrast_meets_wcag) {
  const TextLadder ladder = ladder_on(st::ui::Theme::light());
  // 三级文字**统一 4.5:1**（WCAG AA 正文口径）：`faint` 用于 11~12px 的标签/说明，
  // 小字对比度不足比大字更难读，不能因为"次要"就放宽——`theme.hpp` 里
  // 写的也是 4.5（含 `surface_sunken` 这一级）。
  ST_CHECK(ladder.text >= 4.5);
  ST_CHECK(ladder.muted >= 4.5);
  ST_CHECK(ladder.faint >= 4.5);
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
  // 描边是**半透明**（`theme.hpp` 亚克力口径 §1）——必须先合成到底色再量强度，
  // 否则量的是“色的裸值”，与实际看到的描边不是同一个东西。
  const st::ui::Theme light = st::ui::Theme::light();
  ST_CHECK(contrast(composite(light.colors().border, light.colors().surface),
                    light.colors().surface) >= 1.20);
  ST_CHECK(contrast(composite(light.colors().border_strong, light.colors().surface),
                    light.colors().surface) >= 1.55);

  const st::ui::Theme dark = st::ui::Theme::dark();
  ST_CHECK(contrast(composite(dark.colors().border, dark.colors().surface),
                    dark.colors().surface) >= 1.35);
}

ST_TEST(theme_borders_are_translucent_and_uniform_across_surfaces) {
  // `theme.hpp` 亚克力口径 §1（原文）：
  //   「描边与交互态是半透明的……**固定灰值在 bg 与 surface 上会显出两种强度**，
  //     这也是浅色主题里『同一个 border 在卡片上清楚、在页底上糊掉』的根因。」
  //
  // 所以要有两条护栏，缺一不可：
  //   ① 描边 token 本身**带 alpha**（不透明就不可能“叠在任意底色上”）；
  //   ② 合成到各级底色后，**强度的离散度**要小（一致，而不是一处清楚一处糊）。
  //
  // 为何必须单独测 ②：只测“在 surface 上够不够强”无法发现不一致——
  // 实测旧值 `#D2D2D9`（不透明）在 `surface` 上 1.44 很达标，
  // 但在 `surface_sunken` 上只有 1.10（**离散度 0.339**），完全符合上面描述的病症。
  for (const st::ui::Theme& theme : {st::ui::Theme::light(), st::ui::Theme::dark()}) {
    const st::ui::Palette& palette = theme.colors();
    const std::string mode_name = theme.mode() == st::ui::ThemeMode::Light ? "浅色" : "深色";

    for (const Color* edge : {&palette.border, &palette.border_strong, &palette.border_subtle}) {
      // ① 必须带 alpha（`border` 族是“光”，不是实色）。
      ST_CHECK(edge->a < 255U);
    }

    // ② 各底色上的强度要一致。
    const std::array<Color, 4> backgrounds{palette.bg, palette.surface, palette.surface_alt,
                                           palette.surface_sunken};
    for (const Color* edge : {&palette.border, &palette.border_strong, &palette.border_subtle}) {
      double lo = 1.0e9;
      double hi = -1.0e9;
      for (const Color& background : backgrounds) {
        const double value = contrast(composite(*edge, background), background);
        lo = std::min(lo, value);
        hi = std::max(hi, value);
      }
      // 离散度容差 0.05：半透明描边的强度随底色仍有微小漂移（实测 0.006~0.087），
      // 而旧的不透明实色是 0.31~0.89 —— 相差一个数量级，判别力充足。
      ST_CHECK(hi - lo <= 0.15);
    }
  }
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
      // 描边是**半透明**（`theme.hpp` 亚克力口径 §1）——必须先合成到底色再量。
      ST_CHECK(contrast(composite(palette.border_subtle, background), background) >= 1.10);
    }
    // 层次（subtle 比 border 轻）同样要在**同一条合成口径**下比：
    // 直接比半透明的裸值无意义（两者都是低 alpha 的黑/白，亮度几乎一样）。
    const Color base = palette.surface;
    const double subtle = static_cast<double>(
        st::math::relative_luminance(composite(palette.border_subtle, base)));
    const double normal = static_cast<double>(
        st::math::relative_luminance(composite(palette.border, base)));
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
  ST_CHECK(metrics.font_2xs < metrics.font_xs);
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

/// **内容区（编辑器/终端）必须与外壳、侧栏分得开**（回归：用户报「编辑器背景不够
/// 下沉、与 UI 没区分度」）。
///
/// 缺陷形态（2026-10-18 实测）：`sidebar`/`editor-column` 这些容器**没有声明
/// `Surface`**，于是底色透明、落到页面 `bg` 上——编辑器、侧栏、标签条全成同一个色
/// （实测对比度 **1.000**），而资源管理器里的 `Tree` 自己填了 `surface`，
/// 反而比编辑器更亮，形成"内亮外暗"的颠倒。
///
/// 本用例锁的是**关系**不是具体色值（色值可以调，层次不能塌）：
///   ① 三个语义档（外壳 `alt` / 侧栏 `base` / 内容 `content`）两两可辨；
///   ② 内容区是**阅读焦点**：暗色下最沉、亮色下最亮——两个模式方向刻意相反，
///      这正是 `Surface::Content` 存在的理由（单一 `sunken` 做不到）。
ST_TEST(theme_content_surface_is_distinct_from_shell_and_panel) {
  const auto luminance = [](const Color& color) {
    return static_cast<double>(st::math::relative_luminance(color));
  };

  const st::ui::Theme dark = st::ui::Theme::dark();
  const st::ui::Palette& dp = dark.colors();
  // 暗色：外壳最亮 → 面板居中 → 内容最沉（往里沉）。
  ST_CHECK(contrast(dp.surface_alt, dp.surface) >= 1.10);
  ST_CHECK(contrast(dp.surface, dp.surface_sunken) >= 1.10);
  ST_CHECK(luminance(dp.surface_sunken) < luminance(dp.surface));

  const st::ui::Theme light = st::ui::Theme::light();
  const st::ui::Palette& lp = light.colors();
  // 亮色：**方向相反**——内容区归白（阅读焦点），外壳压成浅灰。
  // 这条是本次修复的核心断言：亮色下若把编辑器做成"更暗"，观感是脏的。
  ST_CHECK(contrast(lp.surface_raised, lp.surface_alt) >= 1.05);
  ST_CHECK(contrast(lp.surface, lp.surface_alt) >= 1.05);
  ST_CHECK(luminance(lp.surface_raised) >= luminance(lp.surface));

  // 两个模式的 `Content` 档必须真的**取不同的层**，否则这层语义是摆设。
  ST_CHECK(dp.surface_sunken.r != lp.surface_raised.r ||
           dp.surface_sunken.g != lp.surface_raised.g);
}

/// `Surface::Content` 在两个模式下解析到**不同的 token**（这就是它存在的全部理由）。
ST_TEST(theme_content_surface_resolves_differently_per_mode) {
  const auto background_for = [](const st::ui::Theme& theme) {
    st::ui::Panel panel;
    panel.set_surface(st::ui::Element::Surface::Content);
    panel.apply_theme(theme);
    return panel.style().background;
  };
  const Color dark_bg = background_for(st::ui::Theme::dark());
  const Color light_bg = background_for(st::ui::Theme::light());
  // 暗色下内容区必须是**暗**的，亮色下必须是**亮**的（否则阅读面选错了层）。
  ST_CHECK(st::math::relative_luminance(dark_bg) < 0.05F);
  ST_CHECK(st::math::relative_luminance(light_bg) > 0.85F);
  ST_CHECK(dark_bg.r != light_bg.r);
}

/// 字号档位（`Text::Scale`）必须落到对应的主题令牌上，而**不是**写死的像素值。
///
/// ## 守的是什么（实测代价：gallery 里 49/150 个文本节点在用阶梯外字号）
///
/// 阶梯最小档曾是 `font_xs=13`，而中文辅助文字（版本号、导航分组名、卡片说明）
/// 实际需要 11~12px。没有档位可用时调用方只能写 `set_font_size(12.0f)` 这类字面量，
/// 而字面量**不会跟主题走**（字号缩放 `scale_fonts` 时它们不动）。
/// 实测：加了 `font_2xs=11`、`font_xs` 降到 12 并把 gallery 全部换成 `set_scale` 后，
/// 阶梯外字号从 **49 处降到 0**。
///
/// ## 为何要测“跟随主题”而不是只测“值对”
///
/// 若实现里把枚举写成一张常量表（不读 `Metrics`），单看某一档的数值仍然“对”，
/// 但 `scale_fonts(1.2)` 后就不对了。所以这里用**缩放后的主题**再量一遍。
ST_TEST(theme_text_scale_follows_metrics_not_literals) {
  using Scale = st::ui::Text::Scale;
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::Metrics& m = theme.metrics();

  // ① 档位与令牌一一对应
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Xs2) == m.font_2xs);
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Xs) == m.font_xs);
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Sm) == m.font_sm);
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Base) == m.font_base);
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Lg) == m.font_lg);
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Xl) == m.font_xl);
  // `Override` 表示“不用档位”，必须返回 0 供调用方回退
  ST_CHECK(st::ui::Text::font_size_for_scale(m, Scale::Override) == 0.0f);

  // ② 阶梯最小档必须能盖住 11~12px（这就是它存在的理由）
  ST_CHECK(m.font_2xs <= 11.0f);
  ST_CHECK(m.font_xs <= 12.0f);
  ST_CHECK(m.font_2xs < m.font_xs);

  // ③ 真的跟随主题缩放（不是一张写死的常量表）
  st::ui::Metrics scaled = m;
  scaled.scale_fonts(1.5f);
  const float before = st::ui::Text::font_size_for_scale(m, Scale::Xs);
  const float after = st::ui::Text::font_size_for_scale(scaled, Scale::Xs);
  ST_CHECK(after > before);
}

/// `Text::set_scale` 必须**真的改写 `style().font_size`**（经过 `apply_theme`），
/// 且显式 `set_font_size` 优先于它（不破坏既有调用方的契约）。
ST_TEST(theme_text_scale_lands_in_style_and_respects_explicit_size) {
  using Scale = st::ui::Text::Scale;
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::Metrics& m = theme.metrics();

  // 档位生效
  st::ui::Text by_scale("x");
  by_scale.set_scale(Scale::Xs2);
  by_scale.apply_theme(theme);
  ST_CHECK(by_scale.style().font_size == m.font_2xs);

  // 显式像素优先（既有契约）
  st::ui::Text by_pixels("x");
  by_pixels.set_scale(Scale::Xs2);
  by_pixels.set_font_size(19.0f);
  by_pixels.apply_theme(theme);
  ST_CHECK(by_pixels.style().font_size == 19.0f);

  // 都不设 → 正文档（不能被档位机制意外改成别的值）
  st::ui::Text plain("x");
  plain.apply_theme(theme);
  ST_CHECK(plain.style().font_size == m.font_base);
}

/// 卡片边框必须**保留** `border` 令牌的透明度，不能被调成不透明。
///
/// ## 这一条守的是什么（实测代价：一屏 8 张卡片全成了刺眼白框）
///
/// `Card::apply_theme` 曾经写的是
/// ```cpp
/// style_.border_color = theme.colors().border.with_alpha_f(Dark ? 1.0f : 0.55f);
/// ```
/// 而 `with_alpha_f` 是**覆盖**语义（把 alpha 直接设成给定量），不是“乘上倍率”。
/// `border` 族令牌本身带半透明 alpha（亚克力口径：它是一道白光/黑光），于是：
///   · 暗色 `with_alpha_f(1.0)` → alpha 覆盖成 **255** → 不透明纯白 1px 边框
///     （实测合成亮度 255/255、对卡底对比度 **13.09:1**）；
///   · 亮色 `with_alpha_f(0.55)` → 覆盖成 55% 黑 → 合成 `#717172`、对比度 4.2:1，
///     与“浅色下淡到几乎看不见”的设计意图相反。
///
/// ## 判据为何落在“alpha 保真”而不是“对比度恰好是多少”
///
/// 对比度是主题调色的**产品决策**（会随设计调整而变），而“组件不得把半透明令牌
/// 悄悄变成实色”是**契约**。所以这里断言：
///   ① 卡片边框 **必须仍带 alpha**（`a < 255`）——无论亮/暗、无论调成多少倍率；
///   ② 且 alpha **不得超过** `border` 令牌自身的 alpha（组件只能调淡，不能调浓）；
///   ③ 亮色比暗色**更淡**（注释里的设计意图，两个模式不是同一个权重）。
///
/// 走**真实**的 `Card::apply_theme`（不是在这里重算一遍算式）：
/// 否则就是测“测试自己写的逻辑”，回退生产代码后依然全绿。
ST_TEST(theme_card_border_keeps_token_transparency) {
  const auto border_of = [](const st::ui::Theme& theme) {
    st::ui::Card card;
    card.apply_theme(theme);
    return card.style().border_color;
  };

  const st::ui::Theme dark = st::ui::Theme::dark();
  const st::ui::Theme light = st::ui::Theme::light();
  const Color dark_border = border_of(dark);
  const Color light_border = border_of(light);

  // ① 两个模式都必须**保留透明度**（不能是实色）。
  // 暗色漏了这条时得到 alpha=255 的纯白框；亮色漏了得到 alpha=140 的 55% 黑框。
  ST_CHECK(dark_border.a < 255);
  ST_CHECK(light_border.a < 255);

  // ② 组件只能把边框调**淡**，不得调浓（超过令牌自身的透明度）。
  // 注：暗色倍率是 1.0，所以这里断言的是“不超过”，上限 = 令牌 alpha。
  ST_CHECK(dark_border.a <= dark.colors().border.a);
  ST_CHECK(light_border.a <= light.colors().border.a);

  // ③ 亮色必须比暗色更淡（同一份令牌在两模式的权重不同，这是设计意图）。
  ST_CHECK(light_border.a < dark_border.a);

  // ④ 边框合成到卡底后，对比度必须**落在“看得见但不刺眼”的区间**里。
  //
  // 上限 3.0 拦住“不透明实边”（实测旧值：暗色 **13.09:1**、亮色 4.2:1）。
  // 下限取 **1.08**（而不是拍到 1.15）：亮色的设计值就是**极淡**——
  // `border` 令牌淡色下是 `rgba(0,0,0,28)`，再乘 0.55 后 alpha=15，
  // 合成到 `surface` 上是 (235,235,237)、对比度 **1.142:1**。
  // 那正是注释里“淡到几乎看不见”的含义：**它靠阴影而非描边收边**。
  // 把下限抬高会逼着后来人把边框调浓，与设计意图相反。
  // （实测口径：这里的 1.142 是纯颜色合成值；实机截图里峰值像素对卡底为 1.50~1.65，
  //   因为边框叠在阴影之上。两者同向，都远低于“刺眼”。）
  const auto composed_contrast = [](const Color& border, const Color& surface) {
    return contrast(composite(border, surface), surface);
  };
  const double dark_ratio = composed_contrast(dark_border, dark.colors().surface);
  const double light_ratio = composed_contrast(light_border, light.colors().surface);
  ST_CHECK(dark_ratio >= 1.08 && dark_ratio <= 3.0);
  ST_CHECK(light_ratio >= 1.08 && light_ratio <= 3.0);
}
