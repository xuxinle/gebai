// **几何缓存按颜色分家的代价**：同形状不同颜色会各建一张贴图。
//
// 背景：`try_geometry_sprite` 的键里含**颜色**（贴图存的是预乘色，颜色必须烘进去），
// 于是「同一图标/圆角在两种颜色下各画一次」= 两个缓存条目 + 两次建图。
//
// 实测 gallery：**命中 64.7% / 未命中 35.3%**，而在纯路径绘制里未命中率高达 ~79%
// —— 同一形状反复以不同颜色出现（正常态/悬停态/禁用态、不同层级的文字色）
// 是真实界面的常态。
//
// 本探针量化这个浪费：同一批形状，**每轮换一个颜色**画 N 次，对比
//   A) 当前设计：颜色进键 → 每个颜色一次建图
//   B) 若改成“贴图只存覆盖率”：同一几何只建一次，颜色在贴出时乘
//
// 判据：只报数（给架构改动一个可信的收益上限）。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;

constexpr int kWidth = 1600;
constexpr int kHeight = 1000;
constexpr int kRounds = 40;

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

/// 一组典型控件形状（尺寸取真实界面常见值）。
struct Shape {
  float width;
  float height;
  float radius;
};
const Shape kShapes[] = {
    {72.0f, 28.0f, 6.0f},   {96.0f, 32.0f, 6.0f},   {120.0f, 30.0f, 8.0f},
    {320.0f, 160.0f, 12.0f}, {240.0f, 48.0f, 8.0f},  {720.0f, 480.0f, 16.0f},
};

/// 模拟一帧：每个形状用 `color_count` 种不同颜色各画一次。
auto draw_frame(Canvas& canvas, int color_count) -> void {
  canvas.clear(Color::rgb(0xF0, 0xF0, 0xF4));
  float y = 20.0f;
  for (const Shape& shape : kShapes) {
    for (int color_index = 0; color_index < color_count; ++color_index) {
      // 同形状、不同颜色（正常/悬停/禁用/不同层级文字色…）
      const auto tint = static_cast<std::uint8_t>(0x80 + color_index * 40);
      canvas.fill_rect(Rect{20.0f, y, shape.width, shape.height},
                       Paint::solid(Color{tint, tint, tint, 255}), shape.radius);
      y += 6.0f;
      if (y > kHeight - 60.0f) y = 20.0f;
    }
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "几何缓存按颜色分家的代价（为“贴图只存覆盖率”的改法提供收益上限）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("sprite_color_key_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::log::out("同一批形状 × N 种颜色（每帧都画一遍）\n\n");
  st::log::out("{:>10}  {:>12}  {:>10}  {:>10}  {:>10}  {:>14}\n", "颜色数", "每帧耗时 ms", "hits",
            "misses", "rejected", "未命中占比");
  for (const int colors : {1, 2, 4, 8}) {
    Canvas canvas(kWidth, kHeight, 1.0f);
    // 预热两帧（第一帧建图，第二帧全命中）。
    draw_frame(canvas, colors);
    draw_frame(canvas, colors);
    const auto before = canvas.geometry_stats();

    const double ms = best_ms([&] { draw_frame(canvas, colors); });
    const auto after = canvas.geometry_stats();
    const std::uint64_t add_hits = after.hits - before.hits;
    const std::uint64_t add_misses = after.misses - before.misses;
    const std::uint64_t add_total = add_hits + add_misses;
    st::log::out("{:>10}  {:>12.3f}  {:>10}  {:>10}  {:>10}  {:>13.1f}%\n", colors, ms, add_hits,
              add_misses, after.rejected - before.rejected,
              add_total == 0 ? 0.0
                             : static_cast<double>(add_misses) /
                                   static_cast<double>(add_total) * 100.0);
  }
  st::log::out("\n注：若“条目数”随颜色数线性增长，说明同一几何在为每种颜色重复建图——\n");
  st::log::out("    改成“贴图只存覆盖率、颜色在贴出时乘”可把条目数压回形状数。\n");
  return st::probe::main_of(spec, opts, verdict);
}
