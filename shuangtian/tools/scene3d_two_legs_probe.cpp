// **三维两条腿（GPU / 软件）的对照**——决定“有显卡就用显卡”到底值不值得。
//
// ## 为什么这个探针比“帧耗时”更要紧
//
// 第一版探针只报了总耗时：1080p 下 GPU 腿 77 ms，看着像“显卡白搭”。
// 但拆开看，**三维光栅本身几乎不花时间**（576 面净耗时 ≈ 0），77 ms 全在
// `end_frame` 那次“纹理回读 + 上传成画布”。也就是说：
//
//   · 如果上层需要**像素**（导出 PNG / 逐帧度量 / 软件画布合成），这笔传输
//     就是硬成本，两条腿都得付——GPU 的优势被它吃掉；
//   · 如果上层是**GPU 画布且不需要像素**（窗口上屏），这次回读可以完全省掉。
//
// 所以判据不是“谁的 ms 小”，而是**分开量**：光栅 + 回读各占多少。
//
// ## 量什么
//
// 对每条腿分别报：
//   ① 只光栅（begin/camera/draw，不 end_frame）——三维真正的计算成本；
//   ② 完整帧（含 end_frame 合成回软件画布）——上层必须付的成本；
//   ③ 目标尺寸对 ① 的影响（判断 GPU 腿是否随分辨率变化）。
//
// 强制指定腿用 `ST_SCENE3D_BACKEND=gpu|software`（本探针自己设，跑完还原）。

// 强制指定腿用 `ST_SCENE3D_BACKEND=gpu|software`（本探针自己设，跑完还原）。
//
// ⚠ 环境变量读写必须走 CRT 的 `getenv`/`_putenv_s`，**不能用 Win32 的
// `SetEnvironmentVariableA`**：`st::fs::read_env` 读的是 CRT 自己那份环境块副本，
// 只改进程环境块（Win32 API）改不动它（实测踩过：两段“强制不同腿”量出一模一样
// 的结果，因为开关根本没生效）。这两组函数只需 `<cstdlib>`，**不需要 `<windows.h>`**
// （也因此避开了 PCH 里 NOMINMAX 的重定义告警）。
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
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

constexpr int kRounds = 8;

[[nodiscard]] auto best_ns(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    if (ns < best) best = ns;
  }
  return best;
}

/// 临时把 `ST_SCENE3D_BACKEND` 设成给定值（RAII 还原）。
///
/// 为何用环境变量而不是给 `Scene3D::create` 加一个“指定腿”的参数：环境变量是
/// **进程级调试开关**的正确形态（与 `ST_GPU_FORCE_WARP` 一致），不需要污染
/// 每个调用点；探针自己用完就还原。
///
/// ⚠ 必须用 `_putenv_s` 而不是 Win32 的 `SetEnvironmentVariableA`：
/// `st::fs::read_env` 走 `std::getenv`，而 MinGW/CRT 的 `getenv` 看的是**CRT 自己的
/// 环境块副本**——只改进程环境块（Win32 API）改不动它。实测踩过：探针里
/// “强制 software” / “强制 gpu” 两段量出来一模一样，因为开关根本没生效。
class ForcedBackend {
 public:
  explicit ForcedBackend(const char* value) {
    const char* previous = std::getenv("ST_SCENE3D_BACKEND");
    if (previous != nullptr) {
      previous_ = previous;
      had_previous_ = true;
    }
    (void)::_putenv_s("ST_SCENE3D_BACKEND", value);
  }
  ~ForcedBackend() {
    (void)::_putenv_s("ST_SCENE3D_BACKEND", had_previous_ ? previous_.c_str() : "");
  }
  ForcedBackend(const ForcedBackend&) = delete;
  auto operator=(const ForcedBackend&) -> ForcedBackend& = delete;

 private:
  std::string previous_{};
  bool had_previous_{false};
};

struct LegResult {
  std::string backend{"?"};
  double raster_ms{0.0};    ///< 只光栅（不含 end_frame）
  double full_ms{0.0};      ///< 完整帧（含合成回软件画布）
  double sync_ms{0.0};      ///< 强制 GPU 完成的额外成本（软件腿为 0）
  double readback_ms{0.0};  ///< full − raster，即合成/传输成本
  double inked{0.0};        ///< 覆盖比例（确认真画了东西）
  std::uint64_t draw_calls{0};
};

/// 强制 GPU 把命令流跑完。
///
/// 为何必须做：D3D11 的 `Draw`/`DrawIndexed` 只是**把命令塞进缓冲**，立刻返回；
/// 真光栅在 GPU 上异步发生。不等待就计时，量到的是 CPU 侧的 API 开销——
/// 这就是第一版探针里“GPU 光栅 0.03 ms”的来源（那不是显卡快，是没量到）。
/// `Scene3D::flush()` 软件腿是空操作，所以同一套计时代码对两条腿都公平。
///

/// 量一条腿（尺寸 `width`×`height`，1000 面级网格）。
[[nodiscard]] auto measure_leg(int width, int height) -> LegResult {
  LegResult result{};
  auto created = Scene3D::create(width, height);
  if (!created.has_value()) return result;
  Scene3D& scene = **created;
  result.backend = scene.backend();

  Canvas canvas{width, height, 1.0f};
  const Color background{10, 14, 26, 255};
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 3.0f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;
  const Mesh mesh = Mesh::sphere(0.9f, 24);
  const Mat4 model = Mat4::rotation(Vec3{0.4f, 1.0f, 0.2f}.normalized(), 0.6f);
  const Rect destination{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};

  // 预热：首次要建目标/着色器/上传网格
  scene.begin_frame(background);
  scene.set_camera(camera);
  scene.draw_mesh(mesh, model);
  scene.end_frame(canvas, destination);

  result.raster_ms = best_ns([&] {
    scene.begin_frame(background);
    scene.set_camera(camera);
    scene.draw_mesh(mesh, model);
    scene.flush();   // 等真光栅跑完（软件腿空操作）
  }) / 1.0e6;
  result.full_ms = best_ns([&] {
    scene.begin_frame(background);
    scene.set_camera(camera);
    scene.draw_mesh(mesh, model);
    scene.end_frame(canvas, destination);
  }) / 1.0e6;
  result.readback_ms = result.full_ms - result.raster_ms;
  if (result.readback_ms < 0.0) result.readback_ms = 0.0;
  result.draw_calls = scene.draw_calls();

  const std::uint32_t bg = st::math::premultiply(background);
  std::size_t inked = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != bg) ++inked;
  }
  result.inked = canvas.pixels().empty()
                     ? 0.0
                     : static_cast<double>(inked) / static_cast<double>(canvas.pixels().size());
  return result;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "三维两条腿对照（GPU / 软件）：光栅成本 vs 回读成本",
      .criterion = "两条腿都画得出东西；GPU 的光栅成本应显著低于软件，回读成本单独计",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("scene3d_two_legs_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  const std::vector<std::pair<int, int>> sizes = {
      {1920, 1080}, {1280, 720}, {720, 720}, {480, 480}, {256, 192}};

  std::vector<LegResult> gpu_rows;
  std::vector<LegResult> software_rows;
  std::string gpu_note;
  {
    const ForcedBackend force("gpu");
    for (const auto& size : sizes) {
      gpu_rows.push_back(measure_leg(size.first, size.second));
    }
  }
  {
    const ForcedBackend force("software");
    for (const auto& size : sizes) {
      software_rows.push_back(measure_leg(size.first, size.second));
    }
  }
  if (!gpu_rows.empty() && gpu_rows.front().backend != "gpu") {
    gpu_note = "（强制 gpu 失败，实际落到了：" + gpu_rows.front().backend + "）";
  }

  st::print("三维两条腿对照（1100 面级网格，best-of-{}）{}\n\n", kRounds, gpu_note);
  st::print("{:<12} {:>22} {:>26} {:>10}\n", "尺寸", "GPU 光栅 / 完整帧 / 回读",
            "软件 光栅 / 完整帧 / 回读", "完整帧快多少");
  for (std::size_t i = 0; i < sizes.size() && i < gpu_rows.size() && i < software_rows.size();
       ++i) {
    const LegResult& g = gpu_rows[i];
    const LegResult& s = software_rows[i];
    st::print("{:<12} {:>7.2f} /{:>7.2f} /{:>7.2f} {:>10.2f} /{:>7.2f} /{:>7.2f} {:>10.2f}×\n",
              std::format("{}x{}", sizes[i].first, sizes[i].second),
              g.raster_ms, g.full_ms, g.readback_ms,
              s.raster_ms, s.full_ms, s.readback_ms,
              g.full_ms > 0.0 ? s.full_ms / g.full_ms : 0.0);
  }

  st::print("\n判读：\n");
  if (!gpu_rows.empty() && !software_rows.empty()) {
    const LegResult& g = gpu_rows.front();
    const LegResult& s = software_rows.front();
    st::print("  1080p：GPU 光栅 {:.2f} ms（占整帧 {:.0f}%），回读 {:.2f} ms；\n", g.raster_ms,
              g.full_ms > 0.0 ? g.raster_ms / g.full_ms * 100.0 : 0.0, g.readback_ms);
    st::print("         软件 光栅 {:.2f} ms（占整帧 {:.0f}%）。\n", s.raster_ms,
              s.full_ms > 0.0 ? s.raster_ms / s.full_ms * 100.0 : 0.0);
    const bool gpu_raster_wins = g.raster_ms < s.raster_ms;
    st::print("  → 光栅环节{}（{:.2f} vs {:.2f} ms）；传输环节是{}的主要成本。\n",
              gpu_raster_wins ? "GPU 更快" : "GPU 没占优", g.raster_ms, s.raster_ms,
              g.readback_ms > g.raster_ms ? "GPU 腿" : "两者各自");
    verdict.add("GPU 光栅快于软件光栅", gpu_raster_wins,
                std::format("{:.2f} vs {:.2f} ms", g.raster_ms, s.raster_ms));
  }
  for (std::size_t i = 0; i < gpu_rows.size(); ++i) {
    verdict.add(std::format("GPU 腿 {} 画出内容", i),
                gpu_rows[i].inked > 0.02 && gpu_rows[i].draw_calls > 0,
                std::format("覆盖 {:.1f}%", gpu_rows[i].inked * 100.0));
  }
  for (std::size_t i = 0; i < software_rows.size(); ++i) {
    verdict.add(std::format("软件腿 {} 画出内容", i),
                software_rows[i].inked > 0.02 && software_rows[i].draw_calls > 0,
                std::format("覆盖 {:.1f}%", software_rows[i].inked * 100.0));
  }
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
