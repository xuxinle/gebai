/// 渐变采样的快路径测试。
///
/// 背景：`blend_coverage_runs` 的渐变分支有三条路径——
/// ① 竖直线性渐变：整行同色 + SIMD 整行混合；② 倾斜线性：位置参数沿行递推；
/// ③ 径向/扫掠：逐像素采样。快路径必须与"逐像素调用 `Paint::sample`"的
/// 参考语义**逐像素一致**（容差极小）——这是 54× 提速（bench 实测）不改变
/// 渲染结果的硬保证。
#include <algorithm>
#include <cmath>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Gradient;
using st::raster::GradientStop;
using st::raster::Paint;

/// 用参考采样器（`Paint::sample`，即快路径优化前的逐像素语义）逐像素对比画布。
/// 只比对内部区域（避开抗锯齿边缘与圆角），容差 ±2（浮点路径差异）。
void expect_matches_reference(Canvas& canvas, const Paint& paint, Rect rect) {
  int worst = 0;
  int worst_x = 0;
  int worst_y = 0;
  for (int y = static_cast<int>(rect.y) + 2; y < static_cast<int>(rect.bottom()) - 2; ++y) {
    for (int x = static_cast<int>(rect.x) + 2; x < static_cast<int>(rect.right()) - 2; ++x) {
      const Color reference = paint.sample(Point{static_cast<float>(x) + 0.5f,
                                                 static_cast<float>(y) + 0.5f});
      const Color actual = canvas.pixel_at(x, y);
      const int delta = std::max({std::abs(static_cast<int>(reference.r) - actual.r),
                                  std::abs(static_cast<int>(reference.g) - actual.g),
                                  std::abs(static_cast<int>(reference.b) - actual.b),
                                  std::abs(static_cast<int>(reference.a) - actual.a)});
      if (delta > worst) {
        worst = delta;
        worst_x = x;
        worst_y = y;
      }
    }
  }
  if (worst > 2) {
    st::log::out("  最大偏差 {} @ ({}, {})\n", worst, worst_x, worst_y);
  }
  ST_CHECK(worst <= 2);
}

std::vector<GradientStop> two_stops() {
  return {{0.0f, Color{0x3B, 0x82, 0xF6, 0xFF}}, {1.0f, Color{0x93, 0xC5, 0xFD, 0xFF}}};
}

}  // namespace

ST_TEST(raster_gradient_vertical_fast_path_matches_reference) {
  Canvas canvas(160, 96);
  canvas.clear(Color{0, 0, 0, 0xFF});
  const Paint paint = Paint::with_gradient(
      Gradient::linear(Point{0.0f, 0.0f}, Point{0.0f, 96.0f}, two_stops()));
  canvas.fill_rect(Rect{8.0f, 8.0f, 144.0f, 80.0f}, paint);
  expect_matches_reference(canvas, paint, Rect{8.0f, 8.0f, 144.0f, 80.0f});
}

ST_TEST(raster_gradient_inclined_fast_path_matches_reference) {
  Canvas canvas(160, 96);
  canvas.clear(Color{0, 0, 0, 0xFF});
  // 倾斜：dy≠0 且 dx≠0 → 走"位置沿行递推"路径
  const Paint paint = Paint::with_gradient(
      Gradient::linear(Point{0.0f, 0.0f}, Point{160.0f, 96.0f}, two_stops()));
  canvas.fill_rect(Rect{8.0f, 8.0f, 144.0f, 80.0f}, paint);
  expect_matches_reference(canvas, paint, Rect{8.0f, 8.0f, 144.0f, 80.0f});
}

/// 横向渐变**起点不在 x=0** 时的快路径（暗角右带就是这个形态）。
///
/// 上方两个用例都从 `from.x = 0` 起，于是位置参数少算一项也仍对——
/// 实测代价：`vignette` 的右带 `from = (w, 0)` → `to = (w-band, 0)`，
/// 漏掉 `-from.x` 后 t 恒 ≥ 1，整条带被填成**端点色**，画面右侧出现一道
/// 112px 宽、亮度恒定的硬边，而左侧正常。只看左边永远发现不了。
///
/// 两端都用**不透明**色：`expect_matches_reference` 比的是
/// `Paint::sample()`（未混合的源色）与 `pixel_at()`（已混合的结果），
/// 只有 alpha 恒为 255 时两者才同义。
ST_TEST(raster_gradient_horizontal_offset_origin_matches_reference) {
  Canvas canvas(160, 96);
  canvas.clear(Color{0x80, 0x80, 0x80, 0xFF});
  // 从画布右缘向左 40px：黑 → 白（与暗角带同形的**反向**朝向）。
  const Paint paint = Paint::with_gradient(
      Gradient::linear(Point{160.0f, 0.0f}, Point{120.0f, 0.0f},
                       {{0.0f, Color{0x00, 0x00, 0x00, 0xFF}},
                        {1.0f, Color{0xFF, 0xFF, 0xFF, 0xFF}}}));
  canvas.fill_rect(Rect{120.0f, 0.0f, 40.0f, 96.0f}, paint);
  expect_matches_reference(canvas, paint, Rect{120.0f, 0.0f, 40.0f, 96.0f});
  // 方向性断言：外缘黑、内侧白（整条恒色会让两者相等）
  const Color outer = canvas.pixel_at(158, 48);
  const Color inner = canvas.pixel_at(122, 48);
  ST_CHECK(outer.r + 60 < inner.r);
}

ST_TEST(raster_gradient_radial_and_sweep_match_reference) {
  {
    Canvas canvas(96, 96);
    canvas.clear(Color{0, 0, 0, 0xFF});
    const Paint paint = Paint::with_gradient(
        Gradient::radial(Point{48.0f, 48.0f}, 40.0f, two_stops()));
    canvas.fill_rect(Rect{4.0f, 4.0f, 88.0f, 88.0f}, paint);
    expect_matches_reference(canvas, paint, Rect{4.0f, 4.0f, 88.0f, 88.0f});
  }
  {
    Canvas canvas(96, 96);
    canvas.clear(Color{0, 0, 0, 0xFF});
    const Paint paint =
        Paint::with_gradient(Gradient::sweep(Point{48.0f, 48.0f}, two_stops()));
    canvas.fill_rect(Rect{4.0f, 4.0f, 88.0f, 88.0f}, paint);
    expect_matches_reference(canvas, paint, Rect{4.0f, 4.0f, 88.0f, 88.0f});
  }
}
