// **LCD 亚像素文字路径的成本构成**（这是 gallery 实际走的路径）。
//
// 关键事实：项目日志写着「文字渲染：LCD 亚像素（每像素 R/G/B 三重覆盖率）」，
// 而 `blend_coverage_row_subpixel` 的每个像素要：
//   ① 三次 `alpha_of()`：每次都 `abs()` + `× opacity` + `clamp01` + `×255+0.5` 浮点量化
//   ② 一次 `over_premul_lcd()`：内部 3 个通道 × **2 次 `fast_div255`** = 6 次
// 合计每像素 **12 次整数乘法除法 + 3 次浮点量化**——比灰度路径（1 次）重得多。
//
// 这解释了为什么 `text` 单项 0.59 ms 而只有 57k 像素：**单价高**，不是像素多。
//
// 本探针量：LCD 路径 vs 灰度路径的同位图成本比，以及当前实现的 ns/px。
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;

constexpr int kWidth = 800;
constexpr int kHeight = 600;
constexpr int kRounds = 200;

[[nodiscard]] auto best_ns(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    if (ns < best) best = ns;
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "LCD 亚像素文字 vs 灰度：同位图的成本比与 ns/px",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("lcd_text_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});
  const st::raster::Paint paint = st::raster::Paint::solid(Color{40, 44, 52, 255});

  constexpr int kGlyphW = 13;
  constexpr int kGlyphH = 13;
  const std::size_t pixel_count = static_cast<std::size_t>(kGlyphW) * kGlyphH;

  // 灰度覆盖率
  std::vector<float> gray(pixel_count, 0.0f);
  // LCD 覆盖率（每像素 3 个通道）
  std::vector<float> lcd(pixel_count * 3U, 0.0f);
  for (int y = 0; y < kGlyphH; ++y) {
    for (int x = 0; x < kGlyphW; ++x) {
      const double dx = (static_cast<double>(x) - 6.0) / 6.0;
      const double dy = (static_cast<double>(y) - 6.0) / 6.0;
      const double value = std::max(0.0, 1.0 - std::sqrt(dx * dx + dy * dy));
      const std::size_t index = static_cast<std::size_t>(y) * kGlyphW +
                                static_cast<std::size_t>(x);
      gray[index] = static_cast<float>(value);
      // 亚像素：三个通道略有差异（模拟真彩边）
      lcd[index * 3U + 0U] = static_cast<float>(value * 1.08);
      lcd[index * 3U + 1U] = static_cast<float>(value);
      lcd[index * 3U + 2U] = static_cast<float>(value * 0.92);
    }
  }

  const double gray_ns = best_ns([&] {
    canvas.blend_coverage_bitmap(10, 10, gray, kGlyphW, kGlyphH, paint, 1.0f,
                                 st::raster::BlendMode::SrcOver, 0,
                                 st::raster::CoverageFormat::Grayscale);
  });
  const double lcd_ns = best_ns([&] {
    canvas.blend_coverage_bitmap(10, 10, lcd, kGlyphW, kGlyphH, paint, 1.0f,
                                 st::raster::BlendMode::SrcOver, 0,
                                 st::raster::CoverageFormat::Lcd);
  });

  st::print("{}x{} 位图（{} 像素）\n\n", kGlyphW, kGlyphH, pixel_count);
  st::print("灰度路径    {:>8.1f} ns   → {:>6.2f} ns/px\n", gray_ns,
            gray_ns / static_cast<double>(pixel_count));
  st::print("LCD 路径    {:>8.1f} ns   → {:>6.2f} ns/px\n", lcd_ns,
            lcd_ns / static_cast<double>(pixel_count));
  st::print("\nLCD / 灰度 = **{:.2f}×**\n",
            gray_ns > 0.0 ? lcd_ns / gray_ns : 0.0);
  st::print("\n参考：短行 `over_row` 单价（`simd_short_row_probe`）：\n");
  st::print("  长 1~6 → 3.6~6.1 ns/px · 长 8 → 0.85 · 长 16 → 0.47\n");
  return st::probe::main_of(spec, opts, verdict);
}
