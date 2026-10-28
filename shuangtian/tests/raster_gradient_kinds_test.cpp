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
using st::ui::Theme;

/// **径向渐变必须在两个后端都画得出来**——这是"能力齐全"的一条实证。
///
/// 背景：`Gradient::radial` 在 GPU 端有专门的着色器分支（`platform_d3d11.cpp`），
/// 而软件端走通用逐像素采样。两者都该出图，只是快慢不同。
/// 歌白用径向渐变 56 处（光晕/氛围），这条保证不会"只有 GPU 能画"。
[[nodiscard]] auto render_gradient(const Gradient& gradient, int w, int h) -> Canvas {
  const Theme theme = Theme::dark();
  st::ui::Style style;
  style.radius = 0.0f;
  style.background_gradient = gradient;

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

ST_TEST(radial_gradient_renders) {
  // 圆心在正中、半径覆盖半宽：中心红 → 边缘蓝。
  // ⚠ 几何是 **0~1 归一化**（`Style::background_gradient` 的约定），
  // 传绝对像素值会被当成“圆心在画布左上、半径 0.5px”而整块 clamp 成端点色。
  Gradient radial = Gradient::radial(Point{0.5f, 0.5f}, 0.5f,
                                     {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                                      {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}});
  const Canvas canvas = render_gradient(radial, 80, 80);
  const Color center = canvas.pixel_at_point(Point{40.0f, 40.0f});
  const Color corner = canvas.pixel_at_point(Point{2.0f, 2.0f});
  st::log::out("[radial] 中心=({},{},{}) 角=({},{},{})\n", center.r, center.g, center.b, corner.r,
               corner.g, corner.b);
  // 中心必须偏红、角上必须偏蓝：证明径向真的按距离插值了，
  // 而不是"整块同色"（把 radial 当 linear 或完全忽略都会这样）。
  ST_CHECK(center.r > 150 && center.b < 110);
  ST_CHECK(corner.b > 130 && corner.r < 140);
}

ST_TEST(sweep_gradient_renders) {
  // 扫掠：绕圆心一圈换色（conic-gradient 的等价物，歌白用它做极光）。
  Gradient sweep = Gradient::sweep(Point{0.5f, 0.5f},
                                   {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                                    {0.5f, Color{0x00, 0xFF, 0x00, 0xFF}},
                                    {1.0f, Color{0xFF, 0x00, 0x00, 0xFF}}});
  const Canvas canvas = render_gradient(sweep, 80, 80);
  // 同一半径上取四个方向：颜色应随角度变化（不是按距离）。
  const Color right = canvas.pixel_at_point(Point{70.0f, 40.0f});
  const Color down = canvas.pixel_at_point(Point{40.0f, 70.0f});
  const Color up = canvas.pixel_at_point(Point{40.0f, 10.0f});
  st::log::out("[sweep] 右=({},{},{}) 下=({},{},{}) 上=({},{},{})\n", right.r, right.g, right.b,
               down.r, down.g, down.b, up.r, up.g, up.b);
  // 至少有两个方向的颜色明显不同 —— 否则说明扫掠没生效（退化成了同色/按距离）。
  const auto distinct = [](const Color& a, const Color& b) {
    return std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)) +
               std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)) +
               std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)) >
           80;
  };
  ST_CHECK(distinct(right, down) || distinct(right, up) || distinct(down, up));
}

}  // namespace
