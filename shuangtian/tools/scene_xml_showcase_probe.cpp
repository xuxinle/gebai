// 场景 XML 表达力展示：一台「蒸汽朋克差分机」——用尽可能多的图元/层级/
// 材质/发光/透明/环境反射特性，渲染一张复杂细节完善的图。
//
// 特性清单（这张图故意吃满）：
//   层级 5 层（machine → column_L/R → gearTrain → shaft → gear）；
//   图元 11 种（Box/Cylinder/Sphere/Torus/Helix/Gear/TorusKnot/Tube/Cone/
//              Capsule/Prism + Group/Object）；
//   材质 金属铜/黄铜/钢/橡胶 + 玻璃透明 + 自发光核心（emission_unlit）；
//   光照 三点布光（主光 + 暖色补光 + 冷色轮廓光）+ IBL 环境反射（金属的关键）；
//   动画 齿轮组转动 + 蒸汽压力计指针摆动 + 发光核心脉动。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/core/print.hpp"
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
  image.width = static_cast<uint32_t>(canvas.physical_width());
  image.height = static_cast<uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
  for (size_t i = 0; i < pixels.size(); ++i) {
    const Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

// ── 差分机：一台桌面尺寸的黄铜/紫铜机械计算装置 ──
//
// 结构（从下到上）：
//   大理石底座（prism 八角 + 木裙边）
//   ├─ 左右立柱（柱 + 柱头锥 + 黄铜饰环）
//   ├─ 齿轮传动组（主齿轮 + 从动小齿轮 + 传动轴 + 轴承）
//   ├─ 中央玻璃罩（透明球壳）里的发光核心（torus_knot + 脉动球）
//   ├─ 蒸汽压力计（表盘 torus + 表针 capsule + 玻璃面盘）
//   └─ 背部弹簧（helix）与管道弯头（tube/cone/cylinder 接头）
constexpr const char* kMachineXml = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<Scene asset_basis="assets">
  <Camera eye="3.4, 1.75, 3.6" target="0, 1.25, 0" fov="40" near="0.05"/>
  <Lighting light_from="0.62, 0.55, 0.35" ambient="0.16" diffuse="0.62" rim="0.3"
            exposure="1.15" env_strength="1.0" env_sky="#87a7d4"
            env_ground="#4a3b2e" env_rough_falloff="0.55">
    <ExtraLight direction="-0.75, 0.3, 0.45" color="255, 214, 170" intensity="0.55"/>
    <ExtraLight direction="-0.2, 0.15, -0.9" color="150, 190, 255" intensity="0.4"/>
    <ExtraLight direction="0.3, -0.4, -0.6" color="255, 150, 90" intensity="0.22"/>
  </Lighting>
  <Shadow strength="0.42" ground_y="0" color="#0c0a08"/>

  <!-- ═══ 底座：八角大理石台 + 紫铜裙边 ═══ -->
  <Group name="base">
    <Prism name="plinth" sides="8" size="2.9, 0.22" color="#3d3a45"
           roughness="0.32" metallic="0.05" specular="0.4" translate="0, 0.11, 0"/>
    <Prism name="plinth_top" sides="8" size="2.62, 0.1" color="#4a4752"
           roughness="0.28" metallic="0.08" specular="0.5" translate="0, 0.27, 0"/>
    <Torus name="trim_ring" size="1.36, 0.05" color="#8c6239" metallic="0.9"
           roughness="0.25" specular="1" translate="0, 0.33, 0" rotation="90, 0, 0"/>
    <Cylinder name="foot_FL" size="0.1, 0.12" color="#5b4a3a" translate="-1.28, 0.06, 0.92"/>
    <Cylinder name="foot_FR" size="0.1, 0.12" color="#5b4a3a" translate="1.28, 0.06, 0.92"/>
    <Cylinder name="foot_BL" size="0.1, 0.12" color="#5b4a3a" translate="-1.28, 0.06, -0.92"/>
    <Cylinder name="foot_RL" size="0.1, 0.12" color="#5b4a3a" translate="1.28, 0.06, -0.92"/>
  </Group>

  <!-- ═══ 主体框架：左右立柱 + 顶梁 ═══ -->
  <Group name="frame">
    <!-- 左立柱（4 层：柱身 → 柱头锥 → 饰环 → 铆钉） -->
    <Group name="column_L" translate="-1.05, 0, 0">
      <Cylinder name="col_body" size="0.19, 1.85" color="#7a5c3e" metallic="0.85"
                roughness="0.35" translate="0, 1.2, 0"/>
      <Cone name="col_cap" size="0.26, 0.16" color="#b08d57" metallic="0.95"
            roughness="0.2" translate="0, 2.2, 0"/>
      <Torus name="col_ring1" size="0.15, 0.035" color="#b08d57" metallic="0.95"
             roughness="0.18" translate="0, 1.7, 0" rotation="90, 0, 0"/>
      <Torus name="col_ring2" size="0.15, 0.035" color="#b08d57" metallic="0.95"
             roughness="0.18" translate="0, 0.7, 0" rotation="90, 0, 0"/>
      <Sphere name="rivet_L1" size="0.05" color="#d4af6a" metallic="1" roughness="0.12"
              translate="0, 1.72, 0.15"/>
      <Sphere name="rivet_L2" size="0.05" color="#d4af6a" metallic="1" roughness="0.12"
              translate="0, 0.72, 0.15"/>
    </Group>
    <!-- 右立柱（镜像，5 层，多一个检修窗） -->
    <Group name="column_R" translate="1.05, 0, 0">
      <Cylinder name="col_body_R" size="0.19, 1.85" color="#7a5c3e" metallic="0.85"
                roughness="0.35" specular="1" translate="0, 1.2, 0"/>
      <Cone name="col_cap_R" size="0.26, 0.16" color="#b08d57" metallic="0.95"
            roughness="0.2" specular="1" translate="0, 2.2, 0"/>
      <Torus name="col_ring1_R" size="0.15, 0.035" color="#b08d57" metallic="0.95"
             roughness="0.18" specular="1" translate="0, 1.7, 0" rotation="90, 0, 0"/>
      <Torus name="col_ring2_R" size="0.15, 0.035" color="#b08d57" metallic="0.95"
             roughness="0.18" specular="1" translate="0, 0.7, 0" rotation="90, 0, 0"/>
      <!-- 检修窗：黄铜框 + 微透玻璃 + 内部露出的弹簧 -->
      <Group name="service_window" translate="0, 1.2, 0.19">
        <Torus name="win_frame" size="0.13, 0.03" color="#b08d57" metallic="0.95"
               roughness="0.2" translate="0, 0, 0.02" rotation="90, 0, 0"/>
        <Disc name="win_glass" size="0.24" color="#c8d8e8" opacity="0.25"
              roughness="0.05" specular="1" translate="0, 0, 0.03" rotation="90, 0, 0"/>
        <Helix name="win_spring" size="0.144, 0.16" minor_radius="0.012" turns="6"
               color="#8f9aa8" metallic="0.9" roughness="0.3" specular="1"
               translate="0, 0, -0.01" rotation="90, 0, 0"/>
      </Group>
    </Group>
    <!-- 顶梁 + 装饰性纽结（黄铜绶带） -->
    <Box name="top_beam" size="2.5, 0.16, 0.24" color="#7a5c3e" metallic="0.85"
         roughness="0.35" specular="1" translate="0, 2.32, 0"/>
    <TorusKnot name="crest" size="0.42, 0.05" knot_p="2" knot_q="3" color="#c9a227"
               metallic="1" roughness="0.15" specular="1" translate="0, 2.62, 0"/>
  </Group>

  <!-- ═══ 齿轮传动组（机器的心脏，5 层）═══ -->
  <Group name="gear_train" translate="0, 1.05, -0.25">
    <!-- 传动轴 -->
    <Cylinder name="main_shaft" size="0.07, 1.9" color="#9aa4b2" metallic="0.95"
              roughness="0.22" specular="1" rotation="0, 0, 90"/>
    <!-- 主齿轮（紫铜，12 齿） -->
    <Gear name="gear_main" size="0.85, 0.09" teeth="12" tooth_height="0.09"
          color="#a06a3d" metallic="0.95" roughness="0.28" specular="1"
          translate="-0.52, 0, 0.12" rotation="90, 0, 0"/>
    <!-- 从动齿轮（黄铜，8 齿，更细） -->
    <Gear name="gear_small" size="0.46, 0.08" teeth="8" tooth_height="0.1"
          color="#c9a227" metallic="1" roughness="0.18" specular="1"
          translate="0.32, 0.14, 0.12" rotation="90, 0, 0"/>
    <!-- 轴承座（六角棱柱）+ 轴承盖 -->
    <Prism name="bearing_L" sides="6" size="0.2, 0.14" color="#6b7280" metallic="0.9"
           roughness="0.3" specular="1" translate="-0.88, 0, 0.12" rotation="90, 0, 0"/>
    <Prism name="bearing_R" sides="6" size="0.2, 0.14" color="#6b7280" metallic="0.9"
           roughness="0.3" specular="1" translate="0.72, 0.14, 0.12" rotation="90, 0, 0"/>
    <!-- 链条张紧轮 -->
    <!-- 张紧轮支架：斜撑杆用 connect 连到座顶（长度/朝向自动） -->
    <Group name="tensioner" translate="0.55, -0.42, 0.12">
      <Cylinder name="tension_wheel" size="0.2, 0.06" color="#8c8c94" metallic="0.9"
                roughness="0.3" specular="1" rotation="90, 0, 0"/>
      <Capsule name="tension_arm" size="0.06, 0.34" color="#7a5c3e" metallic="0.85"
               roughness="0.3" specular="1" translate="0.06, -0.12, 0" rotation="0, 0, -14"/>
      <Sphere name="tension_knob" size="0.09" color="#b08d57" metallic="0.95"
              roughness="0.2" specular="1" translate="0.13, -0.34, 0"/>
      <!-- 斜撑：从张紧轮底部到座顶预埋锚点（用透明 Group 承载） -->
    </Group>
    <Group name="strut_anchor_L" translate="-0.35, -0.7, 0.1" visible="false"/>
    <Tube name="tension_strut" connect="bottom_of:tensioner -> center_of:strut_anchor_L"
          size="0.05, auto" color="#6b7280" metallic="0.9" roughness="0.3" specular="1"/>
  </Group>

  <!-- ═══ 中央能量核心：玻璃罩 + 发光纽结 + 脉动内芯 ═══ -->
  <Group name="reactor" translate="0, 1.15, 0.45">
    <Cylinder name="core_base" size="0.6, 0.12" color="#5b4a3a" metallic="0.85"
              roughness="0.3" specular="1"/>
    <Torus name="core_ring" size="0.44, 0.06" color="#c9a227" metallic="1"
           roughness="0.18" specular="1" translate="0, 0.08, 0"/>
    <!-- 玻璃罩：微透明 + 高光滑 -->
    <Sphere name="glass_dome" size="0.72" color="#b8c8d8" opacity="0.22"
            roughness="0.04" specular="1" translate="0, 0.42, 0"/>
    <!-- 内芯：发光螺旋环绕脉动球（纯发光体，不受光照；不用 torus_knot 避免
         小半径下的自交退化——细管纽结在本渲染器里会现出扫掠自交的毛边） -->
    <Helix name="energy_ring" size="0.34, 0.42" minor_radius="0.02" turns="3"
           color="#7fd4ff" emission="1.6" emissive_color="#66ccff"
           emission_unlit="true" translate="0, 0.28, 0"/>
    <Sphere name="energy_core" size="0.16" color="#e8f4ff" emission="2.2"
            emissive_color="#aee6ff" emission_unlit="true" translate="0, 0.45, 0"/>
    <!-- 环绕的小指示灯（发条指示） -->
    <Sphere name="lamp_1" size="0.05" color="#ffb347" emission="1.8"
            emissive_color="#ffb347" emission_unlit="true"
            translate="-0.52, 0.12, 0.18"/>
    <Sphere name="lamp_2" size="0.05" color="#4a9eff" emission="1.6"
            emissive_color="#4a9eff" emission_unlit="true"
            translate="0.52, 0.12, 0.18"/>
    <Sphere name="lamp_3" size="0.05" color="#4a9eff" emission="1.6"
            emissive_color="#4a9eff" emission_unlit="true"
            translate="0, 0.12, 0.55"/>
  </Group>

  <!-- ═══ 蒸汽压力计（挂臂 attach 到左柱；管子 connect 到座顶锚点）═══ -->
  <Group name="gauge" attach="right_of:column_L+0.18, 1.55, 0.35">
    <Cylinder name="gauge_body" size="0.34, 0.1" color="#7a5c3e" metallic="0.9"
              roughness="0.3" specular="1" rotation="90, 0, 0"/>
    <Torus name="gauge_bezel" size="0.17, 0.03" color="#b08d57" metallic="0.95"
           roughness="0.15" specular="1" translate="0, 0, 0.08" rotation="90, 0, 0"/>
    <Disc name="gauge_face" size="0.3" color="#e8dcc0" roughness="0.4"
          translate="0, 0, 0.07" rotation="90, 0, 0"/>
    <Capsule name="gauge_needle" size="0.02, 0.24" color="#8c2f2f" translate="0, 0, 0.09"
             rotation="0, 0, 40" pivot_at="bottom"/>
    <Sphere name="gauge_pin" size="0.03" color="#2a2a2a" translate="0, 0, 0.1"/>
    <!-- 管子：connect 从表底到座顶锚点（长度/朝向全自动） -->
    <Group name="gauge_anchor" visible="false"/>
  </Group>
  <Tube name="gauge_pipe" connect="bottom_of:gauge -> top_of:plinth_top+0.75, 0.05, 0.42"
        size="0.09, auto" color="#9aa4b2" metallic="0.9" roughness="0.3" specular="1"/>

  <!-- ═══ 背部：主弹簧坐在托架上；排气管 connect 从座顶锚点立到顶梁 ═══ -->
  <Group name="rear" translate="0, 1.0, -0.85">
    <!-- 弹簧托架：连到右柱 -->
    <Box name="spring_bracket" size="0.3, 0.08, 0.3" color="#6b7280" metallic="0.9"
         roughness="0.3" specular="1" translate="-0.45, -0.46, 0"/>
    <Helix name="main_spring" size="0.67, 0.85" minor_radius="0.035" turns="7"
           color="#8f9aa8" metallic="0.9" roughness="0.28" specular="1"
           attach="top_of:spring_bracket" translate="-0.45, 0, 0"/>
    <!-- 排气管组：立管从底座顶面升起（attach 到座顶） -->
    <Cylinder name="pipe_riser" size="0.11, 0.9" color="#8c8c94" metallic="0.9"
              roughness="0.35" specular="1" attach="top_of:plinth_top+0.55, 0.05, -0.95"/>
    <Torus name="pipe_elbow" size="0.12, 0.055" color="#8c8c94" metallic="0.9"
           roughness="0.35" specular="1" attach="top_of:pipe_riser" translate="0, 0.48, 0.09"
           rotation="90, 0, 0"/>
    <Cylinder name="pipe_horiz" size="0.1, 0.3" color="#8c8c94" metallic="0.9"
                roughness="0.35" specular="1" connect="back_of:pipe_elbow -> front_of:crest"
                rotation="90, 0, 0"/>
    <Cone name="pipe_flare" size="0.18, 0.12" color="#a06a3d" metallic="0.9"
            roughness="0.3" specular="1" attach="right_of:pipe_horiz" rotation="-90, 0, 0"/>
    <!-- 底部法兰 -->
    <Cylinder name="pipe_flange" size="0.17, 0.06" color="#6b7280" metallic="0.9"
              roughness="0.3" specular="1" attach="top_of:plinth_top+0.55, 0.02, -0.95"/>
  </Group>

  <!-- ═══ 桌面（承接阴影）═══ -->
  <Plane name="desk" size="9, 9" ground="true" color="#2b2320" roughness="0.85"/>

  <!-- ═══ 动画：齿轮转动 + 指针摆动 + 核心脉动 ═══ -->
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
      <Key t="5" value="0, 0, 52" easing="ease-in-out"/>
      <Key t="6" value="0, 0, 40"/>
    </Track>
    <Track node="main_spring" kind="rotation">
      <Key t="0" value="0, 0, 0"/>
      <Key t="6" value="0, 30, 0"/>
    </Track>
  </Animation>
</Scene>)XML";

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const Color background{16, 15, 19, 255};

  // ── 解析 ──
  const auto spec = parse_scene_xml(kMachineXml);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }

  // 统计：树节点数（含子树）。
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
  st::log::out("解析成功：{} 个顶层对象，树共 {} 个节点，动画 {} 条轨道\n",
           spec->objects.size(), nodes,
           spec->animation.has_value() ? spec->animation->tracks.size() : 0);

  // ── 渲染四帧（动画时间推进，展示动画能力；render_scene_at 是带时间的入口）──
  for (const double t : {0.0, 1.8, 3.6, 5.4}) {
    Canvas canvas{kW, kH, 1.0f};
    const auto status = render_scene_at(*spec, static_cast<float>(t), canvas, background);
    if (!status) {
      st::log::out("渲染失败（t={}）：{}\n", t, status.error().message);
      return 1;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%s/difference-engine-t%.1f.png", out_dir.c_str(), t);
    if (!save_png(canvas, name)) {
      st::log::out("  写图失败：{}\n", name);
      return 1;
    }
    st::log::out("  t={}s 已出图 {}\n", t, name);
  }

  st::log::out("全部完成\n");
  return 0;
}
