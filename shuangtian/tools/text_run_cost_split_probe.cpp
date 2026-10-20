// **文字 run 的时间去哪了**：混合 vs 字形查找 vs 排版。
//
// 已经量出两个互相矛盾的数字：
//   · 单次 `blend_coverage_bitmap`（13×13）：**4.7 ns/px** —— 与短行单价吻合，合理
//   · 完整 `TextRenderer::draw` 摊到触达像素：**61 ns/px** —— 慢 13 倍
//
// 矛盾说明 `draw` 的时间**不在混合**。本探针对比四种情况：
//   A) 完整 `draw`（含排版 + 字形查找 + 混合）
//   B) 同样的文本，但**位图已预热**（排除首次光栅化字形）
//   C) 同样的文本，反复画同一段（排除了“找字形”吗？——不，仍会走一遍）
//   D) 直接调 `blend_coverage_bitmap` 同样的像素量（只有混合）
//
// A 与 D 的差 = 每 run 的非混合开销。
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
#include "st/text/text.hpp"

namespace {

using st::math::Color;

constexpr int kWidth = 1200;
constexpr int kHeight = 800;
constexpr int kRounds = 60;

[[nodiscard]] auto best_ms(const std::function<void()>& body) -> double {
  double best = 1e9;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "文字 run 的时间构成：混合 vs 每 run 的其它开销",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("text_run_cost_split_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    st::print("跳过：拿不到系统字体\n");
    return st::probe::main_of(spec, opts, verdict);
  }
  st::text::TextRenderer renderer(*stack, 1.0f);

  const char* labels[] = {"设置", "文件", "编辑", "视图", "帮助", "status", "OK", "取消"};
  struct Run {
    const char* text;
    float size;
    st::math::Point at;
  };
  std::vector<Run> runs;
  float y = 30.0f;
  for (int index = 0; index < 48; ++index) {
    runs.push_back(Run{labels[index % 8], (index % 3 == 0) ? 13.0f : 16.0f,
                       st::math::Point{30.0f + static_cast<float>(index % 6) * 180.0f, y}});
    if (index % 6 == 5) y += 40.0f;
  }
  const Color ink{40, 44, 52, 255};

  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});

  // 预热：字形位图缓存建好。
  for (int pass = 0; pass < 3; ++pass) {
    for (const Run& run : runs) {
      (void)renderer.draw(canvas, run.text, run.at, run.size, ink);
    }
  }

  // A) 完整 draw（位图已预热）
  const double draw_ms = best_ms([&] {
    for (const Run& run : runs) {
      (void)renderer.draw(canvas, run.text, run.at, run.size, ink);
    }
  });

  // B) 同样的文字，但只画 1/8 的量（看是否线性 → 判断有无“一次性”开销）
  const double eighth_ms = best_ms([&] {
    for (std::size_t index = 0; index < runs.size() / 8; ++index) {
      (void)renderer.draw(canvas, runs[index].text, runs[index].at, runs[index].size, ink);
    }
  });

  // C) 纯混合：造同样数量的像素，直接调 `blend_coverage_bitmap`。
  //    每个 run 按“平均 4 个字形 × 13×13”估算。
  constexpr int kGlyphW = 13;
  constexpr int kGlyphH = 13;
  std::vector<float> coverage(static_cast<std::size_t>(kGlyphW) * kGlyphH, 0.6f);
  const st::raster::Paint paint = st::raster::Paint::solid(ink);
  constexpr int kBitmapsPerRun = 4;
  const double blend_ms = best_ms([&] {
    for (std::size_t index = 0; index < runs.size(); ++index) {
      for (int glyph = 0; glyph < kBitmapsPerRun; ++glyph) {
        canvas.blend_coverage_bitmap(
            static_cast<int>(runs[index].at.x) + glyph * kGlyphW,
            static_cast<int>(runs[index].at.y), coverage, kGlyphW, kGlyphH, paint, 1.0f,
            st::raster::BlendMode::SrcOver, 0, st::raster::CoverageFormat::Grayscale);
      }
    }
  });

  st::print("{} 个 run\n\n", runs.size());
  st::print("A 完整 draw（位图预热）      {:>8.3f} ms   → {:>6.2f} µs/run\n", draw_ms,
            draw_ms * 1000.0 / static_cast<double>(runs.size()));
  st::print("B 只画 1/8（{} 个）           {:>8.3f} ms   → {:>6.2f} µs/run\n", runs.size() / 8,
            eighth_ms, eighth_ms * 1000.0 / static_cast<double>(runs.size() / 8));
  st::print("C 纯混合（同量位图）        {:>8.3f} ms   → {:>6.2f} µs/run\n", blend_ms,
            blend_ms * 1000.0 / static_cast<double>(runs.size()));
  st::print("\nA − C = 每 run 的**非混合开销**（排版/字形查找/状态准备）：\n");
  st::print("  {:.3f} ms（{:.0f}% 的 A）\n", draw_ms - blend_ms,
            draw_ms > 0.0 ? (draw_ms - blend_ms) / draw_ms * 100.0 : 0.0);
  st::print("\n若 B×8 ≈ A，则无“一次性”开销，成本随 run 数线性。\n");
  return st::probe::main_of(spec, opts, verdict);
}
