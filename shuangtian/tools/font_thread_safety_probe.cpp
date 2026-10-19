/// 字体栈并发安全压测：多线程并发 measure_width（模拟 SlowContent 后台排版）
/// 与主线程 measure/draw 交替，检查无崩溃、无数据竞争导致的宽度畸变。
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "st/app/text_port.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

int main() {
  auto fonts = st::text::FontStack::system_default();
  if (!fonts) { std::printf("SKIP: 无字体\n"); return 0; }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);
  const std::string sample = "霜天 Markdown 异步排版基准 abcdefg 0123456789 【】（）";
  const float w_ref = port->measure_width(sample, 14.0f);
  std::atomic<bool> stop{false};
  std::atomic<int> mismatch{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < 3; ++t) {
    workers.emplace_back([&] {
      std::string local;
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 2000; ++i) {
          const float w = port->measure_width(sample, 14.0f);
          if (w != w_ref) mismatch.fetch_add(1);
          local += "x";
          if (local.size() > 64) local.clear();
        }
      }
    });
  }
  // 主线程同时做绘制（字形缓存写路径）
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(200, 100, 1.0f);
  for (int frame = 0; frame < 200; ++frame) {
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    port->draw(canvas, sample, st::math::Point{10, 50}, 14.0f, st::math::Color{0, 0, 0, 0xFF});
  }
  stop.store(true);
  for (auto& worker : workers) worker.join();
  const int bad = mismatch.load();
  std::printf("%s: 并发 measure ×3 线程 + 主线程 draw，宽度畸变 %d 次\n",
              bad == 0 ? "PASS" : "FAIL", bad);
  return bad == 0 ? 0 : 1;
}
