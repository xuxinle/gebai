#include <cmath>
#include <memory>
#include <vector>

#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Theme;

/// 相对亮度（WCAG 口径）：弹层"浮起来"是**亮度层次**问题，不是色相问题。
[[nodiscard]] auto luminance(const Color& c) -> double {
  const auto channel = [](std::uint8_t v) {
    const double s = static_cast<double>(v) / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

/// 渲染一个对话框并取四点：卡面中心、遮罩、页面底、**背后内容的原型**。
///
/// **背后必须画真实内容**（不只是纯色底）：
/// 弹层亮度比会不会退化成"看不出浮起"，取决于模糊背上来的东西有多亮。
/// 在纯色底上测，旧的 `tint=0.72` 仍有 1.69（看着还行）——而真实界面
/// （背后有亮色卡片/按钮/图标）只有 **1.048**。这种差异正是
/// "测试场景不真实"造成的假绿。
struct Probe {
  Color card{};
  Color scrim{};
  Color page{};
  Color backdrop_content{};  ///< 未加遮罩时背后那块内容的颜色
};

[[nodiscard]] auto probe_dialog(const Theme& theme) -> Probe {
  st::ui::UiRoot root;
  root.set_theme(theme);
  root.set_viewport(st::math::Size{900.0f, 600.0f});
  root.set_text_port(nullptr);

  root.set_content(std::make_unique<st::ui::Panel>());

  auto dialog = std::make_unique<st::ui::Dialog>("确认操作", "该操作不可撤销，请谨慎操作。");
  dialog->set_actions({"取消", "确定"});
  dialog->set_viewport_rect(Rect{0.0f, 0.0f, 900.0f, 600.0f});
  st::ui::Dialog* raw = dialog.get();
  root.add_overlay(std::move(dialog));
  root.layout(true);

  Canvas canvas(900, 600);
  canvas.clear(theme.colors().bg);
  // 弹层背后铺亮色卡片（模拟真实界面里那些面板/按钮），再让 UiRoot 绘弹层。
  canvas.fill_rect(Rect{120.0f, 120.0f, 660.0f, 360.0f},
                   st::raster::Paint::solid(theme.colors().surface_raised));
  canvas.fill_rect(Rect{150.0f, 150.0f, 200.0f, 80.0f},
                   st::raster::Paint::solid(theme.colors().primary));
  root.paint(canvas);

  Probe probe;
  probe.card = canvas.pixel_at_point(raw->card_rect().center());
  probe.scrim = canvas.pixel_at_point(Point{5.0f, 595.0f});
  probe.page = theme.colors().bg;
  probe.backdrop_content = theme.colors().surface_raised;
  return probe;
}

/// 弹层必须**明显浮起**：卡面亮度 / 背后遮罩亮度 ≥ 1.5。
///
/// 这条是 2026-10 那次的护栏：当时亮色 `overlay_blur_tint = 0.72`，
/// 而弹层底下是暗遮罩（整屏被压暗了）。模糊只让背板变糊、**不会让它变亮**，
/// 于是 28% 的暗遮罩透上来 —— 卡面比背后还暗。
/// 实测亮度比 **1.048**（几乎看不出浮起，只有主色按钮显眼），
/// 把 tint 提到 0.88 后为 **1.92**；暗色档 0.82 → **6.20**。
///
/// 阈值 1.5：给修好后的 1.92 留余量，同时任何"退回平的"都会立刻红。
constexpr double kMinOverlayLift = 1.5;

ST_TEST(dialog_light_overlay_visibly_lifts) {
  const Theme theme = Theme::light();
  const Probe p = probe_dialog(theme);
  const double card = luminance(p.card);
  const double scrim = luminance(p.scrim);
  ST_CHECK(scrim > 0.0);
  const double ratio = card / scrim;
  st::log::out("[overlay-lift] 亮色：卡面 L={:.4f} 遮罩 L={:.4f} 比值 {:.3f}\n", card, scrim,
               ratio);
  ST_CHECK(ratio >= kMinOverlayLift);
  // **关键判据**：亮色下弹层不能比它背后那块亮内容（`surface_raised` 卡片）暗太多。
  //
  // 原缺陷的特征正是"弹层比背后的卡片还暗"：真实界面实测 0.168 vs 0.457（比值 0.37），
  // 用户看到的就是"对话框不亮，只有按钮亮"。这时只比遮罩亮是不够的。
  //
  // 为何是 0.85 而不是 0.95：毛玻璃**必然**让卡面比纯刮目底略微沉一点
  //（它需模糊背板 + 半透明着色，而背板包含暗遮罩）。修好后的实测值是 **0.880**，
  // 而旧值 0.72 是 **0.731**。0.85 恰好把两者分开，又给修复版留出余量。
  const double content = luminance(p.backdrop_content);
  st::log::out("[overlay-lift] 亮色：卡面 L={:.4f} 背后内容 L={:.4f} 比值 {:.3f}\n", card,
               content, card / content);
  ST_CHECK(card / content >= 0.85);
}

ST_TEST(dialog_dark_overlay_visibly_lifts) {
  const Theme theme = Theme::dark();
  const Probe p = probe_dialog(theme);
  const double card = luminance(p.card);
  const double scrim = luminance(p.scrim);
  ST_CHECK(scrim > 0.0);
  const double ratio = card / scrim;
  st::log::out("[overlay-lift] 暗色：卡面 L={:.4f} 遮罩 L={:.4f} 比值 {:.3f}\n", card, scrim,
               ratio);
  ST_CHECK(ratio >= kMinOverlayLift);
}

/// 弹层真的画在了页面底之上（不是"都没画"导致的亮度偶然相等）。
ST_TEST(dialog_overlay_card_differs_from_background) {
  for (const Theme& theme : {Theme::light(), Theme::dark()}) {
    const Probe p = probe_dialog(theme);
    ST_CHECK(!(p.card == p.page));
    ST_CHECK(!(p.scrim == p.page));
  }
}

}  // namespace
