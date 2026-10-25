// 量 `vignette` 四条渐变带在 1080p 上的代价，并对照"能用竖向代替到什么程度"。
//
// ## 为什么要量
//
// `CAIYUN_PROFILE` 显示 `bg=20.5ms` 是单帧最大一项。`paint_backdrop` 里
// 有 `vertical_gradient` + 两束 `wash_vertical` + `vignette` 四条带。
// 前三者都走竖向（框架有 SIMD 整行混合），只有 vignette 的**左右两条是横向**。
//
// 横向渐变在框架里走"倾斜线性"分支（逐像素 `blend_one`，无 SIMD），
// 而竖向走 `simd::blend_row` 整行。两者差多少，量出来才知道值不值得动。
#include <chrono>
#include <cstdio>   // lint-allow: L7 探针输出（量测工具，不产出交付物）
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/simd.hpp"
#include "st/raster/surface.hpp"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

namespace {

constexpr int W = 1920;
constexpr int H = 1080;
constexpr int kIter = 30;

st::raster::Paint linear(st::math::Point from, st::math::Point to, float a_from, float a_to) {
  const st::math::Color c0{0, 0, 0, static_cast<std::uint8_t>(255.0f * a_from)};
  const st::math::Color c1{0, 0, 0, static_cast<std::uint8_t>(255.0f * a_to)};
  return st::raster::Paint::with_gradient(st::raster::Gradient::linear(
      from, to, {st::raster::GradientStop{0.0f, c0}, st::raster::GradientStop{1.0f, c1}}));
}

}  // namespace

int main() {
  st::raster::Canvas canvas{W, H, 1.0f};
  canvas.clear(st::math::Color{20, 30, 60, 255});

  // vignette 的实际几何（与 motion.cpp 一致：band = 120 设计像素 × 0.96 系数）
  const float band = 120.0f * (0.6f + 0.4f * 0.9f);
  const float w = static_cast<float>(W);
  const float h = static_cast<float>(H);

  struct Case {
    const char* name;
    bool horizontal;
    st::math::Rect box;
  };
  const Case cases[] = {
      {"上带 竖向 (1920x115)", false, {0.0f, 0.0f, w, band}},
      {"下带 竖向 (1920x115)", false, {0.0f, h - band, w, band}},
      {"左带 横向 (115x1080)", true, {0.0f, 0.0f, band, h}},
      {"右带 横向 (115x1080)", true, {w - band, 0.0f, band, h}},
  };

  std::printf("%-26s %10s %12s\n", "带", "耗时", "像素数");
  std::printf("%s\n", "----------------------------------------------------------");
  double vertical_total = 0.0;
  double horizontal_total = 0.0;
  for (const auto& c : cases) {
    auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      if (c.horizontal) {
        canvas.fill_rect(c.box, linear(st::math::Point{c.box.x, 0.0f},
                                       st::math::Point{c.box.x + c.box.width, 0.0f}, 0.5f, 0.0f));
      } else {
        canvas.fill_rect(c.box, linear(st::math::Point{0.0f, c.box.y},
                                       st::math::Point{0.0f, c.box.y + c.box.height}, 0.5f, 0.0f));
      }
    }
    const double ms = ms_since(t0) / kIter;
    const auto px = static_cast<long long>(c.box.width * c.box.height);
    std::printf("%-26s %8.2f ms %12lld\n", c.name, ms, px);
    if (c.horizontal) {
      horizontal_total += ms;
    } else {
      vertical_total += ms;
    }
  }
  std::printf("%s\n", "----------------------------------------------------------");
  std::printf("竖向合计（上下）  %8.2f ms\n", vertical_total);
  std::printf("横向合计（左右）  %8.2f ms\n", horizontal_total);
  std::printf("四带合计          %8.2f ms\n", vertical_total + horizontal_total);

  // 纯色叠加（如果把横向换成"两段纯色"的近似）代价参照
  {
    const auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      canvas.fill_rect(st::math::Rect{0.0f, 0.0f, band, h},
                       st::raster::Paint::solid(st::math::Color{0, 0, 0, 40}));
      canvas.fill_rect(st::math::Rect{w - band, 0.0f, band, h},
                       st::raster::Paint::solid(st::math::Color{0, 0, 0, 40}));
    }
    std::printf("对照：左右两段**纯色** %6.2f ms\n", ms_since(t0) / kIter);
  }

  // ── 候选方案：竖向分条近似横向衰减 ──
  //
  // 把 115px 宽的横带切成 N 条竖条，每条一个**纯色**（alpha 按横向位置递变）。
  // 每条都是整幅高度、115/N 宽 —— 纯色填充走 SIMD，代价与面积成正比且极低。
  // 观感取决于 N：N 越大越平滑，但条数多了反而出现"竖条纹"（离散化）。
  std::printf("\n%s\n", "候选：横向带切成 N 条纯色竖条");
  std::printf("%-8s %10s %s\n", "N", "耗时", "每条宽度");
  for (const int n : {4, 6, 8, 12, 16, 24}) {
    const float strip = band / static_cast<float>(n);
    const auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      for (int k = 0; k < n; ++k) {
        // 横向位置 → alpha 线性衰减（与原来横向渐变同一函数）
        const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
        const auto a = static_cast<std::uint8_t>(255.0f * 0.5f * (1.0f - t) + 0.5f);
        canvas.fill_rect(st::math::Rect{static_cast<float>(k) * strip, 0.0f, strip + 1.0f, h},
                         st::raster::Paint::solid(st::math::Color{0, 0, 0, a}));
        canvas.fill_rect(
            st::math::Rect{w - band + static_cast<float>(k) * strip, 0.0f, strip + 1.0f, h},
            st::raster::Paint::solid(st::math::Color{0, 0, 0, a}));
      }
    }
    std::printf("%-8d %8.2f ms %8.1f px\n", n, ms_since(t0) / kIter,
                static_cast<double>(strip));
  }
  // ── bg 的完整账目：逐层量 ──
  std::printf("\n%s\n", "paint_backdrop 逐层代价（1080p）");
  {
    // ① 底色竖向渐变
    auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      canvas.fill_rect(st::math::Rect{0.0f, 0.0f, w, h},
                       linear(st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, h}, 1.0f, 1.0f));
    }
    std::printf("  底色竖向渐变（全幅）  %6.2f ms\n", ms_since(t0) / kIter);

    // ② 一束竖向铺光（全幅）
    t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      canvas.fill_rect(st::math::Rect{0.0f, 0.0f, w, h},
                       linear(st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, h}, 0.26f, 0.0f));
    }
    std::printf("  一束竖向铺光（全幅）  %6.2f ms\n", ms_since(t0) / kIter);

    // ③ vignette 四带（与 motion.cpp 同几何）
    t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      canvas.fill_rect(st::math::Rect{0.0f, 0.0f, w, band},
                       linear(st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, band}, 0.5f, 0.0f));
      canvas.fill_rect(st::math::Rect{0.0f, h - band, w, band},
                       linear(st::math::Point{0.0f, h}, st::math::Point{0.0f, h - band}, 0.5f, 0.0f));
      canvas.fill_rect(st::math::Rect{0.0f, 0.0f, band, h},
                       linear(st::math::Point{0.0f, 0.0f}, st::math::Point{band, 0.0f}, 0.5f, 0.0f));
      canvas.fill_rect(st::math::Rect{w - band, 0.0f, band, h},
                       linear(st::math::Point{w, 0.0f}, st::math::Point{w - band, 0.0f}, 0.5f, 0.0f));
    }
    std::printf("  vignette 四带合计     %6.2f ms\n", ms_since(t0) / kIter);
  }
  // ── 窄带候选：缩窄左右带能省多少 ──
  std::printf("\n%s\n", "左右带宽度 vs 代价（面积线性）");
  std::printf("%-12s %10s %10s\n", "带宽", "左右合计", "相对");
  for (const float bw : {60.0f, 80.0f, 100.0f, band, 160.0f}) {
    const auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      canvas.fill_rect(st::math::Rect{0.0f, 0.0f, bw, h},
                       linear(st::math::Point{0.0f, 0.0f}, st::math::Point{bw, 0.0f}, 0.5f, 0.0f));
      canvas.fill_rect(st::math::Rect{w - bw, 0.0f, bw, h},
                       linear(st::math::Point{w, 0.0f}, st::math::Point{w - bw, 0.0f}, 0.5f, 0.0f));
    }
    const double ms = ms_since(t0) / kIter;
    std::printf("%-12.0f %8.2f ms %9.0f%%\n", static_cast<double>(bw), ms,
                100.0 * static_cast<double>(bw) / static_cast<double>(band));
  }
  // ── 蒙版方案：一次性离线合成"暗角图"，运行时只贴一次 ──
  //
  // 暗角是**静态**的（只跟尺寸和 strength 有关，不随时间变），
  // 完全可以预渲染成一张图，每帧只 draw 一次。
  // 这里量的是"贴一张全幅半透明图"的代价。
  std::printf("\n%s\n", "对照：贴一张全幅 RGBA 蒙版（即预渲染暗角）");
  {
    st::raster::Canvas mask{W, H, 1.0f};
    mask.clear(st::math::Color{0, 0, 0, 0});
    // 蒙版内容：四边暗、中心透明（这里只关心"贴图"耗时，内容用渐变近似填一下）
    mask.fill_rect(st::math::Rect{0.0f, 0.0f, w, h},
                   linear(st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, h}, 0.30f, 0.0f));
    const auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      canvas.draw_canvas(mask, st::math::Rect{0.0f, 0.0f, w, h});
    }
    std::printf("  全幅贴图 draw_canvas  %6.2f ms\n", ms_since(t0) / kIter);
  }
  // ── 候选：横向渐变用"预计算整行源色 + simd::over_row" ──
  //
  // 横向渐变在**同一列**颜色恒定 ⇒ 每一行的源色序列完全相同。
  // 所以可以只算一遍（带宽 个像素），然后每行复用 + 走 SIMD 混合。
  std::printf("\n%s\n", "候选：预计算整行源色 + simd::over_row");
  {
    const int bw = static_cast<int>(band);
    std::vector<std::uint32_t> src(static_cast<std::size_t>(bw));
    // 预算一遍（只做一次，代价可忽略）
    for (int x = 0; x < bw; ++x) {
      const float t = static_cast<float>(x) / static_cast<float>(bw);
      const auto a = static_cast<std::uint8_t>(255.0f * 0.5f * (1.0f - t) + 0.5f);
      src[static_cast<std::size_t>(x)] =
          st::math::premultiply(st::math::Color{0, 0, 0, a});
    }
    auto* pixels = canvas.pixels().data();
    const std::size_t stride = static_cast<std::size_t>(W);
    const auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      for (int y = 0; y < H; ++y) {
        st::raster::simd::over_row(pixels + static_cast<std::size_t>(y) * stride, src.data(),
                                   static_cast<std::size_t>(bw));
        st::raster::simd::over_row(pixels + static_cast<std::size_t>(y) * stride + (W - bw),
                                   src.data(), static_cast<std::size_t>(bw));
      }
    }
    std::printf("  左右两条（预计算+SIMD）  %6.2f ms\n", ms_since(t0) / kIter);
  }
  // ── 拆开量：57px 短行的 SIMD 收益到底有没有 ──
  std::printf("\n%s\n", "短行（57px）逐像素 vs simd::over_row");
  {
    constexpr int kW = 115, kH = 1080;
    std::vector<std::uint32_t> row_buf(static_cast<std::size_t>(kW));
    for (int x = 0; x < kW; ++x) {
      const float t = static_cast<float>(x) / static_cast<float>(kW);
      row_buf[static_cast<std::size_t>(x)] =
          st::math::premultiply(st::math::Color{0, 0, 0,
                                                static_cast<std::uint8_t>(128.0f * (1.0f - t))});
    }
    auto* pixels = canvas.pixels().data();
    const std::size_t stride = static_cast<std::size_t>(W);

    auto t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      for (int y = 0; y < kH; ++y) {
        auto* row = pixels + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < kW; ++x) {
          // 标量 src-over（与 over_premul 同义，这里内联以便量到"纯标量"下限）
          const std::uint32_t src = row_buf[static_cast<std::size_t>(x)];
          const std::uint32_t sa = src & 0xFFU;
          if (sa == 0U) continue;
          const std::uint32_t inv = 255U - sa;
          const auto ch = [](std::uint32_t px, int shift) {
            return (px >> shift) & 0xFFU;
          };
          const auto mix = [&](std::uint32_t a, std::uint32_t d) {
            return a + (d * inv + 127U) / 255U;
          };
          row[x] = (mix(ch(src, 24), ch(row[x], 24)) << 24U) |
                   (mix(ch(src, 16), ch(row[x], 16)) << 16U) |
                   (mix(ch(src, 8), ch(row[x], 8)) << 8U) |
                   mix(sa, ch(row[x], 0));
        }
      }
    }
    std::printf("  逐像素           %6.2f ms\n", ms_since(t0) / kIter);

    t0 = Clock::now();
    for (int it = 0; it < kIter; ++it) {
      for (int y = 0; y < kH; ++y) {
        auto* row = pixels + static_cast<std::size_t>(y) * stride;
        st::raster::simd::over_row(row, row_buf.data(), static_cast<std::size_t>(kW));
      }
    }
    std::printf("  simd::over_row   %6.2f ms\n", ms_since(t0) / kIter);
  }
  return 0;
}
