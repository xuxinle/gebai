/// GPU 渲染目标的**设备层契约**测试（D3D11）。
///
/// 这一层要钉住的是"GPU 路径真的能起来、并且忠实于软件语义"：
/// 设备探测说实话、离屏渲染不依赖窗口、清屏与像素回读的**色彩约定**与软件画布一致。
///
/// 为什么把色彩约定写成测试：画布内部是**预乘** `0xRRGGBBAA`，而 D3D11 的纹理内存序是
/// R,G,B,A。两边一次搞错就是"红蓝互换"或"半透明看起来偏深"——而这类错误在截图上
/// 只是"颜色不对"，很难反推到哪一行。逐字节断言把它钉死。
///
/// 无 GPU 的机器上：`probe()` 应如实报不可用，其余用例自动跳过（不算失败）。

#include "st/test/test.hpp"

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"

namespace {

using st::math::Color;
using st::raster::Paint;
using st::raster::Surface;
using st::raster::gpu::DeviceInfo;
using st::raster::gpu::Options;

/// 取一次探测结果（不可用时给出原因，供跳过说明）。
[[nodiscard]] auto device_info() -> const st::Result<DeviceInfo>& {
  static const st::Result<DeviceInfo> info = st::raster::gpu::probe();
  return info;
}

[[nodiscard]] auto make_surface(int width, int height) -> std::unique_ptr<Surface> {
  auto surface = st::raster::gpu::create_canvas(width, height, 1.0f, Options{});
  return surface.has_value() ? std::move(*surface) : nullptr;
}

}  // namespace

ST_TEST(gpu_probe_reports_availability_honestly) {
  const auto& info = device_info();
  if (!info.has_value()) {
    // 没有 GPU 的机器：必须给出**具体原因**，而不是一个空错误
    ST_CHECK(!st::raster::gpu::available());
    ST_CHECK(!info.error().message.empty());
    return;
  }
  ST_CHECK(st::raster::gpu::available());
  ST_CHECK(!info->backend.empty());
  // 适配器名必须非空：报"用了 GPU"却不知道是哪一块卡，诊断时等于没说
  ST_CHECK(!info->adapter.empty());
  ST_CHECK(!info->feature_level.empty());
}

ST_TEST(gpu_canvas_clear_and_readback_round_trip) {
  if (!device_info().has_value()) return;
  auto surface = make_surface(64, 48);
  ST_CHECK(surface != nullptr);
  if (surface == nullptr) return;

  // 不透明色：回读应逐通道一致
  surface->clear(Color{0x12, 0x34, 0x56, 0xFF});
  const Color sampled = surface->pixel_at(10, 10);
  ST_CHECK_EQ(static_cast<int>(sampled.r), 0x12);
  ST_CHECK_EQ(static_cast<int>(sampled.g), 0x34);
  ST_CHECK_EQ(static_cast<int>(sampled.b), 0x56);
  ST_CHECK_EQ(static_cast<int>(sampled.a), 0xFF);
  // 四角与中心一致（清屏是全覆盖的）
  ST_CHECK_EQ(static_cast<int>(surface->pixel_at(0, 0).r), 0x12);
  ST_CHECK_EQ(static_cast<int>(surface->pixel_at(63, 47).r), 0x12);
}

ST_TEST(gpu_clear_matches_software_canvas_exactly) {
  // 最关键的一条：GPU 清屏结果与软件画布**逐字节相同**。
  // 它同时验证了：预乘约定、通道顺序、回读路径三件事。
  if (!device_info().has_value()) return;
  auto gpu = make_surface(32, 24);
  ST_CHECK(gpu != nullptr);
  if (gpu == nullptr) return;

  st::raster::Canvas software(32, 24);
  for (const Color color :
       {Color{0xFF, 0xFF, 0xFF, 0xFF}, Color{0x0A, 0x0F, 0x1A, 0xFF}, Color{0x12, 0x1A, 0x2B, 0xFF},
        Color{0x80, 0x40, 0x20, 0xFF}}) {
    gpu->clear(color);
    software.clear(color);
    const auto a = gpu->pixels();
    const auto b = software.pixels();
    ST_CHECK_EQ(a.size(), b.size());
    bool identical = a.size() == b.size();
    for (std::size_t index = 0; index < std::min(a.size(), b.size()); ++index) {
      if (a[index] != b[index]) {
        identical = false;
        break;
      }
    }
    ST_CHECK(identical);
  }
}

ST_TEST(gpu_canvas_reports_device_scale_and_geometry) {
  if (!device_info().has_value()) return;
  auto surface = st::raster::gpu::create_canvas(200, 100, 2.0f, Options{});
  ST_CHECK(surface.has_value());
  if (!surface.has_value()) return;
  Surface& target = **surface;
  ST_CHECK_EQ(target.physical_width(), 200);
  ST_CHECK_EQ(target.physical_height(), 100);
  // 逻辑尺寸 = 物理 / scale
  ST_CHECK_EQ(target.width(), 100);
  ST_CHECK_EQ(target.height(), 50);
  ST_CHECK_NEAR(target.device_scale(), 2.0f, 0.001f);
  // 逻辑矩形 → 物理矩形（向外取整）
  const auto physical = target.to_physical(st::math::Rect{1.0f, 2.0f, 3.0f, 4.0f});
  ST_CHECK_EQ(physical.x, 2);
  ST_CHECK_EQ(physical.y, 4);
  ST_CHECK_EQ(physical.width, 6);
  ST_CHECK_EQ(physical.height, 8);
}

ST_TEST(gpu_canvas_resize_in_place) {
  // 拖动缩放靠的就是这条：每个中间尺寸都要改一次缓冲。
  // 旧路径是"丢掉整块 GpuCanvas、`create_canvas` 一个新的"——那会连带丢掉字形遮罩/
  // 渐变/路径遮罩缓存（每帧重传整个字形集）并重建设备层资源。
  if (!device_info().has_value()) return;
  auto surface = st::raster::gpu::create_canvas(200, 100, 2.0f, Options{});
  ST_CHECK(surface.has_value());
  if (!surface.has_value()) return;
  Surface& target = **surface;

  if (auto status = target.resize(320, 160); !status) {
    ST_CHECK(false);   // GPU 画布必须支持原地改尺寸
    return;
  }
  ST_CHECK_EQ(target.physical_width(), 320);
  ST_CHECK_EQ(target.physical_height(), 160);
  ST_CHECK_NEAR(target.device_scale(), 2.0f, 0.001f);   // DPI 不变
  // 新缓冲可用且能画满整块：裁剪栈重置到新尺寸（不重置的话右下角会被裁掉）
  target.clear(Color{0, 0, 0, 0});
  target.fill_rect(st::math::Rect{0.0f, 0.0f, static_cast<float>(target.width()),
                                  static_cast<float>(target.height())},
                   Paint::solid(Color{0x11, 0x22, 0x33, 0xFF}));
  const auto pixels = target.pixels();
  const auto at = [&pixels, &target](int x, int y) {
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(target.physical_width()) +
                  static_cast<std::size_t>(x)];
  };
  // 右下角必须也被填上（裁剪栈没重置的话这里会是透明）
  // 画布像素布局是 `0xRRGGBBAA`（与软件光栅器同一约定，见 `Surface::pixels`）
  constexpr std::uint32_t kExpected = 0x112233FFU;
  ST_CHECK_EQ(at(319, 159), kExpected);
  ST_CHECK_EQ(at(0, 0), kExpected);
  // 幂等 + 非法尺寸
  ST_CHECK(target.resize(320, 160).has_value());
  ST_CHECK(!target.resize(-1, 160).has_value());
}

ST_TEST(gpu_canvas_image_export_is_rgba8) {
  if (!device_info().has_value()) return;
  auto surface = make_surface(8, 4);
  ST_CHECK(surface != nullptr);
  if (surface == nullptr) return;
  surface->clear(Color{0x10, 0x20, 0x30, 0xFF});
  const std::vector<std::uint8_t> rgba = surface->to_rgba8();
  ST_CHECK_EQ(rgba.size(), static_cast<std::size_t>(8 * 4 * 4));
  // 直通 RGBA8：字节序 R,G,B,A（截图/PNG 编码依赖它）
  ST_CHECK_EQ(static_cast<int>(rgba[0]), 0x10);
  ST_CHECK_EQ(static_cast<int>(rgba[1]), 0x20);
  ST_CHECK_EQ(static_cast<int>(rgba[2]), 0x30);
  ST_CHECK_EQ(static_cast<int>(rgba[3]), 0xFF);
}

ST_TEST(gpu_canvas_content_bounds_tracks_alpha) {
  if (!device_info().has_value()) return;
  auto surface = make_surface(20, 20);
  ST_CHECK(surface != nullptr);
  if (surface == nullptr) return;
  // 全透明底 → 内容包围盒为空
  surface->clear(Color{0, 0, 0, 0});
  ST_CHECK(surface->content_bounds().is_empty());
  // 不透明底 → 占满整块
  surface->clear(Color{0x20, 0x20, 0x20, 0xFF});
  const auto bounds = surface->content_bounds();
  ST_CHECK_EQ(bounds.x, 0);
  ST_CHECK_EQ(bounds.y, 0);
  ST_CHECK_EQ(bounds.width, 20);
  ST_CHECK_EQ(bounds.height, 20);
}

ST_TEST(gpu_canvas_lifetime_is_balanced) {
  if (!device_info().has_value()) return;
  const std::uint32_t before = st::raster::gpu::live_canvas_count();
  {
    auto first = make_surface(16, 16);
    auto second = make_surface(16, 16);
    ST_CHECK(first != nullptr);
    ST_CHECK(second != nullptr);
    ST_CHECK_EQ(st::raster::gpu::live_canvas_count(), before + 2);
  }
  // 设备与上下文按进程共享，画布销毁后计数必须回零（否则是资源泄漏）
  ST_CHECK_EQ(st::raster::gpu::live_canvas_count(), before);
}

ST_TEST(gpu_surface_is_a_surface) {
  // 接口契约：GPU 画布必须能当 `Surface` 用（UI 层只认接口）
  if (!device_info().has_value()) return;
  auto surface = make_surface(10, 10);
  ST_CHECK(surface != nullptr);
  if (surface == nullptr) return;
  Surface& target = *surface;
  target.clear(Color{0xFF, 0x00, 0x00, 0xFF});
  // 通过接口访问像素（而不是靠具体类型）
  const Color sampled = target.pixel_at_point(st::math::Point{5.0f, 5.0f});
  ST_CHECK_EQ(static_cast<int>(sampled.r), 0xFF);
  ST_CHECK_EQ(static_cast<int>(sampled.a), 0xFF);
}

ST_TEST(gpu_rejects_invalid_size) {
  if (!device_info().has_value()) return;
  auto bad = st::raster::gpu::create_canvas(0, 10, 1.0f, Options{});
  ST_CHECK(!bad.has_value());
  ST_CHECK(!bad.error().message.empty());
}

/// 诊断输出（**故意保留**）：GPU 报告是"这一帧到底谁画的"的唯一凭据，
/// 
/// 它的价值在于**可被人工核对**：适配器名（真卡还是 WARP）、特性级别、显存，
/// 配合 `ST_GPU_FORCE_WARP=1` 就能在同一台机器上把"有显卡"与"无显卡"两条路径都跑一遍。
/// 这是"无 GPU 机器"唯一的可验证方式——本机有显卡，不设这个开关就永远测不到回退路径。
ST_TEST(gpu_device_diagnostics) {
  const auto& info = device_info();
  if (!info.has_value()) {
    st::print("[gpu] 不可用: {}\n", info.error().message);
    return;
  }
  st::print("[gpu] backend={} adapter={} level={} warp={} vram={} MiB\n", info->backend,
            info->adapter, info->feature_level, info->warp,
            info->vram_bytes / (1024ULL * 1024ULL));
  st::print("[gpu] live_canvases={}\n", st::raster::gpu::live_canvas_count());
}
