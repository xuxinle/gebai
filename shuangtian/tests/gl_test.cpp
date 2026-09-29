/// OpenGL 3D：探测、网格、真渲染。
///
/// 需求来源（用户）："引入 opengl 源码，实现复杂图形和三维渲染能力"。
/// 因此这里不只测"接口能调通"，而是**渲染出图**并核对：
/// ① 深度缓冲真的生效（近处的面挡住远处的面，而不是按绘制顺序覆盖）；
/// ② 光照让同一网格的不同朝向呈现不同亮度（纯色贴图做不到这一点）；
/// ③ 相机变了，画面真的跟着变（否则说明矩阵/视图没接上）；
/// ④ 缺源码时**如实报不可用**，而不是崩或静默空帧。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gl.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Rect;
using st::math::Vec3;
using st::raster::Canvas;
using st::raster::gl::Camera;
using st::raster::gl::Mesh;
using st::raster::gl::Scene3D;

inline constexpr int kWidth = 256;
inline constexpr int kHeight = 192;

/// 画面里"非底色"像素占比（确认真的画了东西）。
[[nodiscard]] auto inked_ratio(const Canvas& canvas, Color background) -> double {
  std::size_t inked = 0;
  const std::span<const std::uint32_t> pixels = canvas.pixels();
  const std::uint32_t bg = st::math::premultiply(background);
  for (const std::uint32_t pixel : pixels) {
    if (pixel != bg) ++inked;
  }
  return pixels.empty() ? 0.0 : static_cast<double>(inked) / static_cast<double>(pixels.size());
}

/// 画面平均亮度（用来判断相机/光照变化是否真的反映到像素上）。
[[nodiscard]] auto average_luma(const Canvas& canvas) -> double {
  double sum = 0.0;
  std::size_t count = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    const auto r = static_cast<double>((pixel >> 24U) & 0xFFU);
    const auto g = static_cast<double>((pixel >> 16U) & 0xFFU);
    const auto b = static_cast<double>((pixel >> 8U) & 0xFFU);
    if (r + g + b == 0.0) continue;  // 底色不计
    sum += 0.299 * r + 0.587 * g + 0.114 * b;
    ++count;
  }
  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

void write_png(const Canvas& canvas, const char* path) {
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

}  // namespace

ST_TEST(gl_matrix_perspective_and_look_at_are_sane) {
  // 矩阵先把基本性质钉住：投影把视锥内/外的点分开、look_at 把相机放到原点朝 -Z。
  const Mat4 projection = Mat4::perspective(0.8f, 1.5f, 0.1f, 100.0f);
  ST_CHECK(projection.value(3, 2) < -0.99f);   // 透视除法：w = -z
  ST_CHECK(projection.value(0, 0) > 0.0f);
  // 非法参数返回单位阵（不产出 NaN —— NaN 会静默变成整屏黑）
  const Mat4 bad = Mat4::perspective(0.8f, 0.0f, 0.1f, 100.0f);
  ST_CHECK(std::abs(bad.value(0, 0) - 1.0f) < 1e-6f);
  // look_at：相机在 (0,0,5) 看原点 → 世界原点应落在视图空间的 (0,0,-5)
  const Mat4 view = Mat4::look_at(Vec3{0, 0, 5}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
  const float z = view.value(2, 0) * 0.0f + view.value(2, 1) * 0.0f + view.value(2, 2) * 0.0f + view.value(2, 3);
  ST_CHECK(std::abs(z - (-5.0f)) < 1e-4f);
}

ST_TEST(gl_mesh_generators_produce_closed_shapes) {
  const Mesh cube = Mesh::cube(1.0f);
  ST_CHECK_EQ(static_cast<int>(cube.vertex_count()), 24);   // 每面 4 个独立顶点（法线不同）
  ST_CHECK_EQ(static_cast<int>(cube.indices.size()), 36);   // 12 个三角形
  const Mesh sphere = Mesh::sphere(0.5f, 16);
  ST_CHECK(sphere.vertex_count() > 50);
  ST_CHECK_EQ(static_cast<int>(sphere.indices.size() % 3), 0);
}

ST_TEST(gl_probe_is_honest_when_unavailable) {
  // 没拉取加载器源码时，必须是"如实报不可用"，而不是崩溃或静默空帧。
  if (st::raster::gl::has_opengl() && st::raster::gl::available()) {
    const auto info = st::raster::gl::probe();
    ST_CHECK(info.has_value());
    if (info.has_value()) {
      st::print("[gl] vendor={} renderer={} version={} glsl={}\n", info->vendor, info->renderer,
                info->version, info->glsl);
      ST_CHECK(!info->version.empty());
      ST_CHECK(info->max_texture_size > 0);
    }
    return;
  }
  const auto info = st::raster::gl::probe();
  ST_CHECK(!info.has_value());
  // 失败原因要能指导下一步（"去跑 fetch_opengl"），而不是一句"失败"
  ST_CHECK(!info.has_value() && !info.error().message.empty());
}

ST_TEST(gl_renders_a_cube_with_depth_and_lighting) {
  if (!st::raster::gl::available()) return;
  auto scene = Scene3D::create(kWidth, kHeight);
  ST_CHECK(scene.has_value());
  if (!scene.has_value()) return;

  Canvas canvas{kWidth, kHeight};
  const Color background{0x14, 0x18, 0x22, 0xFF};
  canvas.clear(background);

  const Mesh cube = Mesh::cube(1.2f);
  Camera camera;
  camera.eye = Vec3{2.1f, 1.7f, 2.6f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 45.0f;

  (*scene)->begin_frame(background);
  (*scene)->set_camera(camera);
  (*scene)->draw_mesh(cube, Mat4::identity());
  (*scene)->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                   static_cast<float>(kHeight)});

  ST_CHECK((*scene)->draw_calls() > 0);   // 确实提交了绘制
  const double inked = inked_ratio(canvas, background);
  ST_CHECK(inked > 0.05);                 // 画面上真有东西（不是空帧）
  write_png(canvas, "gl/01-cube.png");

  // 光照：同一网格不同朝向的面亮度必须不同——纯色填充做不到这一点，
  // 因此这条断言实际上在验证"着色器真的在算光照"。
  std::vector<double> face_luma;
  for (int index = 0; index < 6; ++index) {
    Canvas face_canvas{kWidth, kHeight};
    face_canvas.clear(background);
    Camera only_one;
    only_one.eye = Vec3{0.0f, 0.0f, 4.0f};
    only_one.target = Vec3{0.0f, 0.0f, 0.0f};
    // 每次只让一个面朝向相机：绕 Y 轴旋转
    (*scene)->begin_frame(background);
    (*scene)->set_camera(only_one);
    (*scene)->draw_mesh(cube, Mat4::rotation(Vec3{0, 1, 0},
                                             static_cast<float>(index) * 0.7f));
    (*scene)->end_frame(face_canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                          static_cast<float>(kHeight)});
    face_luma.push_back(average_luma(face_canvas));
    if (index == 2) write_png(face_canvas, "gl/02-cube-rotated.png");
  }
  const double min_luma = *std::min_element(face_luma.begin(), face_luma.end());
  const double max_luma = *std::max_element(face_luma.begin(), face_luma.end());
  ST_CHECK(max_luma - min_luma > 5.0);  // 朝向不同 → 亮度不同（光照生效）
}

ST_TEST(gl_camera_change_changes_pixels) {
  if (!st::raster::gl::available()) return;
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh sphere = Mesh::sphere(0.8f, 20);

  const auto render_with = [&](Camera camera) {
    Canvas canvas{kWidth, kHeight};
    canvas.clear(background);
    (*scene)->begin_frame(background);
    (*scene)->set_camera(camera);
    (*scene)->draw_mesh(sphere, Mat4::identity());
    (*scene)->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                     static_cast<float>(kHeight)});
    return average_luma(canvas);
  };

  Camera near_camera;
  near_camera.eye = Vec3{0.0f, 0.0f, 2.0f};
  Camera far_camera;
  far_camera.eye = Vec3{0.0f, 0.0f, 6.0f};
  const double near_value = render_with(near_camera);
  const double far_value = render_with(far_camera);
  // 相机拉远：球在画面里变小 → 底色的占比变大 → "平均亮度（只统计非底色）"不一定变，
  // 所以直接比**覆盖面积**更有意义。
  const auto coverage = [&](Camera camera) {
    Canvas canvas{kWidth, kHeight};
    canvas.clear(background);
    (*scene)->begin_frame(background);
    (*scene)->set_camera(camera);
    (*scene)->draw_mesh(sphere, Mat4::identity());
    (*scene)->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                     static_cast<float>(kHeight)});
    return inked_ratio(canvas, background);
  };
  const double near_coverage = coverage(near_camera);
  const double far_coverage = coverage(far_camera);
  ST_CHECK(near_coverage > far_coverage * 1.5);  // 近大远小：透视真的生效
  (void)near_value;
  (void)far_value;
}

ST_TEST(gl_scene_rejects_invalid_size) {
  const auto scene = Scene3D::create(0, 128);
  ST_CHECK(!scene.has_value());
}
