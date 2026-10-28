// **单张卡片阴影 72 µs 的构成**——阴影是整帧最大的单项。
//
// ## 已确认的事实链
//
// 1. `docs/PAINT_DIAGNOSIS.md` §3.1：96 张卡片阴影 **14.0 ms/帧 → 6.9 ms/帧**
//    （合并贴图之后）。**6.9 ms / 96 = 72 µs/张**。
// 2. `tests/app_frame_budget_test.cpp` 实测：整帧（96 卡片带阴影）**43.6 ms**、
//    局部帧 **2.46 ms**——剔除有效（局部只画相交的 1~2 张）。
// 3. gallery 实测 `shadow` 7 次 **0.63 ms**（0.09 ms/次）——那是大卡片，单次更贵。
//
// 所以“阴影贵”不是缺陷，是**这个原语本身的性质**。但 72 µs/张 里
// **有多少是每次都重算的**（而不是缓存命中后纯贴图）？
//
// ## 这个探针量什么
//
// 同一几何的阴影重复画：
//   A) 首次（建遮罩 + 建贴图）
//   B) 之后（纯贴图，缓存命中）
//
// 若 A/B 接近 1.0×，说明**缓存没生效**（每次都在重建遮罩）——那是缺陷；
// 若 A ≫ B，说明缓存有效，72 µs 是“贴图成本”，只能靠减面积/减流量。
//
// 判据：报 A/B。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;

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
      .title = "单张卡片阴影的成本构成：建遮罩 vs 纯贴图",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_unit_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::raster::Canvas canvas(1280, 800, 1.0f);
  canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});

  // 与测试里的卡片同尺寸：140×48，圆角 8，两层阴影。
  const st::math::Rect card{40.0f, 40.0f, 140.0f, 48.0f};
  constexpr float kRadius = 8.0f;
  constexpr float kBlur = 12.0f;
  constexpr float kBlur2 = 24.0f;
  const Color key{0, 0, 0, 40};
  const Color ambient{0, 0, 0, 20};

  const auto one_shadow = [&] {
    canvas.draw_shadow_layered(card, kRadius, key, kBlur, st::math::Point{0.0f, 2.0f}, ambient,
                               kBlur2, st::math::Point{0.0f, 6.0f});
  };

  // 预热：建好贴图缓存
  for (int index = 0; index < 20; ++index) one_shadow();
  // B) 稳态（缓存命中）
  const double cached_ns = best_ns(one_shadow);

  // A) 每次都换位置 → 几何键变 → 必建贴图？
  //    注意：`draw_shadow_layered` 的贴图按**规范化坐标**缓存（与绘制位置无关），
  //    所以移动位置不该导致重建。这一项用来验证那句话。
  int shift = 0;
  const double moved_ns = best_ns([&] {
    ++shift;
    canvas.draw_shadow_layered(
        st::math::Rect{40.0f + static_cast<float>(shift % 7), 40.0f, 140.0f, 48.0f}, kRadius, key,
        kBlur, st::math::Point{0.0f, 2.0f}, ambient, kBlur2, st::math::Point{0.0f, 6.0f});
  });

  st::log::out("卡片 140×48 圆角 8 · 两层阴影（blur 12 / 24）\n\n");
  st::log::out("B 稳态（缓存命中）   {:>9.2f} µs/次\n", cached_ns / 1000.0);
  st::log::out("A 每次换位置          {:>9.2f} µs/次   → B/A = {:.2f}×\n", moved_ns / 1000.0,
            moved_ns > 0.0 ? cached_ns / moved_ns : 0.0);
  st::log::out("\n参考：测试里 96 张 = 6.9 ms → **{:.1f} µs/张**（文档 §3.1 实测）\n",
            6900.0 / 96.0);
  st::log::out("\n判读：\n");
  st::log::out("  · B 与“每张 µs”同量级 → 72 µs 就是**扁平贴图成本**，缓存已生效；\n");
  st::log::out("    要再快只能减面积（更小的 blur/外扩）或减流量（16 位像素）。\n");
  st::log::out("  · 若 B 远大于“每张 µs” → 每次都在重建遮罩（缺陷）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
