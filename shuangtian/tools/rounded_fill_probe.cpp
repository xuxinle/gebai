// 圆角填充 vs 直角填充的**成本对照探针**（release 档跑）。
//
// ## 为什么需要它（2026-10-14）
//
// `tests/raster_bench_test.cpp` 的 `bench_rounded_card_fill` 在 release 档下实测
// **18.84× 清屏单位**，而阈值是 **3.0×**（该阈值由注释记录的"实测 ≈1.35×"定出）。
// 同一次运行里其它三项**都吻合设计值**（渐变 1.45× vs 1.6×、投影 8.80× vs 10.3×、
// 文本 24.95× vs 23×）——所以这不是"机器慢"，而是**圆角这条路本身**的问题：
// 比值是相对量，机器慢会同时抬高分子与分母。
//
// 本探针把"同面积、同位置"的直角填充与圆角填充放在一起量，回答一个问题：
// **多出来的成本是不是"圆角"这个几何特性本身**（若是，说明该路径缺专用扫描线；
// 若不是，说明是外壳开销）。
//
// 用法：rounded_fill_probe（无参数；release 档有意义）

#include <chrono>
#include <cstdio>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kCanvasWidth = 1280;
constexpr int kCanvasHeight = 800;

[[nodiscard]] auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Measure>
[[nodiscard]] auto best_of(Measure&& measure, int repeats = 5) -> double {
  double best = 1e9;
  for (int index = 0; index < repeats; ++index) best = std::min(best, measure());
  return best;
}

/// 清屏每像素成本（与 bench 同一个分母口径）。
[[nodiscard]] auto baseline_ms_per_pixel() -> double {
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kCanvasHeight, 1.0f);
  double best = 1e9;
  for (int index = 0; index < 7; ++index) {
    const auto start = Clock::now();
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    best = std::min(best, elapsed_ms(start));
  }
  return best / (static_cast<double>(kCanvasWidth) * static_cast<double>(kCanvasHeight));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "圆角填充 vs 直角填充的**成本对照探针**（release 档跑）。",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("rounded_fill_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const double unit = baseline_ms_per_pixel();
  const st::raster::Paint paint =
      st::raster::Paint::solid(st::math::Color::rgb(0x2F, 0x6F, 0xE0));
  constexpr int kCards = 60;
  constexpr double kCardWidth = 320.0;
  constexpr double kCardHeight = 160.0;

  // ① 圆角（与 bench 完全相同的几何）
  const double rounded_ms = best_of([&] {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kCanvasHeight, 1.0f);
    const auto start = Clock::now();
    for (int index = 0; index < kCards; ++index) {
      const float x = static_cast<float>(index % 6) * 200.0f + 20.0f;
      const float y = static_cast<float>(index / 6) * 130.0f + 20.0f;
      canvas.fill_rect(
          st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)}, paint,
          12.0f);
    }
    return elapsed_ms(start);
  });

  // ② 直角（同几何、radius=0 → 走 simple 的整行 span 快路径）
  const double square_ms = best_of([&] {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kCanvasHeight, 1.0f);
    const auto start = Clock::now();
    for (int index = 0; index < kCards; ++index) {
      const float x = static_cast<float>(index % 6) * 200.0f + 20.0f;
      const float y = static_cast<float>(index / 6) * 130.0f + 20.0f;
      canvas.fill_rect(
          st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)}, paint,
          0.0f);
    }
    return elapsed_ms(start);
  });

  // ③ 同几何但**非整数对齐**的直角：走 `fill_path_aa`（与圆角同一条路径），
  //    用来区分"是 AA 路径慢"还是"是圆角几何慢"。
  const double square_path_ms = best_of([&] {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kCanvasHeight, 1.0f);
    const auto start = Clock::now();
    for (int index = 0; index < kCards; ++index) {
      const float x = static_cast<float>(index % 6) * 200.0f + 20.3f;   // 故意非整数对齐
      const float y = static_cast<float>(index / 6) * 130.0f + 20.3f;
      canvas.fill_rect(
          st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)}, paint,
          0.0f);
    }
    return elapsed_ms(start);
  });

  const double area = kCardWidth * kCardHeight;
  const auto report = [&](const char* name, double ms) {
    const double per_card = ms / static_cast<double>(kCards);
    st::print("{:<28} {:8.2f} ms（{:.3f} ms/张，比值 {:6.2f}× 清屏单位）\n", name, ms, per_card,
              per_card / area / unit);
  };
  st::print("清屏每像素 {:.6f} ms（{:.3f} ms 整屏）\n", unit, unit * kCanvasWidth * kCanvasHeight);
  report("圆角 radius=12（bench 几何）", rounded_ms);
  report("直角 radius=0（整数对齐）", square_ms);
  report("直角 radius=0（非对齐）", square_path_ms);
  st::print("\n比值口径与 tests/raster_bench_test.cpp 一致（每张卡片每像素 / 清屏每像素）\n");
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
