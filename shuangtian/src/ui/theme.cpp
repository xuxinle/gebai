#include "st/ui/theme.hpp"

#include <algorithm>
#include <iterator>

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
  theme.name_ = "light";
  Palette& palette = theme.colors_;
  // —— 白色亚克力（默认亮色）——
  // 近中性的冷灰白，层次靠**表面阶梯 + 柔和阴影**表达。
  // 品牌色比旧版"冰蓝"更偏蓝紫（对齐歌白亚克力的 #5566EE 一脉），
  // 且经过压暗以保证 `primary/primary_soft` 也达 4.5:1（浅色下柔底按钮的硬要求）。
  palette.bg = hex(0xE8E8EEFFU);
  // 面板是「半透明白叠在背景上」（歌白 `--bg-elev: rgba(255,255,255,0.8)`）——
  // 这里预先合成成实色：#e8e8ee 叠 80% 白 = #fafafc。
  // 自绘管线没有 backdrop-filter，合成一次等价且更省（见 DESIGN §5）。
  palette.surface = hex(0xFAFAFCFFU);
  palette.surface_alt = hex(0xEDEDF2FFU);
  // 凹槽：**比背景更暗**（输入框底/代码底/进度槽）。语义是“凹进去”，不随亮暗反转——
  // 旧值取的灰与 `bg` 太近，凹陷感等于没有。
  palette.surface_sunken = hex(0xDDDDE4FFU);
  // 抬升面（弹层/菜单）：浅色下与 surface 同为白系——抬升由**阴影**表达，
  // 再堆一层灰会把弹层压成"贴平"的卡片。
  palette.surface_raised = hex(0xFFFFFFFFU);
  // 描边是**半透明白/黑光**叠在 surface 上的等效实色（歌白 `--border: rgba(0,0,0,.08)`）：
  // 同一个半透明描边在 `bg` / `surface` / `surface_alt` 上会显出不同强度，
  // 按主体底色合成一次即可（详见 `theme.hpp` 的亚克力口径 §1）。
  palette.border = hex(0xD2D2D9FFU);
  palette.border_strong = hex(0xB8B8C1FFU);
  palette.border_subtle = hex(0xE1E1E7FFU);
  palette.text = hex(0x141418FFU);
  palette.text_muted = hex(0x4E4E5AFFU);
  palette.text_faint = hex(0x6E6E7CFFU);
  palette.primary = hex(0x4655D8FFU);
  palette.primary_hover = hex(0x3A48C8FFU);
  palette.primary_active = hex(0x2E3AB4FFU);
  // 悬浮档位要**看得见**：早先取 0xF2F6FC，与侧栏底色只差 4/2/0（肉眼近乎没有），
  // 实测像素差才 6/255。现在这一步在浅色底上清晰可辨，又不至于喧宾夺主。
  palette.surface_hover = hex(0xEDEDEFFFU);
  palette.surface_pressed = hex(0xE3E3E5FFU);
  palette.border_hover = hex(0x9A9AA6FFU);
  palette.glow = hex(0x4655D82EU);
  palette.primary_soft = hex(0xEDEEF9FFU);
  // 顶部内高光：**浅色档留给透明**。
  //
  // 亚克力的"玻璃边缘"在两个模式下不是同一件事：深色底的边缘是**弱白光**（提亮），
  // 而浅色底已经接近纯白，再叠白光不会可见——歌白浅色档用的是一条**暗色发丝线**
  // （`--shadow-glow: 0 0 0 1px rgba(0,0,0,.06)`），而那正是 `border` 已经承担的职责。
  // 所以浅色档不再重复一层：靠 `border` + 阴影表达边缘，深色档才用本令牌。
  palette.highlight = hex(0xFFFFFF00U);
  palette.on_primary = hex(0xFFFFFFFFU);
  palette.accent = hex(0x0E849CFFU);
  palette.accent_soft = hex(0xE2EEF2FFU);
  palette.success = hex(0x1B8B55FFU);
  palette.warning = hex(0x8F6614FFU);
  palette.danger = hex(0xD13B40FFU);
  palette.focus_ring = hex(0x4655D866U);
  palette.overlay = hex(0x0A0A0F4DU);
  // 阴影基色 alpha 定在 0x26（15%）：`shadow_lg` 的关键层乘 1.05 → ≈16%，
  // 刚好卡在"浅底上单层不超过 16% 否则显脏"的上限内。
  palette.shadow = hex(0x0A0A1226U);
  palette.selection = hex(0xC6CBF7FFU);
  palette.code_bg = hex(0xECECF2FFU);
  palette.code_border = hex(0xDCDCE4FFU);
  // 语法色板（浅色）：关键字紫、字符串绿、数字橙、注释灰、类型青、函数蓝——彼此高辨识度
  theme.syntax_.plain = hex(0x1E2433FFU);
  theme.syntax_.keyword = hex(0x8B31C7FFU);
  theme.syntax_.type = hex(0x0E7C86FFU);
  theme.syntax_.string = hex(0x1A7F4BFFU);
  theme.syntax_.number = hex(0xB45309FFU);
  theme.syntax_.comment = hex(0x8A94A6FFU);
  theme.syntax_.function = hex(0x3B4BC8FFU);
  theme.syntax_.operator_ = hex(0x64748BFFU);
  theme.syntax_.punctuation = hex(0x94A3B8FFU);
  theme.syntax_.preprocessor = hex(0xC2410CFFU);
  theme.syntax_.builtin = hex(0x0F766EFFU);
  theme.syntax_.attribute = hex(0xBE185DFFU);
  theme.syntax_.key = hex(0x2E3AB4FFU);
  theme.syntax_.tag = hex(0xB91C1CFFU);
  theme.syntax_.inserted = hex(0x15803DFFU);
  theme.syntax_.deleted = hex(0xB91C1CFFU);
  theme.syntax_.line_number = hex(0xA0AEC0FFU);
  theme.syntax_.current_line = hex(0x4655D80AU);
  theme.syntax_.selection = hex(0x4655D833U);
  theme.syntax_.find_highlight = hex(0xF5D76E66U);  // 黄系：全部命中（VSCode 同族）
  theme.syntax_.find_active = hex(0xF0A72EFFU);     // 当前命中更深
  theme.syntax_.cursor = hex(0x4655D8FFU);
  theme.syntax_.matching_bracket = hex(0xF59E0B55U);
  // 终端色板（浅色）：**白底 + 暗色 16 色**。
  //
  // 为何不能沿用深底那套：ANSI 色是**协议固定语义**（`31` 永远是红），
  // 但它的**明度必须跟底色走**——为深底调的亮黄 `#E5E510` 放到白底上
  // 对比度只有 1.00（字面看不见）、亮白 `#E5E5E5` 是 1.07、亮绿 `#23D18B` 是 1.47。
  // 这里每个色都对白底 ≥6.2:1（回归断言锁 ≥4.5，即 WCAG AA 正文口径）。
  theme.terminal_.bg = hex(0xFFFFFFFFU);
  theme.terminal_.fg = hex(0x1F2430FFU);
  theme.terminal_.cursor = hex(0x4655D8FFU);
  theme.terminal_.selection = hex(0x4655D833U);
  theme.terminal_.ansi = {
      hex(0x3A3A45FFU), hex(0xB32218FFU), hex(0x0A6234FFU), hex(0x7A5600FFU),
      hex(0x1A46C4FFU), hex(0x861F9BFFU), hex(0x0B6477FFU), hex(0x57616FFFU),
      hex(0x4E4E5CFFU), hex(0xBC1C12FFU), hex(0x0E6F3CFFU), hex(0x865700FFU),
      hex(0x1D4ED8FFU), hex(0x9317A8FFU), hex(0x096B80FFU), hex(0x3A3A45FFU)};
  theme.metrics_ = Metrics{};
  return theme;
}

auto Theme::dark() -> Theme {
  Theme theme;
  theme.mode_ = ThemeMode::Dark;
  theme.name_ = "dark";
  Palette& palette = theme.colors_;
  // —— 黑色亚克力（默认暗色）——
  // 近黑带一丝冷（R=G-2、B=G+2，与歌白 `#0c0c0f` 同族）。
  // 关键约束：**中性色的饱和度极低**（<25%）——亚克力的"高级感"正来自这一点，
  // 而把中性色放进 220° 蓝相（饱和度 47~63%）会让整个界面显蓝，与"黑色亚克力"相反。
  palette.bg = hex(0x08080AFFU);
  // 面板 = 半透明白光叠在背景上的等效实色（歌白 `--bg-elev`）。
  // **必须与 bg 拉开到可量级**：近黑带的阶梯极密，差一档就看不见
  // （实测旧值 #101014 对 #08080a 只有 1.054:1，卡片看上去是"贴"在背景上）。
  palette.surface = hex(0x15151AFFU);
  palette.surface_alt = hex(0x1F1F26FFU);
  palette.surface_sunken = hex(0x050507FFU);
  // 抬升面（弹层/菜单）比 surface 再亮一档：深色下**阴影几乎不可见**，
  // 层次只能靠底色本身拉开。
  palette.surface_raised = hex(0x25252DFFU);
  // 描边 = 半透明白光（歌白 `--border` .12 / `--border-strong` .22）叠在 surface 上的等效值。
  // 三档都要在 surface 上**量得出来**（≥1.35 / ≥1.8 / ≥1.10）——深色下描边是
  // 控件边界的主要依据，淡一档就看不到。
  palette.border = hex(0x313136FFU);
  palette.border_strong = hex(0x4A4A4EFFU);
  palette.border_subtle = hex(0x2A2A2EFFU);
  palette.text = hex(0xF2F2F5FFU);
  palette.text_muted = hex(0xB4B4BEFFU);
  palette.text_faint = hex(0x80808CFFU);
  palette.primary = hex(0x8B9AFFFFU);
  palette.primary_hover = hex(0xA0ADFFFFU);
  palette.primary_active = hex(0x7787F5FFU);
  // 深色主题同样要给这套令牌：漏掉的话默认是全透明，**悬浮在暗色下毫无反馈**——
  // 而这种"某个主题下静默失效"的问题最难发现（浅色看着一切正常）。
  palette.surface_hover = hex(0x2B2B30FFU);
  palette.surface_pressed = hex(0x38383CFFU);
  palette.border_hover = hex(0x606064FFU);
  palette.glow = hex(0x8B9AFF38U);
  palette.primary_soft = hex(0x282B40FFU);
  palette.highlight = hex(0x2F2F33FFU);   ///< 顶部内高光：深色下的玻璃边缘（弱白光）
  palette.on_primary = hex(0x0B0B14FFU);
  palette.accent = hex(0x6FD8E8FFU);
  palette.accent_soft = hex(0x1C292FFFU);
  palette.success = hex(0x55D396FFU);
  palette.warning = hex(0xE6BD6DFFU);
  palette.danger = hex(0xFF7A7AFFU);
  palette.focus_ring = hex(0x8B9AFF66U);
  palette.overlay = hex(0x000000A6U);
  // 深色主题的阴影基色比浅色重得多：深底上投影几乎看不见，需要更高不透明度
  // 才能提供一点“离地”分离度。
  palette.shadow = hex(0x000000B3U);
  palette.selection = hex(0x273A6BFFU);
  palette.code_bg = hex(0x050507FFU);
  palette.code_border = hex(0x1E1E20FFU);
  // 语法色板（深色）：同族色相提亮，保证在深底上仍有足够对比
  theme.syntax_.plain = hex(0xD7E0F0FFU);
  theme.syntax_.keyword = hex(0xC792EAFFU);
  theme.syntax_.type = hex(0x7FDBFFFFU);
  theme.syntax_.string = hex(0xA5E075FFU);
  theme.syntax_.number = hex(0xF7A85FFFU);
  theme.syntax_.comment = hex(0x6B7A90FFU);
  theme.syntax_.function = hex(0x93A1FFFFU);
  theme.syntax_.operator_ = hex(0x9AA9C0FFU);
  theme.syntax_.punctuation = hex(0x7C8AA3FFU);
  theme.syntax_.preprocessor = hex(0xFFB07BFFU);
  theme.syntax_.builtin = hex(0x7FE3D4FFU);
  theme.syntax_.attribute = hex(0xFF9ED2FFU);
  theme.syntax_.key = hex(0xA0ADFFFFU);
  theme.syntax_.tag = hex(0xFF8F8FFFU);
  theme.syntax_.inserted = hex(0x6EE7A0FFU);
  theme.syntax_.deleted = hex(0xFF9A9AFFU);
  theme.syntax_.line_number = hex(0x54637AFFU);
  theme.syntax_.current_line = hex(0x8B9AFF1AU);
  theme.syntax_.selection = hex(0x8B9AFF44U);
  theme.syntax_.find_highlight = hex(0x6B5B1E99U);  // 暗色下的黄系命中
  theme.syntax_.find_active = hex(0x9A7B1FFFU);
  theme.syntax_.cursor = hex(0xB8C4FFFFU);
  theme.syntax_.matching_bracket = hex(0xE6BD6D55U);
  // 终端色板（深色）：经典终端黑 `#0C0C0C` + 亮 16 色。
  // 底取经典黑而不是 `surface_sunken`（`#050507`）：后者是**通用凹槽语义**
  //（输入框/代码底共用），奇黑偏沉；`#0C0C0C` 才是“一块屏幕”应当的观感。
  // 每个色对底 ≥5.2:1（回归断言锁 ≥4.5；black/brBlack 为灰阶，不参与）。
  theme.terminal_.bg = hex(0x0C0C0CU);
  theme.terminal_.fg = hex(0xE6E6E6FFU);
  theme.terminal_.cursor = hex(0x8B9AFFFFU);
  theme.terminal_.selection = hex(0x8B9AFF44U);
  theme.terminal_.ansi = {
      hex(0x3A3A45FFU), hex(0xE05555FFU), hex(0x0DBC79FFU), hex(0xE5E510FFU),
      hex(0x4A8FE0FFU), hex(0xCE63CEFFU), hex(0x11A8CDFFU), hex(0xE5E5E5FFU),
      hex(0x8A8A8AFFU), hex(0xF14C4CFFU), hex(0x23D18BFFU), hex(0xF5F543FFU),
      hex(0x3B8EEAFFU), hex(0xD670D6FFU), hex(0x29B8DBFFU), hex(0xFFFFFFFFU)};
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
///
/// ⚠ 返回的 `color.a` **不受 `shadow_strength` 影响**，只改模糊半径与偏移？——不：两者都受控。
/// 因为 `ui_theme_test.cpp` 的“单层不透明度 ≤ 16%”契约是对**默认档**的断言，
/// 所以强度旋钮作用在**基色透明度**上，而默认值下各层结果与旧值一致。
namespace {

/// 从基色按比例派生一层（`factor` 为相对基色不透明度的倍数）。
[[nodiscard]] auto shadow_layer(const math::Color& base, float factor) -> math::Color {
  return base.with_alpha_f(static_cast<float>(base.a) / 255.0f * factor);
}

/// 应用主题的强度/扩散旋钮：强度乘在透明度上，扩散乘在模糊与偏移上。
/// 非正值忽略（宁可保留默认，也不要静默把阴影变成 0 或负偏移）。
void apply_shadow_tuning(const Theme& theme, Shadow& shadow) {
  const Metrics& metrics = theme.metrics();
  if (metrics.shadow_strength > 0.0f && metrics.shadow_strength != 1.0f) {
    shadow.color = shadow.color.scale_alpha(metrics.shadow_strength);
    shadow.color2 = shadow.color2.scale_alpha(metrics.shadow_strength);
  }
  if (metrics.shadow_spread > 0.0f && metrics.shadow_spread != 1.0f) {
    shadow.blur *= metrics.shadow_spread;
    shadow.blur2 *= metrics.shadow_spread;
  }
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
  apply_shadow_tuning(theme, shadow);
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
  apply_shadow_tuning(theme, shadow);
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
  apply_shadow_tuning(theme, shadow);
  return shadow;
}

// —— 令牌名表 ——
//
// 实现方式是"名字数组 + 一个取址表"，而不是两张 switch：
// 前者能保证"名字清单"与"按名取址"**不会漂移**（两者共用同一份列表），
// 而手写的 switch 很容易新增 token 时漏掉、且编译器不会提醒。
namespace {

/// 每个 token 一个 `(名字, 成员指针)` 对。成员指针让三个入口
/// （名表 / 取色 / 写色）共用一个真值源。
struct ColorToken {
  std::string_view name;
  math::Color Palette::*member;
};

constexpr ColorToken kColorTokens[] = {
    {"bg", &Palette::bg},
    {"surface", &Palette::surface},
    {"surface_alt", &Palette::surface_alt},
    {"surface_sunken", &Palette::surface_sunken},
    {"surface_raised", &Palette::surface_raised},
    {"border", &Palette::border},
    {"border_strong", &Palette::border_strong},
    {"border_subtle", &Palette::border_subtle},
    {"border_hover", &Palette::border_hover},
    {"text", &Palette::text},
    {"text_muted", &Palette::text_muted},
    {"text_faint", &Palette::text_faint},
    {"primary", &Palette::primary},
    {"primary_hover", &Palette::primary_hover},
    {"primary_active", &Palette::primary_active},
    {"primary_soft", &Palette::primary_soft},
    {"on_primary", &Palette::on_primary},
    {"accent", &Palette::accent},
    {"accent_soft", &Palette::accent_soft},
    {"success", &Palette::success},
    {"warning", &Palette::warning},
    {"danger", &Palette::danger},
    {"surface_hover", &Palette::surface_hover},
    {"surface_pressed", &Palette::surface_pressed},
    {"glow", &Palette::glow},
    {"highlight", &Palette::highlight},
    {"focus_ring", &Palette::focus_ring},
    {"overlay", &Palette::overlay},
    {"shadow", &Palette::shadow},
    {"selection", &Palette::selection},
    {"code_bg", &Palette::code_bg},
    {"code_border", &Palette::code_border},
};

struct MetricToken {
  std::string_view name;
  float Metrics::*member;
  /// 写入时的合法区间（含）；`min > max` 表示不限。
  /// 为什么要夹取：主题文件/协议参数是**外部输入**，一个负的圆角或 0 字号会让界面消失，
  /// 而这属于"配置写错了不该让进程崩"的范畴（与 `json_get_*` 退化为默认值同姿态）。
  float minimum;
  float maximum;
};

constexpr MetricToken kMetricTokens[] = {
    {"space_xs", &Metrics::space_xs, 0.0f, 256.0f},
    {"space_sm", &Metrics::space_sm, 0.0f, 256.0f},
    {"space_md", &Metrics::space_md, 0.0f, 256.0f},
    {"space_lg", &Metrics::space_lg, 0.0f, 256.0f},
    {"space_xl", &Metrics::space_xl, 0.0f, 256.0f},
    {"space_2xl", &Metrics::space_2xl, 0.0f, 256.0f},
    {"radius_sm", &Metrics::radius_sm, 0.0f, 256.0f},
    {"radius_md", &Metrics::radius_md, 0.0f, 256.0f},
    {"radius_lg", &Metrics::radius_lg, 0.0f, 256.0f},
    {"radius_xl", &Metrics::radius_xl, 0.0f, 256.0f},
    {"radius_pill", &Metrics::radius_pill, 0.0f, 4096.0f},
    {"font_xs", &Metrics::font_xs, 4.0f, 256.0f},
    {"font_sm", &Metrics::font_sm, 4.0f, 256.0f},
    {"font_base", &Metrics::font_base, 4.0f, 256.0f},
    {"font_lg", &Metrics::font_lg, 4.0f, 256.0f},
    {"font_xl", &Metrics::font_xl, 4.0f, 256.0f},
    {"font_2xl", &Metrics::font_2xl, 4.0f, 256.0f},
    {"font_3xl", &Metrics::font_3xl, 4.0f, 256.0f},
    {"line_height_body", &Metrics::line_height_body, 1.0f, 4.0f},
    {"line_height_heading", &Metrics::line_height_heading, 1.0f, 4.0f},
    {"control_height_sm", &Metrics::control_height_sm, 8.0f, 256.0f},
    {"control_height", &Metrics::control_height, 8.0f, 256.0f},
    {"control_height_lg", &Metrics::control_height_lg, 8.0f, 256.0f},
    {"border_width", &Metrics::border_width, 0.0f, 16.0f},
    {"focus_width", &Metrics::focus_width, 0.0f, 16.0f},
    {"motion_fast", &Metrics::motion_fast, 0.0f, 10000.0f},
    {"motion_normal", &Metrics::motion_normal, 0.0f, 10000.0f},
    {"motion_slow", &Metrics::motion_slow, 0.0f, 10000.0f},
    {"hover_lift", &Metrics::hover_lift, 0.0f, 64.0f},
    {"hover_glow_width", &Metrics::hover_glow_width, 0.0f, 64.0f},
    {"press_sink", &Metrics::press_sink, 0.0f, 64.0f},
    {"shadow_strength", &Metrics::shadow_strength, 0.0f, 4.0f},
    {"shadow_spread", &Metrics::shadow_spread, 0.1f, 4.0f},
};

// `hover_duration` 是 `double`（秒）——单独处理。
//
// 为什么不为它把整张表改成 `double`：其余 30 多个尺度令牌都是 `float`（布局量），
// 为了一个成员把全部类型放宽，反而掩盖了"尺度令牌该是 `float`"这个约束。
using DoubleMetric = double Metrics::*;
constexpr std::string_view kHoverDuration{"hover_duration"};
constexpr DoubleMetric kHoverDurationMember = &Metrics::hover_duration;

}  // namespace

auto palette_token_names() -> const std::vector<std::string_view>& {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> out;
    out.reserve(std::size(kColorTokens));
    for (const ColorToken& token : kColorTokens) out.push_back(token.name);
    return out;
  }();
  return names;
}

auto palette_token(const Palette& palette, std::string_view name) -> std::optional<math::Color> {
  for (const ColorToken& token : kColorTokens) {
    if (token.name == name) return palette.*(token.member);
  }
  return std::nullopt;
}

auto set_palette_token(Palette& palette, std::string_view name, math::Color color) -> bool {
  for (const ColorToken& token : kColorTokens) {
    if (token.name == name) {
      palette.*(token.member) = color;
      return true;
    }
  }
  return false;
}

auto metric_token_names() -> const std::vector<std::string_view>& {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> out;
    out.reserve(std::size(kMetricTokens) + 1);
    for (const MetricToken& token : kMetricTokens) out.push_back(token.name);
    out.push_back(kHoverDuration);
    return out;
  }();
  return names;
}

auto metric_token(const Metrics& metrics, std::string_view name) -> std::optional<float> {
  for (const MetricToken& token : kMetricTokens) {
    if (token.name == name) return metrics.*(token.member);
  }
  if (name == kHoverDuration) return static_cast<float>(metrics.*kHoverDurationMember);
  return std::nullopt;
}

auto set_metric_token(Metrics& metrics, std::string_view name, float value) -> bool {
  if (!(value == value)) return false;  // NaN：宁可拒绝也不要让它污染布局
  for (const MetricToken& token : kMetricTokens) {
    if (token.name == name) {
      metrics.*(token.member) = std::clamp(value, token.minimum, token.maximum);
      return true;
    }
  }
  if (name == kHoverDuration) {
    metrics.*kHoverDurationMember = static_cast<double>(std::clamp(value, 0.0f, 10.0f));
    return true;
  }
  return false;
}

}  // namespace st::ui
