/// 合成加粗的**性能代价**量尺（仅验证用）。
///
/// 做法是「同一份轮廓按采样格平移后重复填充」。若每个字形要多填 N 次，
/// 一屏粗体文本的代价必须量出来——文字是界面里调用次数最多的原语，
/// 「为了字重把每帧拉长 30%」是不能接受的。
///
/// 判据：同一段文本、同一字号，`embolden` 取 0 与各档时的**每帧绘制耗时**。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/color.hpp"
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

/// 一屏典型的中英混排正文（约 2000 字形）。
constexpr std::string_view kParagraph =
    "霜天是一个原生桌面应用框架：全自绘、无系统控件、软硬件渲染兼容，"
    "并可以在没有桌面的服务器上开发与验证。文字是界面里最常见的原语，"
    "因此字形渲染的质量与代价直接决定整个框架的手感。The quick brown fox "
    "jumps over the lazy dog. 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ "
    "abcdefghijklmnopqrstuvwxyz 中文排版测试：进度与状态、光栅器覆盖率、"
    "字体缓存命中、声明式界面、软硬件渲染兼容、跨平台、无头可控、DPI 感知。";

constexpr int kWidth = 900;
constexpr int kHeight = 600;

[[nodiscard]] auto measure_ms(const TextRenderer& renderer, float embolden, int repeats) -> double {
  Canvas canvas{kWidth, kHeight, 1.5f};
  const auto start = std::chrono::steady_clock::now();
  for (int repeat = 0; repeat < repeats; ++repeat) {
    canvas.clear(Color::rgb(255, 255, 255));
    float y = 4.0f;
    for (int line = 0; line < 14; ++line) {
      (void)renderer.draw(canvas, kParagraph, Point{4.0f, y}, 13.5f, Color::rgb(0x0F, 0x17, 0x2A),
                          FontRole::Proportional, embolden);
      y += 20.0f;
    }
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count() / repeats;
}

}  // namespace

auto main() -> int {
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  for (const bool lcd : {true, false}) {
    TextRenderer renderer(fonts, 1.5f);
    renderer.set_subpixel(lcd);
    renderer.set_grid_fit(GridFitMode::Normal);
    st::print("\n=== {} ===", lcd ? "亚像素 LCD" : "灰度");
    // 先热身（字形位图缓存填充不计入）
    (void)measure_ms(renderer, 0.0f, 1);
    const double base = measure_ms(renderer, 0.0f, 5);
    st::print("\n  Regular（无加粗）      {:7.2f} ms/帧", base);
    for (const float physical : {16.5f, 20.25f, 31.5f, 39.0f}) {
      st::print("\n  —— 物理 {}px ——", physical);
      for (const auto [name, divisor] :
           {std::pair{"Medium  ", 48.0f}, {"SemiBold", 32.0f}, {"Bold    ", 20.0f}}) {
        const float radius = physical / divisor;
        const double ms = measure_ms(renderer, radius, 5);
        st::print("\n    {} radius={:.2f}px  {:7.2f} ms/帧  ({:+.1f}%)", name, radius, ms,
                  (ms / base - 1.0) * 100.0);
      }
    }
    st::print("\n");
  }
  return 0;
}
