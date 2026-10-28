// **文字 run 的 61 ns/px 到底花在哪**——用减法实验定位。
//
// 矛盾数据（都已复现）：
//   · 单独调 `blend_coverage_bitmap`（13×13 灰度）：**4.7 ns/px**
//   · 完整 `TextRenderer::draw` 摊到触达像素：**61 ns/px**（慢 13 倍）
//
// 减法实验：同一批文字分别做
//   A) 完整 `draw`
//   B) 只 `measure`（排版 + 字形查找 + 度量，**不混合**）
//   C) 只算 UTF-8 → 字形索引（`measure` 会做这件事）
//
// A − B = 纯“提交位图 + 混合”的净成本。若它远大于 4.7 ns/px，说明提交路径有固定开销；
// 若 B 本身就很大，说明**成本在排版/字形查找**，那才是该优化的地方。
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
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;

constexpr int kWidth = 1200;
constexpr int kHeight = 800;
constexpr int kRounds = 80;

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
      .title = "文字 run 的减法定位：排版/字形查找 vs 位图混合",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("text_subtract_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    st::log::out("跳过：拿不到系统字体\n");
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
  // **打开 LCD 亚像素**（与 gallery 实际配置对齐）——灰度与亚像素的混合成本差 1.5 倍。
  renderer.set_subpixel(true);
  for (int pass = 0; pass < 3; ++pass) {
    for (const Run& run : runs) {
      (void)renderer.draw(canvas, run.text, run.at, run.size, ink);
    }
  }

  // A) 完整 draw
  const double draw_ms = best_ms([&] {
    for (const Run& run : runs) {
      (void)renderer.draw(canvas, run.text, run.at, run.size, ink);
    }
  });

  // B) 只 measure（排版 + 字形查找 + 度量，不混合）
  const double measure_ms = best_ms([&] {
    for (const Run& run : runs) {
      const auto metrics = renderer.measure(run.text, run.size);
      (void)metrics;
    }
  });

  // C) 按**字形数**换算（而非 run 数）：才是能跨场景比较的口径。
  std::uint64_t glyph_total = 0;
  for (const Run& run : runs) {
    // UTF-8 逐字节数非首字节 = 字符数（这批样本全是无组合字符的常规文本）。
    std::size_t chars = 0;
    for (const char* cursor = run.text; *cursor != '\0'; ++cursor) {
      if ((static_cast<unsigned char>(*cursor) & 0xC0U) != 0x80U) ++chars;
    }
    glyph_total += static_cast<std::uint64_t>(chars);
  }

  st::log::out("{} 个 run\n\n", runs.size());
  st::log::out("A 完整 draw        {:>8.3f} ms   → {:>7.2f} µs/run\n", draw_ms,
            draw_ms * 1000.0 / static_cast<double>(runs.size()));
  st::log::out("B 只 measure       {:>8.3f} ms   → {:>7.2f} µs/run\n", measure_ms,
            measure_ms * 1000.0 / static_cast<double>(runs.size()));
  st::log::out("\nA − B（提交位图 + 混合）  {:>8.3f} ms   → {:>7.2f} µs/run\n", draw_ms - measure_ms,
            (draw_ms - measure_ms) * 1000.0 / static_cast<double>(runs.size()));
  st::log::out("\nB 占 A 的 **{:.0f}%**\n",
            draw_ms > 0.0 ? measure_ms / draw_ms * 100.0 : 0.0);
  st::log::out("\n若 B 占比显著，则“排版/字形查找”才是主体，不是混合。\n");
  st::log::out("字符数合计 {} → **{:.2f} µs/字符**（含 LCD 混合）\n", glyph_total,
            glyph_total == 0 ? 0.0 : draw_ms * 1000.0 / static_cast<double>(glyph_total));
  st::log::out("\n参考（gallery 实测）：49 个 run / 0.59 ms。若按中文字符数换算，\n");
  st::log::out("它能对齐到同一个 µs/字符 就是“成本合理”；差很多则是 gallery 的字形更多/更大。\n");
  return st::probe::main_of(spec, opts, verdict);
}
