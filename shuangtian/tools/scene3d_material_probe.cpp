// Principled 材质对照：同一球体，五种材质参数各出一张 PNG + 关键统计。
//
// ## 这份探针要回答什么
//
// 只有“金属与塑料看起来不一样”才算 GGX 真的接上了。而这件事**不能靠断言**——
// 判据得是数值：
//   - 金属（metallic=1）应当**失去漫反射**：整体更暗，但高光更集中；
//   - 粗糙度升高时，高光**变宽变矮**（峰值下降、覆盖面积上升）；
//   - 自发光（emission>0）应当整体抬高，且超过 1 时被色调映射滚降。
//
// 输出：五张 PNG（materials-*.png）+ 每张的均值/峰值/高光像素占比。
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"

namespace {
using namespace st::raster;
using st::math::Color;
using st::math::Vec3;
using st::math::Mat4;

constexpr int kW = 480;
constexpr int kH = 480;

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

struct Stats {
  double mean{0.0};
  double peak{0.0};
  double hot_ratio{0.0};   ///< 接近饱和（>0.95）的像素占比 —— 高光面积的代理
};

auto measure(const Canvas& canvas) -> Stats {
  const Color bg = canvas.pixel_at(0, 0);
  double sum = 0.0, peak = 0.0;
  int n = 0, hot = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d < 100) continue;
      const double luma = (0.299 * c.r + 0.587 * c.g + 0.114 * c.b) / 255.0;
      sum += luma;
      peak = std::max(peak, luma);
      if (luma > 0.95) ++hot;
      ++n;
    }
  }
  Stats s{};
  if (n > 0) {
    s.mean = sum / n;
    s.peak = peak;
    s.hot_ratio = static_cast<double>(hot) / n;
  }
  return s;
}

struct Case {
  const char* name;
  float metallic;
  float roughness;
  float specular;
  float emission;
};

auto render(const Mesh& mesh, const Case& c) -> Canvas {
  auto scene = Scene3D::create(kW, kH);
  Canvas canvas{kW, kH, 1.0f};
  const Color bg{18, 20, 28, 255};
  canvas.clear(bg);
  if (!scene.has_value()) return canvas;
  Camera cam;
  cam.eye = Vec3{0.0f, 0.8f, 3.6f};
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  Material m{};
  m.metallic = c.metallic;
  m.roughness = c.roughness;
  m.specular = c.specular;
  m.emission = c.emission;
  (*scene)->set_material(m);
  (*scene)->draw_mesh(mesh, Mat4::identity());
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  return canvas;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  // 中调灰球：太亮会饱和、太暗看不出高光
  const Mesh sphere = Mesh::colored_sphere(0.9f, 32, Color{140, 140, 150, 255});

  const Case cases[] = {
      {"matte-plastic", 0.0f, 0.95f, 0.0f, 0.0f},   // 基线：接近旧外观
      {"glossy-plastic", 0.0f, 0.15f, 1.0f, 0.0f},  // 光滑绝缘体：锐高光
      {"rough-metal", 1.0f, 0.55f, 1.0f, 0.0f},     // 粗糙金属：宽而暗
      {"polished-metal", 1.0f, 0.10f, 1.0f, 0.0f},  // 抛光金属：锐亮
      {"emissive", 0.0f, 0.95f, 0.0f, 1.5f},        // 自发光（>1，应被滚降）
  };

  st::print("{:<18}{:>9}{:>9}{:>11}{:>9}{:>8}\n", "材质", "均值", "峰值", "高光占比%", "金属",
            "粗糙");
  for (const Case& c : cases) {
    const Canvas canvas = render(sphere, c);
    const Stats s = measure(canvas);
    st::print("{:<18}{:>9.3f}{:>9.3f}{:>11.2f}{:>9.1f}{:>8.2f}\n", c.name, s.mean, s.peak,
              s.hot_ratio * 100.0, static_cast<double>(c.metallic),
              static_cast<double>(c.roughness));
    const std::string path = out_dir + "/materials-" + c.name + ".png";
    st::print("   → {}\n", save_png(canvas, path) ? path : std::string("(写图失败)"));
  }
  return 0;
}
