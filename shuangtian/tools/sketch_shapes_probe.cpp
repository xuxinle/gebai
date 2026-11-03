// Extrude/Revolve 渲染验证：L 支架（凹轮廓）、花瓶（车削）、星形（凹多边形）。
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"

namespace {
using namespace st::raster;
auto save_png(const Canvas& canvas, const std::string& path) -> bool {
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
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const st::math::Color background{20, 19, 24, 255};
  const char* xml = R"XML(<Scene>
  <Camera eye="3.2, 2.2, 3.8" target="0, 0.6, 0.2" fov="42"/>
  <Lighting light_from="0.5, 0.8, 0.4" ambient="0.22" env_strength="0.9" env_sky="#87a7d4"/>
  <Shadow strength="0.4" ground_y="0"/>
  <Extrude name="bracket" profile="-0.5,-0.4 0.5,-0.4 0.5,-0.25 -0.35,-0.25 -0.35,0.4 -0.5,0.4"
           size="0.3" translate="-0.95, 0.5, 0" color="#5a8ca8" metallic="0.4"
           roughness="0.35" specular="0.9"/>
  <Revolve name="vase"
           profile="0.35,0 0.42,0.06 0.30,0.35 0.26,0.75 0.34,1.05 0.22,1.25 0.12,1.32"
           segments="40" translate="0.75, 0.1, -0.5" color="#a06a3d" metallic="0.15"
           roughness="0.35" specular="0.7"/>
  <Extrude name="star"
           profile="0,0.5 0.12,0.16 0.48,0.16 0.19,-0.05 0.3,-0.4 0,-0.2 -0.3,-0.4 -0.19,-0.05 -0.48,0.16 -0.12,0.16"
           size="0.15" translate="0.75, 0.62, 0.85" rotation="90, 0, 0" color="#c9a227"
           metallic="0.95" roughness="0.18" specular="1"/>
  <Plane name="floor" size="6, 6" ground="true" color="#2b2320"/>
</Scene>)XML";
  const auto spec = parse_scene_xml(xml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  Canvas canvas{760, 560, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  save_png(canvas, out_dir + "/sketch-smoke.png");
  st::log::out("已出图 {}/sketch-smoke.png\n", out_dir);
  return 0;
}
