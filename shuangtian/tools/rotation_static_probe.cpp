// 最小验证：静态 rotation 是否影响渲染（判断 draw_mesh 是否用 model 矩阵）。
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

auto render_with_rotation(float degrees, const std::string& path) -> bool {
  char xml[1024];
  std::snprintf(xml, sizeof(xml), R"XML(<Scene>
  <Camera eye="0, 1.2, 3.6" target="0, 0.5, 0" fov="40"/>
  <Lighting light_from="0.5, 0.8, 0.5" ambient="0.25"/>
  <Shadow strength="0.4" ground_y="0"/>
  <Box name="blade" size="2.0, 0.2, 0.4" translate="0, 0.5, 0" rotation="0, %.1f, 0"
       color="#b8c4d0" metallic="0.9" roughness="0.25" specular="1"/>
  <Plane name="floor" size="8, 8" ground="true" color="#2b2320"/>
</Scene>)XML",
               double(degrees));
  const auto spec = parse_scene_xml(xml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return false;
  }
  Canvas canvas{480, 360, 1.0f};
  if (!render_scene(*spec, canvas, st::math::Color{20, 19, 24, 255})) {
    st::log::out("渲染失败\n");
    return false;
  }
  return save_png(canvas, path);
}
}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const bool ok0 = render_with_rotation(0.0f, out_dir + "/rot-static-0.png");
  const bool ok45 = render_with_rotation(45.0f, out_dir + "/rot-static-45.png");
  const bool ok90 = render_with_rotation(90.0f, out_dir + "/rot-static-90.png");
  st::log::out("出图：0°={} 45°={} 90°={}\n", ok0, ok45, ok90);
  return (ok0 && ok45 && ok90) ? 0 : 1;
}
