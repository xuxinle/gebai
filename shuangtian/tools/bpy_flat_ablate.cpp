// 受控平面消融：把 bpy 路径与真 Blender 的 12 级差**逐项归因**。
//
// ## 为什么需要单独一个工具
//
// `bpy_lighting_probe` 只打印抽出来的参数，`align_bench_probe` 只出图。
// 要回答「这 12 级里，多少来自 energy/1000、多少来自 ambient=0.35、
// 多少来自 ACES」，必须**改一个量、看一次像素**。
//
// ## 场景
//
// `tmp/bpy_parity/flat_persp_bpy.py`：0.5 灰平面 + 一枚 SUN energy=1.0
// 正对平面 + lens 50 / sensor 36 + z=20。
//
// ## 真值
//
// 真 Blender 5.2.3（Cycles / EEVEE 一致）：**111**
// = sRGB8(0.5/π × 1.0)
//
// 用法：bpy_flat_ablate <脚本.py> [输出前缀]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/bpy_runtime.hpp"
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法：bpy_flat_ablate <脚本.py> [输出前缀]\n");
    return 2;
  }
  const std::string script_path = argv[1];
  const std::string prefix = argc > 2 ? argv[2] : "tmp/bpy_parity/ablate";
  const int width = argc > 3 ? std::atoi(argv[3]) : 256;
  const int height = argc > 4 ? std::atoi(argv[4]) : 256;
  const int bg_gray = argc > 5 ? std::atoi(argv[5]) : 0;
  const auto script = read_file(script_path);
  if (script.empty()) {
    std::printf("读不到：%s\n", script_path.c_str());
    return 1;
  }
  const auto base = st::raster::scene_spec_from_bpy(script, script_path);
  if (!base.has_value()) {
    std::printf("抽取失败：%s\n", base.error().message.c_str());
    return 1;
  }

  std::printf("抽取：物体 %zu，diffuse=%.6f ambient=%.3f ibl=%.3f tone=%d\n",
              base->objects.size(), double(base->lighting.diffuse),
              double(base->lighting.ambient), double(base->lighting.ibl_strength),
              int(base->lighting.tone_map));
  std::printf("  light_from=(%.3f %.3f %.3f) ibl_sky=(%.3f %.3f %.3f) ibl_ground=(%.3f %.3f %.3f)\n",
              double(base->lighting.light_from.x), double(base->lighting.light_from.y),
              double(base->lighting.light_from.z), double(base->lighting.ibl_sky.x),
              double(base->lighting.ibl_sky.y), double(base->lighting.ibl_sky.z),
              double(base->lighting.ibl_ground.x), double(base->lighting.ibl_ground.y),
              double(base->lighting.ibl_ground.z));

  // 每一档：(名字, 修改 lighting 的 lambda)
  struct Variant {
    const char* name;
    void (*apply)(st::raster::SceneSpec&);
  };
  const Variant variants[] = {
      {"00_asis", [](st::raster::SceneSpec&) {}},
      // ① energy 标度：2026-11-06 已修（去掉多余的 /1000），所以这里改成
      // **反向验证**——除以 1000 应当复现修正前的旧值（66）。
      {"01_energy_old_div1000", [](st::raster::SceneSpec& s) { s.lighting.diffuse /= 1000.0f; }},
      {"02_no_ambient", [](st::raster::SceneSpec& s) { s.lighting.ambient = 0.0f; }},
      {"03_no_ibl", [](st::raster::SceneSpec& s) { s.lighting.ibl_strength = 0.0f; }},
      {"04_tone_standard", [](st::raster::SceneSpec& s) {
         s.lighting.tone_map = st::raster::ToneMap::None;
       }},
      {"05_all_three", [](st::raster::SceneSpec& s) {
         s.lighting.diffuse /= 1000.0f;
         s.lighting.ambient = 0.0f;
         s.lighting.ibl_strength = 0.0f;
         s.lighting.tone_map = st::raster::ToneMap::None;
       }},
  };

  for (const auto& v : variants) {
    auto scene = *base;
    v.apply(scene);
    st::raster::Canvas canvas{width, height, 1.0f};
    const auto background = st::math::Color::rgba(static_cast<std::uint8_t>(bg_gray),
                                                static_cast<std::uint8_t>(bg_gray),
                                                static_cast<std::uint8_t>(bg_gray), 255);
    const auto status = st::raster::render_scene(scene, canvas, background);
    if (!status) {
      std::printf("  %-18s 渲染失败：%s\n", v.name, status.error().message.c_str());
      continue;
    }
    const auto pixels = canvas.pixels();
    const auto c = st::math::unpremultiply(
        pixels[static_cast<std::size_t>(height / 2) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(width / 2)]);
    // 差异探测：左上角（背景）与中心两处
    const auto corner = st::math::unpremultiply(pixels[0]);
    std::printf("  %-18s center=rgb(%3d,%3d,%3d) corner=rgb(%3d,%3d,%3d)\n", v.name, int(c.r),
                int(c.g), int(c.b), int(corner.r), int(corner.g), int(corner.b));
    const std::string out = prefix + "_" + v.name + ".png";
    st::codec::PngImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const auto cc = st::math::unpremultiply(pixels[i]);
      image.rgba[i * 4 + 0] = cc.r;
      image.rgba[i * 4 + 1] = cc.g;
      image.rgba[i * 4 + 2] = cc.b;
      image.rgba[i * 4 + 3] = cc.a;
    }
    (void)st::codec::png_write_file(out, image);
  }
  std::printf("参考：真 Blender 5.2.3 = rgb(111,111,111)\n");
  return 0;
}
