// GPU 导出的时间去向：把"每帧 34ms"拆开，看真正的瓶颈在哪。
//
// ## 为什么要专门量
//
// 之前测过：纯绘制 2.56ms、取像素 5.09ms（探针口径，1600 图元）。
// 但真实导出是 34ms/帧 —— **差 27ms 不知道去哪了**。
// 在查清之前任何优化都是猜。
//
// ## 拆的四段
//
// 1. `Map` 之前（提交 + CopyResource + Flush）
// 2. `Map` 到 `Unmap`（GPU→CPU 传输，受带宽约束）
// 3. 逐像素转换（RGBA 字节 → uint32 → 再拆回字节，两遍）
// 4. `to_rgba8` 的拷贝
#include <array>
#include <chrono>
#include <cstdio>   // lint-allow: L7 探针输出（量测工具，不产出交付物）
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/surface.hpp"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
  constexpr int W = 1920, H = 1080;
  constexpr int kIter = 20;

  // —— 软件画布基线 ——
  {
    st::raster::Canvas canvas{W, H, 1.0f};
    canvas.clear(st::math::Color{10, 20, 40, 255});
    auto t0 = Clock::now();
    std::size_t bytes = 0;
    for (int i = 0; i < kIter; ++i) bytes += canvas.to_rgba8().size();
    std::printf("[软件] to_rgba8      %7.2f ms/次  (%.1f MB)\n", ms_since(t0) / kIter,
                static_cast<double>(bytes) / kIter / 1048576.0);

    t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) (void)canvas.pixels();
    std::printf("[软件] pixels()       %7.4f ms/次\n", ms_since(t0) / kIter);
  }

  // —— GPU 画布 ——
  if (!st::raster::gpu::available()) {
    std::printf("[GPU] 不可用\n");
    return 0;
  }
  auto created = st::raster::gpu::create_canvas(W, H, 1.0f, {});
  if (!created.has_value()) {
    std::printf("[GPU] 创建失败\n");
    return 1;
  }
  st::raster::Surface& gpu = **created;
  gpu.clear(st::math::Color{10, 20, 40, 255});

  {
    // ① 首次 to_rgba8（含一次完整回读）
    auto t0 = Clock::now();
    (void)gpu.to_rgba8();
    std::printf("[GPU] to_rgba8 首次  %7.2f ms（含 staging 纹理创建）\n", ms_since(t0));

    // ② 稳定态：每次都要回读（导出路径本就如此，每帧内容都变）
    t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      gpu.clear(st::math::Color{
          static_cast<std::uint8_t>(i * 3 % 256), 20, 40, 255});
      gpu.clear(st::math::Color{10, 20, 40, 255});
      (void)gpu.to_rgba8();
    }
    std::printf("[GPU] to_rgba8 稳态  %7.2f ms/次（每帧都回读）\n", ms_since(t0) / kIter);

    // ③ 只 pixels()（同样走回读，但不做那次多余的字节拆分）
    t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      gpu.clear(st::math::Color{10, 20, 40, 255});
      (void)gpu.pixels().size();
    }
    std::printf("[GPU] pixels() 稳态  %7.2f ms/次（回读但不转字节）\n", ms_since(t0) / kIter);
  }
  // ── 真实负载：1600 图元的绘制 vs 回读 ──
  //
  // 探针的纯绘制是 2.56ms（空画布清屏为主）。这里用真实图元量一次，
  // 看"29ms/帧"里绘制与回读各占多少，剩下的就是别的东西。
  {
    std::vector<std::array<float, 5>> shapes;
    shapes.reserve(1600);
    const int cols = 52;
    const float cell = 34.0f;
    for (int i = 0; i < 1600; ++i) {
      const int col = i % cols, row = i / cols;
      shapes.push_back({40.0f + static_cast<float>(col) * cell,
                        30.0f + static_cast<float>(row) * cell, cell - 6.0f, cell - 6.0f,
                        static_cast<float>((i * 37) % 140) / 255.0f});
    }
    auto draw_all = [&](st::raster::Surface& surf) {
      for (const auto& sh : shapes) {
        st::raster::Paint paint = st::raster::Paint::solid(
            st::math::Color{static_cast<std::uint8_t>(80 + sh[4] * 140), 140, 220, 255});
        surf.fill_rect(st::math::Rect{sh[0], sh[1], sh[2], sh[3]}, paint, 3.0f);
      }
    };
    gpu.clear(st::math::Color{10, 20, 40, 255});
    draw_all(gpu);
    (void)gpu.to_rgba8();

    auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      gpu.clear(st::math::Color{10, 20, 40, 255});
      draw_all(gpu);
    }
    const double draw_ms = ms_since(t0) / kIter;

    t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      gpu.clear(st::math::Color{10, 20, 40, 255});
      draw_all(gpu);
      (void)gpu.to_rgba8();
    }
    const double total_ms = ms_since(t0) / kIter;
    std::printf("\n[真实负载 1600 图元]\n  只绘制（录制）     %7.2f ms/帧\n"
                "  绘制 + 回读        %7.2f ms/帧\n  回读增量           %7.2f ms/帧\n",
                draw_ms, total_ms, total_ms - draw_ms);
  }

  return 0;
}
