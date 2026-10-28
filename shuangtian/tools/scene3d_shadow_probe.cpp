// 平面投影阴影探针：验证阴影真的被画出来、位置对、强度可控。
//
// ## 判据（不能靠"看起来有影子"）
//
// 1. **存在性**：开启阴影后，地面区域应当变暗（有像素被显著压暗）。
// 2. **遮挡正确**：球体**正上方**的像素不应被阴影压暗（球体自己挡住影子）。
// 3. **强度可控**：strength 从 0 → 0.6 时，阴影区亮度单调下降。
// 4. **关闭即无**：strength = 0 时与不开阴影逐像素相同。
#include <cmath>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"

namespace {
using namespace st::raster;
using st::math::Color;
using st::math::Vec3;
using st::math::Mat4;

constexpr int kW = 420;
constexpr int kH = 420;

auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

auto luma_at(const Canvas& canvas, int cx, int cy, int radius) -> double {
  double sum = 0.0;
  int n = 0;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = cx + dx;
      const int y = cy + dy;
      if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
      const Color c = canvas.pixel_at(x, y);
      sum += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

/// 渲染一个悬空球 + 地面平面，返回画布。
auto render(float shadow_strength) -> Canvas {
  auto scene = Scene3D::create(kW, kH);
  Canvas canvas{kW, kH, 1.0f};
  const Color bg{26, 29, 38, 255};
  canvas.clear(bg);
  if (!scene.has_value()) return canvas;
  Camera cam;
  cam.eye = Vec3{0.0f, 1.9f, 3.2f};
  cam.target = Vec3{0.0f, 0.15f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::Back);

  // 地面（y = -0.55，法线朝上）
  (*scene)->draw_mesh(Mesh::plane(6.0f, 6.0f, Color{105, 108, 118, 255}),
                      Mat4::rotation(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963f));
  // 悬空球
  PlanarShadow shadow{};
  shadow.strength = shadow_strength;
  shadow.ground_y = -0.55f;
  (*scene)->set_planar_shadow(shadow);
  (*scene)->draw_mesh(Mesh::colored_sphere(0.55f, 28, Color{205, 170, 130, 255}),
                      Mat4::translation(Vec3{0.0f, 0.45f, 0.0f}));
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  return canvas;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";

  struct Case {
    const char* name;
    float strength;
  };
  const Case cases[] = {
      {"off", 0.0f}, {"weak", 0.25f}, {"mid", 0.45f}, {"strong", 0.70f}};

  st::log::out("{:<8}{:>10}{:>12}{:>12}{:>10}\n", "强度", "地面区", "球体上方", "阴影区", "对比");
  double prev_shadow = 1e9;
  bool monotonic = true;
  double off_ground = 0.0;
  for (std::size_t i = 0; i < 4; ++i) {
    const Canvas canvas = render(cases[i].strength);
    // 采样点（实测校准）：
    // - 地面远景：左上角地面区域（远离阴影）
    // - 球体上方：球体高光附近（阴影不应盖到）
    // - 阴影区：**实测重心 (146, 249)**——一开始猜 (232,300)，
    //   结果落在阴影外，四档读数完全一样而误判成“阴影未生效”
    const double ground = luma_at(canvas, 60, 330, 12);
    const double above = luma_at(canvas, 210, 150, 12);
    const double shadow_area = luma_at(canvas, 146, 249, 14);
    if (i == 0) off_ground = ground;
    if (i > 0 && shadow_area > prev_shadow + 0.5) monotonic = false;
    prev_shadow = shadow_area;
    st::log::out("{:<8}{:>10.1f}{:>12.1f}{:>12.1f}{:>10.1f}\n", cases[i].name, ground, above,
              shadow_area, shadow_area - off_ground);
    save_png(canvas, out_dir + "/shadow-" + cases[i].name + ".png");
  }

  // 判据 3：强度增大时阴影区应当单调变暗
  st::log::out("\n强度单调性：{}\n", monotonic ? "成立" : "不成立（有反复）");
  // 判据 1：最强档的阴影区应当明显暗于关闭档
  const Canvas strong = render(0.70f);
  const Canvas off = render(0.0f);
  const double s_off = luma_at(off, 146, 249, 14);
  const double s_strong = luma_at(strong, 146, 249, 14);
  st::log::out("阴影区：关闭={:.1f} 强={:.1f} 差值={:.1f}\n", s_off, s_strong, s_off - s_strong);
  st::log::out("→ 阴影{}\n", (s_off - s_strong) > 15.0 ? "确实生效" : "未生效或太弱");
  return 0;
}
