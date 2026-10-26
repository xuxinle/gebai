// **`fill_path` 的像素单价**：它到底画了多少像素、单价多少。
//
// 背景：gallery 里 `fill_path` 18 次 / **0.42 ms**，但 profile 报“0 像素”——
// `rasterize_path_runs` 那条路没把像素数记进统计（`scope.set_pixels` 只覆盖
// 几何贴图那一支）。所以无法从既有数据判断“是像素多还是单价高”。
//
// 本探针直接量：
//   · 一条大圆角矩形（320×200）命中缓存时的实际触达像素与单价
//   · 同形状换成渐变（缓存不适用，必须真光栅化）的单价
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;
using st::math::Path;

constexpr int kWidth = 1200;
constexpr int kHeight = 900;

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "fill_path 的实际触达像素与单价（缓存 vs 真光栅化）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("fill_path_pixels_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});

  const Path big = [] {
    Path path;
    path.add_rounded_rect(Rect{40.0f, 40.0f, 320.0f, 200.0f}, 16.0f);
    return path;
  }();
  const Paint solid_paint = Paint::solid(Color{0x2F, 0x6F, 0xE0, 255});

  // 量“实际触达像素”：画在透明底上，数非零 alpha。
  const auto measure_pixels = [&](const std::function<void(Canvas&)>& draw) -> std::uint64_t {
    Canvas probe(kWidth, kHeight, 1.0f);
    probe.clear(Color{0, 0, 0, 0});
    draw(probe);
    std::uint64_t count = 0;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        if (probe.pixel_at(x, y).a != 0) ++count;
      }
    }
    return count;
  };

  const std::uint64_t pixels =
      measure_pixels([&](Canvas& target) { target.fill_path(big, solid_paint); });

  // 计时：缓存命中（预热后）
  canvas.set_geometry_cache_enabled(true);
  for (int index = 0; index < 20; ++index) canvas.fill_path(big, solid_paint);
  double cached_ns = 1e18;
  for (int round = 0; round < 200; ++round) {
    const auto start = std::chrono::steady_clock::now();
    canvas.fill_path(big, solid_paint);
    const auto end = std::chrono::steady_clock::now();
    cached_ns = std::min(cached_ns, std::chrono::duration<double, std::nano>(end - start).count());
  }

  // 计时：缓存关闭（真光栅化）
  canvas.set_geometry_cache_enabled(false);
  double plain_ns = 1e18;
  for (int round = 0; round < 120; ++round) {
    const auto start = std::chrono::steady_clock::now();
    canvas.fill_path(big, solid_paint);
    const auto end = std::chrono::steady_clock::now();
    plain_ns = std::min(plain_ns, std::chrono::duration<double, std::nano>(end - start).count());
  }

  st::print("圆角矩形 320×200 r16（触达 {} 像素）\n\n", pixels);
  st::print("  缓存命中    {:>9.0f} ns   → {:>7.3f} ns/px\n", cached_ns,
            pixels == 0 ? 0.0 : cached_ns / static_cast<double>(pixels));
  st::print("  真光栅化    {:>9.0f} ns   → {:>7.3f} ns/px\n", plain_ns,
            pixels == 0 ? 0.0 : plain_ns / static_cast<double>(pixels));
  st::print("\n缓存加速 {:.2f}×\n", cached_ns > 0.0 ? plain_ns / cached_ns : 0.0);
  st::print("\n参考：`fill_rect`（直角大面）实测 0.0061 ms / 1.42 M px = 0.0043 ns/px；\n");
  st::print("      `round_rect` 面板 0.35 ns/px。\n");
  return st::probe::main_of(spec, opts, verdict);
}
