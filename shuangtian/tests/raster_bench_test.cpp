// 光栅器性能基准（**性能回归护栏**，不是精确的性能模型）。
//
// 为什么需要它：界面"卡"的时候，争论的焦点永远是"是不是阴影太慢/文字太慢"，
// 而软件光栅器的成本分布又高度依赖几何形态（边缘数、覆盖面积、模糊半径），
// 靠肉眼与直觉判断必然跑偏。这里把界面里最常见的四类绘制负载固化成可重复的测量。
//
// ## 断言怎么定
//
// 上界按 **`st test` 用的 debug 档**（`/Od`，比 dev/release 慢 5~10 倍）实测值留 ~4 倍余量，
// 目的是抓"某次改动让某条路径慢了一个数量级"，而不是把机器差异变成红灯。
// 同时也打印"相对于清屏（纯内存写，1M 像素）的倍数"——它在同一台机器上可比，
// 便于横向看哪类原语贵得不合理（实测：圆角填充 ≈5×、投影 ≈40×、文字 ≈80×、渐变 ≈165×）。

#include <chrono>
#include <cstdint>
#include <format>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kCanvasWidth = 1280;
inline constexpr int kCanvasHeight = 800;

[[nodiscard]] auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto make_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kCanvasHeight, 1.0f);
  canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

/// 基准单位：把整块画布清一次色的**每像素毫秒数**。
[[nodiscard]] auto baseline_ms_per_pixel() -> double {
  st::raster::Canvas canvas = make_canvas();
  constexpr int kRuns = 5;
  const auto start = Clock::now();
  for (int index = 0; index < kRuns; ++index) {
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
  }
  const double ms = elapsed_ms(start) / kRuns;
  return ms / (static_cast<double>(kCanvasWidth) * static_cast<double>(kCanvasHeight));
}

}  // namespace

ST_TEST(bench_rounded_card_fill) {
  const double unit = baseline_ms_per_pixel();
  st::raster::Canvas canvas = make_canvas();
  const st::raster::Paint paint = st::raster::Paint::solid(st::math::Color::rgb(255, 255, 255));
  constexpr int kCards = 60;
  constexpr double kCardWidth = 320.0;
  constexpr double kCardHeight = 160.0;
  const auto start = Clock::now();
  for (int index = 0; index < kCards; ++index) {
    const float x = static_cast<float>(index % 6) * 200.0f + 20.0f;
    const float y = static_cast<float>(index / 6) * 130.0f + 20.0f;
    canvas.fill_rect(st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)},
                     paint, 12.0f);
  }
  const double ms = elapsed_ms(start);
  const double per_card = ms / kCards;
  const double pixels = kCardWidth * kCardHeight;
  st::print("[bench] 圆角卡片填充 ×{}: {:.2f} ms（{:.3f} ms/张，{:.2f}× 清屏单位）\n", kCards, ms,
            per_card, per_card / pixels / unit);
  ST_CHECK(per_card <= 1.5);
}

ST_TEST(bench_card_shadow) {
  const double unit = baseline_ms_per_pixel();
  st::raster::Canvas canvas = make_canvas();
  const st::math::Color color{0, 0, 0, 40};
  constexpr int kShadows = 20;
  constexpr double kCardWidth = 300.0;
  constexpr double kCardHeight = 140.0;
  constexpr double kBlur = 16.0;
  const double area = (kCardWidth + kBlur * 4.0 + 4.0) * (kCardHeight + kBlur * 4.0 + 4.0);
  const auto start = Clock::now();
  for (int index = 0; index < kShadows; ++index) {
    const float x = static_cast<float>(index % 4) * 320.0f + 24.0f;
    const float y = static_cast<float>(index / 4) * 160.0f + 24.0f;
    canvas.draw_shadow(st::math::Rect{x, y, static_cast<float>(kCardWidth),
                                      static_cast<float>(kCardHeight)},
                       12.0f, static_cast<float>(kBlur), color, st::math::Point{0.0f, 4.0f});
  }
  const double ms = elapsed_ms(start);
  const double per_shadow = ms / kShadows;
  st::print("[bench] 卡片投影 ×{}（blur={}）: {:.2f} ms（{:.3f} ms/个，{:.2f}× 清屏单位）\n",
            kShadows, kBlur, ms, per_shadow, per_shadow / area / unit);
  // 同尺寸阴影命中遮罩缓存：稳态成本 = 合成（逐像素混合 + 查表）
  ST_CHECK(per_shadow <= 12.0);
}

ST_TEST(bench_text_draw) {
  const double unit = baseline_ms_per_pixel();
  st::raster::Canvas canvas = make_canvas();
  auto fonts = st::text::FontStack::system_default();
  if (!fonts) return;  // 无字体环境跳过（CI 容器）
  st::text::TextRenderer renderer(*fonts);
  const std::string sample = "霜天 · 组件画廊 1280x800 headless control 0123456789";
  constexpr int kLines = 60;
  const auto start = Clock::now();
  double glyph_pixels = 0.0;
  for (int index = 0; index < kLines; ++index) {
    const float y = static_cast<float>(index % 24) * 30.0f + 20.0f;
    (void)renderer.draw(canvas, sample, st::math::Point{24.0f, y}, 14.0f,
                        st::math::Color::rgb(0x1F, 0x24, 0x2C));
    // 字形位图按 14px 字号估算覆盖面积（用于每像素成本换算）
    glyph_pixels += static_cast<double>(sample.size()) * 14.0 * 14.0 * 0.5;
  }
  const double ms = elapsed_ms(start);
  const double per_line = ms / kLines;
  st::print("[bench] 文本 ×{}（{} 字符/行，字型缓存命中）: {:.2f} ms（{:.3f} ms/行，{:.2f}× 清屏单位）\n",
            kLines, sample.size(), ms, per_line,
            (per_line / (glyph_pixels / kLines)) / unit);
  ST_CHECK(per_line <= 2.0);
}

ST_TEST(bench_gradient_fill) {
  const double unit = baseline_ms_per_pixel();
  st::raster::Canvas canvas = make_canvas();
  const st::raster::Paint paint = st::raster::Paint::with_gradient(st::raster::Gradient::linear(
      st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, 800.0f},
      {{0.0f, st::math::Color::rgb(0x3B, 0x82, 0xF6)},
       {1.0f, st::math::Color::rgb(0x93, 0xC5, 0xFD)}}));
  constexpr int kRects = 20;
  constexpr double kRectWidth = 1200.0;
  constexpr double kRectHeight = 36.0;
  const auto start = Clock::now();
  for (int index = 0; index < kRects; ++index) {
    const float y = static_cast<float>(index) * 40.0f;
    canvas.fill_rect(st::math::Rect{40.0f, y, static_cast<float>(kRectWidth),
                                    static_cast<float>(kRectHeight)},
                     paint, 8.0f);
  }
  const double ms = elapsed_ms(start);
  const double per_rect = ms / kRects;
  const double pixels = kRectWidth * kRectHeight;
  st::print("[bench] 渐变填充 ×{}（1200×36）: {:.2f} ms（{:.3f} ms/块，{:.2f}× 清屏单位）\n", kRects,
            ms, per_rect, per_rect / pixels / unit);
  // 渐变必须逐像素采样画笔：比纯色贵是必然的，但不该贵到几十倍
  ST_CHECK(per_rect <= 25.0);
}
