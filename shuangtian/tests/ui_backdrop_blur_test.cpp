/// 元素级背板毛玻璃（`Style::backdrop_blur` / `backdrop_tint`）的契约测试。
///
/// 这条通路让**任意元素**（工具栏 / Dock / 玻璃卡片）都能声明“模糊我背后的内容”，
/// 不再只限弹层（`paint_raised_surface`）。三条硬契约：
///
/// 1. **生效**：开了 `backdrop_blur` 的元素，其覆盖区域的背板高频细节被抹平
///    （软件腿 `Canvas::supports_blur()` 为真，走真实 `blur_region`）；
/// 2. **着色**：`backdrop_tint < 1` 时底色以该不透明度叠加（毛玻璃 = 模糊 + 半透色）；
/// 3. **降级**：后端不支持模糊时（`supports_blur()` 为假）必须退为**实色底**——
///    观感与“从未开过玻璃”一致，不崩溃、不留半透明裸底。
///
/// 逆向验证钉子（CONVENTIONS §7.1）：`paint_box` 里的接线若被删掉，
/// `backdrop_blur_flattens_background` 会立刻红——断言量的是**像素**，
/// 不经过任何“返回值 == true”的假通道。
#include <cmath>
#include <memory>

#include "no_blur_surface.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Element;
using st::ui::Panel;

/// 竖条纹背板（高频细节）：模糊前中心区极差接近 255，模糊后必须显著收窄。
[[nodiscard]] auto make_striped_canvas(int width, int height) -> Canvas {
  Canvas canvas = Canvas::for_logical_size(width, height, 1.0f);
  canvas.clear(Color{0x20, 0x20, 0x28, 0xFF});
  for (int i = 0; i < width; i += 12) {
    canvas.fill_rect(Rect{static_cast<float>(i), 0.0f, 6.0f, static_cast<float>(height)},
                     st::raster::Paint::solid(Color{0xE8, 0xE8, 0xF0, 0xFF}));
  }
  return canvas;
}

/// 造一块玻璃面板并摆到 `box`（返回裸指针供 paint，所有权在调用方）。
[[nodiscard]] auto make_glass_panel(float blur, float tint, Color fill) -> std::unique_ptr<Panel> {
  auto panel = std::make_unique<Panel>(st::ui::FlexDirection::Column);
  panel->style().set_solid_background(fill);
  panel->style().backdrop_blur = blur;
  panel->style().backdrop_tint = tint;
  return panel;
}

/// 采样区域的红绿极差（灰度条纹下红通道即可代表对比度）。
[[nodiscard]] auto red_span(const Canvas& canvas, Rect area) -> int {
  int lo = 255;
  int hi = 0;
  for (int y = static_cast<int>(area.y); y < static_cast<int>(area.y + area.height); ++y) {
    for (int x = static_cast<int>(area.x); x < static_cast<int>(area.x + area.width); ++x) {
      const int r = canvas.pixel_at(x, y).r;
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
  }
  return hi - lo;
}

/// 不支持模糊的画布桩（`no_blur_surface.hpp`）：`blur_region` 恒 false、
/// `supports_blur` 恒假，其余原语转发给内层真 `Canvas`。
/// （`Canvas` 是 `final`，不能继承；多态入口在 `Surface`。）

}  // namespace

ST_TEST(backdrop_blur_flattens_background) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  // 无玻璃：条纹原样（这是量尺的自检——量不到初始对比度就说明背板造错了）。
  Canvas plain = make_striped_canvas(240, 120);
  ST_REQUIRE(red_span(plain, Rect{40.0f, 20.0f, 160.0f, 80.0f}) > 180);

  // 有玻璃：同背板 + 玻璃面板（tint=0 → 只模糊不叠色，量到的就是纯模糊效果）。
  Canvas glass = make_striped_canvas(240, 120);
  auto panel = make_glass_panel(14.0f, 0.0f, Color{0, 0, 0, 0});
  panel->measure(context, st::ui::Constraints{});
  panel->arrange(context, Rect{40.0f, 20.0f, 160.0f, 80.0f});
  panel->paint(context, glass);

  ST_CHECK(panel->style().backdrop_blur == 14.0f);
  // 覆盖区内高频细节被显著抹平（>180 → <60：模糊半径 14 对 12px 条纹足够）。
  ST_CHECK(red_span(glass, Rect{48.0f, 28.0f, 144.0f, 64.0f}) < 60);
  // 覆盖区**外**不受影响（模糊不越界，blur_region 契约第 2 条的元素级重现）。
  ST_CHECK(red_span(glass, Rect{4.0f, 4.0f, 20.0f, 20.0f}) > 180);
}

ST_TEST(backdrop_tint_composites_over_blur) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  // tint=0.85 深色叠色：玻璃区中心应明显**暗于**纯模糊档（着色层真的压了上去），
  // 且亮于“完全不透明”（tint 起效而不是把模糊白做）。
  Canvas blurred_only = make_striped_canvas(240, 120);
  auto pure = make_glass_panel(14.0f, 0.0f, Color{0x10, 0x12, 0x18, 0xFF});
  pure->arrange(context, Rect{40.0f, 20.0f, 160.0f, 80.0f});
  pure->paint(context, blurred_only);

  Canvas tinted = make_striped_canvas(240, 120);
  auto glass = make_glass_panel(14.0f, 0.85f, Color{0x10, 0x12, 0x18, 0xFF});
  glass->arrange(context, Rect{40.0f, 20.0f, 160.0f, 80.0f});
  glass->paint(context, tinted);

  Canvas opaque = make_striped_canvas(240, 120);
  auto solid = make_glass_panel(0.0f, 1.0f, Color{0x10, 0x12, 0x18, 0xFF});
  solid->arrange(context, Rect{40.0f, 20.0f, 160.0f, 80.0f});
  solid->paint(context, opaque);

  const int center_x = 120;
  const int center_y = 60;
  const int blurred_r = blurred_only.pixel_at(center_x, center_y).r;
  const int tinted_r = tinted.pixel_at(center_x, center_y).r;
  const int opaque_r = opaque.pixel_at(center_x, center_y).r;

  ST_CHECK(tinted_r < blurred_r);   // 着色压暗了模糊结果
  ST_CHECK(tinted_r > opaque_r);    // 但仍透出背板（不是实色底）
  ST_CHECK(std::abs(opaque_r - 0x10) <= 2);  // 实色档自检：中心就是底色
}

ST_TEST(backdrop_blur_degrades_to_solid_when_unsupported) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  stdtest::NoBlurSurface canvas(240, 120);
  // 条纹背板画在桩上（fill_rect 走 Canvas 原实现）。
  for (int i = 0; i < 240; i += 12) {
    canvas.fill_rect(Rect{static_cast<float>(i), 0.0f, 6.0f, 120.0f},
                     st::raster::Paint::solid(Color{0xE8, 0xE8, 0xF0, 0xFF}));
  }
  auto glass = make_glass_panel(14.0f, 0.85f, Color{0x10, 0x12, 0x18, 0xFF});
  glass->arrange(context, Rect{40.0f, 20.0f, 160.0f, 80.0f});
  ST_REQUIRE(!canvas.supports_blur());
  glass->paint(context, canvas);

  // 降级语义：模糊没发生，tint 也不应把底色打薄——中心就是**实底色**。
  const Color center = canvas.pixel_at(120, 60);
  ST_CHECK(std::abs(static_cast<int>(center.r) - 0x10) <= 2);
  ST_CHECK(std::abs(static_cast<int>(center.g) - 0x12) <= 2);
}
