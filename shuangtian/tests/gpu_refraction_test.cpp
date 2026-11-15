/// `Surface::refract_region`（液态玻璃：折射 + 色散）的 GPU 契约测试。
///
/// 折射是 GPU 专属原语（软件腿默认实现返回 false）——本文件只在 GPU 可用时
/// 跑真断言，无 GPU 环境退为「软件腿确认降级」的负例。
///
/// 三条契约：
/// 1. **降级**：软件 `Canvas` 的 `refract_region` 返回 false 且
///    `supports_refraction()` 为假（调用方据此退 `blur_region`）。
/// 2. **强度=0 是恒等**：strength 传 0 → 返回 false（无折射可做）。
/// 3. **折射真的发生**：竖条纹背景 + 高强度折射后，玻璃边缘带内的像素
///    **与中心不同**（背景被吸向边缘/色散混色），且区域外逐位不变。
///    若着色器参数接线错了（如 axis_extra 槽位调换），边缘带与中心
///    会拿到同一采样，这条当场红。
#include <cmath>
#include <memory>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

[[nodiscard]] auto gpu_ready() -> bool { return st::raster::gpu::available(); }

/// 竖条纹（6px 周期，纯黑白——色散在极端对比下最可见）。
void paint_stripes(st::raster::Surface& canvas, int width, int height) {
  canvas.clear(Color{0x20, 0x20, 0x28, 0xFF});
  for (int i = 0; i < width; i += 6) {
    canvas.fill_rect(Rect{static_cast<float>(i), 0.0f, 3.0f, static_cast<float>(height)},
                     st::raster::Paint::solid(Color{0xF0, 0xF0, 0xF0, 0xFF}));
  }
}

/// 区域内红通道极差。
[[nodiscard]] auto red_span(const st::raster::Surface& canvas, Rect area) -> int {
  int lo = 255;
  int hi = 0;
  for (int y = static_cast<int>(area.y); y < static_cast<int>(area.y + area.height); ++y) {
    for (int x = static_cast<int>(area.x); x < static_cast<int>(area.x + area.width); ++x) {
      const int r = canvas.pixel_at(x, y).r;
      lo = std::min(lo, r);
      hi = std::max(hi, r);
    }
  }
  return hi - lo;
}

}  // namespace

ST_TEST(refract_region_degrades_on_software_canvas) {
  // 软件 Canvas 不支持折射：返回 false、能力探测为假——调用方退 blur_region。
  Canvas canvas = Canvas::for_logical_size(120, 80, 1.0f);
  canvas.clear(Color{0x20, 0x20, 0x28, 0xFF});
  ST_CHECK(!canvas.supports_refraction());
  ST_CHECK(!canvas.refract_region(Rect{10, 10, 60.0f, 40.0f}, 8.0f, 10.0f, 0.3f, 10.0f));
}

ST_TEST(refract_region_rejects_zero_strength) {
  if (!gpu_ready()) return;
  auto gpu = st::raster::gpu::create_canvas(200, 150, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  st::raster::Surface& target = **gpu;
  paint_stripes(target, 200, 150);
  // 强度 0 = 无折射可做：明确拒绝（而不是画一个与 blur 无异的假折射）。
  ST_CHECK(!target.refract_region(Rect{30, 30, 120.0f, 80.0f}, 8.0f, 0.0f, 0.3f, 12.0f));
}

ST_TEST(refract_region_bends_edge_band_and_leaves_outside_intact) {
  if (!gpu_ready()) return;
  auto gpu = st::raster::gpu::create_canvas(200, 150, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  st::raster::Surface& target = **gpu;
  paint_stripes(target, 200, 150);

  // 基准：同条纹、无玻璃。
  Canvas reference = Canvas::for_logical_size(200, 150, 1.0f);
  paint_stripes(reference, 200, 150);

  const Rect glass{30.0f, 30.0f, 120.0f, 80.0f};
  const bool ok = target.refract_region(glass, 8.0f, 24.0f, 0.5f, 16.0f);
  ST_CHECK(ok);
  if (!ok) return;

  // ① 区域外逐位不变（折射不越界）。
  bool outside_same = true;
  for (int y = 0; y < 150 && outside_same; ++y) {
    for (int x = 0; x < 200; ++x) {
      if (x >= 30 && x < 150 && y >= 30 && y < 110) continue;
      if (target.pixel_at(x, y).r != reference.pixel_at(x, y).r) {
        outside_same = false;
        break;
      }
    }
  }
  ST_CHECK(outside_same);

  // ② 折射真的发生：中心与边缘带必须不同。
  // 中心（深至 SDF 负区，折射≈0）与边缘带（法线位移最强）的采样不同——
  // 极差口径：两者跨像素颜色构成存在可测差异。
  const Color center = target.pixel_at(90, 70);
  const Color edge = target.pixel_at(34, 70);   // 紧贴左缘（边缘带内）
  const int center_edge_delta = std::abs(static_cast<int>(center.r) - static_cast<int>(edge.r));
  // 中心在条纹区（无玻璃时 red_span>150）；玻璃后中心仍是模糊后的条纹色、
  // 边缘被折射+色散改写。两者若完全相同 → 着色器没接到折射参数。
  ST_CHECK(center_edge_delta > 0 || std::abs(static_cast<int>(center.g) - static_cast<int>(edge.g)) > 0 ||
           std::abs(static_cast<int>(center.b) - static_cast<int>(edge.b)) > 0);

  // ③ 玻璃**中心**（SDF 深度 > 边缘带宽，折射≈0）高频被抹平。
  // 窗口必须避开边缘带（edge_band = max(corner,8) = 16px）：边缘带的像素被折射
  // 拉进了远处条纹的原色（高对比），把边缘带量进去测的是折射而不是模糊。
  ST_CHECK(red_span(target, Rect{48.0f, 48.0f, 84.0f, 44.0f}) < 150);
}
