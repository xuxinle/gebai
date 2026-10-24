// 帧级成本分解基准：clear / to_rgba8 / 双线性重采样 各占多少
//
// 目的：单帧渲染 217ms 里，有多少是"必要的内容绘制"、多少是画布的内存操作。
// 若内存操作占大头，优化方向是复用缓冲；若绘制占大头，才需要脏矩形。
#include <chrono>
#include <cstdio>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
  constexpr int W = 1920, H = 1080;
  constexpr int kIter = 20;

  // ① clear
  st::raster::Canvas canvas(W, H, 1.0f);
  auto t0 = Clock::now();
  for (int i = 0; i < kIter; ++i) canvas.clear(st::math::Color{10, 20, 30, 255});
  std::printf("clear          %.2f ms/次\n", ms_since(t0) / kIter);

  // ② to_rgba8（每帧一次，管道模式必需）
  t0 = Clock::now();
  for (int i = 0; i < kIter; ++i) {
    const auto rgba = canvas.to_rgba8();
    (void)rgba.size();
  }
  std::printf("to_rgba8       %.2f ms/次\n", ms_since(t0) / kIter);

  // ③ 直接读 pixels()（对照：不做转换的开销）
  t0 = Clock::now();
  for (int i = 0; i < kIter; ++i) {
    const auto& px = canvas.pixels();
    (void)px.size();
  }
  std::printf("pixels() 只读   %.4f ms/次\n", ms_since(t0) / kIter);

  return 0;
}
