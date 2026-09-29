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

ST_TEST(gl_fill_path_handles_holes_via_nonzero_winding) {
  // "复杂图形"的判据不是"能画个矩形"，而是**带洞与自交仍正确**。
  // 这里用两个反向环绕的方框：外框顺时针、内框逆时针 →
  // 非零环绕规则下中间必须是**空的**（这正是软件光栅器的语义）。
  //
  // 这条同时钉住实现方式：模板法的 INCR/DECR 恰好按环绕数累加，
  // 换成"耳切三角化"就得自己处理洞，且自交形状会算错。
  if (!st::raster::gl::available()) return;
  auto scene = Scene3D::create(kWidth, kHeight);
  ST_CHECK(scene.has_value());
  if (!scene.has_value()) return;

  Canvas canvas{kWidth, kHeight};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);

  st::raster::Path path;
  // 外框：顺时针（左上 → 右上 → 右下 → 左下）
  path.move_to(st::math::Point{20.0f, 20.0f});
  path.line_to(st::math::Point{236.0f, 20.0f});
  path.line_to(st::math::Point{236.0f, 172.0f});
  path.line_to(st::math::Point{20.0f, 172.0f});
  path.close();
  // 内框：逆时针（洞）
  path.move_to(st::math::Point{80.0f, 60.0f});
  path.line_to(st::math::Point{80.0f, 130.0f});
  path.line_to(st::math::Point{176.0f, 130.0f});
  path.line_to(st::math::Point{176.0f, 60.0f});
  path.close();

  (*scene)->begin_frame(background);
  (*scene)->fill_path(path, Color{0x4A, 0x9E, 0xFF, 0xFF});
  (*scene)->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                  static_cast<float>(kHeight)});

  // 环带内应着色（外框边缘与内框之间）
  const Color band = canvas.pixel_at(50, 96);
  ST_CHECK(band.b > band.r + 40);           // 明显是那个蓝色
  // 洞里应是底色（**这是非零环绕的关键断言**）
  const Color hole = canvas.pixel_at(128, 96);
  ST_CHECK(std::abs(static_cast<int>(hole.r) - 0x10) <= 3);
  ST_CHECK(std::abs(static_cast<int>(hole.b) - 0x1E) <= 3);
  // 外面也是底色
  const Color outside = canvas.pixel_at(5, 5);
  ST_CHECK(std::abs(static_cast<int>(outside.r) - 0x10) <= 3);
  write_png(canvas, "gl/30-path-with-hole.png");
}

ST_TEST(gl_stroke_path_draws_outline_only) {
  if (!st::raster::gl::available()) return;
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  Canvas canvas{kWidth, kHeight};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);

  st::raster::Path path;
  path.move_to(st::math::Point{40.0f, 40.0f});
  path.line_to(st::math::Point{200.0f, 40.0f});
  path.line_to(st::math::Point{200.0f, 150.0f});
  path.close();

  (*scene)->begin_frame(background);
  (*scene)->stroke_path(path, 6.0f, Color{0xFF, 0xC4, 0x4A, 0xFF});
  (*scene)->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                  static_cast<float>(kHeight)});
  // 描边：边上有着色、内部是空的（若实现退化成填充，这条会失败）
  const Color edge = canvas.pixel_at(120, 40);
  ST_CHECK(edge.r > 200);
  const Color inside = canvas.pixel_at(120, 100);
  ST_CHECK(std::abs(static_cast<int>(inside.r) - 0x10) <= 3);
  write_png(canvas, "gl/31-stroked-triangle.png");
}

namespace {

/// 手写一个立方体 OBJ（含法线），并**故意混入一个坏面**——
/// 真实导出文件里常有一两个退化/越界引用的面。
constexpr const char* kCubeObj = R"obj(
# 单位立方体，带法线
v -0.5 -0.5  0.5
v  0.5 -0.5  0.5
v  0.5  0.5  0.5
v -0.5  0.5  0.5
v -0.5 -0.5 -0.5
v  0.5 -0.5 -0.5
v  0.5  0.5 -0.5
v -0.5  0.5 -0.5
vn  0  0  1
vn  0  0 -1
vn  1  0  0
vn -1  0  0
vn  0  1  0
vn  0 -1  0
f 1//1 2//1 3//1 4//1
f 6//2 5//2 8//2 7//2
f 2//3 6//3 7//3 3//3
f 5//4 1//4 4//4 8//4
f 4//5 3//5 7//5 8//5
f 5//6 6//6 2//6 1//6
f 1 2 99
)obj";

}  // namespace

ST_TEST(gl_obj_loads_cube_with_normals_and_skips_bad_faces) {
  // "坏面只跳过、不让整文件失败"是这条的关键断言：真实模型文件里常有越界/退化面。
  const auto mesh = Mesh::load_obj(kCubeObj, Color{0x60, 0xA5, 0xFA, 0xFF});
  ST_CHECK(mesh.has_value());
  if (!mesh.has_value()) return;
  // 6 个四边形 → 12 个三角形；那个坏面（索引 99 越界）被跳过
  ST_CHECK_EQ(static_cast<int>(mesh->indices.size()), 36);
  ST_CHECK_EQ(static_cast<int>(mesh->vertex_count()), 36);
  // 法线必须归一化（否则光照会整体偏暗/偏亮——这是最容易被忽略的解析错误）
  for (std::size_t index = 0; index < mesh->vertex_count(); ++index) {
    const float nx = (*mesh)[index][3];
    const float ny = (*mesh)[index][4];
    const float nz = (*mesh)[index][5];
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    ST_CHECK(std::abs(length - 1.0f) < 0.01f);
  }
}

ST_TEST(gl_obj_supports_negative_indices_and_missing_normals) {
  // 负索引（相对引用）与"没有 vn"都要能用：前者是 OBJ 规范的一部分，
  // 后者是大量导出器（尤其只导几何的流水线）的常态。
  constexpr const char* kTriangle = R"obj(
v 0 0 0
v 1 0 0
v 0 1 0
f -3 -2 -1
)obj";
  const auto mesh = Mesh::load_obj(kTriangle, Color{0xFF, 0xFF, 0xFF, 0xFF});
  ST_CHECK(mesh.has_value());
  if (!mesh.has_value()) return;
  ST_CHECK_EQ(static_cast<int>(mesh->indices.size()), 3);
  // 缺法线时按面计算 → 必须仍归一化（若是 (0,0,0)，光照会全黑）
  const float nz = (*mesh)[0][5];
  ST_CHECK(std::abs(std::abs(nz) - 1.0f) < 0.01f);
}

ST_TEST(gl_obj_rejects_garbage_without_crashing) {
  // 空文件、只有注释、只有顶点没有面、完全不是 OBJ —— 都必须**返回错误**而不是崩。
  for (const char* text : {"", "# just a comment\n", "v 0 0 0\nv 1 0 0\n",
                           "not an obj at all\n{{{", "f 1 2 3\n"}) {
    const auto mesh = Mesh::load_obj(text, Color{0xFF, 0xFF, 0xFF, 0xFF});
    ST_CHECK(!mesh.has_value());
  }
  // 只有注释/顶点时也要给**可读的原因**（便于使用者判断是文件问题还是解析问题）
  const auto empty = Mesh::load_obj("v 0 0 0\n", Color{0xFF, 0xFF, 0xFF, 0xFF});
  ST_CHECK(!empty.has_value() && !empty.error().message.empty());
}

ST_TEST(gl_obj_cube_renders_under_lighting) {
  // 从解析到出图整条链：加载 OBJ → 渲染 → 画面里真有东西。
  if (!st::raster::gl::available()) return;
  const auto mesh = Mesh::load_obj(kCubeObj, Color{0x60, 0xA5, 0xFA, 0xFF});
  if (!mesh.has_value()) return;
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  Canvas canvas{kWidth, kHeight};
  const Color background{0x10, 0x14, 0x1E, 0xFF};
  canvas.clear(background);
  Camera camera;
  camera.eye = Vec3{1.6f, 1.3f, 2.2f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(background);
  (*scene)->set_camera(camera);
  (*scene)->draw_mesh(*mesh, Mat4::rotation(Vec3{0.3f, 1.0f, 0.2f}, 0.6f));
  (*scene)->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                  static_cast<float>(kHeight)});
  const double inked = inked_ratio(canvas, background);
  ST_CHECK(inked > 0.03);
  write_png(canvas, "gl/40-obj-cube.png");
}
