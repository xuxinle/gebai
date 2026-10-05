// 阴影合成的「固定开销 vs 逐像素开销」分解（性能轮）。
//
// `draw_shadow` 每次调用都要重建一张 256 项的 `scaled_sources` 查表（4 通道 `scale_premul`），
// 而阴影颜色在一帧里往往**只有一两种**（同主题的卡片全是同一个 `shadow` 色）。
// 本探针量出：
//   a) 建表 256 项的耗时；
//   b) 逐像素 `over_premul` 的真实吞吐（固定 α，无分支）；
//   c) 带遮罩行读取 + 查表的完整内层循环吞吐。
// 由此判断"把表按颜色缓存"能省多少——以及逐像素成本的天花板在哪。

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/color.hpp"

namespace {

using Clock = std::chrono::steady_clock;

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
[[nodiscard]] constexpr auto scale_premul(std::uint32_t src, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) constexpr noexcept -> std::uint32_t {
    return fast_div255(value * alpha + 127U);
  };
  return pack(scale(channel(src, 24)), scale(channel(src, 16)), scale(channel(src, 8)),
              scale(channel(src, 0)));
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

template <class Body>
[[nodiscard]] auto best_ms(int repeat, Body&& body) -> double {
  double best = 1e9;
  for (int index = 0; index < repeat; ++index) {
    const auto start = Clock::now();
    body();
    best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 2000;
  const st::math::Color color = st::math::Color::rgb(0x0F, 0x17, 0x2A).with_alpha_f(0.15f);
  const std::uint32_t premul = st::math::premultiply(color);

  st::print("阴影合成开销分解（repeat={}）\n", repeat);

  // a) 建表
  std::array<std::uint32_t, 256> table{};
  st::print("  a) 建 256 项查表                {:>9.4} ms\n", best_ms(repeat, [&] {
             for (std::size_t level = 0; level < table.size(); ++level) {
               table[level] = scale_premul(
                   premul, static_cast<std::uint32_t>(st::math::clamp01(
                                                              static_cast<float>(level) / 255.0f) *
                                                              255.0f + 0.5f));
             }
           }));

  // b) 逐像素 over_premul（固定 α，无分支、无查表）
  constexpr int kCount = 16384;
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kCount), 0xFFF7F8FAU);
  const std::uint32_t fixed_src = table[200];
  st::print("  b) 逐像素 over_premul ×{}      {:>9.4} ms · {:.2f} ns/px\n",
            kCount, best_ms(repeat, [&] {
              for (int index = 0; index < kCount; ++index) {
                pixels[static_cast<std::size_t>(index)] =
                    over_premul(pixels[static_cast<std::size_t>(index)], fixed_src);
              }
            }),
            1e6 * best_ms(repeat, [&] {
              for (int index = 0; index < kCount; ++index) {
                pixels[static_cast<std::size_t>(index)] =
                    over_premul(pixels[static_cast<std::size_t>(index)], fixed_src);
              }
            }) / static_cast<double>(kCount));

  // c) 完整内层循环：读遮罩 + 查表 + 累加（模拟 draw_shadow 的热循环）
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(kCount));
  for (std::size_t index = 0; index < mask.size(); ++index) {
    mask[index] = static_cast<std::uint8_t>((index * 37U) % 256U);
  }
  st::print("  c) 读遮罩+查表+合成 ×{}        {:>9.4} ms · {:.2f} ns/px\n", kCount,
            best_ms(repeat, [&] {
              for (int index = 0; index < kCount; ++index) {
                const std::uint8_t m = mask[static_cast<std::size_t>(index)];
                if (m == 0U) continue;
                pixels[static_cast<std::size_t>(index)] =
                    over_premul(pixels[static_cast<std::size_t>(index)], table[m]);
              }
            }),
            1e6 * best_ms(repeat, [&] {
              for (int index = 0; index < kCount; ++index) {
                const std::uint8_t m = mask[static_cast<std::size_t>(index)];
                if (m == 0U) continue;
                pixels[static_cast<std::size_t>(index)] =
                    over_premul(pixels[static_cast<std::size_t>(index)], table[m]);
              }
            }) / static_cast<double>(kCount));

  // d) 对照：纯内存写（清屏）同尺寸
  const std::uint32_t fill = 0xFFF7F8FAU;
  st::print("  d) 对照 纯内存写 ×{}          {:>9.4} ms · {:.2f} ns/px\n", kCount,
            best_ms(repeat, [&] { std::fill(pixels.begin(), pixels.end(), fill); }),
            1e6 * best_ms(repeat, [&] { std::fill(pixels.begin(), pixels.end(), fill); }) /
                static_cast<double>(kCount));
  return 0;
}
