// `draw_shadow_layered` 逐次开销分解（性能轮）。
//
// 疑问：合并贴图的合成核应当只有 `over_premul`（~2.2 ns/px），96 张卡片量出 6.9 ms/帧
// （72 µs/次），而贴图非零跨度只有 15744 px ⇒ 纯合成约 34 µs。差的 38 µs 在哪？
// 本探针把它拆开：
//   A 端到端（含剖析器记账两次时钟读）
//   B 关掉剖析器（去掉每调用的 clock_gettime）
//   C 只查缓存（不发绘制）——量 `layered_shadow()` 的哈希 + 线性扫描
//   D 只做纯 `over_premul`（同像素数，理想下界）

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;

[[nodiscard]] constexpr auto channel(std::uint32_t pixel, unsigned shift) noexcept -> std::uint32_t {
  return (pixel >> shift) & 0xFFU;
}
[[nodiscard]] constexpr auto pack(std::uint32_t r, std::uint32_t g, std::uint32_t b,
                                  std::uint32_t a) noexcept -> std::uint32_t {
  return (r << 24U) | (g << 16U) | (b << 8U) | a;
}
[[nodiscard]] constexpr auto fast_div255(std::uint32_t value) noexcept -> std::uint32_t {
  return (value * 0x8081U) >> 23U;
}
[[nodiscard]] constexpr auto over_premul(std::uint32_t dst, std::uint32_t src) noexcept
    -> std::uint32_t {
  const std::uint32_t sa = channel(src, 0);
  if (sa == 0U) return dst;
  const std::uint32_t inverse = 255U - sa;
  const auto mix = [inverse](std::uint32_t source, std::uint32_t destination) constexpr noexcept {
    return source + fast_div255(destination * inverse + 127U);
  };
  return pack(mix(channel(src, 24), channel(dst, 24)), mix(channel(src, 16), channel(dst, 16)),
              mix(channel(src, 8), channel(dst, 8)), mix(sa, channel(dst, 0)));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 200;
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow md = st::ui::shadow_md(theme);
  constexpr float kRadius = 12.0f;
  constexpr int kCards = 96;

  st::raster::PaintProfiler profiler;
  const auto draw_all = [&](st::raster::Canvas& canvas, bool profile) {
    canvas.set_profiler(profile ? &profiler : nullptr);
    for (int index = 0; index < kCards; ++index) {
      const float x = 40.0f + static_cast<float>(index % 8) * 150.0f;
      const float y = 40.0f + static_cast<float>(index / 8) * 60.0f;
      canvas.draw_shadow_layered(st::math::Rect{x, y, 140.0f, 48.0f}, kRadius, md.color, md.blur,
                                 st::math::Point{md.offset_x, md.offset_y}, md.color2, md.blur2,
                                 st::math::Point{md.offset2_x, md.offset2_y});
    }
  };

  struct Row {
    const char* name;
    double ms{0.0};
  };
  std::vector<Row> rows;

  // A 端到端（无剖析器）
  {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      canvas.set_profiler(nullptr);
      const auto start = Clock::now();
      draw_all(canvas, false);
      best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    rows.push_back({"A 96 张卡片两层阴影（无剖析器）", best});
  }
  // B 端到端（挂剖析器，复刻基线场景）
  {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      canvas.set_profiler(&profiler);
      profiler.clear();
      const auto start = Clock::now();
      draw_all(canvas, true);
      best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    rows.push_back({"B 同上（挂剖析器，含 384 次时钟读）", best});
  }
  // C 逐张的“非缓存命中”路径（每张换几何 → 每张都重建遮罩+贴图）
  {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      canvas.set_profiler(nullptr);
      const auto start = Clock::now();
      for (int card = 0; card < kCards; ++card) {
        const float x = 40.0f + static_cast<float>(card % 8) * 150.0f;
        const float y = 40.0f + static_cast<float>(card / 8) * 60.0f;
        canvas.draw_shadow_layered(st::math::Rect{x, y, 140.0f, 48.0f}, kRadius, md.color, md.blur,
                                   st::math::Point{0.0f, static_cast<float>(card % 64) * 0.25f},
                                   md.color2, md.blur2,
                                   st::math::Point{static_cast<float>(card % 64) * 0.25f, 0.0f});
      }
      best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    rows.push_back({"C 同上但每张换几何（贴图全未命中）", best});
  }
  // D 纯 over_premul 下界（15744 px × 96）
  {
    std::vector<std::uint32_t> buffer(static_cast<std::size_t>(kWidth) * kHeight, 0xFFF7F8FAU);
    double best = 1e9;
    for (int index = 0; index < repeat; ++index) {
      const auto start = Clock::now();
      std::size_t at = 0;
      for (int card = 0; card < kCards; ++card) {
        for (int px = 0; px < 15744; ++px) {
          buffer[at] = over_premul(buffer[at], 0x400F172AU);
          at = at + 1 >= buffer.size() ? 0 : at + 1;
        }
      }
      best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    rows.push_back({"D 下界：96×15744 px 纯 over_premul", best});
  }

  st::print("draw_shadow_layered 开销分解（{} 次重复，取最快）\n", repeat);
  for (const Row& row : rows) {
    st::print("  {:<42} {:>8.3} ms · {:>6.1} µs/张\n", row.name, row.ms, row.ms * 1000.0 / kCards);
  }
  return 0;
}
