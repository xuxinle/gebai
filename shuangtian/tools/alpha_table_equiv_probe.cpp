// **两步量化 vs 一步量化**：`blend_coverage_row` 改查表后是否改变像素。
//
// 背景：为砍掉逐像素 `scale_premul`，`blend_coverage_row` 改成查表
// `table[k] = scale_premul(premul, quantize(k/255 × opacity))`，
// 而**旧写法**是 `scale_premul(premul, quantize(|cov| × opacity))`。
//
// 差别：新写法把「覆盖率」先量化成字节（`k`），再在表里乘 opacity 并**再量化一次**；
// 旧写法是 `cov × opacity` 后**只量化一次**。两步量化在半格处必然与一步差 1。
//
// 这个探针穷举实际取值域，量出：
//   ① 有多少 (cov, opacity) 组合会分歧；
//   ② 最大通道差；
//   ③ 在真实字形覆盖率分布下，预期受影响像素占比。
//
// 若差异存在但极小（≤1 LSB、且只在 0.5 附近），可接受但要**记录下来**；
// 若差异大或广泛，则查表方案得改成“表键保留浮点精度”（不可行）或回退。

#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"

namespace {

[[nodiscard]] auto fast_div255(std::uint32_t value) noexcept -> std::uint32_t {
  return (value * 0x8081U) >> 23U;
}

/// 与 `canvas.cpp` 的 `scale_premul` 同式。
[[nodiscard]] auto scale_premul(std::uint32_t src, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) noexcept {
    return fast_div255(value * alpha + 127U);
  };
  const auto channel = [](std::uint32_t pixel, unsigned shift) noexcept {
    return (pixel >> shift) & 0xFFU;
  };
  return (scale(channel(src, 24)) << 24U) | (scale(channel(src, 16)) << 16U) |
         (scale(channel(src, 8)) << 8U) | scale(channel(src, 0));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "查表（两步量化）与逐像素（一步量化）的一致性",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("alpha_table_equiv_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 取一个典型文字色（不透明深灰）。
  constexpr std::uint32_t kPremul = 0x282C34FFU;   // r=40 g=44 b=52 a=255
  const float opacities[] = {1.0f, 0.9f, 0.75f, 0.5f, 0.35f, 0.2f, 0.1f};

  st::log::out("颜色 0x{:08X}（不透明深灰）\n\n", kPremul);
  st::log::out("{:>10}  {:>12}  {:>14}  {:>12}\n", "opacity", "分歧组数", "最大通道差", "占全域");
  for (const float opacity : opacities) {
    // 旧式：cov 取连续浮点，直接乘 opacity 后量化（**一步**）。
    // 新式：cov 先量化成字节 k（0~255），表里再乘 opacity 量化（**两步**）。
    std::uint64_t mismatches = 0;
    int worst = 0;
    // 用比“256 个量化档”更细的采样（每档 4 个子采样），覆盖“卡在半格”的情形。
    constexpr int kSubSamples = 4;
    const std::uint64_t total = 256ULL * kSubSamples;
    for (int level = 0; level < 256; ++level) {
      for (int sub = 0; sub < kSubSamples; ++sub) {
        // 实际覆盖率：落在该量化档内的某个浮点值。
        const float coverage =
            (static_cast<float>(level) + static_cast<float>(sub) / kSubSamples) / 255.0f;

        // 旧式（一步量化）
        const auto old_alpha =
            static_cast<std::uint32_t>(std::clamp(coverage * opacity, 0.0f, 1.0f) * 255.0f +
                                       0.5f);
        const std::uint32_t old_pixel = scale_premul(kPremul, old_alpha);

        // 新式（两步量化：先量化覆盖率，表里再乘 opacity）
        const auto quantized_coverage =
            static_cast<std::uint32_t>(std::clamp(coverage, 0.0f, 1.0f) * 255.0f + 0.5f);
        const auto table_alpha = static_cast<std::uint32_t>(
            std::clamp(static_cast<float>(quantized_coverage) / 255.0f * opacity, 0.0f, 1.0f) *
                255.0f + 0.5f);
        const std::uint32_t new_pixel = scale_premul(kPremul, table_alpha);

        if (old_pixel == new_pixel) continue;
        ++mismatches;
        for (unsigned shift = 0; shift < 32; shift += 8) {
          const int delta = std::abs(static_cast<int>((old_pixel >> shift) & 0xFFU) -
                                     static_cast<int>((new_pixel >> shift) & 0xFFU));
          if (delta > worst) worst = delta;
        }
      }
    }
    st::log::out("{:>10.2f}  {:>12}  {:>14}  {:>11.2f}%\n", opacity, mismatches, worst,
              static_cast<double>(mismatches) * 100.0 / static_cast<double>(total));
  }
  st::log::out("\n注：分歧 = 新式（两步量化）与旧式（一步量化）产出的像素不同。\n");
  st::log::out("    “最大通道差”是分歧组里的最大单通道差。\n");
  return st::probe::main_of(spec, opts, verdict);
}
