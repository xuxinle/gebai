// 量 grain（每帧 4200 个 1px 小圆）的代价，并对照几种替代实现。
//
// ## 为什么量
//
// `CAIYUN_PROFILE` 显示 clearing 的 `bg=22ms`，而里面的 vignette 只占 3.4ms、
// 铺光 0.4ms —— 剩下约 18ms 全在 `paint_grain`：**每帧 `fill_circle` 4200 次**，
// 半径 0.55~1.3 物理像素（基本是单点）。
//
// 每个圆都是一次完整的路径光栅化（扫描线生成 + 覆盖率 + 混合），
// 画 1 个像素的成本被摊成了几百次运算。
#include <chrono>
#include <cstdio>   // lint-allow: L7 探针输出（量测工具，不产出交付物）
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

namespace {

constexpr int W = 1920;
constexpr int H = 1080;
constexpr int kGrain = 4200;
constexpr int kIter = 20;

/// 与 `fx.cpp` 同构的哈希（只求分布与量级一致，不追求逐位相同）。
[[nodiscard]] auto hash01(std::uint32_t value, std::uint32_t salt) -> float {
  std::uint32_t x = value * 747796405U + salt * 2891336453U;
  x ^= x >> 16U;
  x *= 2246822519U;
  x ^= x >> 13U;
  x *= 3266489917U;
  x ^= x >> 16U;
  return static_cast<float>(x & 0xFFFFFFU) / 16777215.0f;
}

}  // namespace

int main() {
  st::raster::Canvas canvas{W, H, 1.0f};
  canvas.clear(st::math::Color{18, 26, 48, 255});

  // —— 现状：逐点 fill_circle ——
  {
    const auto t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      const std::uint32_t salt = static_cast<std::uint32_t>(it * 77) * 2654435761U;
      for (int i = 0; i < kGrain; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const float x = hash01(index ^ salt, 11U) * static_cast<float>(W);
        const float y = hash01(index ^ salt, 23U) * static_cast<float>(H);
        const float flicker = hash01(index ^ salt, 37U);
        const float shaped = flicker * flicker * flicker;
        const float a = 0.22f * (0.12f + 0.88f * shaped);
        if (a <= 0.004f) continue;
        const auto value = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        const float radius = 0.55f + 0.75f * hash01(index ^ salt, 41U);
        canvas.fill_circle(st::math::Point{x, y}, radius,
                           st::raster::Paint::solid(st::math::Color{226, 232, 255, value}));
      }
    }
    std::printf("%-30s %7.2f ms/帧\n", "现状：逐点 fill_circle", ms_since(t0) / kIter);
  }

  // —— 对照 A：只 set_pixel（无抗锯齿、单像素）——
  {
    const auto t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      const std::uint32_t salt = static_cast<std::uint32_t>(it * 77) * 2654435761U;
      for (int i = 0; i < kGrain; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const int x = static_cast<int>(hash01(index ^ salt, 11U) * static_cast<float>(W));
        const int y = static_cast<int>(hash01(index ^ salt, 23U) * static_cast<float>(H));
        const float flicker = hash01(index ^ salt, 37U);
        const float a = 0.22f * (0.12f + 0.88f * flicker * flicker * flicker);
        if (a <= 0.004f) continue;
        const auto value = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        canvas.set_pixel(x, y, st::math::Color{226, 232, 255, value});
      }
    }
    std::printf("%-30s %7.2f ms/帧\n", "对照 A：逐点 set_pixel", ms_since(t0) / kIter);
  }

  // —— 对照 B：直接写 pixels()（预计算 premultiply 后裸混）——
  //
  // 这是"批量贴颗粒"能达到的下限：没有路径光栅化、没有覆盖率、没有 API 开销。
  {
    struct Grain {
      std::size_t index;
      std::uint32_t premul;
    };
    std::vector<Grain> grains;
    grains.reserve(kGrain);

    const auto t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      const std::uint32_t salt = static_cast<std::uint32_t>(it * 77) * 2654435761U;
      grains.clear();
      for (int i = 0; i < kGrain; ++i) {
        const auto idx = static_cast<std::uint32_t>(i);
        const int x = static_cast<int>(hash01(idx ^ salt, 11U) * static_cast<float>(W));
        const int y = static_cast<int>(hash01(idx ^ salt, 23U) * static_cast<float>(H));
        const float flicker = hash01(idx ^ salt, 37U);
        const float a = 0.22f * (0.12f + 0.88f * flicker * flicker * flicker);
        if (a <= 0.004f) continue;
        const auto value = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        grains.push_back({static_cast<std::size_t>(y) * static_cast<std::size_t>(W) +
                              static_cast<std::size_t>(x),
                          st::math::premultiply(st::math::Color{226, 232, 255, value})});
      }
      auto* px = canvas.pixels().data();
      for (const auto& g : grains) {
        // src-over（标量，与 over_premul 同义）
        const std::uint32_t src = g.premul;
        const std::uint32_t sa = src & 0xFFU;
        if (sa == 0U) continue;
        const std::uint32_t inv = 255U - sa;
        const std::uint32_t d = px[g.index];
        const auto ch = [](std::uint32_t v, int s) { return (v >> s) & 0xFFU; };
        const auto mix = [&](std::uint32_t a2, std::uint32_t d2) {
          return a2 + (d2 * inv + 127U) / 255U;
        };
        px[g.index] = (mix(ch(src, 24), ch(d, 24)) << 24U) |
                      (mix(ch(src, 16), ch(d, 16)) << 16U) |
                      (mix(ch(src, 8), ch(d, 8)) << 8U) | mix(sa, ch(d, 0));
      }
    }
    std::printf("%-30s %7.2f ms/帧\n", "对照 B：预计算+裸写 pixels", ms_since(t0) / kIter);
  }

  // —— 对照 C：1×1 fill_rect（走纯色 SIMD 快路径）——
  {
    const auto t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      const std::uint32_t salt = static_cast<std::uint32_t>(it * 77) * 2654435761U;
      for (int i = 0; i < kGrain; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const float fx = hash01(index ^ salt, 11U) * static_cast<float>(W);
        const float fy = hash01(index ^ salt, 23U) * static_cast<float>(H);
        const float flicker = hash01(index ^ salt, 37U);
        const float a = 0.22f * (0.12f + 0.88f * flicker * flicker * flicker);
        if (a <= 0.004f) continue;
        const auto value = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        canvas.fill_rect(st::math::Rect{std::floor(fx), std::floor(fy), 1.0f, 1.0f},
                         st::raster::Paint::solid(st::math::Color{226, 232, 255, value}));
      }
    }
    std::printf("%-30s %7.2f ms/帧\n", "对照 C：1x1 fill_rect", ms_since(t0) / kIter);
  }

  // —— 对照 D：2x2 fill_rect（模拟柔边，面积 4x）——
  {
    const auto t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      const std::uint32_t salt = static_cast<std::uint32_t>(it * 77) * 2654435761U;
      for (int i = 0; i < kGrain; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        const float fx = hash01(index ^ salt, 11U) * static_cast<float>(W);
        const float fy = hash01(index ^ salt, 23U) * static_cast<float>(H);
        const float flicker = hash01(index ^ salt, 37U);
        const float a = 0.22f * (0.12f + 0.88f * flicker * flicker * flicker);
        if (a <= 0.004f) continue;
        const auto value = static_cast<std::uint8_t>(std::clamp(a * 0.6f, 0.0f, 1.0f) * 255.0f);
        canvas.fill_rect(st::math::Rect{std::floor(fx), std::floor(fy), 2.0f, 2.0f},
                         st::raster::Paint::solid(st::math::Color{226, 232, 255, value}));
      }
    }
    std::printf("%-30s %7.2f ms/帧\n", "对照 D：2x2 fill_rect", ms_since(t0) / kIter);
  }
  return 0;
}
