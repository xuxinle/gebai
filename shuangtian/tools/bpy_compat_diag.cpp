// 诊断：同一份 bpy 文本，在「默认口径」与「声明 compat」两种情况下，
// `SceneSpec` 的关键字段到底是多少。
//
// ## 为什么需要它
//
// `tests/raster_bpy_cycles_parity_test.cpp` 里有一条断言：
// 默认口径必须是 `tone_map = Agx` + `ambient > 0`。它在测试里红了，
// 但在探针里（`bpy_flat_ablate`）打印出来是对的（`tone=4 ambient=0.350`）。
//
// 两者不一致只有两种可能：
//  ① 测试用的脚本文本与探针用的不是同一份（拼装差异）
//  ② `SceneSpec` 在某个路径上被改写了
//
// 本工具把测试里**同一份拼装文本**直接喂进来，让两边的数对得上。
#include <cstdio>
#include <string>

#include "st/raster/scene3d_script.hpp"

namespace {

/// 与测试文件里的 `flat_plane_script` 逐字一致（含三元组 Euler、显式 type）。
auto build(float energy, bool compat) -> std::string {
  std::string s = R"PY(
import bpy
from mathutils import Vector, Euler

scene = bpy.context.scene

camera_data = bpy.data.cameras.new("probe_camera")
camera_data.lens = 50.0
camera_data.sensor_width = 36.0
camera = bpy.data.objects.new("probe_camera", camera_data)
camera.location = Vector((0.0, 0.0, 20.0))
camera.rotation_euler = Euler((0.0, 0.0, 0.0))

bpy.ops.mesh.primitive_plane_add(size=40.0, location=(0.0, 0.0, 0.0))
plane = bpy.data.objects[-1]
plane.name = 'probe_plane'
material = bpy.data.materials.new("probe_material")
material.diffuse_color = (0.5, 0.5, 0.5, 1.0)
material.metallic = 0.0
material.roughness = 1.0
plane.data.materials.append(material)

sun_data = bpy.data.lights.new("probe_sun", "SUN")
sun_data.energy = )PY";
  s += std::to_string(energy);
  s += R"PY(
sun_data.color = (1.0, 1.0, 1.0)
sun = bpy.data.objects.new("probe_sun", sun_data)
sun.location = Vector((0.0, 0.0, 5.0))
sun.rotation_euler = Euler((0.0, 0.0, 0.0))
)PY";
  if (compat) s += "\nbpy.context.scene.__st_compat__ = 'cycles'\n";
  s += "\nresult = scene\n";
  return s;
}

void dump(const char* tag, float energy, bool compat) {
  const auto spec = st::raster::parse_scene_spec_text(build(energy, compat));
  if (!spec.has_value()) {
    std::printf("%-10s 解析失败：%s\n", tag, spec.error().message.c_str());
    return;
  }
  std::printf(
      "%-10s objects=%-3zu tone=%-2d ambient=%.3f ibl=%.3f diffuse=%.3f compat=%-2d "
      "bpy_len=%-5zu three_len=%zu\n",
      tag, spec->objects.size(), int(spec->lighting.tone_map),
      double(spec->lighting.ambient), double(spec->lighting.ibl_strength),
      double(spec->lighting.diffuse), int(spec->compat), spec->bpy_script.size(),
      spec->three_script.size());
}

}  // namespace

int main() {
  std::printf("口径：tone 0=None 1=Reinhard 2=Aces 3=Filmic 4=Agx 5=AcesThree\n");
  std::printf("      compat 0=None 1=Cycles 2=Eevee\n\n");
  dump("默认", 1.0f, false);
  dump("declare", 1.0f, true);
  dump("默认e2", 2.0f, false);
  dump("declare_e2", 2.0f, true);
  return 0;
}
