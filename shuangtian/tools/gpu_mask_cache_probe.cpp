// **GPU 文字为何是 0.67 ms（比软件还慢）**——查它的真实构成。
//
// ## 实测（gallery 1600×1000，release，GPU）
//
// ```
// text  调用 305 · 0.67 ms · 113358 像素 · 0.0022 ms/次
// round_rect 33 · 0.02 ms   shadow 14 · 0.01 ms   fill_path 18 · 0.03 ms
// 原语合计 0.80 ms · 未归因 0.47 ms（37%）
// ```
//
// 对比软件路径：`text` 49 次 0.58 ms。**GPU 上文字反而是最大项（占 84%）**。
//
// ## 三个候选原因（读代码发现）
//
// 1. **`mask_cache_` 是线性扫描的 vector**（`platform_d3d11.cpp:1501`）：
//    上限 512 项，每次 `blend_coverage_bitmap` 都遍历到命中为止。
//    305 个字形 → 平均扫 ~150 项 → 约 45000 次比较/帧。
// 2. **LCD 每字形两次 draw call**（`lcd_attenuate` + `lcd_add`）；
// 3. **每字形一次纹理上传**（首次未命中时）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"

namespace {

constexpr int kRounds = 2000;

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "GPU 字形遮罩缓存的线性查找成本（305 字形 / 512 项上限）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("gpu_mask_cache_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::print("模拟 `mask_cache_` 的线性扫描（`platform_d3d11.cpp:1501`）：\n");
  st::print("每帧 305 个字形，每个都要从头扫到命中为止。\n\n");
  st::print("{:>10}{:>16}{:>16}\n", "缓存项数", "每帧总比较次数", "仅查找耗时/帧");

  // 字形身份是散列的 uint64，命中位置近似均匀分布。
  for (const int fill : {64, 128, 256, 512}) {
    std::vector<std::uint64_t> keys;
    keys.reserve(static_cast<std::size_t>(fill));
    for (int index = 0; index < fill; ++index) {
      keys.push_back(0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(index + 1));
    }

    // 每次查最后一项 = 最坏情况；这里取“平均位置”更接近真实（随机字形集合）。
    std::uint64_t comparisons = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < kRounds; ++round) {
      for (int glyph = 0; glyph < 305; ++glyph) {
        // 命中的位置随字形而变（取模让分布均匀）。
        const std::size_t index = static_cast<std::size_t>((glyph * 7919) % fill);
        const auto wanted = keys[index];
        // 手写计数版（真实实现没有计数，这里为了量“比较次数”）
        for (std::size_t probe = 0; probe <= index; ++probe) {
          ++comparisons;
          if (keys[probe] == wanted) break;
        }
      }
    }
    const auto end = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(end - start).count() / kRounds;

    st::print("{:>10}{:>16}{:>13.3f} µs\n", fill,
              static_cast<std::uint64_t>(comparisons / kRounds), total_ms * 1000.0);
  }

  st::print("\n参考：GPU 实测 `text` 总耗时 **0.67 ms/帧**（305 次）。\n");
  st::print("\n判读：\n");
  st::print("  · 若“仅查找”就占 0.67 ms 的可观比例 → 改哈希表值得；\n");
  st::print("  · 若只有几十 µs → 瓶颈在 draw call 与纹理上传，查找不是重点。\n");
  st::print("\n注：本探针只量**查找比较**的 CPU 成本，不含 draw call / 纹理上传。\n");
  return st::probe::main_of(spec, opts, verdict);
}
