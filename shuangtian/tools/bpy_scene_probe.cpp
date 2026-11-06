// bpy 脚本 → ThreeScene 端到端：真跑一段建模脚本，验抽取结果。
//
// ## 这个探针存在的理由
//
// "能建 bpy 对象"与"能把它变成渲染器吃得下的场景"是两件事。中间隔着一层
// 抽取（世界矩阵、三角化、材质、相机），而那层最容易出**静默错**：
// 转置漏了、法线反了、fov 单位错了——画面"有点不对"但没人报错。
//
// 所以这里用可断言的量（顶点数、包围盒、材质值）把抽取钉住。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/raster/bpy_runtime.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

int failures = 0;   // lint-allow: L8 探针的用例计数器（一次性量尺，与 tests/ 同口径）

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-30s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

/// 场景的包围盒（世界空间：物体顶点 × 世界矩阵）。
auto bounds_of(const st::raster::ThreeScene& scene, float& min_y, float& max_y,
               float& min_x, float& max_x) -> bool {
  bool any = false;
  min_y = min_x = 1e30f;
  max_y = max_x = -1e30f;
  for (const auto& object : scene.objects) {
    const auto& mesh = object.mesh;
    for (std::size_t i = 0; i + 2 < mesh.vertices.size(); i += st::raster::Mesh::kVertexFloats) {
      const auto x = mesh.vertices[i];
      const auto y = mesh.vertices[i + 1];
      const auto z = mesh.vertices[i + 2];
      // 应用世界矩阵（用 w 分量处理平移）
      const float wx = object.world.value(0, 0) * x + object.world.value(0, 1) * y +
                       object.world.value(0, 2) * z + object.world.value(0, 3);
      const float wy = object.world.value(1, 0) * x + object.world.value(1, 1) * y +
                       object.world.value(1, 2) * z + object.world.value(1, 3);
      if (wx < min_x) min_x = wx;
      if (wx > max_x) max_x = wx;
      if (wy < min_y) min_y = wy;
      if (wy > max_y) max_y = wy;
      any = true;
    }
  }
  return any;
}

}  // namespace

int main() {
  if (!st::raster::BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n", st::raster::BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }

  // ① 一个立方体：8 顶点、6 面 → 扇形三角化后 6×2=12 三角形 = 36 个渲染顶点
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy
bpy.ops.mesh.primitive_cube_add(size=2.0)
cube = bpy.context.active_object
cube.material_slots.append(bpy.data.materials.new('box'))
result = cube
)PY");
    if (!scene.has_value()) {
      std::printf("  [FAIL] 立方体抽取失败：%s\n", scene.error().message.c_str());
      return 1;
    }
    expect(scene->objects.size() == 1, "立方体：1 个物体",
           std::to_string(scene->objects.size()) + " 个");
    if (!scene->objects.empty()) {
      const auto& mesh = scene->objects[0].mesh;
      const auto count = mesh.vertices.size() / st::raster::Mesh::kVertexFloats;
      // 6 个四边形面 × 2 三角形 × 3 顶点 = 36
      expect(count == 36, "立方体：36 渲染顶点（6 面三角化）", std::to_string(count) + " 个");
      expect(mesh.indices.size() == 36, "立方体：36 索引",
             std::to_string(mesh.indices.size()) + " 个");
      expect(scene->objects[0].name == "Cube", "立方体：名字", scene->objects[0].name);
    }
  }

  // ② 变换：把立方体平移 (5,0,0)，包围盒应当平移过去
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy
from mathutils import Vector
bpy.ops.mesh.primitive_cube_add(size=2.0)
obj = bpy.context.active_object
obj.location = Vector((5.0, 0.0, 0.0))
result = obj
)PY");
    float min_y = 0, max_y = 0, min_x = 0, max_x = 0;
    const bool any = scene.has_value() && bounds_of(*scene, min_y, max_y, min_x, max_x);
    expect(any, "变换：能算包围盒", any ? "ok" : "失败");
    if (any) {
      // 立方体 size=2 → x ∈ [-1,1]，平移 5 → [4,6]
      const bool ok = std::fabs(min_x - 4.0f) < 0.01f && std::fabs(max_x - 6.0f) < 0.01f;
      char detail[128];
      std::snprintf(detail, sizeof(detail), "x∈[%.2f, %.2f]（期望 [4,6]）",
                    static_cast<double>(min_x), static_cast<double>(max_x));
      expect(ok, "变换：世界矩阵生效", detail);
    }
  }

  // ③ 材质：显式设的金属度/粗糙度要透出来
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy
mat = bpy.data.materials.new('steel')
mat.metallic = 1.0
mat.roughness = 0.15
mat.diffuse_color = (0.9, 0.2, 0.1, 1.0)
bpy.ops.mesh.primitive_uv_sphere_add(segments=8, ring_count=4, radius=1.0)
bpy.context.active_object.material_slots.append(mat)
result = bpy.context.active_object
)PY");
    if (!scene.has_value() || scene->objects.empty()) {
      expect(false, "材质：抽取成功", "失败");
    } else {
      const auto& mat = scene->objects[0].material;
      const bool ok = std::fabs(mat.metallic - 1.0f) < 0.01f &&
                      std::fabs(mat.roughness - 0.15f) < 0.01f &&
                      std::fabs(mat.base_color.x - 0.9f) < 0.01f;
      char detail[160];
      std::snprintf(detail, sizeof(detail), "metallic=%.2f roughness=%.2f r=%.2f",
                    static_cast<double>(mat.metallic),
                    static_cast<double>(mat.roughness),
                    static_cast<double>(mat.base_color.x));
      expect(ok, "材质：PBR 参数透出", detail);
    }
  }

  // ④ bmesh 逐顶点建模（齿轮这类靠它）也能抽
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy, bmesh
mesh = bpy.data.meshes.new('tri')
bm = bmesh.new()
a = bm.verts.new((0.0, 0.0, 0.0))
b = bm.verts.new((1.0, 0.0, 0.0))
c = bm.verts.new((0.0, 1.0, 0.0))
bm.faces.new((a, b, c))
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('tri', mesh)
result = obj
)PY");
    if (!scene.has_value()) {
      expect(false, "bmesh：抽取成功", scene.error().message);
    } else {
      const auto count = scene->objects.empty()
                             ? 0
                             : scene->objects[0].mesh.vertices.size() /
                                   st::raster::Mesh::kVertexFloats;
      expect(count == 3, "bmesh：三角形 3 顶点", std::to_string(count) + " 个");
    }
  }

  // ⑤ 相机：fov 单位必须被转换（弧度 → 角度）
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy
from mathutils import Vector
cam_data = bpy.data.cameras.new('Cam')
cam_data.lens = 50.0
cam = bpy.data.objects.new('Cam', cam_data)
cam.location = Vector((0.0, 0.0, 5.0))
bpy.ops.mesh.primitive_cube_add(size=1.0)
result = cam
)PY");
    if (!scene.has_value()) {
      expect(false, "相机：抽取成功", scene.error().message);
    } else {
      // 50mm / 36mm 传感器 → fov_y = 2*atan(18/50) ≈ 0.6978 rad ≈ 39.99°
      const float fov = scene->camera.fov_y_degrees;
      const bool ok = std::fabs(fov - 39.99f) < 1.5f;
      char detail[128];
      std::snprintf(detail, sizeof(detail), "fov=%.2f°（期望 ≈40°，若是 0.70 说明漏了弧度→角度）",
                    static_cast<double>(fov));
      expect(ok, "相机：fov 单位转换", detail);
    }
  }

  // ⑥ 错误路径：脚本语法错要给出可读原因（不是静默空场景）
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script("import bpy\nthis is not python\n");
    const bool ok = !scene.has_value() && !scene.error().message.empty();
    expect(ok, "错误：语法错有可读原因",
           scene.has_value() ? "(没报错，不符合预期)" : scene.error().message);
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("bpy → ThreeScene 抽取全部通过\n");
  return 0;
}
