// Ring/Row 阵列渲染验证。
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
  <Camera eye="3.0, 2.2, 3.4" target="0, 0.3, 0" fov="42"/>
  <Lighting light_from="0.5, 0.8, 0.4" ambient="0.22" env_strength="0.9" env_sky="#87a7d4"/>
  <Shadow strength="0.4" ground_y="0"/>
  <Ring name="fins" pattern_count="12" pattern_radius="0.8" translate="0, 0.35, 0">
    <Extrude profile="-0.06,-0.2 0.06,-0.2 0.06,0.2 -0.06,0.2"
             size="0.55" color="#8c8c94" metallic="0.9" roughness="0.3" specular="1"/>
  </Ring>
  <Cylinder name="hub" size="0.5, 0.62" translate="0, 0.33, 0" color="#6b7280"
            metallic="0.9" roughness="0.3" specular="1"/>
  <Row name="steps" pattern_count="5" pattern_step="0.55, 0.11, 0" translate="-2.2, 0.06, 0.9">
    <Box size="0.5, 0.12, 0.6" color="#a06a3d" metallic="0.2" roughness="0.4" specular="0.6"/>
  </Row>
  <Plane name="floor" size="8, 8" ground="true" color="#2b2320"/>
</Scene>)XML";
  const auto spec = parse_scene_xml(xml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  // 打印各顶层对象的包围盒推断阵列是否铺开
  for (const auto& object : spec->objects) {
    st::log::out("  {} name={} pattern={} translate=({:.2f},{:.2f},{:.2f})\n",
                 object.shape, object.name, object.pattern.empty() ? "-" : object.pattern,
                 double(object.translate.x), double(object.translate.y),
                 double(object.translate.z));
  }
  Canvas canvas{760, 560, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  save_png(canvas, out_dir + "/pattern-smoke.png");
  st::log::out("已出图 {}/pattern-smoke.png\n", out_dir);
  return 0;
}
