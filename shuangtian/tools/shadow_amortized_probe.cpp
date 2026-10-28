// **96 张卡片阴影：单张 5.8 µs vs 实测 71.9 µs —— 差 12.4 倍的来源**。
//
// ## 矛盾
//
// · `shadow_unit_cost_probe`：单张 140×48 卡片两层阴影（缓存命中）**5.80 µs**；
// · `docs/PAINT_DIAGNOSIS.md` §3.1 + `tests/app_frame_budget_test.cpp`：
//   96 张 = 6.9 ms → **71.9 µs/张**。
//
// 同尺寸、同阴影参数，**差 12.4 倍**。只有三种可能：
//   ① 96 张时缓存**命中率低**（每张都重建遮罩/贴图）；
//   ② 贴到画布上时目标区域**更大**（卡片间距导致贴图覆盖重叠？）不对——尺寸相同；
//   ③ 缓存有**容量上限**，96 张把缓存挤爆、不断驱逐重建。
//
// ## 这个探针量什么
//
// 逐张增加卡片数（1 / 4 / 16 / 32 / 96），量**总耗时 / 张数**（每张摊销）：
//
//   · 若每张摊销随数量**上升** → 缓存容量/命中率问题（可修）；
//   · 若恒定 ≈ 5.8 µs → 文档那个 71.9 µs 是**旧版本**的数字（已过时，应更新文档）；
//   · 若恒定但高于 5.8 → 差异在“同屏 96 张”的某些共享资源上。
//
// 判据：报摊销曲线。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 120;
constexpr int kWidth = 1280;
constexpr int kHeight = 800;
constexpr int kColumns = 8;
constexpr float kCardWidth = 140.0f;
constexpr float kCardHeight = 48.0f;
constexpr float kGapX = 8.0f;
constexpr float kGapY = 12.0f;
constexpr float kRadius = 8.0f;

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

/// 按行列排布的卡片位置（与 `build_representative_ui` 同布局）。
[[nodiscard]] auto layout_cards(int count) -> std::vector<st::math::Rect> {
  std::vector<st::math::Rect> rects;
  rects.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const int row = index / kColumns;
    const int column = index % kColumns;
    rects.push_back(st::math::Rect{40.0f + static_cast<float>(column) * (kCardWidth + kGapX),
                                   40.0f + static_cast<float>(row) * (kCardHeight + kGapY),
                                   kCardWidth, kCardHeight});
  }
  return rects;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "96 张卡片阴影的摊销成本（找 12.4 倍差异的来源）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_amortized_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});

  const Color key{0, 0, 0, 40};
  const Color ambient{0, 0, 0, 20};

  st::log::out("{:>8}  {:>12}  {:>14}  {:>12}\n", "张数", "总耗时", "每张摊销", "相对单张");

  const std::vector<st::math::Rect> all_cards = layout_cards(96);
  const double single_ns = 5.80e3;   // 单张实测基线（`shadow_unit_cost_probe`）

  for (const int count : {1, 4, 16, 32, 64, 96}) {
    // 预热（让该数量的遮罩/贴图都进缓存）
    for (int index = 0; index < count; ++index) {
      const auto& rect = all_cards[static_cast<std::size_t>(index)];
      canvas.draw_shadow_layered(rect, kRadius, key, 12.0f, st::math::Point{0.0f, 2.0f}, ambient,
                                 24.0f, st::math::Point{0.0f, 6.0f});
    }
    const double total_ns = best_ns([&] {
      for (int index = 0; index < count; ++index) {
        const auto& rect = all_cards[static_cast<std::size_t>(index)];
        canvas.draw_shadow_layered(rect, kRadius, key, 12.0f, st::math::Point{0.0f, 2.0f},
                                   ambient, 24.0f, st::math::Point{0.0f, 6.0f});
      }
    });
    const double per = total_ns / static_cast<double>(count);
    st::log::out("{:>8}  {:>9.1f} µs  {:>11.2f} µs  {:>10.2f}×\n", count, total_ns / 1000.0,
              per / 1000.0, per / single_ns);
  }

  st::log::out("\n判读：\n");
  st::log::out("  · 每张摊销随张数**上升** → 缓存容量/命中率问题，值一轮修复；\n");
  st::log::out("  · 恒定 ≈ 5.8 µs → 文档 §3.1 的“71.9 µs/张”是**旧数字**，应更新；\n");
  st::log::out("  · 恒定但明显高于 5.8 → 差异在“同屏 96 张”的共享资源上。\n");
  return st::probe::main_of(spec, opts, verdict);
}
