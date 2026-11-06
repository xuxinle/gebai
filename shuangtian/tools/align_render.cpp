// 霜天侧的对标驱动：同一份 three.js 场景，走 scene_spec_from_three + render_scene，
// 并**显式覆盖** tone_map / background，好与浏览器的同一组参数严格对齐。
//
// 用法: align_render <scene.js> <out.png> [W] [H] [toneMapNum] [bgR] [bgG] [bgB]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

auto save_png(const st::raster::Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 3) {
    std::printf("用法: align_render <scene.js> <out.png> [W] [H] [toneNum] [bgR] [bgG] [bgB]\n");
    return 2;
  }
  const std::string scene_path = argv[1];
  const std::string out_path = argv[2];
  const int width = argc > 3 ? std::atoi(argv[3]) : 400;
  const int height = argc > 4 ? std::atoi(argv[4]) : 400;
  const int tone_num = argc > 5 ? std::atoi(argv[5]) : -1;
  const int bg_r = argc > 6 ? std::atoi(argv[6]) : 0;
  const int bg_g = argc > 7 ? std::atoi(argv[7]) : 0;
  const int bg_b = argc > 8 ? std::atoi(argv[8]) : 0;
  // 可选：光照覆盖（用于消融实验，把差异归因到具体某一项）
  //   9=ambient  10=diffuse  11=rim  12=env_strength  （-1 = 不覆盖）
  const float ov_ambient = argc > 9 ? float(std::atof(argv[9])) : -1.0f;
  const float ov_diffuse = argc > 10 ? float(std::atof(argv[10])) : -1.0f;
  const float ov_rim = argc > 11 ? float(std::atof(argv[11])) : -1.0f;
  const float ov_env = argc > 12 ? float(std::atof(argv[12])) : -1.0f;

  const auto script = read_file(scene_path);
  if (script.empty()) {
    std::printf("读不到场景：%s\n", scene_path.c_str());
    return 1;
  }
  auto spec = st::raster::parse_scene_spec_text(script);
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 1;
  }

  if (tone_num >= 0) {
    // 编号**必须与 `ToneMap` 枚举一致**（否则“以为在测 A、实际测的是 B”）：
    // 0=None 1=Reinhard 2=Aces 3=Filmic 4=Agx 5=AcesThree
    switch (tone_num) {
      case 0: spec->lighting.tone_map = st::raster::ToneMap::None; break;
      case 1: spec->lighting.tone_map = st::raster::ToneMap::Reinhard; break;
      case 2: spec->lighting.tone_map = st::raster::ToneMap::Aces; break;
      case 3: spec->lighting.tone_map = st::raster::ToneMap::Filmic; break;
      case 4: spec->lighting.tone_map = st::raster::ToneMap::Agx; break;
      case 5: spec->lighting.tone_map = st::raster::ToneMap::AcesThree; break;
      default: break;
    }
  }

  if (ov_ambient >= 0.0f) spec->lighting.ambient = ov_ambient;
  if (ov_diffuse >= 0.0f) spec->lighting.diffuse = ov_diffuse;
  // `ov_rim` 保留参数位（历史的消融脚本还在传），但内核已无边缘光——忽略。
  (void)ov_rim;
  if (ov_env >= 0.0f) spec->lighting.ibl_strength = ov_env;

  st::raster::Canvas canvas{width, height, 1.0f};
  const auto background = st::math::Color::rgba(static_cast<std::uint8_t>(bg_r),
                                                static_cast<std::uint8_t>(bg_g),
                                                static_cast<std::uint8_t>(bg_b), 255);
  const auto status = st::raster::render_scene(*spec, canvas, background);
  if (!status) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return 1;
  }
  if (!save_png(canvas, out_path)) {
    std::printf("写图失败：%s\n", out_path.c_str());
    return 1;
  }
  const auto pixels = canvas.pixels();
  // 真中心像素（不是 pixels.size()/2 —— 那是左边缘 x=0，会让人把背景当成物体）
  const std::size_t center_index =
      static_cast<std::size_t>(height / 2) * static_cast<std::size_t>(width) +
      static_cast<std::size_t>(width / 2);
  const auto center = st::math::unpremultiply(pixels[center_index]);
  const auto corner = st::math::unpremultiply(pixels[0]);
  std::printf("已出图 %s  center=rgb(%d,%d,%d) corner=rgb(%d,%d,%d)\n", out_path.c_str(),
              int(center.r), int(center.g), int(center.b), int(corner.r), int(corner.g),
              int(corner.b));
  std::printf("  tone_map=%d ambient=%.3f diffuse=%.3f ibl_strength=%.3f\n",
              int(spec->lighting.tone_map), double(spec->lighting.ambient),
              double(spec->lighting.diffuse), double(spec->lighting.ibl_strength));
  return 0;
}
