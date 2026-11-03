// 表达能力 showcase：工业轴流风扇。
//
// 三件套各出场一次（这是「之前根本做不出来」的形状）：
//   1. **环形散热孔**：扁盘 Subtract Ring(12 个孔) —— 布尔 + 阵列组合；
//   2. **异形叶片**：Extrude 凹轮廓（后掠翼型）× Ring(9 片) 且每片带自身扭转；
//   3. **导流罩**：Revolve 车削（外筒 + 内收缩段），并 Subtract 出内腔。
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
constexpr int kW = 960;
constexpr int kH = 700;

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

// 叶片轮廓（后掠翼型，XZ 平面）：前缘圆钝、后缘尖削、带后掠弯。
// 单位化后在 Extrude 里靠 profile 的绝对坐标定尺寸。
constexpr const char* kBladeProfile =
    "0.10,-0.55  0.30,-0.42  0.42,-0.10  0.46,0.22  0.40,0.48  "
    "0.28,0.58  0.22,0.50  0.20,0.20  0.14,-0.16  -0.02,-0.40  "
    "-0.16,-0.50 -0.06,-0.56";

constexpr const char* kFanXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<Scene snap="0.05">
  <Camera eye="2.4, 1.7, 2.9" target="0, 0.62, 0" fov="42" near="0.05"/>
  <Lighting light_from="0.55, 0.72, 0.42" ambient="0.20" diffuse="0.62"
            exposure="1.18" env_strength="1.0" env_sky="#8fb6de"
            env_ground="#3b3630" env_rough_falloff="0.5">
    <ExtraLight direction="-0.6, 0.25, 0.5" color="255, 220, 180" intensity="0.5"/>
    <ExtraLight direction="0.1, 0.1, -0.9" color="140, 185, 255" intensity="0.42"/>
  </Lighting>
  <Shadow strength="0.38" ground_y="0" color="#0b0a0e"/>

  <!-- ═══ 1. 机座：扁盘 + 环形散热孔阵列（布尔 × 阵列）═══ -->
  <Subtract name="base" translate="0, 0.06, 0" color="#4a4f57" metallic="0.85"
            roughness="0.32" specular="1">
    <Cylinder size="2.5, 0.12"/>
    <!-- 12 个散热孔，环半径 0.92 -->
    <Ring name="base_vents" pattern_count="12" pattern_radius="0.92">
      <Cylinder size="0.17, 0.4"/>
    </Ring>
    <!-- 中央电机孔 -->
    <Cylinder size="0.62, 0.4"/>
  </Subtract>

  <!-- ═══ 2. 导流罩：车削外筒 + 布尔挖内腔 ═══ -->
  <Subtract name="shroud" translate="0, 0.12, 0" color="#7e8894" metallic="0.9"
            roughness="0.28" specular="1">
    <Revolve name="shroud_shell" segments="48"
             profile="1.02,0; 1.04,0.10; 1.02,0.55; 0.98,0.92; 0.94,1.02"/>
    <!-- 内腔（略小的车削体，车到薄壁） -->
    <Revolve name="shroud_cavity" segments="48" translate="0, -0.02, 0"
             profile="0.90,0; 0.92,0.12; 0.90,0.55; 0.86,0.92"/>
  </Subtract>

  <!-- ═══ 3. 叶轮：9 片后掠叶片（挤出 × 环形阵列）+ 轮毂 ═══ -->
  <Ring name="rotor" pattern_count="9" pattern_radius="0.30"
        translate="0, 0.62, 0">
    <Extrude profile="0.10,-0.55; 0.30,-0.42; 0.42,-0.10; 0.46,0.22; 0.40,0.48; 0.28,0.58; 0.22,0.50; 0.20,0.20; 0.14,-0.16; -0.02,-0.40; -0.16,-0.50; -0.06,-0.56"
             size="1, 0.06, 1" color="#b8c4d0" metallic="0.95" roughness="0.22"
             specular="1" rotation="14, 0, 0"/>
  </Ring>
  <Revolve name="hub" segments="40" translate="0, 0.62, 0" color="#8d99a8"
           metallic="0.95" roughness="0.25" specular="1"
           profile="0,-0.14; 0.30,-0.10; 0.34,0; 0.30,0.10; 0,0.12"/>

  <!-- ═══ 4. 顶部护网：两层同心环 + 径向辐条（阵列）═══ -->
  <Ring name="grille_rings" pattern_count="4" pattern_radius="0.0"
        translate="0, 1.10, 0">
    <Torus name="ring_band" size="0.5, 0.018" metallic="0.9" roughness="0.3"
           color="#9aa4b0" specular="1"/>
  </Ring>
  <Ring name="grille_spokes" pattern_count="16" pattern_radius="0.0"
        translate="0, 1.10, 0">
    <Box name="spoke" size="1.9, 0.016, 0.016" metallic="0.9" roughness="0.3"
         color="#8c96a2" specular="1"/>
  </Ring>

  <!-- ═══ 5. 支腿：三根斜撑（connect 两点定线；锚点用隐藏小方块）═══ -->
  <Box name="leg_anchor_a" size="0.04, 0.04, 0.04" translate="0, 0.02, 0"
       visible="false"/>
  <Box name="leg_anchor_b" size="0.04, 0.04, 0.04" translate="0.92, 0.42, 0"
       visible="false"/>
  <Tube name="leg_a" connect="center_of:leg_anchor_a -> center_of:leg_anchor_b"
        size="0.1, auto" color="#5a616b" metallic="0.9" roughness="0.3" specular="1"/>

  <Plane name="floor" size="9, 9" ground="true" color="#241f1c" roughness="0.85"/>

  <!-- ═══ 动画：叶轮旋转 + 微幅气动抖动 ═══ -->
  <Animation name="spin" duration="4">
    <Track node="rotor" kind="rotation">
      <Key t="0" value="0, 0, 0"/>
      <Key t="4" value="0, 480, 0"/>
    </Track>
    <!-- 同时转轮毂（有几何的实体）：验证动画链路本身通 -->
    <Track node="hub" kind="rotation">
      <Key t="0" value="0, 0, 0"/>
      <Key t="4" value="0, 480, 0"/>
    </Track>
  </Animation>
</Scene>)XML";

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const st::math::Color background{17, 18, 22, 255};

  const auto spec = parse_scene_xml(kFanXml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  int nodes = 0;
  {
    struct Walk {
      static void run(const std::vector<SceneObjectSpec>& objects, int& n) {
        for (const auto& object : objects) {
          ++n;
          run(object.children, n);
        }
      }
    };
    Walk::run(spec->objects, nodes);
  }
  st::log::out("轴流风扇：{} 顶层对象，树共 {} 节点\n", spec->objects.size(), nodes);
  if (spec->animation.has_value()) {
    st::log::out("动画 {} 轨道：", spec->animation->tracks.size());
    for (const auto& track : spec->animation->tracks) {
      st::log::out("{} kind={} keys={} ", track.node, static_cast<int>(track.kind),
                   track.keys.size());
      for (const auto& key : track.keys) {
        st::log::out("[t={} v=({:.2f},{:.2f},{:.2f})] ", double(key.time),
                     double(key.value.x), double(key.value.y), double(key.value.z));
      }
    }
    st::log::out("\n");
  }

  // 两帧选得开一点：叶轮 9 片（周期 40°），分度取 60°（60 mod 40 = 20°）
  //——与周期错开才能看清旋转；直接用 39° 会赶上“几乎重合”。
  for (const double t : {0.0, 0.5}) {
    Canvas canvas{kW, kH, 1.0f};
    const auto status = render_scene_at(*spec, static_cast<float>(t), canvas, background);
    if (!status) {
      st::log::out("渲染失败（t={}）：{}\n", t, status.error().message);
      return 1;
    }
    char name[72];
    std::snprintf(name, sizeof(name), "%s/fan-t%.1f.png", out_dir.c_str(), t);
    if (!save_png(canvas, name)) {
      st::log::out("写图失败：{}\n", name);
      return 1;
    }
    st::log::out("  t={}s 已出图 {}\n", t, name);
  }
  st::log::out("全部完成\n");
  return 0;
}
