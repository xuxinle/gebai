// 圆角边框环的光栅化精度诊断：把「外圈 / 内圈 / 环」三条路径分别光栅化，
// 与解析面积逐项比对；并对照「竖带 vs 斜带」——用来判定「圆弧墨量偏少」发生在哪一步。
//
// ⚠ `Canvas::fill_path` 收的是**逻辑坐标**（内部按 device_scale 缩放），
// 所以本探针所有几何都用逻辑值，不要预先乘 scale。
//
// 用法： ring_raster_probe [scale]

#include <cmath>
#include <cstdlib>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace {

using st::raster::Canvas;
using st::raster::Path;

constexpr int kLogicalW = 260;
constexpr int kLogicalH = 200;
constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] auto analytic_area(double w, double h, double r) -> double {
  if (r <= 0.0) return w * h;
  return w * h - (4.0 - kPi) * r * r;
}

[[nodiscard]] auto analytic_ring(double w, double h, double r, double width) -> double {
  const double iw = w - 2.0 * width;
  const double ih = h - 2.0 * width;
  const double ir = r - width < 0.0 ? 0.0 : r - width;
  return analytic_area(w, h, r) - analytic_area(iw, ih, ir);
}

/// 白色底（0xFFFFFF）上黑色墨的面积（物理像素²）。
[[nodiscard]] auto ink_area(const Canvas& canvas) -> double {
  double total = 0.0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const st::math::Color pixel = canvas.pixel_at(x, y);
      const double lum =
          (0.299 * static_cast<double>(pixel.r) + 0.587 * static_cast<double>(pixel.g) +
           0.114 * static_cast<double>(pixel.b)) /
          255.0;
      total += (1.0 - lum);
    }
  }
  return total;
}

void report(const char* label, const Path& path, double expect_logical, float scale) {
  Canvas canvas = Canvas::for_logical_size(kLogicalW, kLogicalH, scale);
  canvas.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  canvas.fill_path(path, st::raster::Paint::solid(st::math::Color::rgb(0x00, 0x00, 0x00)));
  const double measured_physical = ink_area(canvas);
  const double expect_physical = expect_logical * static_cast<double>(scale) * static_cast<double>(scale);
  st::print("  {:<12} 实测 {:9.2f}   解析 {:9.2f}   比 {:.4f}   差 {:+.2f}\n", label,
            measured_physical, expect_physical, measured_physical / expect_physical,
            measured_physical - expect_physical);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const float scale = argc > 1 ? static_cast<float>(std::atof(argv[1])) : 2.0f;
  const st::math::Rect box{40.0f, 30.0f, 100.0f, 60.0f};
  constexpr float kRadius = 14.0f;
  constexpr float kWidth = 1.0f;
  const double w = 100.0;
  const double h = 60.0;

  st::print("=== 圆角环光栅化精度（逻辑 100×60，r=14，宽=1，scale={:.1f}）===\n",
            static_cast<double>(scale));

  Path outer;
  outer.add_rounded_rect(box, kRadius);
  report("外圈", outer, analytic_area(w, h, kRadius), scale);

  const st::math::Rect inner_box{box.x + kWidth, box.y + kWidth, box.width - 2.0f * kWidth,
                                 box.height - 2.0f * kWidth};
  Path inner;
  inner.add_rounded_rect(inner_box, kRadius - kWidth);
  report("内圈", inner,
         analytic_area(w - 2.0 * static_cast<double>(kWidth), h - 2.0 * static_cast<double>(kWidth),
                       static_cast<double>(kRadius) - static_cast<double>(kWidth)),
         scale);

  report("环(工厂)", st::raster::make_rounded_border_ring(box, kRadius, kWidth),
         analytic_ring(w, h, static_cast<double>(kRadius), static_cast<double>(kWidth)), scale);

  // —— 决定性对照：竖带 vs 斜带（几何完全已知，面积都是 w × len）——
  st::print("\n=== 对照：同宽度（1 逻辑px）的带，角度不同 ===\n");
  constexpr double kLen = 60.0;
  constexpr double kBandWidth = 1.0;
  for (int angle_deg : {0, 15, 30, 45, 60, 75, 90}) {
    const double th = angle_deg * kPi / 180.0;
    const double dx = std::cos(th) * kLen;
    const double dy = std::sin(th) * kLen;
    const double nx = -std::sin(th) * kBandWidth * 0.5;
    const double ny = std::cos(th) * kBandWidth * 0.5;
    const double x0 = 60.0;
    const double y0 = 100.0;
    Path band;
    band.move_to(st::math::Point{static_cast<float>(x0 + nx), static_cast<float>(y0 + ny)});
    band.line_to(st::math::Point{static_cast<float>(x0 + dx + nx), static_cast<float>(y0 + dy + ny)});
    band.line_to(st::math::Point{static_cast<float>(x0 + dx - nx), static_cast<float>(y0 + dy - ny)});
    band.line_to(st::math::Point{static_cast<float>(x0 - nx), static_cast<float>(y0 - ny)});
    band.close();
    Canvas canvas = Canvas::for_logical_size(kLogicalW, kLogicalH, scale);
    canvas.clear(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
    canvas.fill_path(band, st::raster::Paint::solid(st::math::Color::rgb(0x00, 0x00, 0x00)));
    const double measured = ink_area(canvas);
    const double expect = kLen * kBandWidth * static_cast<double>(scale) * static_cast<double>(scale);
    st::print("  {:>2}°  实测 {:9.2f}  解析 {:9.2f}  比 {:.4f}  差 {:+.2f}\n", angle_deg, measured,
              expect, measured / expect, measured - expect);
  }
  return 0;
}
