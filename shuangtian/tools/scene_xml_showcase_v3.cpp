// 场景 XML 表达力展示 v3：差分机——**装配大纲驱动 + Part/Use 部件化重写**。
//
// ## 与 v2 的差别（对治「物体组织乱」）
//
// v2：60+ 零件平铺，坐标手算，左右立柱各写一遍。
// v3：
//   1. 先写装配大纲（tmp/difference_v3_outline.md：整机→部件→零件）；
//   2. 每个部件一个 <Part>（局部自洽：内部 attach/connect 引用自己的零件）；
//   3. 左右立柱一个定义 + mirror 复用；
//   4. 全部装配关系约束化（零手算坐标）；
//   5. snap=0.05 网格对齐手写摆放。
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
using st::math::Color;

constexpr int kW = 960;
constexpr int kH = 640;

auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

// ── 差分机 v3：装配大纲的直接落码 ──
// 结构对齐 tmp/difference_v3_outline.md：base / column / gauge / exhaust
// 各一个 Part，立柱 mirror 复用，跨部件用锚点引用。
constexpr const char* kMachineXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<Scene snap="0.05">
  <Camera eye="3.4, 1.75, 3.6" target="0, 1.25, 0" fov="40" near="0.05"/>
  <Lighting light_from="0.62, 0.55, 0.35" ambient="0.16" diffuse="0.62" rim="0.3"
            exposure="1.15" env_strength="1.0" env_sky="#87a7d4"
            env_ground="#4a3b2e" env_rough_falloff="0.55">
    <ExtraLight direction="-0.75, 0.3, 0.45" color="255, 214, 170" intensity="0.55"/>
    <ExtraLight direction="-0.2, 0.15, -0.9" color="150, 190, 255" intensity="0.4"/>
  </Lighting>
  <Shadow strength="0.42" ground_y="0" color="#0c0a08"/>

  <!-- ══ 部件：底座 ══ -->
  <Part name="base">
    <Prism name="plinth" sides="8" size="2.9, 0.22" color="#3d3a45"
           roughness="0.32" metallic="0.05" specular="0.4" translate="0, 0.11, 0"/>
    <Prism name="top" sides="8" size="2.62, 0.1" color="#4a4752"
           roughness="0.28" metallic="0.08" specular="0.5" attach="top_of:plinth"/>
    <Torus name="trim" size="1.36, 0.05" color="#8c6239" metallic="0.9"
           roughness="0.25" specular="1" attach="top_of:top+0, 0.03, 0"
           rotation="90, 0, 0"/>
    <Cylinder name="foot_a" size="0.1, 0.12" color="#5b4a3a" translate="-1.3, 0.06, 0.95"/>
    <Cylinder name="foot_b" size="0.1, 0.12" color="#5b4a3a" translate="1.3, 0.06, 0.95"/>
    <Cylinder name="foot_c" size="0.1, 0.12" color="#5b4a3a" translate="-1.3, 0.06, -0.95"/>
    <Cylinder name="foot_d" size="0.1, 0.12" color="#5b4a3a" translate="1.3, 0.06, -0.95"/>
  </Part>

  <!-- ══ 部件：立柱（一次定义，左右镜像复用）══ -->
  <Part name="column">
    <Cylinder name="body" size="0.19, 1.85" color="#7a5c3e" metallic="0.85"
              roughness="0.35" specular="1" translate="0, 1.2, 0"/>
    <Cone name="cap" size="0.26, 0.16" color="#b08d57" metallic="0.95"
          roughness="0.2" specular="1" attach="top_of:body"/>
    <Torus name="ring_hi" size="0.15, 0.035" color="#b08d57" metallic="0.95"
           roughness="0.18" specular="1" attach="center_of:body+0, 0.5, 0"
           rotation="90, 0, 0"/>
    <Torus name="ring_lo" size="0.15, 0.035" color="#b08d57" metallic="0.95"
           roughness="0.18" specular="1" attach="center_of:body+0, -0.5, 0"
           rotation="90, 0, 0"/>
    <Sphere name="rivet_hi" size="0.05" color="#d4af6a" metallic="1" roughness="0.12"
            specular="1" attach="center_of:ring_hi+0, 0, 0.17"/>
    <Sphere name="rivet_lo" size="0.05" color="#d4af6a" metallic="1" roughness="0.12"
            specular="1" attach="center_of:ring_lo+0, 0, 0.17"/>
  </Part>

  <!-- ══ 部件：压力计（内部装配链自洽）══ -->
  <Part name="gauge">
    <Cylinder name="body" size="0.34, 0.1" color="#7a5c3e" metallic="0.9"
              roughness="0.3" specular="1" rotation="90, 0, 0"/>
    <Torus name="bezel" size="0.17, 0.03" color="#b08d57" metallic="0.95"
           roughness="0.15" specular="1" attach="center_of:body+0, 0, 0.08"
           rotation="90, 0, 0"/>
    <Disc name="face" size="0.3" color="#e8dcc0" roughness="0.4"
          attach="center_of:body+0, 0, 0.07" rotation="90, 0, 0"/>
    <Capsule name="needle" size="0.02, 0.24" color="#8c2f2f"
             attach="center_of:body+0, 0, 0.09" rotation="0, 0, 40"
             pivot_at="bottom"/>
    <Sphere name="pin" size="0.03" color="#2a2a2a"
            attach="center_of:body+0, 0, 0.1"/>
  </Part>

  <!-- ══ 部件：排气管（riser attach 座面，elbow attach riser 顶）══ -->
  <Part name="exhaust">
    <Cylinder name="riser" size="0.11, 0.9" color="#8c8c94" metallic="0.9"
              roughness="0.35" specular="1"/>
    <Torus name="elbow" size="0.12, 0.055" color="#8c8c94" metallic="0.9"
           roughness="0.35" specular="1" attach="top_of:riser+0, 0, 0.09"
           rotation="90, 0, 0"/>
    <Cylinder name="horiz" size="0.1, 0.3" color="#8c8c94" metallic="0.9"
              roughness="0.35" specular="1" attach="center_of:elbow+0, 0, 0.16"
              rotation="90, 0, 0"/>
    <Cone name="flare" size="0.18, 0.12" color="#a06a3d" metallic="0.9"
          roughness="0.3" specular="1" attach="center_of:horiz+0, 0, 0.18"
          rotation="-90, 0, 0"/>
    <Cylinder name="flange" size="0.17, 0.06" color="#6b7280" metallic="0.9"
              roughness="0.3" specular="1" attach="bottom_of:riser+0, 0.03, 0"/>
  </Part>

  <!-- ══ 装配：整机 = 部件实例 + 场景级零件 ══ -->
  <Use part="base" name="base"/>
  <Use part="column" name="colL" translate="-1.05, 0.22, 0"/>
  <Use part="column" name="colR" translate="1.05, 0.22, 0"/>
  <Box name="beam" size="2.5, 0.16, 0.24" color="#7a5c3e" metallic="0.85"
       roughness="0.35" specular="1"
       attach="top_of:colL_cap+1.05, 0.08, 0"/>
  <TorusKnot name="crest" size="0.42, 0.05" knot_p="2" knot_q="3" color="#c9a227"
             metallic="1" roughness="0.15" specular="1" attach="top_of:beam"/>

  <!-- 传动组：直接引用场景级（跨部件装配）-->
  <Group name="gear_train">
    <Cylinder name="main_shaft" size="0.07, 1.9" color="#9aa4b2" metallic="0.95"
              roughness="0.22" specular="1" attach="center_of:beam+0, -0.9, -0.25"
              rotation="0, 0, 90"/>
    <Gear name="gear_main" size="0.85, 0.09" teeth="12" tooth_height="0.09"
          color="#a06a3d" metallic="0.95" roughness="0.28" specular="1"
          attach="center_of:main_shaft+-0.5, 0, 0.37" rotation="90, 0, 0"/>
    <Gear name="gear_small" size="0.46, 0.08" teeth="8" tooth_height="0.1"
          color="#c9a227" metallic="1" roughness="0.18" specular="1"
          attach="center_of:main_shaft+0.32, 0.14, 0.37" rotation="90, 0, 0"/>
    <Prism name="bearing_L" sides="6" size="0.2, 0.14" color="#6b7280" metallic="0.9"
           roughness="0.3" specular="1" attach="left_of:main_shaft+0.05, 0, 0.37"
           rotation="90, 0, 0"/>
    <Prism name="bearing_R" sides="6" size="0.2, 0.14" color="#6b7280" metallic="0.9"
           roughness="0.3" specular="1" attach="right_of:main_shaft+-0.05, 0.14, 0.37"
           rotation="90, 0, 0"/>
  </Group>

  <!-- 能量核心 -->
  <Group name="reactor">
    <Cylinder name="core_base" size="0.6, 0.12" color="#5b4a3a" metallic="0.85"
              roughness="0.3" specular="1" attach="top_of:base_top+0, 0.45, 0.45"/>
    <Torus name="core_ring" size="0.44, 0.06" color="#c9a227" metallic="1"
           roughness="0.18" specular="1" attach="top_of:core_base+0, 0.08, 0"/>
    <Sphere name="glass_dome" size="0.72" color="#b8c8d8" opacity="0.22"
            roughness="0.04" specular="1" attach="top_of:core_base+0, 0.42, 0"/>
    <Helix name="energy_ring" size="0.34, 0.42" minor_radius="0.02" turns="3"
           color="#7fd4ff" emission="1.6" emissive_color="#66ccff"
           emission_unlit="true" attach="center_of:glass_dome+0, -0.14, 0"/>
    <Sphere name="energy_core" size="0.16" color="#e8f4ff" emission="2.2"
            emissive_color="#aee6ff" emission_unlit="true"
            attach="center_of:glass_dome"/>
    <Sphere name="lamp_1" size="0.05" color="#ffb347" emission="1.8"
            emissive_color="#ffb347" emission_unlit="true"
            attach="top_of:core_base+-0.52, 0.05, 0.18"/>
    <Sphere name="lamp_2" size="0.05" color="#4a9eff" emission="1.6"
            emissive_color="#4a9eff" emission_unlit="true"
            attach="top_of:core_base+0.52, 0.05, 0.18"/>
  </Group>

  <!-- 压力计：Use 到左柱外侧；管道 connect 表底→座面锚点 -->
  <Use part="gauge" name="gauge" attach="right_of:colL_body+0.15, 1.33, 0.28"/>
  <Tube name="gauge_pipe" connect="back_of:gauge_body -> top_of:base_top+0.75, 0.05, 0.35"
        size="0.09, auto" color="#9aa4b2" metallic="0.9" roughness="0.3" specular="1"/>

  <!-- 排气与弹簧 -->
  <Use part="exhaust" name="exh" attach="top_of:base_top+0.6, 0.2, -0.85"/>
  <Box name="spring_bracket" size="0.3, 0.08, 0.3" color="#6b7280" metallic="0.9"
       roughness="0.3" specular="1" attach="center_of:colR_body+-0.3, -0.45, -0.85"/>
  <Helix name="main_spring" size="0.67, 0.85" minor_radius="0.035" turns="7"
         color="#8f9aa8" metallic="0.9" roughness="0.28" specular="1"
         attach="top_of:spring_bracket"/>

  <!-- 桌面 -->
  <Plane name="desk" size="9, 9" ground="true" color="#2b2320" roughness="0.85"/>

  <!-- ══ 动画（按节点名跨部件）══ -->
  <Animation name="running" duration="6">
    <Track node="gear_main" kind="rotation">
      <Key t="0" value="0, 0, 0"/>
      <Key t="6" value="0, 360, 0"/>
    </Track>
    <Track node="gear_small" kind="rotation">
      <Key t="0" value="0, 0, 0"/>
      <Key t="6" value="0, -540, 0"/>
    </Track>
    <Track node="energy_ring" kind="rotation">
      <Key t="0" value="0, 0, 0"/>
      <Key t="6" value="0, 360, 0"/>
    </Track>
    <Track node="energy_core" kind="scale">
      <Key t="0" value="1, 1, 1" easing="ease-in-out"/>
      <Key t="1.5" value="1.18, 1.18, 1.18" easing="ease-in-out"/>
      <Key t="3" value="1, 1, 1" easing="ease-in-out"/>
      <Key t="4.5" value="1.18, 1.18, 1.18" easing="ease-in-out"/>
      <Key t="6" value="1, 1, 1"/>
    </Track>
    <Track node="gauge_needle" kind="rotation">
      <Key t="0" value="0, 0, 40" easing="ease-in-out"/>
      <Key t="1" value="0, 0, 62" easing="ease-in-out"/>
      <Key t="2.2" value="0, 0, 48" easing="ease-in-out"/>
      <Key t="3.5" value="0, 0, 66" easing="ease-in-out"/>
      <Key t="6" value="0, 0, 40"/>
    </Track>
  </Animation>
</Scene>)XML";

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const Color background{16, 15, 19, 255};

  const auto spec = parse_scene_xml(kMachineXml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  int nodes = 0;
  {
    struct Walk {
      static void run(const std::vector<SceneObjectSpec>& objects, int& n) {
        for (const auto& obj : objects) {
          ++n;
          run(obj.children, n);
        }
      }
    };
    Walk::run(spec->objects, nodes);
  }
  st::log::out("v3 解析成功：{} 顶层对象，树共 {} 节点，动画 {} 轨道\n",
           spec->objects.size(), nodes,
           spec->animation.has_value() ? spec->animation->tracks.size() : 0);

  for (const double t : {0.0, 1.8, 3.6, 5.4}) {
    Canvas canvas{kW, kH, 1.0f};
    const auto status = render_scene_at(*spec, static_cast<float>(t), canvas, background);
    if (!status) {
      st::log::out("渲染失败（t={}）：{}\n", t, status.error().message);
      return 1;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%s/difference-v3-t%.1f.png", out_dir.c_str(), t);
    if (!save_png(canvas, name)) {
      st::log::out("写图失败：{}\n", name);
      return 1;
    }
    st::log::out("  t={}s 已出图 {}\n", t, name);
  }
  st::log::out("全部完成\n");
  return 0;
}
