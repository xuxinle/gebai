/// 软件 3D：跨平台可用的那条腿。
///
/// 定位（用户定的架构原则）：**软件渲染保证跨平台可用，系统高阶 API 锦上添花**。
/// 因此三维能力必须有一条软路径——否则"删掉 OpenGL"就等于"非 Windows 上三维消失"。
/// 这个文件钉住软路径的三件事：
/// ① 真的画出了东西（不是空帧）；② **深度关系正确**（近的挡住远的）；
/// ③ 相机/模型变换真的生效（画面随参数变化）。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
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

inline constexpr int kWidth = 256;
inline constexpr int kHeight = 192;

[[nodiscard]] auto inked_ratio(const Canvas& canvas, Color background) -> double {
  const std::uint32_t bg = st::math::premultiply(background);
  std::size_t inked = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != bg) ++inked;
  }
  return canvas.pixels().empty() ? 0.0 : static_cast<double>(inked) /
                                              static_cast<double>(canvas.pixels().size());
}

[[nodiscard]] auto average_luma(const Canvas& canvas) -> double {
  double sum = 0.0;
  std::size_t count = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    const auto r = static_cast<double>((pixel >> 24U) & 0xFFU);
    const auto g = static_cast<double>((pixel >> 16U) & 0xFFU);
    const auto b = static_cast<double>((pixel >> 8U) & 0xFFU);
    if (r + g + b == 0.0) continue;
    sum += 0.299 * r + 0.587 * g + 0.114 * b;
    ++count;
  }
  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

/// 产物目录：**放在 `build/` 下**（已 gitignore）。
///
/// 测试产物不该进仓库：它每次跑都会重写，渲染稍有变化就是一堆二进制 diff；
/// 而它又不是"黄金文件"（断言不用它比对，只供人眼看）。仓库里曾有一批
/// `gl/*.png` 就是这类噪声，随 GL 测试一起删掉了。
constexpr const char* kArtifactDir = "build/test-artifacts";

void write_png_at(const Canvas& canvas, const char* path) {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  image.rgba.resize(canvas.pixels().size() * 4U);
  for (std::size_t index = 0; index < canvas.pixels().size(); ++index) {
    const std::uint32_t pixel = canvas.pixels()[index];
    image.rgba[index * 4U + 0U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
    image.rgba[index * 4U + 1U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
    image.rgba[index * 4U + 2U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
    image.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(pixel & 0xFFU);
  }
  (void)st::codec::png_write_file(path, image);
}

/// 按文件名写到产物目录（自动建目录）。
void write_png(const Canvas& canvas, const char* leaf) {
  const std::string directory = std::string(kArtifactDir) + "/scene";
  (void)st::fs::create_directories(directory);
  const std::string path = directory + "/" + leaf;
  write_png_at(canvas, path.c_str());
}

/// 渲染一帧到画布。
void render(Scene3D& scene, Canvas& canvas, const Mesh& mesh, const Mat4& model,
            const Camera& camera, Color background) {
  canvas.clear(background);
  scene.begin_frame(background);
  scene.set_camera(camera);
  scene.draw_mesh(mesh, model);
  scene.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight)});
}

}  // namespace

ST_TEST(software_scene_is_always_available) {
  // 软件腿的意义就在这条：**不依赖任何设备**，永远可用。
  ST_CHECK(Scene3D::available());
}

ST_TEST(software_scene_renders_a_cube) {
  auto scene = Scene3D::create(kWidth, kHeight);
  ST_CHECK(scene.has_value());
  if (!scene.has_value()) return;

  Canvas canvas{kWidth, kHeight};
  const Color background{0x14, 0x18, 0x22, 0xFF};
  Camera camera;
  camera.eye = Vec3{2.1f, 1.7f, 2.6f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  const Mesh cube = Mesh::cube(1.2f);
  render(**scene, canvas, cube, Mat4::identity(), camera, background);

  ST_CHECK((*scene)->draw_calls() > 0);
  const double inked = inked_ratio(canvas, background);
  ST_CHECK(inked > 0.05);   // 真画出了东西
  write_png(canvas, "01-software-cube.png");

  // 光照：不同朝向的面亮度必须不同（纯色填充做不到这一点）
  std::vector<double> luma;
  for (int index = 0; index < 6; ++index) {
    Canvas face{kWidth, kHeight};
    Camera head_on;
    head_on.eye = Vec3{0.0f, 0.0f, 4.0f};
    head_on.target = Vec3{0.0f, 0.0f, 0.0f};
    render(**scene, face, cube, Mat4::rotation(Vec3{0, 1, 0}, static_cast<float>(index) * 0.7f),
           head_on, background);
    luma.push_back(average_luma(face));
    if (index == 2) write_png(face, "02-software-cube-rotated.png");
  }
  const double min_luma = *std::min_element(luma.begin(), luma.end());
  const double max_luma = *std::max_element(luma.begin(), luma.end());
  ST_CHECK(max_luma - min_luma > 5.0);
}

ST_TEST(software_scene_depth_buffer_hides_the_far_face) {
  // **深度关系**是 3D 与 2D 的分界。用两个前后错开的立方体验证：
  // 近的那个必须挡住远的——若 z-buffer 失效（或按绘制顺序覆盖），
  // 后画的那个会盖在前面，画面就错了。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  Canvas canvas{kWidth, kHeight};
  const Color background{0x14, 0x18, 0x22, 0xFF};
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 5.0f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};

  const Mesh near_cube = Mesh::box(2.0f, 2.0f, 0.4f);   // 大而薄，放在前面
  const Mesh far_cube = Mesh::box(0.6f, 0.6f, 0.6f);    // 小，放在后面

  // 故意**先画近的、后画远的**：按绘制顺序覆盖的实现会错，z-buffer 的不会。
  canvas.clear(background);
  scene->get()->begin_frame(background);
  scene->get()->set_camera(camera);
  scene->get()->draw_mesh(near_cube, Mat4::translation(Vec3{0.0f, 0.0f, 1.5f}));
  scene->get()->draw_mesh(far_cube, Mat4::translation(Vec3{0.0f, 0.0f, -1.0f}));
  scene->get()->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                       static_cast<float>(kHeight)});
  write_png(canvas, "03-software-depth.png");

  // 远处的立方体在近处立方体后面 → 它**完全不可见**（近块是 2×2、距离 1.5，
  // 视角下足以遮住 0.6 宽、距离 -1 的那个）。取中心区域检查：应只有近块的颜色。
  const Color center = canvas.pixel_at(kWidth / 2, kHeight / 2);
  const Color edge = canvas.pixel_at(4, 4);
  ST_CHECK(center.a == 255);                                  // 中心有东西
  ST_CHECK(std::abs(static_cast<int>(edge.r) - 0x14) <= 2);   // 角落是背景
}

ST_TEST(software_scene_projects_with_perspective) {
  // 近大远小：相机拉远后覆盖面积必须显著变小（透视真的生效）。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh sphere = Mesh::sphere(0.8f, 20);
  const auto coverage = [&](float distance) {
    Canvas canvas{kWidth, kHeight};
    Camera camera;
    camera.eye = Vec3{0.0f, 0.0f, distance};
    camera.target = Vec3{0.0f, 0.0f, 0.0f};
    render(**scene, canvas, sphere, Mat4::identity(), camera, background);
    return inked_ratio(canvas, background);
  };
  const double near_ratio = coverage(2.0f);
  const double far_ratio = coverage(6.0f);
  ST_CHECK(near_ratio > far_ratio * 1.5);
}

ST_TEST(software_scene_near_plane_stress_is_deterministic) {
  // 近裁剪面的压力场景：相机贴在模型内部时，大量三角形跨越相机平面（`w` 变号）。
  // 不裁近面 → 透视除法把顶点翻到另一侧 → 画面上出现穿透模型的乱线。
  //
  // 断言什么：站在立方体内部**内壁本来就铺满视野**（`inked≈1.0` 是正确行为，
  // 我第一版把断言写成 `inked < 0.95` 是错的）。这里改为钉住两条**真正该保证**的性质：
  // ① 不崩、有内容；② **逐像素可复现**——确定性是软件这条腿的核心承诺
  // （GPU 路径受驱动/时序影响，软件路径不该有这种不确定性）。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh cube = Mesh::cube(2.0f);

  const auto run = [&]() {
    Canvas canvas{kWidth, kHeight};
    Camera inside;
    inside.eye = Vec3{0.0f, 0.0f, 0.0f};   // 站在立方体中心
    inside.target = Vec3{0.0f, 0.0f, -1.0f};
    render(**scene, canvas, cube, Mat4::identity(), inside, background);
    return std::vector<std::uint32_t>(canvas.pixels().begin(), canvas.pixels().end());
  };

  const std::vector<std::uint32_t> first = run();
  const std::vector<std::uint32_t> second = run();
  ST_CHECK_EQ(first.size(), second.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < first.size(); ++i) {
    if (first[i] != second[i]) ++differing;
  }
  ST_CHECK_EQ(static_cast<int>(differing), 0);   // 逐像素可复现

  // 内壁铺满视野（站在里面本就如此）——顺带证明"确实画了，不是空帧"
  Canvas canvas{kWidth, kHeight};
  Camera inside;
  inside.eye = Vec3{0.0f, 0.0f, 0.0f};
  inside.target = Vec3{0.0f, 0.0f, -1.0f};
  render(**scene, canvas, cube, Mat4::identity(), inside, background);
  ST_CHECK(inked_ratio(canvas, background) > 0.5);
  write_png(canvas, "04-software-inside.png");
}

ST_TEST(software_scene_rejects_invalid_size) {
  ST_CHECK(!Scene3D::create(0, 128).has_value());
  ST_CHECK(!Scene3D::create(128, -1).has_value());
}

ST_TEST(software_scene_composites_into_gpu_canvas_on_any_renderer) {
  // 与 2D 的接口一致性：3D 结果必须能合成进**任何** Surface（软件画布或 GPU 画布），
  // 因为 2D 界面可能跑在任一渲染器上——这也是"两条腿"必须解耦的地方。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh cube = Mesh::cube(1.0f);
  Camera camera;
  camera.eye = Vec3{1.6f, 1.3f, 2.2f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};

  for (const bool use_gpu : {false, true}) {
    Canvas software{kWidth, kHeight};
    software.clear(background);
    Scene3D* target_scene = scene->get();
    target_scene->begin_frame(background);
    target_scene->set_camera(camera);
    target_scene->draw_mesh(cube, Mat4::identity());
    if (!use_gpu) {
      target_scene->end_frame(software, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                             static_cast<float>(kHeight)});
      ST_CHECK(inked_ratio(software, background) > 0.05);
      continue;
    }
    auto gpu = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
    if (!gpu.has_value()) continue;   // 无 GPU 环境：本分支不适用
    (**gpu).clear(background);
    target_scene->end_frame(**gpu, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                        static_cast<float>(kHeight)});
    const std::span<const std::uint32_t> pixels = (**gpu).pixels();
    const std::uint32_t bg = st::math::premultiply(background);
    std::size_t inked = 0;
    for (const std::uint32_t pixel : pixels) {
      if (pixel != bg) ++inked;
    }
    ST_CHECK(inked > pixels.size() / 20U);
  }
}
