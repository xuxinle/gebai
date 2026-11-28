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
#include <vector>

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

/// 覆盖率 < 1 必须**真的画出墨**（而不只是“开表与关表一致”）。
///
/// 为何必须有这一条：上面那条只比“开表 / 关表”，两者走的是**同一个缺陷**时
/// 依旧全绿——实测就因此漏掉一个极重的缺陷：
/// `alpha_table_premul_` 的“未建表”哨兵原来是 `0xFFFFFFFF`，而那恰好是
/// **不透明白**的预乘值，于是第一次用不透明白绘字时 `premul` 与哨兵相等、
/// 直接返回**全 0 的空表**——所有覆盖率 < 0.999 的像素被写成 0。
///
/// 现象：手贴一张 4×32、覆盖率全 0.5 的图，**画布上一个非零像素都没有**；
/// 而覆盖率 1.0 正常（它走 SIMD 快路径，不查表）。
/// 落到真实界面就是：**没有满格像素的字形（绝大多数汉字、细笔画拉丁字）整字不显示**，
/// 只剩「一」「口」这类含满格的字能看出墨。
ST_TEST(coverage_below_full_actually_paints) {
  // 白色不透明是触发那个哨兵冲突的颜色（预乘值恰为 0xFFFFFFFF）。
  const Color colors[] = {
      Color{0xFF, 0xFF, 0xFF, 0xFF},   // 预乘 = 0xFFFFFFFF（原哨兵）
      Color{40, 44, 52, 255},           // 普通深色
  };
  for (const Color color : colors) {
    for (const float coverage : {0.05f, 0.25f, 0.5f, 0.75f, 0.999f}) {
      Canvas canvas = make_canvas();
      const std::vector<float> one{coverage};
      canvas.blend_coverage_bitmap(20, 20, one, 1, 1, Paint::solid(color), 1.0f,
                                   st::raster::BlendMode::SrcOver, 0,
                                   st::raster::CoverageFormat::Grayscale);
      const Color before = Color::rgb(0xF7, 0xF8, 0xFA);
      const Color after = canvas.pixel_at(20, 20);
      const int delta = std::abs(static_cast<int>(after.r) - static_cast<int>(before.r)) +
                        std::abs(static_cast<int>(after.g) - static_cast<int>(before.g)) +
                        std::abs(static_cast<int>(after.b) - static_cast<int>(before.b));
      st::log::out("[coverage] color=({},{},{}) cov={}：像素 ({},{},{})\n",
                   static_cast<int>(color.r), static_cast<int>(color.g),
                   static_cast<int>(color.b), static_cast<double>(coverage),
                   static_cast<int>(after.r), static_cast<int>(after.g),
                   static_cast<int>(after.b));
      // 反例（缺陷态）：像素**一点没变**（delta=0）——那是“表空、根本没写”。
      //
      // ⚠ 不能要求每个覆盖率都变：白字 × 0.05 覆盖率叠在 0xF7 浅底上，
      // 理论值就差 0.4 LSB，量化后仍是原值——**那是正确行为**（实测：
      //   白字 cov=0.05 → (247,248,250) 与底色相同）。
      // 所以只对“理应看出差异”的组合断言：深色任意覆盖率、白字覆盖率 ≥ 0.5。
      const bool visible = color.r == 0xFF && color.g == 0xFF && color.b == 0xFF
                               ? coverage >= 0.5f
                               : true;
      if (visible) ST_CHECK(delta > 0);
    }
  }
}
