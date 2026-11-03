// XML 布尔冒烟渲染：<Subtract> 标签打孔板。
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
  <Camera eye="2.8, 2.4, 2.8" target="0, 0.4, 0" fov="40"/>
  <Lighting light_from="0.5, 0.8, 0.4" ambient="0.22" env_strength="0.9" env_sky="#87a7d4"/>
  <Shadow strength="0.4" ground_y="0"/>
  <Subtract name="plate" translate="0, 0.5, 0" color="#b08d57" metallic="0.9"
            roughness="0.3" specular="1">
    <Box size="2, 0.3, 2"/>
    <Cylinder size="0.5, 1"/>
    <Cylinder size="0.2, 1" translate="0.7, 0, 0.7"/>
    <Cylinder size="0.2, 1" translate="-0.7, 0, 0.7"/>
    <Cylinder size="0.2, 1" translate="0.7, 0, -0.7"/>
    <Cylinder size="0.2, 1" translate="-0.7, 0, -0.7"/>
  </Subtract>
  <Cylinder name="plug" size="0.48, 0.5" attach="top_of:plate+0, 0.35, 0"
            color="#8c2f2f" metallic="0.3" roughness="0.4" specular="0.6"/>
  <Plane name="floor" size="6, 6" ground="true" color="#2b2320"/>
</Scene>)XML";
  const auto spec = parse_scene_xml(xml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  Canvas canvas{720, 540, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  save_png(canvas, out_dir + "/boolean-xml-smoke.png");
  st::log::out("已出图 {}/boolean-xml-smoke.png\n", out_dir);
  return 0;
}
