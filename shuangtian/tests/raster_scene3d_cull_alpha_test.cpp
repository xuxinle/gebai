/// 三维：背面剔除与透明混合的回归测试。
///
/// ## 为什么剔除必须单独测「方向」
///
/// 对**封闭实体**，剔正面与剔背面在深度测试下都能画出"看起来正常的实体"
/// （一个看到外表面、一个看到内表面，形状相同）。所以"立方体能画出来"
/// 这类断言**完全分辨不出方向对错**——实测过：方向写反时，
/// `software_scene_renders_a_cube` 与 `scene3d_depth_ordering` 全都绿。
///
/// 唯一能分辨的是**单面物体**（平面）：法线 +Z，从 +Z 看可见、从 -Z 看应不可见。
/// 方向反了这两条会互换。
///
/// ## 为什么透明要测「先后」
///
/// 透明混合的正确性**只取决于绘制顺序**。用两个前后错开的半透明平面：
/// 颜色不同时，"近的叠在远的上"与"远的叠在近的上"结果不同。
/// 判据用**颜色混合结果**而不是"看起来透明"——后者无法自动化。

#include <cmath>
#include <cstdint>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;
using st::raster::Camera;
using st::raster::Canvas;
using st::raster::CullMode;
using st::raster::Material;
using st::raster::Mesh;
using st::raster::Scene3D;

namespace {

constexpr int kW = 240;
constexpr int kH = 240;

/// 数实体像素。背景基准取**左上角**：`begin_frame` 的清屏色会经色调映射 + sRGB
/// 编码，画布里的值不等于传进去的那个值——拿传入值当背景会把全屏都算成实体
/// （实测踩过：三种剔除模式都读到 102400 = 全屏）。
auto lit_pixels(const Canvas& canvas) -> int {
  const Color bg = canvas.pixel_at(0, 0);
  int n = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d > 100) ++n;
    }
  }
  return n;
}

/// 画布中心像素（透明混合的结果就看这里）。
auto center_pixel(const Canvas& canvas) -> Color {
  return canvas.pixel_at(canvas.physical_width() / 2, canvas.physical_height() / 2);
}

/// 渲染一个网格（可选剔除模式 / 材质）。
auto render(const Mesh& mesh, Vec3 eye, CullMode cull = CullMode::Back,
            const Material* material = nullptr) -> Canvas {
  Canvas canvas{kW, kH, 1.0f};
  const Color bg{10, 12, 18, 255};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return canvas;
  Camera cam;
  cam.eye = eye;
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(cull);
  if (material != nullptr) (*scene)->set_material(*material);
  (*scene)->draw_mesh(mesh, Mat4::identity());
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  return canvas;
}

}  // namespace

ST_TEST(scene3d_cull_direction_is_correct_on_a_single_sided_plane) {
  // 平面法线 +Z：从 +Z 看是正面，从 -Z 看是背面。
  const Mesh plane = Mesh::plane(2.0f, 2.0f, Color{200, 200, 200, 255});
  const Vec3 front{0.0f, 0.0f, 4.0f};
  const Vec3 back{0.0f, 0.0f, -4.0f};

  // 双面：两个方向都可见
  ST_CHECK(lit_pixels(render(plane, front, CullMode::None)) > 1000);
  ST_CHECK(lit_pixels(render(plane, back, CullMode::None)) > 1000);

  // 剔背面（默认）：正面可见、背面**完全不可见**
  ST_CHECK(lit_pixels(render(plane, front, CullMode::Back)) > 1000);
  ST_CHECK(lit_pixels(render(plane, back, CullMode::Back)) == 0);

  // 剔正面：反过来
  ST_CHECK(lit_pixels(render(plane, front, CullMode::Front)) == 0);
  ST_CHECK(lit_pixels(render(plane, back, CullMode::Front)) > 1000);
}

ST_TEST(scene3d_cull_default_is_back_facing) {
  // 不调 `set_cull_mode` 时必须是剔背面（与 Blender / 游戏引擎一致）。
  // 这条防止"默认值被悄悄改成 None"——那会让封闭物体的内壁重新出现。
  const Mesh plane = Mesh::plane(2.0f, 2.0f, Color{200, 200, 200, 255});
  const Vec3 back{0.0f, 0.0f, -4.0f};
  // 用 `render` 的默认参数（不显式传 CullMode）
  Canvas canvas{kW, kH, 1.0f};
  const Color bg{10, 12, 18, 255};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return;
  Camera cam;
  cam.eye = back;
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->draw_mesh(plane, Mat4::identity());   // 不设剔除模式
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  ST_CHECK(lit_pixels(canvas) == 0);
}

ST_TEST(scene3d_transparent_shows_the_far_surface_through_the_near_one) {
  // 透明混合的核心断言：**后面的东西能透出来**。
  // 关掉深度写 + 不剔除时，一个半透明的平面后面若还有东西，那里必须不是纯背景。
  //
  // 这里用两个前后错开的平面：近的红色半透明、远的绿色不透明。
  // 正确结果：中心像素带绿（透过红看到绿），而不是纯红。
  const Color bg{10, 12, 18, 255};
  Canvas canvas{kW, kH, 1.0f};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return;
  Camera cam;
  cam.eye = Vec3{0.0f, 0.0f, 5.0f};
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::None);   // 两个平面单面，双面才都看得见
  // 远的绿平面（不透明）
  Material opaque{};
  (*scene)->set_material(opaque);
  (*scene)->draw_mesh(Mesh::plane(2.0f, 2.0f, Color{40, 220, 90, 255}),
                      Mat4::translation(Vec3{0.0f, 0.0f, -0.6f}));
  // 近的红平面（半透明）
  Material glass{};
  glass.opacity = 0.5f;
  (*scene)->set_material(glass);
  (*scene)->draw_mesh(Mesh::plane(2.0f, 2.0f, Color{230, 50, 40, 255}),
                      Mat4::translation(Vec3{0.0f, 0.0f, 0.4f}));
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();

  const Color c = center_pixel(canvas);
  // ## 两腿数值不同的原因（不是缺陷）
  //
  // 实测：同一场景中心像素，软件腿 (178,184,62)、GPU 腿 (118,121,53)。
  // 差别来自**混合发生的位置**：
  //   · 软件腿：自己在**线性空间**混（解码已画像素 → 混 → 再编码），
  //     这是物理上最正确的做法；
  //   · GPU 腿：走 D3D 固定功能混合，而渲染目标存的是**已编码 sRGB 值**，
  //     所以它在编码值上混（标准做法；要得到线性混合需用 sRGB 目标格式）。
  // 两者都对，只是不在同一个空间。所以判据只能钉“**绿透出来了**”这种定性事实，
  // 不能钉具体字节——否则会退化成“只能在一条腿上跑”的测试。
  ST_CHECK(c.r > 60);              // 红：确实被画了
  ST_CHECK(static_cast<int>(c.g) > 60);   // 绿：透过红看得见
  // 若透明完全不生效（红盖住绿），绿会接近 0；若完全不透明，红会占主导
  ST_CHECK(static_cast<int>(c.g) > static_cast<int>(c.r) / 2);
}

ST_TEST(scene3d_opacity_zero_erases_the_near_surface) {
  // 最直接的分辨力：opacity = 0 时近处平面应当**完全不出现**。
  // 若还是红色，说明 transparent 队列/混合根本没有生效。
  const Color bg{10, 12, 18, 255};
  Canvas canvas{kW, kH, 1.0f};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return;
  Camera cam;
  cam.eye = Vec3{0.0f, 0.0f, 5.0f};
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::None);
  Material green_mat{};
  (*scene)->set_material(green_mat);
  (*scene)->draw_mesh(Mesh::plane(2.0f, 2.0f, Color{40, 220, 90, 255}),
                      Mat4::translation(Vec3{0.0f, 0.0f, -0.6f}));
  Material invisible{};
  invisible.opacity = 0.0f;
  (*scene)->set_material(invisible);
  (*scene)->draw_mesh(Mesh::plane(2.0f, 2.0f, Color{230, 50, 40, 255}),
                      Mat4::translation(Vec3{0.0f, 0.0f, 0.4f}));
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
    const Color c = center_pixel(canvas);
  // opacity=0 → 红色不应贡献；中心应仍是绿色（绿 220 主导）
  ST_CHECK(static_cast<int>(c.g) > 150);
  ST_CHECK(static_cast<int>(c.r) < 100);
}

ST_TEST(scene3d_opaque_material_still_hides_what_is_behind) {
  // 反向保护：**不透明**平面必须挡住后面的东西。
  // 没有这条，"透明永远生效"的错误实现也能过上面那条。
  const Color bg{10, 12, 18, 255};
  Canvas canvas{kW, kH, 1.0f};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return;
  Camera cam;
  cam.eye = Vec3{0.0f, 0.0f, 5.0f};
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::None);
  (*scene)->draw_mesh(Mesh::plane(2.0f, 2.0f, Color{40, 220, 90, 255}),
                      Mat4::translation(Vec3{0.0f, 0.0f, -0.6f}));
  (*scene)->draw_mesh(Mesh::plane(2.0f, 2.0f, Color{230, 50, 40, 255}),
                      Mat4::translation(Vec3{0.0f, 0.0f, 0.4f}));
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  const Color c = center_pixel(canvas);
  ST_CHECK(c.r > 150);                        // 近处红色是主导
  ST_CHECK(static_cast<int>(c.g) < 100);      // 绿色被挡住，几乎看不到
}
