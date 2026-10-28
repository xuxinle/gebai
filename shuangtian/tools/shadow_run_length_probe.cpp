// **阴影按 mask 值分组的可行性**：同 α 的像素在行内是否足够“成段”。
//
// 背景：`draw_shadow` 现在逐像素调 `over_premul`（变 α，用不上 `simd::blend_row`
// 那条“整段同 α”的快路径）。已知：
//   · 遮罩值只有 256 种，且 α=0 只有 16~27%（`shadow_mask_distribution_probe`）
//   · `simd::blend_row` 能把“整段同 α”的混合向量化
//
// 所以问题变成：**同 α 的像素在行内有多长？**
//   · 若每个 α 值的 run 平均 ≥ 16 像素 → 分组 + `blend_row` 能省下大部分算术
//   · 若 run 平均只有 1~2 像素 → 分组本身就是白花（开销 > 收益）
//
// 判据：报“每行内同 α 连续段的平均/中位长度”与“≥8 像素的像素占比”。

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "阴影遮罩的“同 α 连续段”长度（判断分组 + blend_row 是否值得）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_run_length_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  constexpr int kWidth = 1600;
  constexpr int kHeight = 1000;
  struct Params {
    const char* name;
    float width;
    float height;
    float radius;
    float blur;
  };
  const Params params[] = {
      {"卡片 320x160 r12 blur24", 320.0f, 160.0f, 12.0f, 24.0f},
      {"大面板 720x480 r16 blur32", 720.0f, 480.0f, 16.0f, 32.0f},
  };

  st::log::out("{:<26}  {:>10}  {:>12}  {:>12}  {:>14}\n", "参数", "段数", "平均段长",
            "中位段长", "≥8px 像素占比");
  for (const Params& item : params) {
    Canvas canvas(kWidth, kHeight, 1.0f);
    canvas.clear(Color{0, 0, 0, 0});
    const Rect rect{100.0f, 100.0f, item.width, item.height};
    canvas.draw_shadow(rect, item.radius, item.blur, Color{0, 0, 0, 90},
                       st::math::Point{0.0f, 4.0f});

    const float padding = item.blur * 2.0f + 2.0f;
    const Rect region{rect.x, rect.y + 4.0f, rect.width, item.height};
    const auto area =
        region.inflate(padding).round_out().intersect(st::math::IntRect{0, 0, kWidth, kHeight});

    std::vector<std::size_t> run_lengths;
    std::uint64_t pixels_total = 0;
    std::uint64_t pixels_in_long_runs = 0;
    for (int y = area.y; y < area.bottom(); ++y) {
      int run = 0;
      int previous = -1;
      const auto flush = [&] {
        if (run > 0) {
          run_lengths.push_back(static_cast<std::size_t>(run));
          pixels_total += static_cast<std::uint64_t>(run);
          if (run >= 8) pixels_in_long_runs += static_cast<std::uint64_t>(run);
        }
        run = 0;
      };
      for (int x = area.x; x < area.right(); ++x) {
        const int alpha = canvas.pixel_at(x, y).a;
        if (alpha == previous) {
          ++run;
        } else {
          flush();
          previous = alpha;
          run = 1;
        }
      }
      flush();
    }

    if (run_lengths.empty()) continue;
    std::uint64_t sum = 0;
    for (const auto value : run_lengths) sum += value;
    const double mean = static_cast<double>(sum) / static_cast<double>(run_lengths.size());
    auto sorted = run_lengths;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t median = sorted[sorted.size() / 2];
    const double long_pct = pixels_total == 0
                                ? 0.0
                                : static_cast<double>(pixels_in_long_runs) * 100.0 /
                                      static_cast<double>(pixels_total);
    st::log::out("{:<26}  {:>10}  {:>12.2f}  {:>12}  {:>13.1f}%\n", item.name, run_lengths.size(),
              mean, median, long_pct);
  }
  st::log::out("\n注：平均段长 ≥ 16 且 ≥8px 占比高 → 分组 + `blend_row` 有戏；\n");
  st::log::out("    若中位段长为 1（噪点式分布） → 分组白花，应该找其它路子。\n");
  return st::probe::main_of(spec, opts, verdict);
}
