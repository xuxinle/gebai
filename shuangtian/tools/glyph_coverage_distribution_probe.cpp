// **文字 11.1 ns/px 花在哪种像素上**——从渲染结果反推。
//
// 背景（gallery 1600×1000 实测）：`text` 49 次 / **0.68 ms** / 57 k 像素
// = **11.1 ns/px**，而 `shadow` 0.43、`round_rect` 0.82。
//
// `Canvas::blend_coverage_row` 有一条快路径：覆盖率 ≥0.999 的连续段整段走
// `simd::blend_row`。它假设「内部有大量满格像素」（对大面填充成立）。
// 而字形的形态可能相反：**笔画细、满格像素少、绝大多数是 0.1~0.9 的边缘**。
// 若如此，那条快路径对文字基本没用上，每次都得走
// 「逐像素浮点 + `scale_premul`（4 次整数除法）」。
//
// 做法：把一段文字画到**透明底**上，读 alpha 分布——那就是喂给混合的覆盖率
//（不透明色 + alpha=255 时，结果的 alpha 就是覆盖率量化值）。
//
// 判据：只报数。

#include <algorithm>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "字形覆盖率分布（判断 `blend_row` 快路径对文字是否用得上）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("glyph_coverage_distribution_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    st::print("跳过：拿不到系统字体\n");
    return st::probe::main_of(spec, opts, verdict);
  }
  st::text::TextRenderer renderer(*stack, 1.0f);

  struct Sample {
    const char* name;
    const char* text;
    float size;
  };
  const Sample samples[] = {
      {"24px 中文", "界面文字渲染", 24.0f},
      {"24px 英文", "Hello World", 24.0f},
      {"13px 中文（小字）", "状态栏文字", 13.0f},
      {"48px 英文（大字）", "Title", 48.0f},
  };

  st::print("{:<22}  {:>10}  {:>10}  {:>10}  {:>10}\n", "样本", "非零像素", "满格占比",
            "0.1~0.99", "≤0.1");
  for (const Sample& item : samples) {
    constexpr int kWidth = 600;
    constexpr int kHeight = 200;
    st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
    canvas.clear(Color{0, 0, 0, 0});
    (void)renderer.draw(canvas, item.text, st::math::Point{10.0f, item.size}, item.size,
                        Color{255, 255, 255, 255});
    std::uint64_t nonzero = 0;
    std::uint64_t full = 0;
    std::uint64_t mid = 0;
    std::uint64_t low = 0;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int alpha = canvas.pixel_at(x, y).a;
        if (alpha == 0) continue;
        const double value = static_cast<double>(alpha) / 255.0;
        ++nonzero;
        if (value >= 0.999) {
          ++full;
        } else if (value > 0.1) {
          ++mid;
        } else {
          ++low;
        }
      }
    }
    const auto pct = [nonzero](std::uint64_t value) {
      return nonzero == 0 ? 0.0
                          : static_cast<double>(value) * 100.0 / static_cast<double>(nonzero);
    };
    st::print("{:<22}  {:>10}  {:>9.1f}%  {:>9.1f}%  {:>9.1f}%\n", item.name, nonzero, pct(full),
              pct(mid), pct(low));
  }
  st::print("\n「满格占比」= 覆盖率 ≥0.999 的像素，那部分走 `simd::blend_row`；\n");
  st::print("其余走逐像素浮点 + `scale_premul`（4 次整数除法）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
