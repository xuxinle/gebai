// window_chrome_cost_probe —— 窗框几何与位图生成的**可复算的尺子**。
//
// 两个成本点必须分开量（用户的验收要求里专门有"注意性能"这一条）：
//   · **重建**：`build_ring_pixels`（生成本地位图）+ 平台层的 `memcpy` 与 `UpdateLayeredWindow`；
//     只在几何变化（尺寸/DPI/最大化）时发生；
//   · **逐帧**：几何不变时只走 `ring_layout` + 一次整数签名比较，实测为 0。
//
// 本程序只量纯函数部分（平台层的 GDI 调用由 `window_chrome_measure` 的外部耗时覆盖）。
// 用法：g++ -std=c++23 -O2 -Iinclude tools/window_chrome_cost_probe.cpp src/shell/window_chrome.cpp -o bench

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "st/shell/window_chrome.hpp"

namespace {

[[nodiscard]] auto now_ms() -> double {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count()) /
         1.0e6;
}

[[nodiscard]] auto median(std::vector<double> values) -> double {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

}  // namespace

int main() {
  using st::shell::WindowChromeGeometry;
  using st::shell::WindowChromeStyle;
  WindowChromeStyle style;
  const struct {
    const char* label;
    int width;
    int height;
    float scale;
  } cases[] = {
      {"1280x800 @1.0x（小窗）", 1280, 800, 1.0f},
      {"1600x1000 @1.0x（1080p 常见）", 1600, 1000, 1.0f},
      {"2560x1400 @1.5x（2.5K 全屏）", 2560, 1400, 1.5f},
      {"3533x1921 @1.5x（4K 全屏）", 3533, 1921, 1.5f},
  };
  std::printf("==== 窗框重建开销（7 次取中位）====\n");
  std::printf("判据：4K 下重建 < 3 ms（拖动时每帧一次；带位图不再重新分配/清零）\n\n");
  for (const auto& item : cases) {
    WindowChromeGeometry geometry;
    geometry.client_width = item.width;
    geometry.client_height = item.height;
    geometry.scale = item.scale;
    const auto layout = st::shell::ring_layout(geometry, style);
    // 缓冲只分配一次（真实接入方也是这个形态：DIB 像素地址复用），重建时只写环。
    std::vector<std::uint32_t> buffer(static_cast<std::size_t>(layout.width) * layout.height, 0U);
    std::vector<double> build_ms;
    for (int round = 0; round < 7; ++round) {
      const double t0 = now_ms();
      const bool ok = st::shell::build_ring_pixels(layout, style, 0x0A0A12U, buffer);
      const double t1 = now_ms();
      build_ms.push_back(t1 - t0);
      if (!ok) std::printf("  !! build_ring_pixels 失败\n");
    }
    std::size_t painted = 0;
    for (std::uint32_t pixel : buffer) {
      if ((pixel >> 24) != 0) ++painted;
    }
    const double bytes = static_cast<double>(buffer.size() * sizeof(std::uint32_t));
    std::printf("%s\n  位图 %dx%d（%.1f MiB）  不透明 %.2f%%\n", item.label, layout.width,
                layout.height, bytes / (1024.0 * 1024.0),
                buffer.empty() ? 0.0
                               : 100.0 * static_cast<double>(painted) /
                                     static_cast<double>(buffer.size()));
    std::printf("  重建（只写环）: %.3f ms\n\n", median(build_ms));
  }

  // 逐帧路径：几何不变，只算布局 + 签名。
  WindowChromeGeometry geometry;
  geometry.client_width = 3533;
  geometry.client_height = 1921;
  geometry.scale = 1.5f;
  std::vector<double> idle_ms;
  for (int i = 0; i < 500; ++i) {
    const double t0 = now_ms();
    const auto layout = st::shell::ring_layout(geometry, style);
    const std::uint64_t signature = (static_cast<std::uint64_t>(layout.width) << 32) |
                                    (static_cast<std::uint64_t>(layout.height) << 8) |
                                    static_cast<std::uint64_t>(layout.radius & 0xFF) |
                                    (static_cast<std::uint64_t>(layout.margin & 0x0F) << 4);
    (void)signature;
    idle_ms.push_back(now_ms() - t0);
  }
  std::printf("逐帧开销（几何不变，500 次中位）：%.5f ms\n", median(idle_ms));
  return 0;
}
