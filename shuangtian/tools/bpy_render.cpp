// 用**同一份 bpy 脚本**在霜天里渲一张图（与真 Blender 对比用）。
//
// 用法：bpy_render <scene.py> <out.png> [width] [height]
//
// ## 为什么需要这个工具（对比方法的正确性）
//
// 自己手写两份「同构」场景去比是不可靠的：每一次换算（sRGB→线性、
// 直径→半径、指向光源 vs 照射方向、FOV→lens）都是一个可能出错的环节。
// 错了之后两张图仍然「都有东西」，很难判断差异来自渲染器还是我的换算。
//
// 正确做法：**同一份 bpy 脚本，两边各跑一次**。场景定义只有一份。
//
// 本工具就是霜天这一侧的运行器；Blender 那一侧是 `tools/run_bpy_scene.py`。
//
// ## 出图设置必须与 Blender 侧一致
//
// | 项 | 值 | 为什么 |
// |---|---|---|
// | 色调映射 | AgX | 两边同一算子（Blender 侧 `view_settings`，这边 `tone_map`）|
// | 分辨率 | 1100×800 | 与其它对比图同口径 |
// | dither | 无 | 本框架本就无抖动 |
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};
  std::string out{};
  char buffer[8192];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.append(buffer, got);
  }
  std::fclose(file);
  return out;
}

auto save_png(const st::raster::Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    image.rgba[i * 4U + 0U] = static_cast<std::uint8_t>((pixels[i] >> 24U) & 0xFFU);
    image.rgba[i * 4U + 1U] = static_cast<std::uint8_t>((pixels[i] >> 16U) & 0xFFU);
    image.rgba[i * 4U + 2U] = static_cast<std::uint8_t>((pixels[i] >> 8U) & 0xFFU);
    image.rgba[i * 4U + 3U] = static_cast<std::uint8_t>(pixels[i] & 0xFFU);
  }
  return st::codec::png_write_file(path, image).has_value();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("用法：bpy_render <scene.py> <out.png> [width] [height]\n");
    return 2;
  }
  const std::string script_path = argv[1];
  const std::string out_path = argv[2];
  const int width = argc > 3 ? std::atoi(argv[3]) : 1100;
  const int height = argc > 4 ? std::atoi(argv[4]) : 800;

  const std::string script = read_file(script_path);
  if (script.empty()) {
    std::printf("读不到脚本：%s\n", script_path.c_str());
    return 1;
  }
  std::printf("脚本：%s（%zu 字节）\n", script_path.c_str(), script.size());

  auto spec = st::raster::parse_scene_spec_text(script);
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 1;
  }
  // 色调映射与 Blender 侧对齐（AgX；两边同一算子）。
  spec->lighting.tone_map = st::raster::ToneMap::Agx;

  st::raster::Canvas canvas{width, height, 1.0f};
  const auto status = st::raster::render_scene(
      *spec, canvas, st::math::Color::rgba(10, 10, 10, 255));
  if (!status.has_value()) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return 1;
  }
  if (!save_png(canvas, out_path)) {
    std::printf("写图失败：%s\n", out_path.c_str());
    return 1;
  }
  std::printf("## DONE %s（%dx%d）\n", out_path.c_str(), width, height);
  return 0;
}
