// 量 `simd::over_row` 在**极短长度**下的单位成本（解释 covered_region 为何无收益）。
//
// 背景：`covered_region` 把一行切成「左段 + 右段」（中间跳过）。对卡片这类形状，
// 左右两段往往只有几个像素，而 `over_row` 的 AVX2 主循环要 `count >= 8` 才生效，
// 更短就走**标量回退**，且每段都付一次常量构造 + 函数调用。
//
// 若短行的 ns/px 明显高于长行，那「跳过中间」省下的混合量就抵不过这两次短调用的开销。
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
#include "st/raster/paint.hpp"
#include "st/raster/simd.hpp"

namespace {

/// 反复对同一段做 `over_row`，量 ns/px。
[[nodiscard]] auto ns_per_px(std::size_t count, int iterations) -> double {
  std::vector<std::uint32_t> dst(count, 0xFF808080U);
  std::vector<std::uint32_t> src(count, 0x40402020U);   // 半透明源，确保真的混合
  // 预热（让分支预测与指令缓存到位）。
  for (int index = 0; index < 1000; ++index) st::raster::simd::over_row(dst.data(), src.data(), count);
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < iterations; ++index) {
    st::raster::simd::over_row(dst.data(), src.data(), count);
  }
  const auto end = std::chrono::steady_clock::now();
  const double total_ns =
      std::chrono::duration<double, std::nano>(end - start).count();
  return total_ns / (static_cast<double>(iterations) * static_cast<double>(count));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "simd::over_row 的短行成本（短段是否吃掉 covered_region 的收益）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("simd_short_row_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::log::out("后端 = {}\n\n", st::raster::simd::backend());
  st::log::out("{:>8}  {:>12}  {:>10}\n", "长度", "ns/px", "相对 512");
  constexpr std::size_t kRef = 512;
  const double ref = ns_per_px(kRef, 20000);
  for (const std::size_t count : {1, 2, 4, 6, 8, 12, 16, 32, 64, 128, 256, 512, 1024}) {
    // 短行的迭代数放大，保证总样本量相当。
    const int iterations = static_cast<int>(std::max<std::size_t>(2000, 4000000 / (count + 1)));
    const double value = ns_per_px(count, iterations);
    st::log::out("{:>8}  {:>12.4f}  {:>9.2f}×\n", count, value, value / ref);
  }
  st::log::out("\n注：相对 512 一列 > 1 表示该长度每像素更贵（受固定开销拖累）。\n");

  // **关键对照**：整行一次 vs 切两段跳过中间（正是 covered_region 的形态）。
  // 这直接回答“切分到底划不划算”，不依赖阴影路径的其它开销。
  st::log::out("\n分段对照（总宽 140px，中间跳过 skipped px，两侧均分剩余）：\n");
  st::log::out("每行跑 140 像素——单次调用只有几十纳秒，必须靠**内层多次重复**压噪声：\n");
  st::log::out("{:>8}  {:>12}  {:>12}  {:>10}\n", "跳过", "整行 ns", "两段 ns", "收益");
  constexpr int kTotal = 140;
  for (const int skipped : {40, 80, 120}) {
    const int side = (kTotal - skipped) / 2;
    std::vector<std::uint32_t> dst_full(kTotal, 0xFF808080U);
    std::vector<std::uint32_t> src_full(kTotal, 0x40402020U);
    // 每次计时内跑 kBatch 次该行，把时间拉到数十微秒以上再除。
    constexpr int kBatch = 2000;
    constexpr int kRepeats = 60;

    const auto time_it = [&](const std::function<void()>& body) -> double {
      double best = 1e18;
      for (int repeat = 0; repeat < kRepeats; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < kBatch; ++index) body();
        const auto end = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(end - start).count());
      }
      return best / static_cast<double>(kBatch);
    };

    const double whole = time_it([&] {
      st::raster::simd::over_row(dst_full.data(), src_full.data(), kTotal);
    });
    const double split = time_it([&] {
      st::raster::simd::over_row(dst_full.data(), src_full.data(),
                                 static_cast<std::size_t>(side));
      st::raster::simd::over_row(dst_full.data() + kTotal - side,
                                 src_full.data() + kTotal - side,
                                 static_cast<std::size_t>(side));
    });
    st::log::out("{:>8}  {:>12.2f}  {:>12.2f}  {:>9.1f}%\n", skipped, whole, split,
              (whole - split) / whole * 100.0);
  }
  return st::probe::main_of(spec, opts, verdict);
}
