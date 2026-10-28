// `Canvas::draw_shadow` 的**遮罩形状**诊断：沿一条中线取剖面，看
// 「矩形内 / 边缘 / 矩形外」的强度如何衰减。
//
// 为什么需要它：做内阴影时把偏移取反、裁到框内，本以为只有边缘一圈会暗，
// 实测却是**整块**一起变暗（深处与上内缘同值）。这说明对 `draw_shadow` 的
// 衰减带宽度/形状的直觉是错的——而这类错误在代码里读不出来，只能量。
//
// 用法： draw_shadow_profile_probe [blur] [offset_y]

#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;

[[nodiscard]] auto luminance(const Color& c) -> double {
  return 0.2126 * static_cast<double>(c.r) + 0.7152 * static_cast<double>(c.g) +
         0.0722 * static_cast<double>(c.b);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const float blur = argc > 1 ? static_cast<float>(std::atof(argv[1])) : 6.0f;
  const float offset_y = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 1.0f;

  constexpr int kW = 320;
  constexpr int kH = 240;
  const st::math::Rect rect{100.0f, 100.0f, 120.0f, 60.0f};
  const Color background = Color::rgb(0x0D, 0x0D, 0x10);
  const Color shadow = Color{0, 0, 0, 66};  // 26% 黑（暗色 sunken 档）

  Canvas canvas = Canvas::for_logical_size(kW, kH, 1.0f);
  canvas.clear(background);
  canvas.draw_shadow(rect, 6.0f, blur, shadow, st::math::Point{0.0f, offset_y});

  const double base = luminance(background);
  st::log::out("=== draw_shadow 剖面（rect y=100..160, blur={:.1f}, offset_y={:.1f}）===\n",
            static_cast<double>(blur), static_cast<double>(offset_y));
  st::log::out("矩形外上方 / 矩形内（上→下） / 矩形外下方，取 x=160 一列：\n");
  constexpr int kCol = 160;
  for (int y = 90; y <= 175; y += 2) {
    const double drop = base - luminance(canvas.pixel_at(kCol, y));
    const char* where = (y < 100) ? "外" : (y <= 160 ? "内" : "外");
    st::log::out("  y={:3d} [{}] 压暗 {:.1f}\n", y, where, drop);
  }
  return 0;
}
