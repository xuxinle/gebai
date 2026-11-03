// 新图元渲染验证：型钢四型 / 螺旋楼梯 / 星形十字柱 / 紧固件三件套。
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
  const st::math::Color background{19, 18, 23, 255};
  const char* xml = R"XML(<Scene>
  <Camera eye="4.2, 3.0, 5.2" target="0, 0.95, 0.2" fov="42"/>
  <Lighting light_from="0.5, 0.8, 0.45" ambient="0.24" env_strength="0.95" env_sky="#8fb6de"/>
  <Shadow strength="0.42" ground_y="0"/>

  <!-- 型钢四型（截面选型） -->
  <Beam name="ibeam" section="i" size="0.42, 2.2" thickness="0.05"
        translate="-2.2, 1.1, 0" color="#7e8894" metallic="0.85" roughness="0.35" specular="1"/>
  <Beam name="channel" section="channel" size="0.42, 2.2" thickness="0.05"
        translate="-1.5, 1.1, 0" color="#8c7a5e" metallic="0.85" roughness="0.35" specular="1"/>
  <Beam name="angle" section="angle" size="0.42, 2.2" thickness="0.05"
        translate="-0.85, 1.1, 0" color="#6b7280" metallic="0.85" roughness="0.35" specular="1"/>
  <Beam name="boxbeam" section="box" size="0.42, 2.2" thickness="0.05"
        translate="-0.2, 1.1, 0" color="#9aa4b2" metallic="0.85" roughness="0.35" specular="1"/>

  <!-- 螺旋楼梯（1.5 圈 18 级） -->
  <Staircase name="spiral" size="0.85" turns="1.5" teeth="18" thickness="0.16"
             translate="1.55, 1.05, 0" color="#a06a3d" metallic="0.2" roughness="0.45" specular="0.6"/>

  <!-- 星形 / 十字柱 -->
  <Ornament name="star" section="star" sides="5" size="0.5, 0.22" thickness="0.22"
            translate="2.95, 0.36, 0.85" color="#c9a227" metallic="0.95" roughness="0.18" specular="1"/>
  <Ornament name="cross" section="cross" size="0.46, 0.22" thickness="0.15"
            translate="2.95, 0.36, -0.25" color="#b08d57" metallic="0.9" roughness="0.22" specular="1"/>

  <!-- 紧固件三件套 -->
  <Fastener name="bolt" section="bolt" size="0.14, 0.1" thickness="0.05" turns="5"
            translate="0.55, 0.62, 1.5" rotation="90, 0, 0"
            color="#c8ccd2" metallic="1" roughness="0.2" specular="1"/>
  <Fastener name="nut" section="nut" size="0.14, 0.09" thickness="0.05"
            translate="1.05, 0.5, 1.5" color="#b8bcc2" metallic="1" roughness="0.22" specular="1"/>
  <Fastener name="washer" section="washer" size="0.17, 0.1" thickness="0.05"
            translate="0.55, 0.06, 1.5" color="#a8acb2" metallic="1" roughness="0.25" specular="1"/>

  <Plane name="floor" size="11, 11" ground="true" color="#241f1c"/>
</Scene>)XML";
  const auto spec = parse_scene_xml(xml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  Canvas canvas{900, 660, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  save_png(canvas, out_dir + "/primitives-new.png");
  st::log::out("已出图 {}/primitives-new.png\n", out_dir);
  return 0;
}
