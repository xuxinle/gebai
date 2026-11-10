/// 变体（`Variant`）的**像素级**护栏：九个档位各自「该画什么底」。
///
/// ## 为什么必须断言像素而不是 `style.background`
///
/// 2026-11-10 相期「全部忽略」蓝底事故的根因是：
///
/// ```
/// style.background       = 透明（Ghost 语义，正确）
/// style.background_gradient = 主色渐变（Primary 残影，没人清）
/// paint_box 的规则          = 渐变优先于纯色
/// ⇒ 屏幕上是主色块，而读回口一切正常
/// ```
///
/// 任何只查 `style.background` 的测试**都不可能发现它**（读回完全正确）。
/// 所以这里的判据一律取**画出来的像素**——那是唯一不会说谎的事实。
///
/// ## 判据为什么是「与父容器同色」而不是「不等于主色」
///
/// “不等于主色”是弱判据：渐变的中段、别的变体的色、甚至纯黑都能过。
/// `Ghost` 的语义是**没有底**——那唯一可验证的事实就是「背景原样透上来」。
/// 于是把它放进一个已知纯色的容器里，断言按钮区域像素 == 容器底色。

#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/xml_ui.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::ui::Button;
using st::ui::Element;
using st::ui::Theme;
using st::ui::UiRoot;
using st::ui::Variant;

/// 页面底色（与任何变体色都不同，任何残留都看得出来）。
constexpr Color kPageFill{0x20, 0x80, 0x40, 0xFF};   // 醒目的绿：一旦漏出来立刻可见

/// 把一个按钮放进已知底色里画出来，返回画布。
[[nodiscard]] auto render_button(const Theme& theme, const std::string& xml) -> st::raster::Canvas {
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(xml, &report);
  if (!report.clean()) st::log::out("[variant] 建树报告：tags={} attrs={} parse={}\n",
                                    report.unknown_tags.size(), report.unknown_attributes.size(),
                                    report.parse_warning);
  UiRoot root;
  root.set_theme(theme);
  root.set_viewport({400.0f, 200.0f});
  root.set_content(std::move(tree));
  root.layout(true);

  st::raster::Canvas canvas(400, 200);
  canvas.clear(kPageFill);
  root.paint(canvas);
  return canvas;
}

/// 取按钮**内部**一点（避开圆角与描边）。
[[nodiscard]] auto sample_inside(st::raster::Canvas& canvas, Element* button) -> Color {
  const auto box = button->bounds();
  return canvas.pixel_at_point(Point{box.x + box.width * 0.5f, box.y + box.height * 0.5f});
}

/// 封面助手：把 XML 里的按钮换成指定变体。
[[nodiscard]] auto page_with(const std::string& variant) -> std::string {
  return std::format(
      R"(<Column id="page"><Button id="b" label="动作" variant="{}" scale="small"/></Column>)",
      variant);
}

// ─────────────────────────────────────────────────────────────────────────────
// ① Ghost：**没有底**——像素必须与容器底色逐位相同
// ─────────────────────────────────────────────────────────────────────────────
//
// 这条就是相期事故的回归。旧实现在这里拿到的是主色渐变的某一点。
ST_TEST(button_ghost_paints_no_background_at_all) {
  const Theme theme = Theme::light();
  // 走 XML 真实链路（`Appearance` 落位），不是直接 set_variant——
  // 事故就发生在声明式那条路上。
  auto canvas = render_button(theme, page_with("ghost"));
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(page_with("ghost"), &report);
  UiRoot root;
  root.set_theme(theme);
  root.set_viewport({400.0f, 200.0f});
  root.set_content(std::move(tree));
  root.layout(true);
  Element* b = root.find("b");
  ST_REQUIRE(b != nullptr);
  st::raster::Canvas shot(400, 200);
  shot.clear(kPageFill);
  root.paint(shot);
  const Color pixel = sample_inside(shot, b);
  st::log::out("[variant] ghost 像素 = ({},{},{})\n", static_cast<int>(pixel.r),
               static_cast<int>(pixel.g), static_cast<int>(pixel.b));
  // “没有底”的唯一可验证事实：容器底色原样透上来。
  ST_CHECK_EQ(static_cast<int>(pixel.r), static_cast<int>(kPageFill.r));
  ST_CHECK_EQ(static_cast<int>(pixel.g), static_cast<int>(kPageFill.g));
  ST_CHECK_EQ(static_cast<int>(pixel.b), static_cast<int>(kPageFill.b));
  (void)canvas;
}

// ────────────────────────────────���────────────────────────────────────────────
// ② Primary：必须是**渐变**（对角两端不同色），不是纯色
// ─────────────────────────────────────────────────────────────────────────────
//
// 反向判据：`Ghost` 要求「一点颜色都不留」，`Primary` 要求「真的有渐变」。
// 两条一起才能钉死「渐变该在的时候在、不该在的时候不在」。
//
// ⚠ **取样点必须沿渐变方向**（实测踩到，一轮才想清）：主按钮的渐变是
// `(0,0) → (1,1)` 的**对角**渐变，沿水平中线取两点恰好落在同一条等值线上——
// 两侧必然同色。那不是我想要的“纯色”，但它与“真的纯色”像素完全一样。
// 这种判据失效最危险：它把“代码对”报成“代码错”，让人去改本来没问题的代码。
// 所以取样本框的**左上角与右下角**两块（沿对角轴投影值差最大）。
ST_TEST(button_primary_paints_a_gradient_not_a_solid) {
  const Theme theme = Theme::light();
  // 前提：本判据只在主题**真的**给了不同的 `accent` 时才有意义
  //（缺省两者相等 → 按设计就是纯色，零开销回退）。主题改了也先断言前提，
  // 免得这条测静默失效（前提不成立时它“永远绿”）。
  ST_REQUIRE(theme.colors().primary.r != theme.colors().accent.r ||
             theme.colors().primary.g != theme.colors().accent.g ||
             theme.colors().primary.b != theme.colors().accent.b);

  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml(page_with("primary"), &report);
  UiRoot root;
  root.set_theme(theme);
  root.set_viewport({400.0f, 200.0f});
  root.set_content(std::move(tree));
  root.layout(true);
  Element* b = root.find("b");
  ST_REQUIRE(b != nullptr);

  st::raster::Canvas shot(400, 200);
  shot.clear(kPageFill);
  root.paint(shot);

  const auto box = b->bounds();
  // 取内部**对角两端**的两点（避开圆角半径与描边）。
  const float inset_x = std::min(box.width, box.height) * 0.22f;
  const float inset_y = std::min(box.width, box.height) * 0.30f;
  const Color top_left = shot.pixel_at_point(Point{box.x + inset_x, box.y + inset_y});
  const Color bottom_right =
      shot.pixel_at_point(Point{box.right() - inset_x, box.bottom() - inset_y});
  st::log::out("[variant] primary 左上=({},{},{}) 右下=({},{},{})\n",
               static_cast<int>(top_left.r), static_cast<int>(top_left.g),
               static_cast<int>(top_left.b), static_cast<int>(bottom_right.r),
               static_cast<int>(bottom_right.g), static_cast<int>(bottom_right.b));

  const int delta_r = std::abs(static_cast<int>(top_left.r) - static_cast<int>(bottom_right.r));
  const int delta_g = std::abs(static_cast<int>(top_left.g) - static_cast<int>(bottom_right.g));
  const int delta_b = std::abs(static_cast<int>(top_left.b) - static_cast<int>(bottom_right.b));
  ST_CHECK(delta_r + delta_g + delta_b > 8);
  // 且都不是页面底色（真的铺了底，不是漏底）。
  ST_CHECK(!(top_left.r == kPageFill.r && top_left.g == kPageFill.g &&
             top_left.b == kPageFill.b));
}

// ─────────────────────────────────────────────────────────────────────────────
// ③ 从 Primary 切到任何变体：**不得留下渐变**
// ─────────────────────────────────────────────────────────────────────────────
//
// 这是 C++ 侧的入口（`set_variant`）。旧实现把 `reset()` 放在 `case Primary`
// 分支内部，于是「先 Primary 后 Ghost」的路径永不清渐变——
// gbcode 侧栏开关（`set_variant(on ? Soft : Ghost)`，初值 Primary）全是这个形态。
ST_TEST(switching_away_from_primary_clears_the_gradient) {
  const Theme theme = Theme::light();
  for (const Variant target : {Variant::Ghost, Variant::Neutral, Variant::Soft,
                               Variant::Danger, Variant::Plain}) {
    Button button("动作", Variant::Primary, Button::Size::Small);
    button.refresh_theme(theme);
    ST_REQUIRE(button.style().background_gradient.has_value());   // 前提：确实铺过渐变

    button.set_variant(target);
    button.refresh_theme(theme);   // `UiRoot` 每帧就是这么做的

    const bool leaked = button.style().background_gradient.has_value();
    if (leaked) {
      st::log::out("[variant] 切到 {} 后仍残留渐变（缺陷复现）\n",
                   st::ui::variant_name(target));
    }
    ST_CHECK(!leaked);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ④ `variant` 的读回必须与**声明**一致（相期事故里“读回说谎”那一半）
// ─────────────────────────────────────────────────────────────────────────────
ST_TEST(button_get_variant_reports_the_declared_value) {
  const Theme theme = Theme::light();
  for (const std::string_view name : {"ghost", "soft", "neutral", "danger", "primary"}) {
    st::ui::XmlBuildReport report;
    auto tree = st::ui::build_from_xml(page_with(std::string(name)), &report);
    UiRoot root;
    root.set_theme(theme);
    root.set_viewport({400.0f, 200.0f});
    root.set_content(std::move(tree));
    root.layout(true);

    Element* b = root.find("b");
    ST_REQUIRE(b != nullptr);
    const auto got = b->get_property("variant");
    ST_REQUIRE(got.has_value());
    st::log::out("[variant] 声明 {} → get(variant)={}\n", name, *got);
    // 旧实现在这里一律报 "primary"（读的是组件成员，声明只写了 style）。
    ST_CHECK_EQ(*got, std::string(name));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ⑤ 每个变体的字色都必须与底色成对比（防止“底对了、字看不清”）
// ─────────────────────────────────────────────────────────────────────────────
ST_TEST(every_variant_keeps_text_readable_against_its_background) {
  const Theme theme = Theme::light();
  for (const Variant v : {Variant::Plain, Variant::Neutral, Variant::Primary, Variant::Accent,
                          Variant::Success, Variant::Warning, Variant::Danger, Variant::Ghost,
                          Variant::Soft}) {
    Button button("动作", v, Button::Size::Small);
    button.refresh_theme(theme);
    const auto& style = button.style();
    // “身后是什么颜色”得按**真正画出来的**算：
    //  · 有渐变 → 取渐变端点里与字色**对比度最差**的那个（最坏情形才是判据）；
    //  · 无底（透明）→ 对的是**页面底色**，不是 `background`；
    //  · 否则 → `background`。
    //
    // 为何要分这三种（实测踩到）：第一版只用 `style.background`，而 Primary 的
    // `background` 现在是**回退色**（重力其实在渐变上），于是报出 1.09 的假红。
    const auto lum = [](const Color& c) {
      const auto ch = [](std::uint8_t value) {
        const double s = static_cast<double>(value) / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
      };
      return 0.2126 * ch(c.r) + 0.7152 * ch(c.g) + 0.0722 * ch(c.b);
    };
    const auto contrast = [&](const Color& raw) {
      const double l1 = lum(style.color);
      const double l2 = lum(raw);
      return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
    };
    double ratio = 0.0;
    if (style.background_gradient.has_value() && !style.background_gradient->stops().empty()) {
      ratio = std::numeric_limits<double>::max();
      for (const auto& stop : style.background_gradient->stops()) {
        ratio = std::min(ratio, contrast(stop.color));
      }
    } else {
      ratio = contrast(style.background.a == 0U ? theme.colors().bg : style.background);
    }
    st::log::out("[variant] {:<10} 对比度 {:.2f}\n", st::ui::variant_name(v), ratio);
    // 3:1 是 WCAG 对「大号/非文本」的下限；按钮文案至少要有这个量级，
    // 取 3.0 而不是 4.5：禁用态与小字号另有全局令牌约束，这里只防“同色不可读”。
    ST_CHECK(ratio >= 3.0);
  }
}

}  // namespace
