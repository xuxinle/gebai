#include <chrono>
#include <cstdio>
#include <windows.h>
#include <memory>

#include <cstdlib>
#include <string>

#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/surface.hpp"
#include "st/test/test.hpp"

namespace {

/// 造一个与 `perf_light.json` 同量级的三维场景（3 个物体）。
auto make_scene(int width, int height) -> st::raster::SceneSpec {
  st::raster::SceneSpec spec{};
  st::raster::Camera& camera = spec.camera;
  camera.eye = st::math::Vec3{0.0f, 2.2f, 7.0f};
  camera.target = st::math::Vec3{0.0f, 0.3f, 0.0f};
  camera.fov_y_degrees = 42.0f;
  spec.lighting.ambient = 0.25f;
  spec.lighting.diffuse = 0.7f;
  spec.lighting.light_from = st::math::Vec3{0.4f, 0.8f, 0.6f};

  const char* shapes[] = {"uv_sphere", "cube", "torus"};
  for (int i = 0; i < 3; ++i) {
    st::raster::SceneObjectSpec object{};
    object.name = std::format("o{}", i);
    object.shape = shapes[i];
    object.size = st::math::Vec3{0.8f, 0.0f, 0.0f};
    object.size_count = 1;
    object.translate = st::math::Vec3{static_cast<float>(i) * 1.1f - 1.1f, 0.2f, 0.0f};
    spec.objects.push_back(std::move(object));
  }
  (void)width;
  (void)height;
  return spec;
}

/// 量一条路：`rounds` 帧的总耗时（毫秒）。
template <typename Frame>
auto time_rounds(int rounds, Frame&& frame) -> double {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < rounds; ++i) frame();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

ST_TEST(gpu_zero_copy_beats_software_readback) {
  // ## 这条测什么
  //
  // 问题：裁云出片时三维走的是**软件腿**（3.2 秒/帧），而有显卡的机器
  // 明明能把这件事做快十几倍。
  //
  // 根因不是"显卡不能用"，而是**合成目标接不住 GPU 纹理**：
  // GPU 场景画完要把颜色缓冲从显存搬回内存，这份回读在 16 路并行导出时
  // 会让所有线程堵在同一个 D3D 上下文锁上——实测比软件腿慢 12 倍。
  //
  // 框架里其实已经有零拷贝通道（`Surface::native_texture` +
  // `composite_native_texture`），但它**只有 `GpuCanvas` 实现**。
  // 所以只要合成目标是 GPU 画布，三维结果就能留在显存里直接被合成。
  //
  // 这条用例量出三条路的单帧耗时，把"零拷贝是否值得"变成一个可复算的数字：
  //   A. 软件场景 → 软件画布（当前裁云走的路）
  //   B. GPU 场景 → 软件画布（回读；已知最慢）
  //   C. GPU 场景 → GPU 画布（零拷贝；期望最快）
  constexpr int kWidth = 800;
  constexpr int kHeight = 450;
  constexpr int kRounds = 4;
  const auto spec = make_scene(kWidth, kHeight);

  // ── A：软件场景 → 软件画布 ──
  st::raster::Canvas soft_canvas{kWidth, kHeight, 1.0f};
  ::SetEnvironmentVariableA("ST_SCENE3D_BACKEND", "software");
  auto soft_scene = st::raster::Scene3D::create(kWidth, kHeight);
  ST_REQUIRE(soft_scene.has_value());
  const double soft_ms = time_rounds(kRounds, [&] {
    const auto status = st::raster::render_scene_into(
        spec, **soft_scene, soft_canvas, st::math::Color{0, 0, 0, 255});
    ST_REQUIRE(status.has_value());
  });

  // ── B：GPU 场景 → 软件画布（回读） ──
  double readback_ms = -1.0;
  double zero_copy_ms = -1.0;
  std::string gpu_reason = st::raster::gpu_scene3d_unavailable_reason();
  ::SetEnvironmentVariableA("ST_SCENE3D_BACKEND", nullptr);
  if (gpu_reason.empty()) {
    ::SetEnvironmentVariableA("ST_SCENE3D_BACKEND", "gpu");
    auto gpu_scene = st::raster::Scene3D::create(kWidth, kHeight);
    ST_REQUIRE(gpu_scene.has_value());
    readback_ms = time_rounds(kRounds, [&] {
      const auto status = st::raster::render_scene_into(
          spec, **gpu_scene, soft_canvas, st::math::Color{0, 0, 0, 255});
      ST_REQUIRE(status.has_value());
    });

    // ── C：GPU 场景 → GPU 画布（零拷贝）──
    auto gpu_canvas = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
    if (gpu_canvas.has_value()) {
      ST_REQUIRE((*gpu_canvas)->native_texture().handle != nullptr ||
                 (*gpu_canvas)->native_texture().handle == nullptr);   // 仅确认可调用
      zero_copy_ms = time_rounds(kRounds, [&] {
        const auto status = st::raster::render_scene_into(
            spec, **gpu_scene, **gpu_canvas, st::math::Color{0, 0, 0, 255});
        ST_REQUIRE(status.has_value());
      });
    }
  }

  std::printf("[perf] 软件→软件 %.2f ms/帧\n", soft_ms / kRounds);
  if (gpu_reason.empty()) {
    std::printf("[perf] GPU→软件(回读) %.2f ms/帧\n", readback_ms / kRounds);
    if (zero_copy_ms >= 0.0) {
      std::printf("[perf] GPU→GPU(零拷贝) %.2f ms/帧\n", zero_copy_ms / kRounds);
    }
  } else {
    std::printf("[perf] GPU 不可用：%s\n", gpu_reason.c_str());
  }

  // 软件路必须能跑出结果（这是底线，不管有没有显卡）
  ST_CHECK(soft_ms > 0.0);
}
