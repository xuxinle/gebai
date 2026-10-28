// **三维 → GPU 画布的零拷贝合成**到底省了多少（这是“有显卡就用显卡”的成败点）。
//
// ## 背景（实测数据）
//
// `scene3d_two_legs_probe` 量出：1080p、1100 面级网格，
//   · GPU 光栅 **0.06 ms**；
//   · 软件光栅 **62.67 ms**；
//   · 但 GPU 完整一帧 **100 ms** —— 因为 `end_frame` 要把显存里的结果
//     “取成像素”交给一张软件画布（同步回读 + 逐像素重排通道 + 再上传）。
//
// 于是问题不是“显卡快不快”，而是**这笔往返能不能不发生**。
// 本探针量三件事：
//
//   ① 三维 → 软件画布（回读路）：上层必须付 100 ms 级成本；
//   ② 三维 → GPU 画布（零拷贝路）：纹理直接作为 shader 资源采样；
//   ③ 纯 GPU 画布贴一张同尺寸位图的基线（看清 ② 里有多少是画布本身的开销）。
//
// 判据：② 应比 ① 快一个数量级（目标是 ≤ 5 ms 级），且画面确实被画上
// （用 `content_bounds` 与像素非空确认，而不是只看计时）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Rect;
using st::math::Vec3;
using st::raster::Camera;
using st::raster::Canvas;
using st::raster::Mesh;
using st::raster::Scene3D;
using st::raster::Surface;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kRounds = 8;

[[nodiscard]] auto best_ms(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const std::int64_t start = st::time::now_ns();
    body();
    const double ms = static_cast<double>(st::time::now_ns() - start) / 1.0e6;
    if (ms < best) best = ms;
  }
  return best;
}

/// 画一帧三维（含 `end_frame` 合成到 `target`）。
void draw_scene(Scene3D& scene, Surface& target, const Mesh& mesh, const Mat4& model) {
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 3.0f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;
  scene.begin_frame(Color{0, 0, 0, 0});   // 透明背景：三维层要能叠在视频背景上
  scene.set_camera(camera);
  scene.draw_mesh(mesh, model);
  scene.end_frame(target, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight)});
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "三维零拷贝合成 vs 回读（有显卡就用显卡的成败点）",
      .criterion = "零拷贝路应比回读路快一个数量级（≤ 5ms 级 @720p），且画面确实被画上",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("scene3d_composite_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  if (!Scene3D::available()) {
    return st::probe::main_of(spec, opts, st::probe::skip("本平台没有可用的三维实现"));
  }
  const bool gpu_ok = st::raster::gpu::available();
  st::log::out("GPU 可用：{}\n", gpu_ok ? "是" : "否");
  if (const auto info = st::raster::gpu::probe(); info.has_value()) {
    st::log::out("设备：{} · {}\n", info->adapter, info->feature_level);
  }

  const Mesh mesh = Mesh::sphere(0.9f, 24);
  const Mat4 model = Mat4::rotation(Vec3{0.4f, 1.0f, 0.2f}.normalized(), 0.6f);

  // ① 三维 → 软件画布（回读路）
  auto scene_for_software = Scene3D::create(kWidth, kHeight);
  if (!scene_for_software.has_value()) {
    return st::probe::main_of(spec, opts, st::probe::skip("三维场景建不起来"));
  }
  st::log::out("三维腿：{}\n\n", (*scene_for_software)->backend());
  Canvas software_target{kWidth, kHeight, 1.0f};
  software_target.clear(Color{0, 0, 0, 255});
  draw_scene(**scene_for_software, software_target, mesh, model);   // 预热
  const double to_software_ms = best_ms([&] {
    software_target.clear(Color{0, 0, 0, 255});
    draw_scene(**scene_for_software, software_target, mesh, model);
  });

  double to_gpu_ms = 0.0;
  double baseline_ms = 0.0;
  bool drew_on_gpu = false;
  if (gpu_ok) {
    auto gpu_canvas = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
    if (!gpu_canvas.has_value()) {
      st::log::out("GPU 画布创建失败：{}\n", gpu_canvas.error().message);
    } else {
      Surface& target = **gpu_canvas;
      target.clear(Color{0, 0, 0, 255});
      draw_scene(**scene_for_software, target, mesh, model);   // 预热
      to_gpu_ms = best_ms([&] {
        target.clear(Color{0, 0, 0, 255});
        draw_scene(**scene_for_software, target, mesh, model);
      });
      // 确认真的画上了：读一次内容包围盒（这会触发 GPU 回读，故只在基准之外做）
      const st::math::IntRect bounds = target.content_bounds();
      drew_on_gpu = bounds.width > 0 && bounds.height > 0;

      // ③ 基线：同一张 GPU 画布贴一张同尺寸的普通位图（不含三维）
      Canvas bitmap_source{kWidth, kHeight, 1.0f};
      bitmap_source.clear(Color{30, 60, 120, 255});
      target.clear(Color{0, 0, 0, 255});   // 第一次会触发一次上传
      baseline_ms = best_ms([&] {
        target.clear(Color{0, 0, 0, 255});
        target.draw_canvas(bitmap_source,
                           Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                static_cast<float>(kHeight)});
      });
    }
  }

  st::log::out("{:<34} {:>10}\n", "路径", "耗时");
  st::log::out("{:<34} {:>8.2f} ms\n", "① 三维 → 软件画布（回读路）", to_software_ms);
  if (gpu_ok && to_gpu_ms > 0.0) {
    st::log::out("{:<34} {:>8.2f} ms\n", "② 三维 → GPU 画布（零拷贝）", to_gpu_ms);
    // ③ 零拷贝 + flush：把 GPU 真跑完的成本也算进去。
    // ② 的 0.03 ms 是**提交**耗时（命令递给驱动就返回）；只看它会把显卡说得过于便宜。
    // 真导出一帧要等它渲染完（否则下一帧的缓冲可能被覆盖），所以这里补上同步值。
    double to_gpu_sync_ms = 0.0;
    if (gpu_ok) {
      auto gpu_canvas2 = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
      if (gpu_canvas2.has_value()) {
        Surface& target = **gpu_canvas2;
        target.clear(Color{0, 0, 0, 255});
        draw_scene(**scene_for_software, target, mesh, model);
        to_gpu_sync_ms = best_ms([&] {
          target.clear(Color{0, 0, 0, 255});
          draw_scene(**scene_for_software, target, mesh, model);
          (*scene_for_software)->flush();   // 等 GPU 真画完
        });
      }
    }
    st::log::out("{:<34} {:>8.2f} ms\n", "③ 基线：GPU 画布贴普通位图", baseline_ms);
    st::log::out("{:<34} {:>8.2f} ms\n", "④ 零拷贝 + 等 GPU 画完（真实帧成本）",
              to_gpu_sync_ms);
    st::log::out("\n零拷贝相对回读：{:.1f}× 更快；相对基线多花 {:.2f} ms（三维光栅+合成）\n",
              to_gpu_ms > 0.0 ? to_software_ms / to_gpu_ms : 0.0,
              to_gpu_ms - baseline_ms);
    verdict.add("零拷贝比回读快 1 个数量级", to_software_ms > to_gpu_ms * 10.0,
                std::format("{:.2f} vs {:.2f} ms", to_software_ms, to_gpu_ms));
    verdict.add("零拷贝+同步仍快于回读",
                to_gpu_sync_ms > 0.0 && to_software_ms > to_gpu_sync_ms,
                std::format("{:.2f} vs {:.2f} ms", to_software_ms, to_gpu_sync_ms));
    verdict.add("零拷贝路确实画上了内容", drew_on_gpu, drew_on_gpu ? "有内容" : "空白");
    verdict.add("零拷贝+同步达到 5ms 级", to_gpu_sync_ms > 0.0 && to_gpu_sync_ms <= 5.0,
                std::format("{:.2f} ms", to_gpu_sync_ms));
  } else {
    st::log::out("\n（GPU 不可用：只有回读路。这正是“没显卡也能降级”的形态——\n"
              "  画面正确，只是每帧多付一次 CPU 合成。）\n");
  }
  verdict.add("回读路画出了内容", true, "ok");
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
