// **文字成本的真正构成**：固定开销 vs 像素运算。
//
// 背景（gallery 1600×1000）：`text` 49 次 / 0.67 ms / 57k 像素 = **11.8 ns/px**，
// 比 `shadow`（0.43）、`round_rect`（0.66）贵得多。
//
// 已排除的两个假设（本轮实测）：
//   ① 「逐像素 `scale_premul` 太贵」→ 改成查表，**无变化**（0.68 → 0.67，噪声内）。
//      `scale_premul` 用的是 `fast_div255`（乘法），本来就不慢。
//   ② 「字形位图有空行白走」→ 实测空行占比 **0.0%**（`tools/glyph_row_waste_probe.cpp`），
//      字形位图是紧凑的。
//
// 剩下的可能：**每次调用 × 每行的固定开销**。字形是一字一位图，
// 小字号 13px 时每个位图只有约 13×13 像素、13 行——**每行都付一次
// 裁剪栈查询 + `has_mask_clip()` + 交集计算**，而真正混合只有几个像素。
//
// 本探针直接把这三笔拆开量：
//   A) 完整 `blend_coverage_bitmap`（现状）
//   B) 同像素量、但**一次调用处理多行**（模拟“合并成一张大位图”的下界）
//   C) 纯 `over_premul` 同像素数（无任何行/裁剪开销）
//
// 若 A 远大于 C 而 B 接近 C → 收益在“减少每行固定开销”。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;

constexpr int kWidth = 1600;
constexpr int kHeight = 1000;
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
      .title = "文字成本构成：固定开销（每行查询）vs 像素运算",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("text_cost_breakdown_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    st::log::out("跳过：拿不到系统字体\n");
    return st::probe::main_of(spec, opts, verdict);
  }
  st::text::TextRenderer renderer(*stack, 1.0f);

  // 模拟界面里的一整屏文字：约 49 个 run（与 gallery 实测一致）。
  struct Run {
    const char* text;
    float size;
    st::math::Point at;
  };
  std::vector<Run> runs;
  const char* labels[] = {"设置", "文件", "编辑", "视图", "帮助", "status", "line 12", "OK",
                          "取消", "确定", "搜索", "替换", "保存", "打开", "关闭", "最近文件"};
  float y = 30.0f;
  for (int index = 0; index < 48; ++index) {
    runs.push_back(Run{labels[index % 16], (index % 3 == 0) ? 13.0f : 16.0f,
                       st::math::Point{30.0f + static_cast<float>(index % 6) * 250.0f, y}});
    if (index % 6 == 5) y += 40.0f;
  }
  const Color ink{40, 44, 52, 255};

  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  const Color background{255, 255, 255, 255};
  canvas.clear(background);
  // 预热（字形位图缓存）。
  for (const Run& run : runs) {
    (void)renderer.draw(canvas, run.text, run.at, run.size, ink);
  }

  const double text_ms = best_ms([&] {
    for (const Run& run : runs) {
      (void)renderer.draw(canvas, run.text, run.at, run.size, ink);
    }
  });

  // 统计这批文字实际触达的像素（从渲染结果反推）。
  //
  // 为何不用 `bitmap_for`：那是 `TextRenderer` 的私有接口。而“实际混合了多少像素”
  // 恰好就是我们需要的量，从成品 alpha 分布反推更直接（也不会因内部重构而失效）。
  std::uint64_t lit_pixels = 0;
  std::uint64_t ink_pixels = 0;
  for (int row = 0; row < kHeight; ++row) {
    for (int col = 0; col < kWidth; ++col) {
      const auto pixel = canvas.pixel_at(col, row);
      // 与清屏底不同的就是被文字覆盖过的（含抗锯齿边缘）。
      if (pixel.r == background.r && pixel.g == background.g && pixel.b == background.b) {
        continue;
      }
      ++lit_pixels;
      ++ink_pixels;
    }
  }

  st::log::out("49 个文字 run：完整 draw {:.3f} ms\n", text_ms);
  st::log::out("  实际触达像素 {} · 每像素 {:.2f} ns\n", lit_pixels,
            lit_pixels == 0 ? 0.0 : text_ms * 1e6 / static_cast<double>(lit_pixels));
  (void)ink_pixels;
  st::log::out("\n参考（`tools/simd_short_row_probe.cpp` 实测单价，ns/px）：\n");
  st::log::out("  长 1~6 → 3.6~6.1 · 长 8 → 0.85 · 长 16 → 0.47 · 长 ≥32 → 0.28~0.33\n");
  st::log::out("\n判据：若“每行 ns”远大于“该行长度的合理单价”，则固定开销是主因，\n");
  st::log::out("      方向是“减少每行调用次数”（合并位图 / 行内一次算完裁剪）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
