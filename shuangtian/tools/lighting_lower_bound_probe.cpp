// 诊断：为什么本框架渲染**暗部永远是 0%**。
//
// ## 现象
//
// 与本框架 vs three.js 的分区统计：
//
// | | 本框架 | three.js |
// |---|---|---|
// | 暗部（luma<64） | **0.0%** | 9.8% |
// | 中间调 | **99.9%** | 89.3% |
//
// ——本框架的画面**没有任何暗区**。这不是"gamma 偏高"，而是
// **光照的下界被抬起来了**：每个像素至少拿到 ambient 那么多。
//
// ## 本探针要回答的问题
//
// 1. ambient / env 各自贡献多少？
// 2. 把 ambient 降到 0 会怎样？
// 3. metallic 材质的暗部到底能压到多低？
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

struct Hist {
  double mean{0.0};
  double dark{0.0};
  double mid{0.0};
  double bright{0.0};
  double max_luma{0.0};
  std::vector<double> bins;
};

auto analyze(const st::codec::PngImage& image) -> Hist {
  Hist out;
  out.bins.assign(64, 0.0);
  const std::size_t pixels = std::size_t(image.width) * image.height;
  if (pixels == 0) return out;
  double sum = 0.0;
  std::size_t dark = 0;
  std::size_t mid = 0;
  std::size_t bright = 0;
  for (std::size_t i = 0; i < pixels; ++i) {
    const double r = image.rgba[i * 4 + 0];
    const double g = image.rgba[i * 4 + 1];
    const double b = image.rgba[i * 4 + 2];
    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    sum += luma;
    if (luma > out.max_luma) out.max_luma = luma;
    if (luma < 64.0) ++dark;
    else if (luma <= 192.0) ++mid;
    else ++bright;
    const std::size_t bin = static_cast<std::size_t>(luma) / 4U;
    out.bins[bin < 64 ? bin : 63] += 1.0;
  }
  out.mean = sum / double(pixels);
  out.dark = double(dark) / double(pixels);
  out.mid = double(mid) / double(pixels);
  out.bright = double(bright) / double(pixels);
  for (auto& value : out.bins) value /= double(pixels);
  return out;
}

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

auto render(const st::raster::SceneSpec& spec) -> st::codec::PngImage {
  st::raster::Canvas canvas{1100, 800, 1.0f};
  const auto background = st::math::Color::rgba(18, 20, 24, 255);
  const auto status = st::raster::render_scene(spec, canvas, background);
  st::codec::PngImage out;
  if (!status) return out;
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
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "apps/jianmu/scenes/bpy_watch.py";
  const auto script = read_file(path);
  if (script.empty()) {
    std::printf("读不到 %s\n", path.c_str());
    return 1;
  }

  std::printf("%-30s %-8s %-8s %-8s %-8s %-8s\n", "配置", "均值", "暗部%", "中%", "亮%",
              "最亮");
  std::printf("%s\n", std::string(72, '-').c_str());

  struct Variant {
    const char* label;
    float ambient;
    float diffuse;
    float env;
  };
  const Variant variants[] = {
      {"现状 (amb .35 diff 1.65 env .6)", 0.35f, 1.65f, 0.6f},
      {"ambient=0", 0.0f, 1.65f, 0.6f},
      {"ambient=0 env=0", 0.0f, 1.65f, 0.0f},
      {"ambient=0 env=0 diff=1.0", 0.0f, 1.0f, 0.0f},
      {"ambient=0.02 env=0.3", 0.02f, 1.65f, 0.3f},
      {"ambient=0.05 env=0.5", 0.05f, 1.65f, 0.5f},
      {"diff=0.8 其余低", 0.02f, 0.8f, 0.3f},
  };

  for (const auto& v : variants) {
    auto spec = st::raster::parse_scene_spec_text(script);
    if (!spec.has_value()) {
      std::printf("解析失败：%s\n", spec.error().message.c_str());
      return 1;
    }
    spec->lighting.ambient = v.ambient;
    spec->lighting.diffuse = v.diffuse;
    spec->lighting.env_strength = v.env;
    const auto image = render(*spec);
    if (image.rgba.empty()) {
      std::printf("%-30s 渲染失败\n", v.label);
      continue;
    }
    const auto s = analyze(image);
    std::printf("%-30s %-8.1f %-8.1f %-8.1f %-8.1f %-8.0f\n", v.label, s.mean, s.dark * 100.0,
                s.mid * 100.0, s.bright * 100.0, s.max_luma);
  }

  std::printf("\n结论要看三件事：\n");
  std::printf("  1. 把 ambient 降到 0 后暗部是否出现（出现说明 ambient 是下界来源）\n");
  std::printf("  2. env 对暗部的影响（金属的反射方向变化能造出明暗）\n");
  std::printf("  3. 出现暗部后画面是否\"更接近 three\"（均值下移且层次分明）\n");
  return 0;
}
