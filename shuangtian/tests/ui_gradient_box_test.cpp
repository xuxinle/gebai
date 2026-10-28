#include <memory>

#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Gradient;
using st::raster::GradientStop;
using st::ui::Theme;

/// 直接画一个带 `Style` 的 `Panel`，用于验证 `paint_box` 的渐变分支。
[[nodiscard]] auto render_panel(const Theme& theme, const st::ui::Style& style, int w, int h)
    -> Canvas {
  st::ui::UiRoot root;
  root.set_theme(theme);
  root.set_viewport(st::math::Size{static_cast<float>(w), static_cast<float>(h)});
  root.set_text_port(nullptr);
  auto panel = std::make_unique<st::ui::Panel>();
  panel->style() = style;
  root.set_content(std::move(panel));
  root.layout(true);
  Canvas canvas(w, h);
  canvas.clear(theme.colors().bg);
  root.paint(canvas);
  return canvas;
}

/// 纯色底必须与改动前**逐像素一致**（这是全仓最热的绘制分支，不能有回归）。
ST_TEST(paint_box_solid_background_unchanged) {
  const Theme theme = Theme::dark();
  st::ui::Style style;
  style.background = Color{0x33, 0x66, 0x99, 0xFF};
  style.radius = 0.0f;
  const Canvas canvas = render_panel(theme, style, 40, 30);
  // 整块都是那个颜色（无渐变、无裁剪、无描边）。
  ST_CHECK(canvas.pixel_at_point(Point{20.0f, 15.0f}) == style.background);
  ST_CHECK(canvas.pixel_at_point(Point{2.0f, 2.0f}) == style.background);
}

/// 设了渐变就**优先用渐变**，`background` 不再出现在画面里。
ST_TEST(paint_box_gradient_overrides_solid_background) {
  const Theme theme = Theme::dark();
  st::ui::Style style;
  // 纯色故意用一个鲜艳的绿——如果它被画出来，下面会立刻发现。
  style.background = Color{0x00, 0xFF, 0x00, 0xFF};
  style.radius = 0.0f;
  style.background_gradient =
      Gradient::linear(Point{0.0f, 0.0f}, Point{1.0f, 1.0f},
                       {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                        {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}});
  const Canvas canvas = render_panel(theme, style, 60, 60);

  const Color left = canvas.pixel_at_point(Point{3.0f, 3.0f});
  const Color right = canvas.pixel_at_point(Point{56.0f, 56.0f});
  // 两端必须分别是红/蓝方向，且**都不是绿色**（说明纯色回退没有覆盖上来）。
  ST_CHECK(left.r > 150 && left.b < 100);
  ST_CHECK(right.b > 150 && right.r < 100);
  ST_CHECK(left.g < 80 && right.g < 80);
  // 中点必须是混合色（红蓝都在），说明渐变是真的在插值而不是两半拼接。
  const Color mid = canvas.pixel_at_point(Point{30.0f, 30.0f});
  ST_CHECK(mid.r > 60 && mid.b > 60);
}

/// **归一化坐标必须随元素尺寸伸缩**：同样的 `Style` 画在两种尺寸上，
/// 两端颜色都应当落在各自的角上（而不是"只有大尺寸对、小尺寸错"）。
///
/// 这条防的是"把 0~1 坐标当绝对坐标用"那类错误——`apply_theme` 跑在布局之前，
/// 拿不到尺寸，所以 `Style` 里只能存归一化值，由 `paint_box` 映射。
ST_TEST(paint_box_gradient_scales_with_box) {
  const Theme theme = Theme::dark();
  const auto make_style = [] {
    st::ui::Style style;
    style.radius = 0.0f;
    style.background_gradient =
        Gradient::linear(Point{0.0f, 0.0f}, Point{1.0f, 1.0f},
                         {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                          {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}});
    return style;
  };

  for (const int size : {40, 120}) {
    const Canvas canvas = render_panel(theme, make_style(), size, size);
    const Color near_origin = canvas.pixel_at_point(Point{3.0f, 3.0f});
    const Color far_corner =
        canvas.pixel_at_point(Point{static_cast<float>(size) - 4.0f,
                                    static_cast<float>(size) - 4.0f});
    st::log::out("[gradient] size={} origin=({},{},{}) corner=({},{},{})\n", size, near_origin.r,
                 near_origin.g, near_origin.b, far_corner.r, far_corner.g, far_corner.b);
    // 起点附近是红、对角附近是蓝——两种尺寸都要成立。
    ST_CHECK(near_origin.r > 150 && near_origin.b < 110);
    ST_CHECK(far_corner.b > 150 && far_corner.r < 110);
  }
}

/// 渐变在圆角裁剪下仍然正确（不能溢出到圆角外）。
ST_TEST(paint_box_gradient_respects_radius) {
  const Theme theme = Theme::dark();
  st::ui::Style style;
  style.radius = 16.0f;
  style.background_gradient =
      Gradient::linear(Point{0.0f, 0.0f}, Point{1.0f, 0.0f},
                       {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                        {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}});
  const Canvas canvas = render_panel(theme, style, 60, 60);
  // 圆角外的四角必须仍是主题底色（渐变不能溢出）。
  const Color corner = canvas.pixel_at_point(Point{1.0f, 1.0f});
  ST_CHECK(corner == theme.colors().bg);
}

/// `gradient_opacity` 必须真的降低渐变强度（0 = 完全不画渐变，露出纯色底）。
ST_TEST(paint_box_gradient_opacity_is_respected) {
  const Theme theme = Theme::dark();
  const auto make_style = [](float opacity) {
    st::ui::Style style;
    style.radius = 0.0f;
    style.background = Color{0x00, 0x11, 0x22, 0xFF};
    style.gradient_opacity = opacity;
    style.background_gradient =
        Gradient::linear(Point{0.0f, 0.0f}, Point{1.0f, 0.0f},
                         {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                          {1.0f, Color{0xFF, 0x00, 0x00, 0xFF}}});
    return style;
  };
  const Canvas opaque = render_panel(theme, make_style(1.0f), 40, 20);
  const Canvas transparent = render_panel(theme, make_style(0.0f), 40, 20);
  const Color full = opaque.pixel_at_point(Point{20.0f, 10.0f});
  const Color none = transparent.pixel_at_point(Point{20.0f, 10.0f});
  ST_CHECK(full.r > 200);                       // 渐变全强度
  st::log::out("[gradient] opacity=0 → ({},{},{},{})，期望纯色底 (0,17,34,255)\n", none.r,
               none.g, none.b, none.a);
  // 用 tolerance 而不是精确等：opacity=0 时渐变贡献应为 0，但实现可能走“画了但全透明”，
  // 经过一次合成后舍入可能有 1~2 阶差异。
  ST_CHECK(none.r <= 2 && none.g >= 15 && none.g <= 19 && none.b >= 32 && none.b <= 36);
}

}  // namespace
