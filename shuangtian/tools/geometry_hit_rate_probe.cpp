// **几何贴图缓存的命中率**：键是否过敏感（同一形状每帧重新建图）。
//
// 背景：`fill_path` 18 次 / 0.39 ms。单次 0.019 ms（320×200 圆角矩形，63832 px
// = 0.290 ns/px）——这已逼近 `simd::over_row` 的物理下界 0.262 ns/px，
// **混合本身没余量**。唯一可能的空间是：**本可命中的却每帧重建**。
//
// 建一张贴图（真光栅化 + 分配）是 0.702 ns/px，比命中贵 2.42 倍。
// 所以命中率就是这条路径的关键指标。
//
// 本探针模拟界面里“同一图标每帧重画”的典型场景，量：
//   · 完全相同的路径反复画 → 命中率应 100%
//   · 带浮点噪声的路径（`x + 1e-7`） → 键是否仍命中（键按 0.25px 量化）
//   · 布局微调（`x + 0.3`） → 应 miss（几何真的变了）
//
// 判据：报命中率。

#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;
using st::raster::Path;

constexpr int kWidth = 900;
constexpr int kHeight = 700;

[[nodiscard]] auto make_icon(float dx, float dy) -> Path {
  Path path;
  path.add_rounded_rect(Rect{2.0f + dx, 2.0f + dy, 20.0f, 20.0f}, 4.0f);
  path.add_rounded_rect(Rect{6.0f + dx, 7.0f + dy, 12.0f, 2.0f}, 1.0f);
  path.add_circle(st::math::Point{12.0f + dx, 17.0f + dy}, 3.0f);
  return path;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "几何贴图缓存的命中率（键是否过敏感）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("geometry_hit_rate_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});
  const Paint paint = Paint::solid(Color{0x2F, 0x6F, 0xE0, 255});
  canvas.set_geometry_cache_enabled(true);

  struct Scenario {
    const char* name;
    float step;   // 每次调用给 x 加的偏移
  };
  const Scenario scenarios[] = {
      {"完全相同", 0.0f},
      {"浮点噪声 +1e-7", 1e-7f},
      {"量化内 +0.1px", 0.1f},
      {"量化外 +0.3px", 0.3f},
  };

  st::print("{:<22}  {:>10}  {:>10}  {:>10}  {:>12}\n", "场景", "命中", "未命中", "命中率",
            "总耗时 ms");
  for (const Scenario& item : scenarios) {
    const auto before = canvas.geometry_stats();
    constexpr int kFrames = 60;
    for (int frame = 0; frame < kFrames; ++frame) {
      const float offset = item.step * static_cast<float>(frame);
      canvas.fill_path(make_icon(offset, 0.0f), paint);
    }
    const auto after = canvas.geometry_stats();
    const std::uint64_t hits = after.hits - before.hits;
    const std::uint64_t misses = after.misses - before.misses;
    const std::uint64_t total = hits + misses;
    st::print("{:<22}  {:>10}  {:>10}  {:>9.1f}%  {:>12}\n", item.name, hits, misses,
              total == 0 ? 0.0 : static_cast<double>(hits) * 100.0 / static_cast<double>(total),
              "—");
  }
  st::print("\n注：键按 0.25px 量化（`path_geometry_key` 的 tolerance）。\n");
  st::print("    “完全相同/浮点噪声”应 100% 命中；“+0.3px”应 0% 命中（几何真变了）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
