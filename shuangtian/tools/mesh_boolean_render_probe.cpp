// 布尔渲染冒烟：带孔法兰 + 挖槽方块，肉眼验证布尔结果的渲染正确性。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"

namespace {
using namespace st::raster;
constexpr st::math::Color kWhite{255, 255, 255, 255};

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

  // 法兰：板 + 中心孔 + 4 螺栓孔（链式 Subtract）
  std::vector<Mesh> operands;
  operands.push_back(Mesh::cylinder(1.1f, 0.25f, 32, kWhite));
  operands.push_back(Mesh::cylinder(0.55f, 1.0f, 24, kWhite));  // 中心孔
  for (int i = 0; i < 6; ++i) {
    const float angle = 6.2831853f * static_cast<float>(i) / 6.0f;
    auto bolt = Mesh::cylinder(0.12f, 1.0f, 12, kWhite);
    operands.push_back(transform(
        bolt, st::math::Mat4::translation(
                  st::math::Vec3{std::cos(angle) * 0.82f, 0.0f,
                                 std::sin(angle) * 0.82f})));
  }
  const auto flange = boolean_chain(operands, BooleanOp::Subtract);
  if (!flange) {
    st::log::out("法兰布尔失败：{}\n", flange.error().message);
    return 1;
  }
  st::log::out("法兰：{} 三角形\n", flange->indices.size() / 3U);

  // 挖槽块：立方体 - 中央立方（凹槽）
  const auto slotted = boolean(Mesh::box(1.4f, 0.9f, 1.4f),
                               transform(Mesh::box(1.0f, 0.5f, 1.0f),
                                         st::math::Mat4::translation(
                                             st::math::Vec3{0.0f, 0.45f, 0.0f})),
                               BooleanOp::Subtract);
  if (!slotted) {
    st::log::out("挖槽布尔失败：{}\n", slotted.error().message);
    return 1;
  }

  // 组场景（obj: 形状走 base64？不——直接写临时 OBJ 文件引用）
  const std::string flange_obj = out_dir + "/boolean-flange.obj";
  const std::string slotted_obj = out_dir + "/boolean-slotted.obj";
  if (!write_obj_file(flange_obj, *flange, "flange") ||
      !write_obj_file(slotted_obj, *slotted, "slotted")) {
    st::log::out("OBJ 写出失败\n");
    return 1;
  }

  const char* xml = R"XML(<Scene>
  <Camera eye="2.6, 2.0, 3.0" target="0, 0.3, 0" fov="40"/>
  <Lighting light_from="0.5, 0.8, 0.4" ambient="0.2" env_strength="0.9" env_sky="#87a7d4"/>
  <Shadow strength="0.35" ground_y="0"/>
  <Object name="flange" shape="obj:__FLANGE__" translate="0, 0.9, 0"
          color="#b08d57" metallic="0.9" roughness="0.3" specular="1"/>
  <Object name="slotted" shape="obj:__SLOTTED__" translate="0, 0.45, -0.0"
          color="#5a8ca8" metallic="0.1" roughness="0.45" specular="0.6"/>
  <Plane name="floor" size="6, 6" ground="true" color="#2b2320"/>
</Scene>)XML";
  std::string scene_xml = xml;
  const auto replace = [&](const std::string& from, const std::string& to) {
    const auto position = scene_xml.find(from);
    scene_xml.replace(position, from.size(), to);
  };
  replace("__FLANGE__", flange_obj);
  replace("__SLOTTED__", slotted_obj);

  const auto spec = parse_scene_xml(scene_xml);
  if (!spec) {
    st::log::out("场景解析失败：{}\n", spec.error().message);
    return 1;
  }
  Canvas canvas{720, 540, 1.0f};
  if (!render_scene(*spec, canvas, background)) {
    st::log::out("渲染失败\n");
    return 1;
  }
  save_png(canvas, out_dir + "/boolean-smoke.png");
  st::log::out("已出图 {}/boolean-smoke.png\n", out_dir);
  return 0;
}
