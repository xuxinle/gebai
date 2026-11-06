// 工件级标度验证：同一份 gears_portable.py，扫 ENERGY_SCALE，看哪个标度
// 才能让「本框架」与「真 Blender」落在同一曝光。
//
// ## 为什么必须扫而不是推
//
// `kSunEnergyToIrradiance` 的注释声称「energy 1 W/m² → intensity 1.0」，
// 而代码里曾多除了一层 `/1000`。到底哪个口径对，只有一个判据：
// **同一份工件、同一盏灯，两边的画面对不对得上**。
//
// ## 真值
//
// `tmp/bpy_parity/watch_truth_Standard_*` 由 `bpy_watch_truth.py` 跑
// `gears_portable.py` 得到（同一个 .py，一字不改）：
//
// | 引擎 | 主体（齿轮）像素 | 值 |
// |---|---|---|
// | EEVEE energy=2600 | 10249 px | **255（全白，过曝）** |
// | Cycles energy=2600 | ~同 | **255（全白，过曝）** |
//
// 所以 energy=2600 时**两边都过曝**——这一档不能用来判标度。
// 判标度要用「不过曝」的档：把 ENERGY_SCALE 调到 1/1000 之后，
// Blender 侧主体落在中灰，本框架也应当落在同一处。
//
// 用法：bpy_energy_scale <场景.py> <真值.png> <输出前缀> [W] [H]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

auto load_png(const std::string& path) -> st::codec::PngImage {
  auto decoded = st::codec::png_read_file(path);
  return decoded.has_value() ? *decoded : st::codec::PngImage{};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("用法：bpy_energy_scale <场景.py> <真值.png> <输出前缀> [W] [H]\n");
    return 2;
  }
  const std::string scene_path = argv[1];
  const std::string truth_path = argv[2];
  const std::string prefix = argv[3];
  const int width = argc > 4 ? std::atoi(argv[4]) : 800;
  const int height = argc > 5 ? std::atoi(argv[5]) : 600;

  const auto script = read_file(scene_path);
  auto base = st::raster::parse_scene_spec_text(script);
  if (!base.has_value()) {
    std::printf("解析失败：%s\n", base.error().message.c_str());
    return 1;
  }
  const auto truth = load_png(truth_path);
  if (truth.rgba.empty() || static_cast<int>(truth.width) != width ||
      static_cast<int>(truth.height) != height) {
    std::printf("真值读不到或尺寸不符\n");
    return 1;
  }

  // 真值的亮度统计（先判断它是不是过曝）
  double tsum = 0.0;
  std::size_t tn = 0;
  std::size_t tsat = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
    const std::uint8_t r = truth.rgba[i * 4 + 0];
    const std::uint8_t g = truth.rgba[i * 4 + 1];
    const std::uint8_t b = truth.rgba[i * 4 + 2];
    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    if (luma <= 64.5) continue;   // 排除背景（63）
    ++tn;
    tsum += luma;
    if (luma >= 254.5) ++tsat;
  }
  std::printf("真值：主体 %zu px，均值 %.1f，全白占比 %.1f%%\n", tn,
              tn ? tsum / double(tn) : 0.0, tn ? 100.0 * double(tsat) / double(tn) : 0.0);

  std::printf("%-14s %-10s %-10s %-10s\n", "energy_scale", "主体均值", "全白%", "非背景px");
  const double scales[] = {0.001, 0.002, 0.0026, 0.003, 0.004, 0.01, 0.1, 1.0};
  for (const double scale : scales) {
    auto spec = *base;
    spec.lighting.diffuse = static_cast<float>(double(spec.lighting.diffuse) * scale);
    for (auto& light : spec.lighting.extra_lights) {
      light.intensity = static_cast<float>(double(light.intensity) * scale);
    }
    spec.lighting.ibl_strength = 0.0f;
    spec.lighting.ambient = 0.0f;
    spec.lighting.tone_map = st::raster::ToneMap::None;

    st::raster::Canvas canvas{width, height, 1.0f};
    const auto status = st::raster::render_scene(
        spec, canvas, st::math::Color::rgba(13, 13, 13, 255));
    if (!status) {
      std::printf("%-14.5f 渲染失败\n", scale);
      continue;
    }
    const auto pixels = canvas.pixels();
    double sum = 0.0;
    std::size_t n = 0;
    std::size_t sat = 0;
    for (const auto& premul : pixels) {
      const auto c = st::math::unpremultiply(premul);
      const double luma = 0.2126 * double(c.r) + 0.7152 * double(c.g) + 0.0722 * double(c.b);
      if (luma <= 1.0) continue;
      ++n;
      sum += luma;
      if (luma >= 254.5) ++sat;
    }
    std::printf("%-14.5f %-10.1f %-10.1f %zu\n", scale, n ? sum / double(n) : 0.0,
                n ? 100.0 * double(sat) / double(n) : 0.0, n);
    if (scale == 1.0 || scale == 0.0026) {
      st::codec::PngImage image;
      image.width = static_cast<std::uint32_t>(width);
      image.height = static_cast<std::uint32_t>(height);
      image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
      for (std::size_t i = 0; i < pixels.size(); ++i) {
        const auto c = st::math::unpremultiply(pixels[i]);
        image.rgba[i * 4 + 0] = c.r;
        image.rgba[i * 4 + 1] = c.g;
        image.rgba[i * 4 + 2] = c.b;
        image.rgba[i * 4 + 3] = c.a;
      }
      char name[512];
      std::snprintf(name, sizeof(name), "%s_scale%.5f.png", prefix.c_str(), scale);
      (void)st::codec::png_write_file(name, image);
    }
  }
  return 0;
}
