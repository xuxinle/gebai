// **文字每行固定开销的构成**——把 `blend_coverage_bitmap` 的一次调用拆开量。
//
// 已知（`tools/text_cost_breakdown_probe.cpp`）：49 个文字 run 只触达 **13751 像素**
// 却要 ~0.6 ms = **43 ns/px**，比短行单价（3.6~6.1 ns/px）慢 7~12 倍。
// 结论是“成本在每字形 × 每行的固定开销”，但**具体是哪几笔**还没量。
//
// `blend_coverage_bitmap` 对位图的每一行都调一次 `blend_coverage_row`，后者每次要做：
//   ① `current_clip()`（vector 末元素 + 边界检查）
//   ② `has_mask_clip()`（**遍历整个裁剪栈**）
//   ③ `begin/end` 与裁剪区求交
//   ④ `alpha_table(premul)` 查表缓存命中判断
//   ⑤ 逐像素循环
//
// 本探针直接量 ①②③④ 的裸成本（不含像素循环），以及真实 `blend_coverage_bitmap`
// 的总成本，从而判断“减少每行调用次数”能拿回多少。
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;

constexpr int kWidth = 800;
constexpr int kHeight = 600;
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
      .title = "文字每行固定开销的构成（裁剪查询 / 汇交 / 查表命中）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("text_row_overhead_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});

  // ---- ① 裸 `clip_rect()`（含 `current_clip()`）----
  const double clip_ns = best_ns([&] {
    for (int index = 0; index < 10000; ++index) {
      const auto rect = canvas.clip_rect();
      (void)rect;
    }
  });

  // ---- ② 裸 `has_mask_clip()`（遍历裁剪栈）----
  const double mask_ns = best_ns([&] {
    for (int index = 0; index < 10000; ++index) {
      (void)canvas.has_mask_clip();
    }
  });

  // ---- ③ 一次真实 `blend_coverage_bitmap` 调用（含逐行循环）----
  // 用与真实字形相近的尺寸：13px 字形的位图约 13x13。
  constexpr int kGlyphW = 13;
  constexpr int kGlyphH = 13;
  std::vector<float> coverage(static_cast<std::size_t>(kGlyphW) * kGlyphH);
  // 造一个“中间实、边缘渐隐”的覆盖率场（与字形相近）。
  for (int y = 0; y < kGlyphH; ++y) {
    for (int x = 0; x < kGlyphW; ++x) {
      const double dx = (static_cast<double>(x) - 6.0) / 6.0;
      const double dy = (static_cast<double>(y) - 6.0) / 6.0;
      const double distance = std::sqrt(dx * dx + dy * dy);
      coverage[static_cast<std::size_t>(y) * kGlyphW + static_cast<std::size_t>(x)] =
          static_cast<float>(std::max(0.0, 1.0 - distance));
    }
  }
  const st::raster::Paint paint = st::raster::Paint::solid(Color{40, 44, 52, 255});

  const double bitmap_ns = best_ns([&] {
    canvas.blend_coverage_bitmap(10, 10, coverage, kGlyphW, kGlyphH, paint, 1.0f,
                                 st::raster::BlendMode::SrcOver, 0,
                                 st::raster::CoverageFormat::Grayscale);
  });

  st::print("裁剪查询 clip_rect()        {:>8.1f} ns/次\n", clip_ns / 10000.0);
  st::print("has_mask_clip()（遍历栈）   {:>8.1f} ns/次\n", mask_ns / 10000.0);
  st::print("\n一次 blend_coverage_bitmap（{}x{} 位图）  {:>8.1f} ns\n", kGlyphW, kGlyphH,
            bitmap_ns);
  st::print("  每行平均 {:>6.1f} ns（共 {} 行）\n", bitmap_ns / kGlyphH, kGlyphH);
  st::print("  每像素   {:>6.1f} ns（共 {} px）\n",
            bitmap_ns / (static_cast<double>(kGlyphW) * kGlyphH), kGlyphW * kGlyphH);

  const double per_row_fixed = mask_ns / 10000.0 + clip_ns / 10000.0;
  st::print("\n若每行只付 ①+②，那么 {:.1f} ns/行 × {} 行 = {:.1f} ns\n", per_row_fixed, kGlyphH,
            per_row_fixed * kGlyphH);
  st::print("占一次调用（{:.1f} ns）的 **{:.0f}%**\n", bitmap_ns,
            bitmap_ns > 0.0 ? per_row_fixed * kGlyphH / bitmap_ns * 100.0 : 0.0);
  return st::probe::main_of(spec, opts, verdict);
}
