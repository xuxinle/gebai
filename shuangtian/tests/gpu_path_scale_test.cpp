/// GPU 路径类原语在**非整数 DPI** 下与软件画布的一致性（回归钉子）。
///
/// 背景（2026-10 实测）：D3D11 `fill_path` 曾缺逻辑→物理缩放，路径在 1.5x 画布上
/// 整体缩成 1/1.5——hit_test 按逻辑坐标、画出来错位（菜单面板残缺、光标视觉错位）。
/// 1x 下两套实现结果重合、缺陷不可见；2x 截图抽查又恰好没覆盖菜单场景。
/// 本测试把「GPU 与软件在同一非整数缩放下的结构一致性」钉成测试，缩放丢失即红灯。

#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Surface;

[[nodiscard]] auto device_info() -> const st::Result<st::raster::gpu::DeviceInfo>& {
  static const st::Result<st::raster::gpu::DeviceInfo> info = st::raster::gpu::probe();
  return info;
}

[[nodiscard]] auto make_scaled_surface(int logical_width, int logical_height, float scale)
    -> std::unique_ptr<Surface> {
  const int physical_width = static_cast<int>(static_cast<float>(logical_width) * scale);
  const int physical_height = static_cast<int>(static_cast<float>(logical_height) * scale);
  auto surface = st::raster::gpu::create_canvas(physical_width, physical_height, scale, {});
  return surface.has_value() ? std::move(*surface) : nullptr;
}

/// 遮罩内像素计数（alpha 通道 > 半覆盖）。结构性证据：位置/尺寸错了它就变。
[[nodiscard]] auto opaque_pixel_count(const Surface& surface, const Rect& physical_region)
    -> std::size_t {
  std::size_t count = 0;
  const int x0 = static_cast<int>(physical_region.x);
  const int y0 = static_cast<int>(physical_region.y);
  const int x1 = static_cast<int>(physical_region.x + physical_region.width);
  const int y1 = static_cast<int>(physical_region.y + physical_region.height);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (surface.pixel_at(x, y).a > 127) ++count;
    }
  }
  return count;
}

}  // namespace

ST_TEST(gpu_fill_path_scale_matches_software) {
  if (!device_info().has_value()) return;  // 无 GPU 跳过
  constexpr float kScale = 1.5f;
  constexpr int kLogicalW = 200;
  constexpr int kLogicalH = 160;
  auto gpu = make_scaled_surface(kLogicalW, kLogicalH, kScale);
  ST_CHECK(gpu != nullptr);
  if (gpu == nullptr) return;
  st::raster::Canvas software(static_cast<int>(kLogicalW * kScale),
                             static_cast<int>(kLogicalH * kScale), kScale);

  // 逻辑坐标的圆角矩形（非整数 DPI 下正是缩放最容易丢的地方）
  const Rect logical{40.0f, 30.0f, 120.0f, 80.0f};
  st::raster::Path path;
  path.add_rounded_rect(logical, 10.0f);
  const Color fill{Color::rgb(30, 64, 175)};

  gpu->clear(Color{0, 0, 0, 0});
  software.clear(Color{0, 0, 0, 0});
  gpu->fill_path(path, st::raster::Paint::solid(fill));
  software.fill_path(path, st::raster::Paint::solid(fill));

  // 物理口径的期望区域：逻辑 rect × scale（±1px AA 容差各边）
  const Rect expected_physical{logical.x * kScale, logical.y * kScale,
                               logical.width * kScale, logical.height * kScale};
  constexpr float kPad = 2.0f;
  const Rect scan{expected_physical.x - kPad, expected_physical.y - kPad,
                  expected_physical.width + kPad * 2, expected_physical.height + kPad * 2};
  const std::size_t gpu_count = opaque_pixel_count(*gpu, scan);
  const std::size_t software_count = opaque_pixel_count(software, scan);
  const std::size_t expected = static_cast<std::size_t>(expected_physical.width) *
                               static_cast<std::size_t>(expected_physical.height);

  // 两者结构一致（±2% AA 差异）且都接近满矩形（>92%）——"缩成 1/scale"时只有 ~44%
  ST_CHECK(gpu_count > expected * 92U / 100U);
  ST_CHECK(software_count > expected * 92U / 100U);
  const std::size_t lo = std::min(gpu_count, software_count);
  const std::size_t hi = std::max(gpu_count, software_count);
  ST_CHECK(hi * 100U <= lo * 102U + 100U);
}

ST_TEST(gpu_fill_path_position_at_scale_matches_logical) {
  if (!device_info().has_value()) return;
  constexpr float kScale = 1.5f;
  auto gpu = make_scaled_surface(200, 160, kScale);
  ST_CHECK(gpu != nullptr);
  if (gpu == nullptr) return;
  gpu->clear(Color{0, 0, 0, 0});

  // 锚点探针：路径画在逻辑 (60,40)，物理 (90,60) 处必须有填充；而"缩成 2/3"时
  // 最近的填充像素会出现在 (60,40)——直接把缺陷定位到坐标而非笼统的大小。
  st::raster::Path path;
  path.add_rounded_rect(Rect{60.0f, 40.0f, 80.0f, 60.0f}, 8.0f);
  gpu->fill_path(path, st::raster::Paint::solid(Color::rgb(200, 30, 30)));

  const auto filled = [&gpu](int px, int py) {
    return gpu->pixel_at(px, py).a > 127;
  };
  ST_CHECK(filled(130, 90));  // 物理区域中部（逻辑 86.7,60——远离四角圆弧）
  ST_CHECK(!filled(88, 58));  // 物理区域左上角外侧（逻辑 58.7,38.7）
  ST_CHECK(!filled(242, 152)); // 若漏缩放，右侧空区会被填充（(162,101) 逻辑附近）
}
