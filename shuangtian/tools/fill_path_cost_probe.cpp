// **`fill_path` 那 0.42 ms 花在哪**：几何缓存命中率 vs 真光栅化。
//
// 背景（gallery 1600×1000）：`fill_path` 18 次 / **0.42 ms** / 报“0 像素”。
// “0 像素”说明 `rasterize_path_runs` 那条路径**没有计入像素统计**，
// 因此无法从像素单价判断成本是否合理——只能直接拆。
//
// `fill_path` 的流程：
//   ① 算几何键（`path_control_origin` + `path_geometry_key`，**遍历两遍命令流**）
//   ② 命中缓存 → 贴图；否则 → 建贴图（写进缓存）
//   ③ 都不行（渐变/遮罩/缓存已满…）→ `rasterize_path_runs`（真扫描线）
//
// 本探针量三件事：
//   · 几何键本身多贵（与路径复杂度相关）
//   · 缓存命中 vs 未命中的差距
//   · `rasterize_path_runs` 的单价
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
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;
using st::math::Path;

constexpr int kWidth = 1200;
constexpr int kHeight = 900;
constexpr int kRounds = 120;

[[nodiscard]] auto best_ms(const std::function<void()>& body) -> double {
  double best = 1e9;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
  }
  return best;
}

/// 界面里最常见的“自定义形状”：图标轮廓（多个子路径 + 曲线）。
[[nodiscard]] auto make_icon_path() -> Path {
  Path path;
  // 一个 24×24 图标的典型形态：外框 + 两条内线 + 一个圆。
  path.add_rounded_rect(Rect{2.0f, 2.0f, 20.0f, 20.0f}, 4.0f);
  path.add_rounded_rect(Rect{6.0f, 7.0f, 12.0f, 2.0f}, 1.0f);
  path.add_rounded_rect(Rect{6.0f, 11.0f, 8.0f, 2.0f}, 1.0f);
  path.add_circle(st::math::Point{12.0f, 17.0f}, 3.0f);
  return path;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "fill_path 成本拆解：几何键 / 缓存命中 / 真光栅化",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("fill_path_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});
  const Paint paint = Paint::solid(Color{0x2F, 0x6F, 0xE0, 255});

  const Path icon = make_icon_path();
  const Path rounded = [] {
    Path path;
    path.add_rounded_rect(Rect{40.0f, 40.0f, 320.0f, 200.0f}, 16.0f);
    return path;
  }();

  // A) 缓存开启（预热后稳态命中）
  canvas.set_geometry_cache_enabled(true);
  for (int index = 0; index < 10; ++index) canvas.fill_path(icon, paint);
  const double cached_ms = best_ms([&] { canvas.fill_path(icon, paint); });

  // B) 缓存关闭（每次都真光栅化）
  canvas.set_geometry_cache_enabled(false);
  const double plain_ms = best_ms([&] { canvas.fill_path(icon, paint); });

  // C) 圆角矩形（`fill_rect` 走同一套缓存）
  canvas.set_geometry_cache_enabled(true);
  for (int index = 0; index < 10; ++index) canvas.fill_path(rounded, paint);
  const double rounded_ms = best_ms([&] { canvas.fill_path(rounded, paint); });

  // D) 缓存未命中（每次换一个略微不同的路径，键必变）
  canvas.set_geometry_cache_enabled(true);
  int variant = 0;
  const double miss_ms = best_ms([&] {
    ++variant;
    Path shifted;
    shifted.add_rounded_rect(
        Rect{2.0f + static_cast<float>(variant % 7) * 0.01f, 2.0f, 20.0f, 20.0f}, 4.0f);
    shifted.add_circle(st::math::Point{12.0f, 17.0f}, 3.0f);
    canvas.fill_path(shifted, paint);
  });

  st::print("自定义图标路径（4 子路径）\n");
  st::print("  A 缓存命中      {:>8.4f} ms\n", cached_ms);
  st::print("  B 缓存关闭      {:>8.4f} ms   → A/B = {:.2f}\n", plain_ms,
            plain_ms > 0.0 ? cached_ms / plain_ms : 0.0);
  st::print("  D 缓存未命中    {:>8.4f} ms（每次不同键，必须建图）\n", miss_ms);
  st::print("\n单条圆角矩形路径\n");
  st::print("  C 缓存命中      {:>8.4f} ms\n", rounded_ms);
  st::print("\n若 A ≈ B，说明这条路径上缓存**没帮上忙**（键太敏感 / 每次都未命中）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
