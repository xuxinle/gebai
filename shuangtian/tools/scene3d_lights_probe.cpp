// 多光源对照：同一球体，逐步加光，各出一张 PNG + 关键统计。
//
// ## 这份探针要回答什么
//
// "多光源接上了"不能靠"看起来亮了一些"。判据得是数值，且要能分辨：
//   - 补光（从暗侧打）应当**抬高暗面**，而亮面基本不变；
//   - 暖色补光应当**改变色相**（R 通道涨幅 > B 通道）；
//   - 关闭所有附加光源时应当与只有主光**逐像素相同**（渐进采用底线）。
#include <cmath>
#include <string>

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

struct Regions {
  double bright{0.0};   ///< 亮面（球体右上）平均亮度
  double dark{0.0};     ///< 暗面（球体左下）平均亮度
  double mean_r{0.0};
  double mean_b{0.0};
};

auto sample_region(const Canvas& canvas, int cx, int cy, int radius, double& r_out,
                   double& b_out) -> double {
  double sum = 0.0;
  double sr = 0.0;
  double sb = 0.0;
  int n = 0;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = cx + dx;
      const int y = cy + dy;
      if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
      const Color c = canvas.pixel_at(x, y);
      sum += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      sr += c.r;
      sb += c.b;
      ++n;
    }
  }
  if (n == 0) return 0.0;
  r_out = sr / n;
  b_out = sb / n;
  return sum / n;
}

auto measure(const Canvas& canvas) -> Regions {
  Regions out{};
  double dummy_r = 0.0;
  double dummy_b = 0.0;
  // 亮面：球体右上（主光来自 +x+y+z）
  out.bright = sample_region(canvas, 265, 150, 22, dummy_r, dummy_b);
  // 暗面：球体左下
  out.dark = sample_region(canvas, 155, 285, 22, out.mean_r, out.mean_b);
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const Mesh sphere = Mesh::colored_sphere(0.95f, 40, Color{150, 150, 155, 255});

  struct Case {
    const char* name;
    int light_count;
    Vec3 dirs[3];
    Vec3 colors[3];
    float intensities[3];
  };
  const Case cases[] = {
      {"key-only", 0, {}, {}, {}},
      {"key+fill", 1,
       {{-0.6f, -0.2f, 0.5f}, {}, {}},
       {{1.0f, 1.0f, 1.0f}, {}, {}},
       {0.55f, 0.0f, 0.0f}},
      {"key+fill+rim", 2,
       {{-0.6f, -0.2f, 0.5f}, {-0.15f, 0.75f, -0.35f}, {}},
       {{1.0f, 1.0f, 1.0f}, {0.75f, 0.85f, 1.0f}, {}},
       {0.55f, 0.5f, 0.0f}},
      {"warm-fill", 1,
       {{-0.7f, 0.1f, 0.4f}, {}, {}},
       {{1.0f, 0.72f, 0.42f}, {}, {}},
       {0.85f, 0.0f, 0.0f}},
  };

  st::print("{:<16}{:>9}{:>9}{:>9}{:>9}{:>9}\n", "布光", "亮面", "暗面", "暗/亮", "R", "B");
  for (const Case& c : cases) {
    auto scene = Scene3D::create(kW, kH);
    Canvas canvas{kW, kH, 1.0f};
    const Color bg{18, 20, 28, 255};
    canvas.clear(bg);
    if (!scene.has_value()) return 1;
    Camera cam;
    cam.eye = Vec3{0.0f, 0.6f, 3.4f};
    cam.target = Vec3{0.0f, 0.0f, 0.0f};
    Lighting lit{};
    for (int i = 0; i < c.light_count; ++i) {
      lit.extra_lights[static_cast<std::size_t>(i)].direction = c.dirs[i];
      lit.extra_lights[static_cast<std::size_t>(i)].color = c.colors[i];
      lit.extra_lights[static_cast<std::size_t>(i)].intensity = c.intensities[i];
      st::print("   [set] #{} dir=({:.2f},{:.2f},{:.2f}) int={:.2f}\n", i, c.dirs[i].x,
                c.dirs[i].y, c.dirs[i].z, c.intensities[i]);
    }
    (*scene)->begin_frame(bg);
    (*scene)->set_camera(cam);
    (*scene)->set_lighting(lit);
    (*scene)->draw_mesh(sphere, Mat4::identity());
    (*scene)->end_frame(canvas,
                        st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                       static_cast<float>(kH)});
    (*scene)->flush();
    const Regions r = measure(canvas);
    const double ratio = r.bright > 1e-6 ? r.dark / r.bright : 0.0;
    st::print("{:<16}{:>9.1f}{:>9.1f}{:>9.3f}{:>9.1f}{:>9.1f}\n", c.name, r.bright, r.dark,
              ratio, r.mean_r, r.mean_b);
    const std::string path = out_dir + "/lights-" + c.name + ".png";
    st::print("   -> {}\n", save_png(canvas, path) ? path : std::string("(写图失败)"));
  }
  return 0;
}
