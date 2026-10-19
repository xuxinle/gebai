// 内存带宽探针：判断「行带并行」到底有没有空间。
//
// ## 为什么要先测这个
//
// 软件光栅器的逐像素工作是**内存带宽型**的：`clear` 是纯 `memset`，`fill_rect` 是整行
// 写，`over_row` 是读+写。这类工作多线程能加速的前提是**单线程还没打满带宽**。
// 如果单线程已到 14~15 GB/s（接近单通道上限），那么再开 8 个线程也只能小幅提升，
// 而线程调度/同步的开销在 5 ms 的帧里会吃掉那点收益。
//
// 本探针直接量三件事（都用**与渲染器相同的访问模式**）：
//   ① 单线程 `memset` 写带宽
//   ② 多线程按**水平带**切分写同一块缓冲的带宽（行带并行的理想情况）
//   ③ 单线程「读+写」`src-over` 带宽（`over_row` 的模式，带宽需求翻倍）
//
// 判据：只有当 ② 比 ① 明显快（≥1.5×）时，行带并行才值得做。
//
// 用法：memory_bandwidth_probe（无参数）

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <thread>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/simd.hpp"

namespace {

constexpr int kWidth = 1600;
constexpr int kHeight = 1000;
constexpr int kRounds = 60;

[[nodiscard]] auto make_buffer() -> std::vector<std::uint32_t> {
  return std::vector<std::uint32_t>(static_cast<std::size_t>(kWidth) * kHeight, 0U);
}

/// 单线程：整块 `fill`（与 `Canvas::clear` 同模式）。
[[nodiscard]] auto measure_single(std::vector<std::uint32_t>& buffer) -> double {
  double best = 1.0e9;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    std::ranges::fill(buffer, 0xFF204060U);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    best = std::min(best, ms);
  }
  return best;
}

/// 多线程：按**水平带**切分（行带并行就是这个模式；每带内部是连续地址）。
[[nodiscard]] auto measure_banded(std::vector<std::uint32_t>& buffer, int threads) -> double {
  double best = 1.0e9;
  const auto rows_per_band = (kHeight + threads - 1) / threads;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int index = 0; index < threads; ++index) {
      const auto begin = index * rows_per_band;
      const auto end = std::min(kHeight, begin + rows_per_band);
      pool.emplace_back([&buffer, begin, end] {
        auto* base = buffer.data() + static_cast<std::size_t>(begin) * kWidth;
        std::fill(base, base + static_cast<std::size_t>(end - begin) * kWidth, 0xFF204060U);
      });
    }
    for (auto& worker : pool) worker.join();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    best = std::min(best, ms);
  }
  return best;
}

/// 单线程 `src-over`（`over_row` 的模式：读 + 写，带宽需求是一倍写）。
[[nodiscard]] auto measure_over(std::vector<std::uint32_t>& buffer,
                                const std::vector<std::uint32_t>& source) -> double {
  double best = 1.0e9;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    for (int y = 0; y < kHeight; ++y) {
      const auto offset = static_cast<std::size_t>(y) * kWidth;
      st::raster::simd::over_row(buffer.data() + offset, source.data() + offset, kWidth);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    best = std::min(best, ms);
  }
  return best;
}

/// 带宽（GB/s）：`bytes` 在该毫秒数内被搬运。
[[nodiscard]] auto bandwidth_gb_s(std::size_t bytes, double ms) -> double {
  if (ms <= 0.0) return 0.0;
  return static_cast<double>(bytes) / (ms * 1.0e6);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "内存带宽探针：判断行带并行是否还有空间。",
      .criterion = "行带并行只在「多线程写明显快于单线程」时才值得做。",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("memory_bandwidth_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  auto buffer = make_buffer();
  auto source = make_buffer();
  for (auto& pixel : source) pixel = 0x80406080U;  // 半透明，`over_row` 才会真算
  const std::size_t write_bytes =
      static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U;

  const double single = measure_single(buffer);
  st::print("缓冲 {}×{} = {:.1f} MB（每轮）· {} 轮取最小\n", kWidth, kHeight,
            static_cast<double>(write_bytes) / (1024.0 * 1024.0), kRounds);
  st::print("  单线程整块写           {:>7.3f} ms · {:>6.2f} GB/s\n", single,
            bandwidth_gb_s(write_bytes, single));

  const int hardware = static_cast<int>(std::thread::hardware_concurrency());
  std::vector<int> counts;
  for (const int candidate : {2, 4, 8, 16}) {
    if (candidate <= hardware) counts.push_back(candidate);
  }

  double best_banded = single;
  int best_threads = 1;
  for (const int threads : counts) {
    const double banded = measure_banded(buffer, threads);
    const double speedup = single / std::max(banded, 1.0e-9);
    st::print("  {:>2} 线程按行带写      {:>7.3f} ms · {:>6.2f} GB/s · 加速 {:.2f}×\n", threads,
              banded, bandwidth_gb_s(write_bytes, banded), speedup);
    if (banded < best_banded) {
      best_banded = banded;
      best_threads = threads;
    }
  }

  const double over = measure_over(buffer, source);
  st::print("  单线程 src-over(读+写){:>11.3f} ms · {:>6.2f} GB/s（按写字节算）\n", over,
            bandwidth_gb_s(write_bytes, over));

  const double best_speedup = single / std::max(best_banded, 1.0e-9);
  st::print("\n结论：行带并行上限 {:.2f}×（{} 线程）——", best_speedup, best_threads);
  // 判据：≥1.5× 才值得做（低于这个量级，线程建/同步开销会吃掉大半）。
  const bool worth = best_speedup >= 1.5;
  st::print("{}\n", worth ? "有空间，值得做" : "空间有限，收益会被线程开销吃掉");
  verdict.add("多线程写显著快于单线程（≥1.5×）", worth,
              std::format("{:.2f}× @ {} 线程（单线程 {:.2f} GB/s）", best_speedup, best_threads,
                          bandwidth_gb_s(write_bytes, single)));
  return st::probe::main_of(spec, opts, verdict.conclude());
}
