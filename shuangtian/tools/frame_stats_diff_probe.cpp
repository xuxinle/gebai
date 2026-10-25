// 验证 GPU 画布的回读三条路径是否一致（`to_rgba8` / `pixels` / `pixel_at`）。
//
// 背景：把 `to_rgba8` 改成直接返回 memcpy 的 RGBA 字节后，
// `gpu_clear_matches_software_canvas_exactly` 报 "0 != 768"（to_rgba8 返回空）。
#include <cstdio>   // lint-allow: L7 探针输出（量测工具，不产出交付物）

#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/surface.hpp"

int main() {
  if (!st::raster::gpu::available()) {
    std::printf("GPU 不可用\n");
    return 0;
  }
  auto created = st::raster::gpu::create_canvas(64, 48, 1.0f, {});
  if (!created.has_value()) {
    std::printf("创建失败\n");
    return 1;
  }
  st::raster::Surface& s = **created;
  s.clear(st::math::Color{0x12, 0x34, 0x56, 0xFF});

  const auto rgba = s.to_rgba8();
  std::printf("to_rgba8 后立即取 pixels: %zu\n", s.pixels().size());
  const std::size_t px_size = s.pixels().size();
  const auto color = s.pixel_at(10, 10);
  const std::size_t px_size_after = s.pixels().size();
  std::printf("to_rgba8 大小 = %zu（期望 %d）\n", rgba.size(), 64 * 48 * 4);
  std::printf("pixels   大小 = %zu（期望 %d）\n", px_size, 64 * 48);
  std::printf("pixels 再取 = %zu\n", px_size_after);
  std::printf("pixel_at(10,10) = (%u,%u,%u,%u)（期望 18,52,86,255）\n", color.r, color.g,
              color.b, color.a);
  if (!rgba.empty()) {
    std::printf("to_rgba8 首 4 字节 = (%u,%u,%u,%u)\n", rgba[0], rgba[1], rgba[2], rgba[3]);
  }
  return 0;
}
