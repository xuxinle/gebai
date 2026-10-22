// **三维渲染腿的单帧成本**——裁云要把三维当画面主体用之前，先把底子量清楚。
//
// ## 为什么先量再改
//
// `DESIGN.md` §8.4.2 把「系统高阶 3D 腿」的触发条件定在**测量**上（界面上的一块
// 卡片 960×200 软件腿是个位数毫秒）。现在需求变了：裁云要把三维当**画面主体**用，
// 尺寸从「卡片」变成「半屏」「整屏」，1080p 逐帧导出（30fps × 若干秒 = 数百帧）。
// 于是那条触发条件里的前提全都要重量一遍：
//
//   · 全屏尺寸下软件腿单帧多久？（1080p / 720p / 720 / 480 四档）
//   · 成本随三角形数怎么涨？（cube 12 面 / sphere seg=24 ≈ 1000 面 / seg=64 ≈ 8000 面）
//   · 成本随覆盖像素还是随三角形数涨？
//   · **当前这条腿是 GPU 还是软件**？（同一个探针量两条腿，差值才是显卡的收益）
//
// 判据（写死在这里，免得下一轮凭感觉）：**1080p 全屏、1000 面级网格、单帧 ≤ 8ms**
// 才算「能当导出主体用」（30fps 预算 33ms，三维自己最多占 1/4）。
//
// ## 量什么、不量什么
//
// 只量 `Scene3D` 的 `begin_frame`/`set_camera`/`draw_mesh`/`end_frame` 四段
// （best-of-N 取最优值去抖动，与 `cull_effectiveness_probe` 同口径）。**不含** 上层
// 二维画布的其它绘制，也不含 PNG 编码——那些是裁云侧的事，不该混进这里的结论。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
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

constexpr int kRounds = 12;

/// 一次渲染的耗时（纳秒，best-of-N）。
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

struct SizeCase {
  int width{0};
  int height{0};
  const char* label{""};
};

/// 一次渲染的成本拆解。
///
/// `frame_ns` 是**整段**（清屏 + 摄像机 + 画网格 + 合成）；
/// `clear_ns` 是同样尺寸下**不画网格**的整段。两者之差就是网格的净成本，
/// 把「清屏与深度缓冲复位」这类固定开销从结论里剥出来。
struct Cost {
  double frame_ns{0.0};
  double clear_ns{0.0};
  std::uint64_t draw_calls{0};
  double inked_ratio{0.0};
  int triangles{0};
};

[[nodiscard]] auto measure(const Mesh& mesh, const SizeCase& size, float camera_distance)
    -> Cost {
  auto created = Scene3D::create(size.width, size.height);
  if (!created.has_value()) return Cost{};
  Scene3D& scene = **created;
  Canvas canvas{size.width, size.height, 1.0f};
  const Color background{10, 14, 26, 255};

  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, camera_distance};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;

  const Mat4 model = Mat4::rotation(Vec3{0.4f, 1.0f, 0.2f}.normalized(), 0.6f);
  const Rect destination{0.0f, 0.0f, static_cast<float>(size.width),
                         static_cast<float>(size.height)};

  const double frame_ns = best_ns([&] {
    scene.begin_frame(background);
    scene.set_camera(camera);
    scene.draw_mesh(mesh, model);
    scene.end_frame(canvas, destination);
  });
  const double clear_ns = best_ns([&] {
    scene.begin_frame(background);
    scene.set_camera(camera);
    scene.end_frame(canvas, destination);
  });

  // 覆盖比例：确认「真的画了东西」而不是量了个空帧（空帧 8ms 是另一类问题）
  const std::uint32_t bg = st::math::premultiply(background);
  std::size_t inked = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != bg) ++inked;
  }
  const double ratio = canvas.pixels().empty()
                           ? 0.0
                           : static_cast<double>(inked) /
                                 static_cast<double>(canvas.pixels().size());
  return Cost{frame_ns, clear_ns, scene.draw_calls(), ratio,
              static_cast<int>(mesh.indices.size() / 3)};
}

/// 当前这条三维腿是谁（"gpu" / "software"）。
[[nodiscard]] auto backend_name() -> std::string {
  auto created = Scene3D::create(64, 64);
  if (!created.has_value()) return "不可用";
  return (*created)->backend();
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "三维渲染腿的单帧成本（裁云把三维当画面主体前的基线）",
      .criterion = "1080p 全屏 + 1000 面级网格：单帧 ≤ 8ms（30fps 预算的 1/4）",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("scene3d_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  if (!Scene3D::available()) {
    return st::probe::main_of(spec, opts, st::probe::skip("本平台没有可用的三维实现"));
  }

  const std::string backend = backend_name();
  st::print("三维腿        : {}\n", backend);
  if (const auto info = st::raster::gpu::probe(); info.has_value()) {
    st::print("GPU 设备      : {} · {} · {}{}\n", info->adapter, info->feature_level,
              info->backend, info->warp ? "（WARP，CPU 模拟）" : "");
  } else {
    st::print("GPU 设备      : 不可用（{}）\n", info.error().message);
  }

  const std::vector<SizeCase> sizes = {
      {1920, 1080, "1080p 全屏"},
      {1280, 720, "720p 全屏"},
      {720, 720, "半屏方块"},
      {480, 480, "缩略"},
  };

  struct MeshCase {
    std::string label{};
    Mesh mesh{};
  };
  std::vector<MeshCase> meshes;
  meshes.push_back({"cube(1.4) 12 面", Mesh::cube(1.4f)});
  meshes.push_back({"sphere(0.9,seg=24)", Mesh::sphere(0.9f, 24)});
  meshes.push_back({"sphere(0.9,seg=64)", Mesh::sphere(0.9f, 64)});

  st::print("\n单帧成本（best-of-{}）\n", kRounds);
  // ── 表 1：尺寸 × 网格 ──
  st::print("{:<22}", "网格 \\ 尺寸");
  for (const auto& size : sizes) st::print("{:>14}", size.label);
  st::print("\n");
  for (const auto& item : meshes) {
    st::print("{:<22}", item.label);
    for (const auto& size : sizes) {
      const Cost cost = measure(item.mesh, size, 3.0f);
      st::print("{:>14}", std::format("{:.2f} ms", cost.frame_ns / 1.0e6));
    }
    st::print("\n");
  }

  // ── 表 2：净成本拆解（帧总 − 只清屏+合成） ──
  st::print("\n净成本拆解（1080p）：帧总 / 空场景 / 网格净耗时\n");
  for (const auto& item : meshes) {
    const Cost cost = measure(item.mesh, SizeCase{1920, 1080, "1080p"}, 3.0f);
    const double net = cost.frame_ns - cost.clear_ns;
    st::print("  {:<22} {:>7.2f} ms / {:>6.2f} ms / {:>6.2f} ms   覆盖 {:>5.1f}%  三角形 {}\n",
              item.label, cost.frame_ns / 1.0e6, cost.clear_ns / 1.0e6, net / 1.0e6,
              cost.inked_ratio * 100.0, cost.triangles);
  }

  // ── 表 3：每百万像素的单价（判断成本随面积还是随面数涨） ──
  st::print("\n单价（sphere seg=24，按像素面积归一）：\n");
  for (const auto& size : sizes) {
    const Cost cost = measure(meshes[1].mesh, size, 3.0f);
    const double megapixels =
        static_cast<double>(size.width) * static_cast<double>(size.height) / 1.0e6;
    st::print("  {:<14} {:>7.2f} ms   {:.2f} Mpx   {:.3f} ms/Mpx\n", size.label,
              cost.frame_ns / 1.0e6, megapixels,
              megapixels > 0.0 ? cost.frame_ns / 1.0e6 / megapixels : 0.0);
  }

  // ── 判据：1080p + 1000 面级 ──
  const Cost judge = measure(meshes[1].mesh, SizeCase{1920, 1080, "1080p"}, 3.0f);
  const double judge_ms = judge.frame_ns / 1.0e6;
  st::print("\n判据：1080p + {} 面 = {:.2f} ms（阈值 8.00 ms）→ {}\n", judge.triangles, judge_ms,
            judge_ms <= 8.0 ? "合格" : "超标");
  verdict.add("1080p/1000面单帧 ≤ 8ms", judge_ms <= 8.0, std::format("{:.2f} ms", judge_ms));
  verdict.add("确实画出了内容（非空帧）", judge.inked_ratio > 0.02,
              std::format("覆盖 {:.1f}%，draw_calls={}", judge.inked_ratio * 100.0,
                          judge.draw_calls));
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
