// 精美场景 showcase：黄昏螺旋观景塔。
//
// ## 用上了几乎全部能力
//
// - **Part/Use**：楼层模板（板 + 开孔 + 栏杆环 + 扶手）定义一次，三层复用；
// - **布尔**：楼板中心开孔（螺旋楼梯穿过）、底座拱形入口；
// - **Staircase**：中央螺旋楼梯盘旋而上（3.5 圈 42 级）；
// - **Beam**：四角工字钢立柱 + 环梁；
// - **Ring 阵列**：每层 16 根栏杆柱、顶部灯笼辐条；
// - **Revolve**：穹顶车削；
// - **Ornament**：星形尖顶；
// - **Fastener**：节点螺栓（细节层）；
// - **约束**：斜撑用 connect 两点定线。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"

namespace {
using namespace st::raster;

constexpr int kW = 820;
constexpr int kH = 1120;

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

// 黄昏光：主光低角度暖橙（夕阳），补光冷蓝（天光），环境偏低。
constexpr const char* kTowerXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<Scene snap="0.05">
  <Camera eye="8.6, 4.6, 10.2" target="0, 3.3, 0" fov="32" near="0.05"/>
  <Lighting light_from="0.78, 0.42, 0.22" ambient="0.08" diffuse="0.42"
            rim="0.28" exposure="0.78" env_strength="0.6"
            env_sky="#c07a3e" env_ground="#1a1420" env_rough_falloff="0.6">
    <ExtraLight direction="-0.7, 0.18, 0.6" color="100, 140, 210" intensity="0.38"/>
    <ExtraLight direction="0.1, -0.2, -0.85" color="230, 145, 90" intensity="0.18"/>
  </Lighting>
  <Shadow strength="0.46" ground_y="0" color="#0a0810"/>

  <!-- ═══ 底座：八角台 + 布尔拱门入口 ═══ -->
  <Subtract name="plinth" translate="0, 0.16, 0" color="#3a3644" metallic="0.35"
            roughness="0.45" specular="0.7">
    <Prism sides="8" size="4.4, 0.32"/>
    <Box name="door_cut" size="1.5, 0.9, 1.6" translate="0, -0.05, 1.9"/>
  </Subtract>
  <Prism name="plinth_cap" sides="8" size="4.1, 0.1" translate="0, 0.41, 0"
         color="#4a4552" metallic="0.4" roughness="0.4" specular="0.8"/>

  <!-- ═══ 四角工字钢立柱（型材）═══ -->
  <Beam name="col_a" section="i" size="0.26, 5.3" thickness="0.035"
        translate="-1.32, 2.97, -1.32" rotation="0, 45, 0"
        color="#6e7a88" metallic="0.92" roughness="0.32" specular="1"/>
  <Beam name="col_b" section="i" size="0.26, 5.3" thickness="0.035"
        translate="1.32, 2.97, -1.32" rotation="0, -45, 0"
        color="#6e7a88" metallic="0.92" roughness="0.32" specular="1"/>
  <Beam name="col_c" section="i" size="0.26, 5.3" thickness="0.035"
        translate="1.32, 2.97, 1.32" rotation="0, 45, 0"
        color="#6e7a88" metallic="0.92" roughness="0.32" specular="1"/>
  <Beam name="col_d" section="i" size="0.26, 5.3" thickness="0.035"
        translate="-1.32, 2.97, 1.32" rotation="0, -45, 0"
        color="#6e7a88" metallic="0.92" roughness="0.32" specular="1"/>

  <!-- ═══ 斜撑：connect 两点定线（X 形交叉支撑）═══ -->
  <Box name="br_a1" size="0.05, 0.05, 0.05" translate="-1.32, 0.5, -1.32" visible="false"/>
  <Box name="br_a2" size="0.05, 0.05, 0.05" translate="-1.32, 2.6, -1.32" visible="false"/>
  <Box name="br_b1" size="0.05, 0.05, 0.05" translate="1.32, 2.6, -1.32" visible="false"/>
  <Tube name="brace_a" connect="center_of:br_a1 -> center_of:br_b1" size="0.045, auto"
        color="#5d6875" metallic="0.9" roughness="0.35" specular="1"/>
  <Tube name="brace_b" connect="center_of:br_a2 -> center_of:br_b1" size="0.04, auto"
        color="#5d6875" metallic="0.9" roughness="0.35" specular="1"/>

  <!-- ═══ 楼层模板：八角板 + 中心开孔 + 栏杆环 + 扶手 ═══ -->
  <Part name="deck">
    <Subtract name="slab" color="#7a6a58" metallic="0.25" roughness="0.52"
              specular="0.5">
      <Prism sides="8" size="3.7, 0.15"/>
      <Cylinder name="stair_well" size="1.72, 1.2"/>
    </Subtract>
    <!-- 栏杆柱环阵列（16 根） -->
    <Ring name="rail" pattern_count="16" pattern_radius="1.66">
      <Box size="0.055, 0.52, 0.055" translate="0, 0.33, 0"
           color="#8c6239" metallic="0.85" roughness="0.3" specular="1"/>
    </Ring>
    <!-- 顶部扶手环 -->
    <Torus name="handrail" size="3.34, 0.045" rotation="90, 0, 0"
           translate="0, 0.6, 0" color="#b08d57" metallic="0.95"
           roughness="0.18" specular="1"/>
  </Part>

  <Use part="deck" name="d1" translate="0, 1.42, 0"/>
  <Use part="deck" name="d2" translate="0, 2.92, 0"/>
  <Use part="deck" name="d3" translate="0, 4.42, 0"/>

  <!-- ═══ 中央螺旋楼梯（3.5 圈 42 级，从底座到顶层）═══ -->
  <Staircase name="spiral" size="1.52" turns="3.5" teeth="42" thickness="0.2"
             translate="0, 2.42, 0" color="#8a5a34" metallic="0.18"
             roughness="0.62" specular="0.3"/>

  <!-- ═══ 环梁（每层楼板下方一圈工字钢）═══ -->
  <Torus name="ring_beam1" size="3.42, 0.075" rotation="90, 0, 0"
         translate="0, 1.31, 0" color="#6e7a88" metallic="0.9" roughness="0.34"
         specular="1"/>
  <Torus name="ring_beam2" size="3.42, 0.075" rotation="90, 0, 0"
         translate="0, 2.81, 0" color="#6e7a88" metallic="0.9" roughness="0.34"
         specular="1"/>
  <Torus name="ring_beam3" size="3.42, 0.075" rotation="90, 0, 0"
         translate="0, 4.31, 0" color="#6e7a88" metallic="0.9" roughness="0.34"
         specular="1"/>

  <!-- ═══ 屋顶：车削穹顶 ═══ -->
  <Revolve name="dome" segments="48" translate="0, 4.58, 0"
           profile="1.72,0; 1.66,0.16; 1.5,0.42; 1.16,0.72; 0.68,0.94; 0.24,1.06; 0,1.1"
           color="#8c6239" metallic="0.88" roughness="0.28" specular="1"/>
  <!-- 穹顶肋条（径向阵列） -->
  <Ring name="dome_ribs" pattern_count="8" pattern_radius="0.0"
        translate="0, 4.62, 0">
    <Box size="0.045, 1.0, 0.045" translate="1.35, 0.5, 0"
         color="#b08d57" metallic="0.95" roughness="0.2" specular="1"/>
  </Ring>

  <!-- ═══ 塔顶：灯笼 + 星形尖顶 ═══ -->
  <Cylinder name="lantern_base" size="0.42, 0.12" translate="0, 5.74, 0"
            color="#b08d57" metallic="0.95" roughness="0.18" specular="1"/>
  <Sphere name="lantern" size="0.5" translate="0, 5.98, 0" color="#fff0c8"
          emission="2.4" emissive_color="#ffd68a" emission_unlit="true"/>
  <Ornament name="finial" section="star" sides="6" size="0.26, 0.5" thickness="0.12"
            translate="0, 6.3, 0" color="#e0b464" metallic="1" roughness="0.12"
            specular="1"/>

  <!-- ═══ 地面（有限尺寸：留出黄昏天空背景）═══ -->
  <Plane name="ground" size="11, 11" ground="true" color="#2a2228" roughness="0.9"
         specular="0.15"/>
</Scene>)XML";

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const st::math::Color background{14, 12, 20, 255};

  const auto spec = parse_scene_xml(kTowerXml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  int nodes = 0;
  {
    struct Walk {
      static void run(const std::vector<SceneObjectSpec>& objects, int& count) {
        for (const auto& object : objects) {
          ++count;
          run(object.children, count);
        }
      }
    };
    Walk::run(spec->objects, nodes);
  }
  st::log::out("黄昏观景塔：{} 顶层对象，树共 {} 节点\n", spec->objects.size(), nodes);

  Canvas canvas{kW, kH, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  const std::string path = out_dir + "/spiral-tower.png";
  if (!save_png(canvas, path)) {
    st::log::out("写图失败\n");
    return 1;
  }
  st::log::out("已出图 {}\n", path);
  return 0;
}
