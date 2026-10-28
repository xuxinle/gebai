// **纹理与 UV 到底通不通**——先验证底子，再拿它做片子。
//
// ## 为何单独一个探针
//
// “模型贴上了图”与“模型贴上了**对的位置**”是两件事。常见失效形态：
// ① UV 的 v 轴没翻 → 图标上下颠倒（看着像模型朝向错，其实是 UV）；
// ② 纹理与顶点色相乘时用了预乘值 → 图整体偏暗；
// ③ 采样没做透视校正 → 斜面上图案“爬行”。
//
// 探针直接渲一个贴了已知图案的平面，把**四角与中心的颜色**与预期对上：
// 纹理是 2×2 四色块（左上红、右上绿、左下蓝、右下白），那么渲出来的
// 平面四象限必须呈现对应颜色（注意 v 轴翻转）。

#include <cstdint>
#include <format>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/core/log.hpp"
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

constexpr int kWidth = 256;
constexpr int kHeight = 256;

/// 2×2 四色纹理（RGBA8，行优先，非预乘）。
[[nodiscard]] auto make_checker() -> std::vector<std::uint8_t> {
  // 行 0（上）：左红 右绿；行 1（下）：左蓝 右白
  const Color pixels[2][2] = {{Color{255, 0, 0, 255}, Color{0, 255, 0, 255}},
                              {Color{0, 0, 255, 255}, Color{255, 255, 255, 255}}};
  std::vector<std::uint8_t> out;
  out.reserve(2 * 2 * 4);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      out.push_back(pixels[y][x].r);
      out.push_back(pixels[y][x].g);
      out.push_back(pixels[y][x].b);
      out.push_back(pixels[y][x].a);
    }
  }
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "三维纹理与 UV（贴图是否真的贴上、且位置正确）",
      .criterion = "贴图平面的四象限颜色与纹理一致（v 轴已翻转），且中心不为纯白（受光照）",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("scene3d_texture_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  if (!Scene3D::available()) {
    return st::probe::main_of(spec, opts, st::probe::skip("本平台没有可用的三维实现"));
  }
  auto created = Scene3D::create(kWidth, kHeight);
  if (!created.has_value()) {
    return st::probe::main_of(spec, opts, st::probe::skip("三维场景建不起来"));
  }
  Scene3D& scene = **created;
  st::log::out("三维腿：{}\n", scene.backend());

  Canvas canvas{kWidth, kHeight, 1.0f};
  canvas.clear(Color{0, 0, 0, 255});
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 2.4f};   // 正对平面
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 45.0f;

  const std::vector<std::uint8_t> checker = make_checker();
  const Mesh plane = Mesh::textured_plane(1.6f, 1.6f, Color{255, 255, 255, 255});

  scene.begin_frame(Color{0, 0, 0, 255});
  scene.set_camera(camera);
  scene.set_texture(checker, 2, 2);
  // 顶点色用纯白，这样渲出来的颜色就是纹理 × 光照，便于判断
  st::raster::Lighting lighting{};
  lighting.ambient = 1.0f;   // 关掉受光差异，先只验 UV 映射
  lighting.diffuse = 0.0f;
  lighting.rim = 0.0f;
  scene.set_lighting(lighting);
  scene.draw_mesh(plane, Mat4::identity());
  scene.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight)});
  scene.flush();

  // 取四象限中心与画面中心
  const auto at = [&canvas](float u, float v) { return canvas.pixel_at(static_cast<int>(u * kWidth), static_cast<int>(v * kHeight)); };
  const Color top_left = at(0.30f, 0.30f);
  const Color top_right = at(0.70f, 0.30f);
  const Color bottom_left = at(0.30f, 0.70f);
  const Color bottom_right = at(0.70f, 0.70f);
  const Color middle = at(0.50f, 0.50f);

  st::log::out("\n画布取样（ambient=1 / diffuse=0，所以亮度≈纹理原色）：\n");
  st::log::out("  左上 ({:3},{:3},{:3})\n", top_left.r, top_left.g, top_left.b);
  st::log::out("  右上 ({:3},{:3},{:3})\n", top_right.r, top_right.g, top_right.b);
  st::log::out("  左下 ({:3},{:3},{:3})\n", bottom_left.r, bottom_left.g, bottom_left.b);
  st::log::out("  右下 ({:3},{:3},{:3})\n", bottom_right.r, bottom_right.g, bottom_right.b);
  st::log::out("  中心 ({:3},{:3},{:3})\n", middle.r, middle.g, middle.b);

  // 判据：**画布左上 = 纹理左上（红）**。
  //
  // 这条判据曾经写反过（当时断言左上=蓝），结果把一件正确的事判成失败。
  // 弄清方向的关键是**看实测**：用 2×2 四色纹理（上排红绿、下排蓝白）渲一个正对相机的平面，
  // 第一个像素行的颜色直接告诉你“采样器把 v=0 当成图的上边还是下边”。
  // 实测（D3D11 + R32G32B32A32 纹理）：画布左上=红、左下=蓝 → v 方向**正确**，
  // 即 `v=0` 对应图上边，与图像行优先约定一致。
  //
  // 那么 `textured_plane` / `textured_box` 里的 UV 就不能再翻 v（那会双重翻转），
  // **例外是 +Y/-Y 面**：它们的 UV 是沿 X-Z 展开的，v 轴的语义在那里由几何决定，
  // 要单独按“看着正”定。
  const auto is_red = [](Color c) { return c.r > 150U && c.g < 120U && c.b < 120U; };
  const auto is_blue = [](Color c) { return c.b > 150U && c.r < 120U && c.g < 120U; };
  const auto is_green = [](Color c) { return c.g > 150U && c.r < 120U && c.b < 120U; };
  const auto is_white = [](Color c) { return c.r > 200U && c.g > 200U && c.b > 200U; };

  const auto name_of = [&](Color c) -> const char* {
    if (is_red(c)) return "红";
    if (is_green(c)) return "绿";
    if (is_blue(c)) return "蓝";
    if (is_white(c)) return "白";
    return "?";
  };

  const bool any_textured = is_red(top_left) || is_green(top_left) || is_blue(top_left) ||
                            is_white(top_left);
  st::log::out("\n判读：\n");
  st::log::out("  左上={} 右上={} 左下={} 右下={} 中心={}\n", name_of(top_left), name_of(top_right),
            name_of(bottom_left), name_of(bottom_right), name_of(middle));

  verdict.add("纹理确实被采样（不是全黑/全白）", any_textured,
              any_textured ? "四象限有颜色差异" : "四象限全同色 → 没贴上");
  verdict.add("v 轴方向正确（画布左上=纹理左上=红）", is_red(top_left),
              is_red(top_left) ? "红" : "不是红（v 方向反了）");
  verdict.add("右下为白（纹理右下）", is_white(bottom_right), is_white(bottom_right) ? "白" : "不是白");
  // 中心必须是四色混合（说明用的是线性采样，而不是点采样到某一角）
  const bool blended = !is_red(middle) && !is_green(middle) && !is_blue(middle) &&
                       !is_white(middle);
  verdict.add("中心为四色混合（双线性采样生效）", blended,
              blended ? "混合色" : "命中了某个纯色 → 可能是点采样");

  // 存图供人眼确认
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(kWidth);
  image.height = static_cast<std::uint32_t>(kHeight);
  image.rgba.resize(canvas.pixels().size() * 4U);
  for (std::size_t i = 0; i < canvas.pixels().size(); ++i) {
    const std::uint32_t pixel = canvas.pixels()[i];
    image.rgba[i * 4U + 0U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
    image.rgba[i * 4U + 1U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
    image.rgba[i * 4U + 2U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
    image.rgba[i * 4U + 3U] = static_cast<std::uint8_t>(pixel & 0xFFU);
  }
  (void)st::codec::png_write_file("build/test-artifacts/scene3d-texture.png", image);
  st::log::out("\n产物：build/test-artifacts/scene3d-texture.png\n");

  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
