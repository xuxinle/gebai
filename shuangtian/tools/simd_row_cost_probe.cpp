// **`simd::over_row` vs `simd::blend_row` 的单价差**——为什么贴图比直角填充慢 67 倍。
//
// 已量出的事实：
//   · `fill_rect`（直角大面，走 `blend_span` → `simd::blend_row`）：**0.0043 ns/px**
//   · 几何贴图贴出（走 `simd::over_row`）：**0.290 ns/px**
//   · 同样 63832 像素，**差 67 倍**
//
// 两者都是 SIMD 整行混合，差距这么大不合理。可能原因：
//   ① `over_row` 需要逐字节解包源像素（源色逐像素不同），指令数多得多；
//   ② 贴图路径每行**只贴一个窄跨度**（被 `covered_region` 分段 / 裁剪切碎），
//      短行单价高（`simd_short_row_probe` 已证长 1~6 是 3.6~6.1 ns/px）；
//   ③ `blend_row` 的 alpha 是常量，可整行向量化；`over_row` 不行。
//
// 本探针直接测两个 SIMD 函数在不同行长下的单价，把 ② 与 ③ 分开。
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/simd.hpp"

namespace {

constexpr int kRounds = 400;
constexpr std::size_t kBufferPixels = 4096;

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
      .title = "simd::over_row vs blend_row 单价（解释贴图为何比直角填充慢）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("simd_row_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  std::vector<std::uint32_t> dst(kBufferPixels, 0xFFF0F0F4U);
  std::vector<std::uint32_t> src(kBufferPixels, 0x802F6FE0U);

  st::log::out("{:>8}  {:>14}  {:>14}  {:>10}\n", "行长", "over_row", "blend_row", "比值");
  const std::size_t lengths[] = {4, 8, 16, 32, 64, 128, 512, 2048};
  for (const std::size_t length : lengths) {
    // 每次调用只处理一行 `length` 像素；重复多次以放大到可测。
    constexpr int kRepeat = 200;
    const double over_ns = best_ns([&] {
      for (int index = 0; index < kRepeat; ++index) {
        st::raster::simd::over_row(dst.data(), src.data(), length);
      }
    });
    const double blend_ns = best_ns([&] {
      for (int index = 0; index < kRepeat; ++index) {
        st::raster::simd::blend_row(dst.data(), length, 0x802F6FE0U, 255U);
      }
    });
    const double total_px = static_cast<double>(length) * kRepeat;
    st::log::out("{:>8}  {:>9.3f} ns/px  {:>9.3f} ns/px  {:>9.2f}×\n", length,
              over_ns / total_px, blend_ns / total_px,
              blend_ns > 0.0 ? over_ns / blend_ns : 0.0);
  }
  st::log::out("\n注：两者都直接调用 `simd::*`，不含画布/裁剪开销。\n");
  st::log::out("若“长 512/2048”时比值仍很大 → 是**算法本身**的差（解包 vs 常量 alpha）；\n");
  st::log::out("若只在短行时差 → 是**每行固定开销**（调用/初始化）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
