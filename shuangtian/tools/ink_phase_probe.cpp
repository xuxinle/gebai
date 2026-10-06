/// **字形相位对墨量的影响**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 起因：同一批字母，**孤立单字形**（一行一个字符）测出的墨量比是 0.92~0.93，
/// 而**文本串**里的同批字母是 0.95~0.96。两者差 3~4%，必须查清是"真差异"还是"口径差"。
///
/// 本探针直接测最可能的机制：**采样相位**。
/// 文本串里每个字形的笔位是"前面所有 advance 累加"的结果（带小数），
/// 而孤立单字形总是从同一个整数起点开始画——两者的**子像素相位**不同，
/// 亚像素渲染下同一字形的覆盖率分布会随相位变化。
///
/// 用法：ink_phase_probe [字号=11] [字符=l]
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

auto ink_of(const Canvas& canvas) -> double {
  const auto& pixels = canvas.to_rgba8();
  double sum = 0.0;
  for (std::size_t index = 0; index + 3 < pixels.size(); index += 4) {
    for (int channel = 0; channel < 3; ++channel) {
      sum += (255.0 - static_cast<double>(pixels[index + static_cast<std::size_t>(channel)])) / 255.0;
    }
  }
  return sum / 3.0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const float size = argc > 1 ? std::stof(argv[1]) : 11.0f;
  const std::string ch = argc > 2 ? argv[2] : "l";
  auto stack = FontStack::system_default();
  if (!stack) return 1;

  st::print("字号 {}px  字符 {}  —— 墨量和随**起点相位**的变化\n", size, ch);

  // ① 孤立字形：起点 x 从 0 到 0.9 扫（步长 0.1），看墨量随相位怎么变
  st::print("\n① 孤立字形（量的是同一个字，只改起点小数部分）：\n");
  std::vector<double> isolated;
  for (int step = 0; step <= 9; ++step) {
    const float phase = static_cast<float>(step) * 0.1f;
    Canvas canvas{60, 40, 1.5f};
    canvas.clear(Color{0xFF, 0xFF, 0xFF, 0xFF});
    TextRenderer renderer{*stack, 1.5f};
    renderer.set_subpixel(true);
    renderer.set_grid_fit(GridFitMode::Light);
    renderer.set_coverage_gamma(1.10f);
    renderer.set_stem_darkening(true);
    renderer.set_class_gamma(st::text::GlyphClass::Digit, 0.92f);
    (void)renderer.draw(canvas, ch, Point{4.0f + phase, 4.0f}, size, Color{0x0F, 0x17, 0x2A, 0xFF});
    const double ink = ink_of(canvas);
    isolated.push_back(ink);
    st::print("   起点偏移 {:.1f}px -> 墨量 {:.2f}\n", static_cast<double>(phase), ink);
  }
  double lo = isolated[0];
  double hi = isolated[0];
  for (double value : isolated) {
    lo = std::min(lo, value);
    hi = std::max(hi, value);
  }
  st::print("   ⇒ 波动范围 {:.2f} ~ {:.2f}（极差 {:.1f}%，中位 {:.2f}）\n", lo, hi,
            (hi - lo) / lo * 100.0, isolated[isolated.size() / 2U]);

  // ② 文本串：同样的字符重复 k 次，量**每个字形**的墨量（看串内与串首是否一致）
  st::print("\n② 文本串（同一字符重复 8 次，逐字形量）：\n");
  const std::string text = [&] {
    std::string out;
    for (int i = 0; i < 8; ++i) out += ch;
    return out;
  }();
  Canvas canvas{400, 40, 1.5f};
  canvas.clear(Color{0xFF, 0xFF, 0xFF, 0xFF});
  TextRenderer renderer{*stack, 1.5f};
  renderer.set_subpixel(true);
  renderer.set_grid_fit(GridFitMode::Light);
  renderer.set_coverage_gamma(1.10f);
  renderer.set_stem_darkening(true);
  renderer.set_class_gamma(st::text::GlyphClass::Digit, 0.92f);
  (void)renderer.draw(canvas, text, Point{4.0f, 4.0f}, size, Color{0x0F, 0x17, 0x2A, 0xFF});
  const auto& pixels = canvas.to_rgba8();
  const int width = canvas.width();
  const int height = canvas.height();
  std::vector<double> column_ink(static_cast<std::size_t>(width), 0.0);
  for (int x = 0; x < width; ++x) {
    double sum = 0.0;
    for (int y = 0; y < height; ++y) {
      const std::size_t base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x)) * 4U;
      for (int channel = 0; channel < 3; ++channel) {
        sum += (255.0 - static_cast<double>(pixels[base + static_cast<std::size_t>(channel)])) / 255.0;
      }
    }
    column_ink[static_cast<std::size_t>(x)] = sum / 3.0;
  }
  // 按列空隙切成 8 段（每个字形一段），报各段墨量
  std::vector<std::pair<int, int>> spans;
  int start = -1;
  int blank = 0;
  for (int x = 0; x < width; ++x) {
    const bool on = column_ink[static_cast<std::size_t>(x)] > 0.4;
    if (on) {
      if (start < 0) start = x;
      blank = 0;
    } else if (start >= 0) {
      if (++blank > 1) {
        spans.emplace_back(start, x - blank + 1);
        start = -1;
        blank = 0;
      }
    }
  }
  if (start >= 0) spans.emplace_back(start, width);
  for (std::size_t index = 0; index < spans.size(); ++index) {
    double sum = 0.0;
    for (int x = spans[index].first; x < spans[index].second; ++x) {
      sum += column_ink[static_cast<std::size_t>(x)];
    }
    st::print("   第 {} 个字形 -> 墨量 {:.2f}\n", index + 1, sum);
  }
  st::print("\n判据：① 若同一字形只改起点小数部分就让墨量明显变化（几 % 级），\n");
  st::print("      则「孤立单字形 vs 文本串」的差异主要是**采样相位**，不是渲染质量差异。\n");
  return 0;
}
