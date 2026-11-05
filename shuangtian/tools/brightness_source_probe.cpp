// 确认：本框架画面亮度到底由哪一项主导。
//
// ## 为什么要这一步
//
// 标定 `diffuse` 时发现它扫 5 倍范围（0.4→2.0），均值只从 89.2 变到 91.9。
// 而参照（真 Blender）是 76.2。也就是说**无论怎么调漫反射都到不了 76**。
//
// 那亮度必然来自别处。候选：`ambient`（环境光下限）与 `env_strength`（IBL）。
//
// ## 判据
//
// 逐项归零，看均值掉多少。掉得最多的那项就是主导。
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

struct Summary {
  double mean{0.0};
  double dark{0.0};
  double max_luma{0.0};
};

auto analyze(const st::codec::PngImage& image) -> Summary {
  Summary out;
  const std::size_t pixels = std::size_t(image.width) * image.height;
  if (pixels == 0) return out;
  double sum = 0.0;
  std::size_t dark = 0;
  for (std::size_t i = 0; i < pixels; ++i) {
    const double luma = 0.2126 * image.rgba[i * 4 + 0] + 0.7152 * image.rgba[i * 4 + 1] +
                        0.0722 * image.rgba[i * 4 + 2];
    sum += luma;
    if (luma < 64.0) ++dark;
    if (luma > out.max_luma) out.max_luma = luma;
  }
  out.mean = sum / double(pixels);
  out.dark = double(dark) / double(pixels);
  return out;
}

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

auto render(const st::raster::SceneSpec& spec, st::codec::PngImage& out) -> bool {
  st::raster::Canvas canvas{1100, 800, 1.0f};
  const st::math::Color background = st::math::Color::rgba(16, 17, 21, 255);
  const auto status = st::raster::render_scene(spec, canvas, background);
  if (!status) return false;
  out.width = static_cast<std::uint32_t>(canvas.physical_width());
  out.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  out.rgba.resize(std::size_t(out.width) * out.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const auto c = st::math::unpremultiply(pixels[i]);
    out.rgba[i * 4 + 0] = c.r;
    out.rgba[i * 4 + 1] = c.g;
    out.rgba[i * 4 + 2] = c.b;
    out.rgba[i * 4 + 3] = c.a;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "examples/jianmu/scenes/bpy_watch.py";
  const auto script = read_file(path);
  if (script.empty()) {
    std::printf("读不到 %s\n", path.c_str());
    return 1;
  }

  struct Variant {
    const char* label;
    float ambient;
    float env;
    float diffuse;
  };
  const Variant variants[] = {
      {"全默认（amb .35 env .6 diff 1.95）", 0.35f, 0.6f, 1.95f},
      {"ambient=0", 0.0f, 0.6f, 1.95f},
      {"env=0", 0.35f, 0.0f, 1.95f},
      {"ambient=0 env=0", 0.0f, 0.0f, 1.95f},
      {"全部归零", 0.0f, 0.0f, 0.0f},
      {"amb=0 env=.2 diff=.6", 0.0f, 0.2f, 0.6f},
      {"amb=.05 env=.3 diff=1.0", 0.05f, 0.3f, 1.0f},
      {"amb=.1 env=.4 diff=1.2", 0.1f, 0.4f, 1.2f},
  };

  std::printf("%-32s %-8s %-8s %-8s\n", "配置", "均值", "暗部%", "最亮");
  std::printf("%s\n", std::string(62, '-').c_str());

  double base_mean = -1.0;
  for (const auto& v : variants) {
    auto spec = st::raster::parse_scene_spec_text(script);
    if (!spec.has_value()) {
      std::printf("解析失败\n");
      return 1;
    }
    spec->lighting.ambient = v.ambient;
    spec->lighting.env_strength = v.env;
    spec->lighting.diffuse = v.diffuse;
    st::codec::PngImage image;
    if (!render(*spec, image)) {
      std::printf("%-32s 渲染失败\n", v.label);
      continue;
    }
    const auto s = analyze(image);
    if (base_mean < 0.0) base_mean = s.mean;
    std::printf("%-32s %-8.1f %-8.1f %-8.0f  (Δ%+.1f)\n", v.label, s.mean, s.dark * 100.0,
                s.max_luma, s.mean - base_mean);
  }

  std::printf("\n解读：\n");
  std::printf("  Δ 最大的那一行就是亮度主导项。\n");
  std::printf("  若 amb 与 env 都归零后均值仍远高于 40，说明还有别的常数项在抬亮。\n");
  return 0;
}
