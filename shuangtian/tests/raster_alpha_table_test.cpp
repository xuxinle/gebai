// **`blend_coverage_row` 的查表必须与逐像素 `scale_premul` 逐像素相同**。
//
// 这条改动把逐像素的 `scale_premul(premul, abandoned_byte)` 换成
// **一张按颜色缓存的 256 项表**（`Canvas::alpha_table`）。
//
// 为什么必须有这条测试：表一旦把 opacity 也烧进去（“优化”想法很自然），
// 量化就从**一次**变成**两次**，实测（`tools/alpha_table_equiv_probe.cpp`）
// 在 opacity=0.9/0.75/0.5 时有 **19~25%** 的覆盖率组合会差 1 LSB——
// 而本项目像素是零容差。这条测试把“开表 / 关表”钉死在同一进程里对比。
//
// 开关：`Canvas::set_alpha_table_enabled(false)` 走逐像素原式。

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;
using st::raster::Paint;

[[nodiscard]] auto make_canvas() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(160, 120, 1.0f);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

/// 逐像素比两张画布，返回 (差异像素数, 最大通道差)。
[[nodiscard]] auto diff_of(const Canvas& lhs, const Canvas& rhs) -> std::pair<int, int> {
  int differing = 0;
  int worst = 0;
  for (int y = 0; y < lhs.physical_height(); ++y) {
    for (int x = 0; x < lhs.physical_width(); ++x) {
      const Color a = lhs.pixel_at(x, y);
      const Color b = rhs.pixel_at(x, y);
      const int delta = std::max({std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                                  std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                                  std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)),
                                  std::abs(static_cast<int>(a.a) - static_cast<int>(b.a))});
      if (delta == 0) continue;
      ++differing;
      worst = std::max(worst, delta);
    }
  }
  return {differing, worst};
}

}  // namespace

ST_TEST(alpha_table_matches_per_pixel_scaling) {
  // 覆盖率来源：一个圆形（边缘覆盖率连续变化，正好覆盖 0~1 全域）。
  // 比用矩形好——矩形的内部全是满格（走 SIMD 快路径），测不到查表那段。
  struct Case {
    const char* name;
    float opacity;
    Color color;
  };
  const Case cases[] = {
      {"不透明", 1.0f, Color{40, 44, 52, 255}},
      {"opacity 0.9", 0.9f, Color{40, 44, 52, 255}},
      {"opacity 0.75", 0.75f, Color{40, 44, 52, 255}},
      {"opacity 0.5", 0.5f, Color{40, 44, 52, 255}},
      {"opacity 0.35", 0.35f, Color{40, 44, 52, 255}},
      {"半透明色", 1.0f, Color{0x2F, 0x6F, 0xE0, 128}},
      {"半透明色 × 0.6", 0.6f, Color{0x2F, 0x6F, 0xE0, 128}},
  };

  for (const Case& item : cases) {
    const Paint paint = Paint::solid(item.color);

    Canvas with_table = make_canvas();
    with_table.set_alpha_table_enabled(true);
    with_table.fill_circle(st::math::Point{60.0f, 50.0f}, 34.0f, paint, {.opacity = item.opacity});
    with_table.fill_path(
        [] {
          st::math::Path path;
          path.add_rounded_rect(st::math::Rect{80.0f, 60.0f, 60.0f, 40.0f}, 12.0f);
          return path;
        }(),
        paint, {.opacity = item.opacity});

    Canvas per_pixel = make_canvas();
    per_pixel.set_alpha_table_enabled(false);
    per_pixel.fill_circle(st::math::Point{60.0f, 50.0f}, 34.0f, paint, {.opacity = item.opacity});
    per_pixel.fill_path(
        [] {
          st::math::Path path;
          path.add_rounded_rect(st::math::Rect{80.0f, 60.0f, 60.0f, 40.0f}, 12.0f);
          return path;
        }(),
        paint, {.opacity = item.opacity});

    const auto [differing, worst] = diff_of(with_table, per_pixel);
    st::log::out("[alpha-table] {}：差异 {} px（Δ≤{}）\n", item.name, differing, worst);
    ST_CHECK(differing == 0);
  }
}
