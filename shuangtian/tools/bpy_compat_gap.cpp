// 量测试 `bpy_default_mode_differs_from_parity_mode` 里两个口径的实际像素。
//
// ## 为什么要单独量
//
// 那条测试断言「默认口径比对标口径亮 >20 级」——它守的是「默认不改」这个承诺。
// AgX 修好之后默认口径**变暗了**（AgX 会压中调），于是那条断言的**前提变了**。
//
// 但断言失败有两种可能，必须分清：
//  ① 默认口径真的变暗到接近对标口径了（那是 AgX 生效的正常后果）
//  ② 对标模式意外失效了（那是回归）
//
// 本工具把两个值都打出来，一眼可判。
#include <cstdio>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

auto script(bool compat) -> std::string {
  std::string s = R"PY(
import bpy
from mathutils import Vector, Euler
scene = bpy.context.scene
cd = bpy.data.cameras.new("probe_camera")
cd.lens = 50.0
cd.sensor_width = 36.0
camera = bpy.data.objects.new("probe_camera", cd)
camera.location = Vector((0.0, 0.0, 20.0))
camera.rotation_euler = Euler((0.0, 0.0, 0.0))
bpy.ops.mesh.primitive_plane_add(size=40.0, location=(0.0, 0.0, 0.0))
plane = bpy.data.objects[-1]
material = bpy.data.materials.new("m")
material.diffuse_color = (0.5, 0.5, 0.5, 1.0)
material.metallic = 0.0
material.roughness = 1.0
plane.data.materials.append(material)
sun_data = bpy.data.lights.new("s", "SUN")
sun_data.energy = 1.0
sun = bpy.data.objects.new("s", sun_data)
sun.location = Vector((0.0, 0.0, 5.0))
sun.rotation_euler = Euler((0.0, 0.0, 0.0))
)PY";
  if (compat) s += "bpy.context.scene.__st_compat__ = 'cycles'\n";
  s += "result = scene\n";
  return s;
}

auto render(bool compat, int size) -> int {
  const auto spec = st::raster::parse_scene_spec_text(script(compat));
  if (!spec.has_value()) return -1;
  st::raster::Canvas canvas{size, size, 1.0f};
  if (!st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255))
           .has_value()) {
    return -1;
  }
  const auto pixels = canvas.pixels();
  const auto c = st::math::unpremultiply(
      pixels[static_cast<std::size_t>(size / 2) * static_cast<std::size_t>(size) +
             static_cast<std::size_t>(size / 2)]);
  return int(c.r);
}

}  // namespace

int main() {
  const int normal = render(false, 128);
  const int parity = render(true, 128);
  std::printf("默认口径（AgX + ambient 0.35 + ibl）  = %d\n", normal);
  std::printf("对标口径（None + ambient 0 + ibl 0）  = %d\n", parity);
  std::printf("差 = %d\n", normal - parity);
  return 0;
}
