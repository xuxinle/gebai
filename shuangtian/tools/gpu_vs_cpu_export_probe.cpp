// 软件画布 vs GPU 画布的实测对比（裁云导出场景的口径：离屏、无窗口、1080p）。
//
// 为什么必须实测而不是看架构：框架文档里有过"GPU 在三维导出上反而慢 12 倍"
// 的记录（`model3d.cpp`），因为回读+并行排队。所以二维离屏是否真的更快，
// 只能用裁云真实的图元负载量出来。
//
// 口径与裁云一致：
//   · 1080p / device_scale = 1
//   · 图元数量取 clearing 的峰值（约 1600 个矩形）
//   · 每帧先 clear，再画全部图元
//   · 关键指标是 **to_rgba8()**（导出要拿像素送 ffmpeg，这是必经之路）
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/surface.hpp"

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

namespace {

struct Shape {
  float x, y, w, h;
  st::math::Color color;
};

// 模拟 clearing：密集网格 + 若干大块。数量与真实片子一致（约 1600）。
std::vector<Shape> build_shapes(int count) {
  std::vector<Shape> out;
  out.reserve(static_cast<std::size_t>(count));
  const int cols = 52;
  const float cell = 34.0f;
  for (int i = 0; i < count; ++i) {
    const int col = i % cols;
    const int row = i / cols;
    const float x = 40.0f + static_cast<float>(col) * cell;
    const float y = 30.0f + static_cast<float>(row) * cell;
    const std::uint8_t v = static_cast<std::uint8_t>(80 + (i * 37) % 140);
    out.push_back({x, y, cell - 6.0f, cell - 6.0f, st::math::Color{v, 140, 220, 255}});
  }
  return out;
}

void draw_all(st::raster::Surface& surface, const std::vector<Shape>& shapes) {
  for (const auto& s : shapes) {
    st::raster::Paint paint = st::raster::Paint::solid(s.color);
    surface.fill_rect(st::math::Rect{s.x, s.y, s.w, s.h}, paint);
  }
}

}  // namespace

int main() {
  constexpr int W = 1920, H = 1080;
  constexpr int kFrames = 30;
  const auto shapes = build_shapes(1600);
  std::printf("负载：%d 个矩形 / %dx%d\n\n", static_cast<int>(shapes.size()), W, H);

  // ── 软件画布 ──
  {
    st::raster::Canvas canvas{W, H, 1.0f};
    // 预热
    canvas.clear(st::math::Color{10, 20, 40, 255});
    draw_all(canvas, shapes);
    (void)canvas.to_rgba8();

    auto t0 = Clock::now();
    for (int f = 0; f < kFrames; ++f) {
      canvas.clear(st::math::Color{10, 20, 40, 255});
      draw_all(canvas, shapes);
    }
    const double draw_ms = ms_since(t0) / kFrames;

    t0 = Clock::now();
    std::size_t bytes = 0;
    for (int f = 0; f < kFrames; ++f) {
      const auto rgba = canvas.to_rgba8();
      bytes += rgba.size();
    }
    const double copy_ms = ms_since(t0) / kFrames;
    std::printf("[软件画布]\n  绘制  %.2f ms/帧\n  取像素 %.2f ms/帧  （%.1f MB/帧）\n"
                "  合计  %.2f ms/帧\n\n",
                draw_ms, copy_ms, static_cast<double>(bytes) / kFrames / 1048576.0,
                draw_ms + copy_ms);
  }

  // ── GPU 画布 ──
  {
    if (!st::raster::gpu::available()) {
      std::printf("[GPU 画布] 不可用，跳过\n");
      return 0;
    }
    auto created = st::raster::gpu::create_canvas(W, H, 1.0f, {});
    if (!created.has_value()) {
      std::printf("[GPU 画布] 创建失败：%s\n", created.error().message.c_str());
      return 1;
    }
    std::unique_ptr<st::raster::Surface> gpu = std::move(*created);
    st::raster::Surface& surface = *gpu;

    surface.clear(st::math::Color{10, 20, 40, 255});
    draw_all(surface, shapes);
    (void)surface.to_rgba8();

    auto t0 = Clock::now();
    for (int f = 0; f < kFrames; ++f) {
      surface.clear(st::math::Color{10, 20, 40, 255});
      draw_all(surface, shapes);
    }
    // GPU 是异步的：计时前必须等它真的画完，否则量到的是"提交命令"的时间
    // （那会得到一个虚假的 0.1ms，和实测完全对不上）。
    // `to_rgba8()` 强制回读 = 隐式全同步，这里借它做屏障。
    (void)surface.to_rgba8();
    const double draw_ms = ms_since(t0) / kFrames;

    t0 = Clock::now();
    std::size_t bytes = 0;
    for (int f = 0; f < kFrames; ++f) {
      const auto rgba = surface.to_rgba8();
      bytes += rgba.size();
    }
    const double copy_ms = ms_since(t0) / kFrames;
    std::printf("[GPU 画布]\n  绘制  %.2f ms/帧（含 wait）\n"
                "  取像素 %.2f ms/帧  （%.1f MB/帧）\n  合计  %.2f ms/帧\n",
                draw_ms, copy_ms, static_cast<double>(bytes) / kFrames / 1048576.0,
                draw_ms + copy_ms);
  }
  // ── 确定性：同一份图元画两次，GPU 画布内部是否逐像素一致 ──
  //
  // 裁云的硬前提（motion.hpp 里写着）：同一时刻渲染两次必须**逐像素相同**，
  // 这是 e2e 判据与逐帧可复现的基础。GPU 有浮点与线程调度差异，
  // 所以"能不能用于导出"必须先回答这一条。
  {
    auto a = st::raster::gpu::create_canvas(W, H, 1.0f, {});
    auto b = st::raster::gpu::create_canvas(W, H, 1.0f, {});
    if (a.has_value() && b.has_value()) {
      for (auto* surf : {a->get(), b->get()}) {
        surf->clear(st::math::Color{10, 20, 40, 255});
        draw_all(*surf, shapes);
      }
      const auto pa = (*a)->to_rgba8();
      const auto pb = (*b)->to_rgba8();
      std::size_t diff = 0;
      std::size_t maxdiff = 0;
      if (pa.size() == pb.size()) {
        for (std::size_t i = 0; i < pa.size(); ++i) {
          const auto d = static_cast<std::size_t>(
              std::abs(static_cast<int>(pa[i]) - static_cast<int>(pb[i])));
          if (d > 0) ++diff;
          if (d > maxdiff) maxdiff = d;
        }
      }
      std::printf("\n[确定性] 两次独立 GPU 画布：不同字节 %zu / %zu（最大差 %zu）\n",
                  diff, pa.size(), maxdiff);
    }
  }

  // ── 与软件画布的像素差异（GPU 是否"画得一样"）──
  {
    st::raster::Canvas cpu{W, H, 1.0f};
    cpu.clear(st::math::Color{10, 20, 40, 255});
    draw_all(cpu, shapes);
    const auto pc = cpu.to_rgba8();
    auto g = st::raster::gpu::create_canvas(W, H, 1.0f, {});
    if (g.has_value()) {
      (*g)->clear(st::math::Color{10, 20, 40, 255});
      draw_all(**g, shapes);
      const auto pg = (*g)->to_rgba8();
      std::size_t diff = 0, maxdiff = 0;
      long long sum = 0;
      if (pc.size() == pg.size()) {
        for (std::size_t i = 0; i < pc.size(); ++i) {
          const auto d = static_cast<std::size_t>(
              std::abs(static_cast<int>(pc[i]) - static_cast<int>(pg[i])));
          if (d > 2) ++diff;
          if (d > maxdiff) maxdiff = d;
          sum += static_cast<long long>(d);
        }
      }
      std::printf("[对齐] 软件 vs GPU：差>2 的字节 %zu / %zu（最大差 %zu，平均差 %.3f）\n",
                  diff, pc.size(), maxdiff,
                  pc.empty() ? 0.0 : static_cast<double>(sum) / static_cast<double>(pc.size()));
    }
  }

  return 0;
}
