// 凹多边形三角化：先证明缺陷，再验修好。
//
// ## 缺陷是什么
//
// 现在的实现是**扇形三角化**（`(v0, vi, vi+1)`）。对凸多边形完全正确，
// 对凹多边形会生成**跨出边界的三角形**——那些三角形覆盖了本不该被填充的
// 区域。
//
// 症状的特殊之处：**不报错**。画面看起来"有个多边形"，但形状是错的
// （凹角被填平）。这类缺陷比崩溃危险——崩溃会被人发现，画错会被当成
// "设计成这样"。
//
// ## 判据怎么定
//
// 用一个**已知面积的 L 形**（凹多边形态）：
//
//     (0,0) ────── (3,0)
//       │            │
//       │   (1,1)────(3,1)
//       │     │
//       │     │
//     (0,2) ─ (1,2)
//
// 面积 = 3×2 − 2×1 = 4（凹角缺口 2×1）
//
// 扇形三角化会得到面积 > 4（把缺口填了）。耳切会得到精确 4。
//
// 判据就是**三角化后的总面积**：等于 4 才对。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/raster/bpy_runtime.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-34s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

/// 三角形面积（XY 平面）。
auto tri_area(const float* a, const float* b, const float* c) -> double {
  // 全用 double 算，避免 float→double 的隐式提升警告（-Werror=double-promotion）
  const double ax = static_cast<double>(a[0]);
  const double ay = static_cast<double>(a[1]);
  const double bx = static_cast<double>(b[0]);
  const double by = static_cast<double>(b[1]);
  const double cx = static_cast<double>(c[0]);
  const double cy = static_cast<double>(c[1]);
  return std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
}

/// 场景里所有三角形在 XY 平面的面积之和。
auto total_area(const st::raster::ThreeScene& scene) -> double {
  double sum = 0.0;
  for (const auto& object : scene.objects) {
    const auto& mesh = object.mesh;
    const std::size_t stride = st::raster::Mesh::kVertexFloats;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      const auto i0 = static_cast<std::size_t>(mesh.indices[i]) * stride;
      const auto i1 = static_cast<std::size_t>(mesh.indices[i + 1]) * stride;
      const auto i2 = static_cast<std::size_t>(mesh.indices[i + 2]) * stride;
      if (i2 + 2 >= mesh.vertices.size()) continue;
      sum += tri_area(&mesh.vertices[i0], &mesh.vertices[i1], &mesh.vertices[i2]);
    }
  }
  return sum;
}

/// 场景里三角形总数。
auto triangle_count(const st::raster::ThreeScene& scene) -> std::size_t {
  std::size_t count = 0;
  for (const auto& object : scene.objects) {
    count += object.mesh.indices.size() / 3;
  }
  return count;
}

}  // namespace

int main() {
  if (!st::raster::BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n",
                st::raster::BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }

  // ── ① L 形（凹）：面积必须 = 4 ──────────────────────────────────────
  //
  // 顶点顺序（逆时针，凹角在 (1,1)-(1,2) 一线）：
  //   0:(0,0)  1:(3,0)  2:(3,1)  3:(1,1)  4:(1,2)  5:(0,2)
  // 面积 = 3×2 − 2×1 = 4
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy, bmesh
mesh = bpy.data.meshes.new('L')
bm = bmesh.new()
pts = [(0.0, 0.0, 0.0), (3.0, 0.0, 0.0), (3.0, 1.0, 0.0),
       (1.0, 1.0, 0.0), (1.0, 2.0, 0.0), (0.0, 2.0, 0.0)]
verts = [bm.verts.new(p) for p in pts]
bm.faces.new(verts)
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('L', mesh)
result = obj
)PY");
    if (!scene.has_value()) {
      std::printf("  [FAIL] L 形抽取：%s\n", scene.error().message.c_str());
      return 1;
    }
    const double area = total_area(*scene);
    const auto tris = triangle_count(*scene);
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "面积 %.3f（期望 4.000，扇形会偏大）；%zu 三角形（n-2=4 才对）",
                  area, tris);
    expect(std::fabs(area - 4.0) < 0.01, "凹多边形：面积正确（没填平凹角）", detail);
    expect(tris == 4, "凹多边形：三角形数 = n-2", std::to_string(tris) + " 个");
  }

  // ── ② 凸多边形（六边形）：两种算法都得对，验证没改坏 ────────────────
  //
  // 正六边形外接圆半径 1：面积 = 3√3/2 ≈ 2.598
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy, bmesh, math
mesh = bpy.data.meshes.new('hex')
bm = bmesh.new()
pts = []
for i in range(6):
    a = 6.283185307179586 * i / 6.0
    pts.append((math.cos(a), math.sin(a), 0.0))
verts = [bm.verts.new(p) for p in pts]
bm.faces.new(verts)
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('hex', mesh)
result = obj
)PY");
    if (!scene.has_value()) {
      std::printf("  [FAIL] 六边形抽取：%s\n", scene.error().message.c_str());
      return 1;
    }
    const double area = total_area(*scene);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "面积 %.3f（期望 2.598）", area);
    expect(std::fabs(area - 2.598) < 0.01, "凸多边形：面积仍正确（回归）", detail);
  }

  // ── ③ 极端凹（星形）：面积必须明显小于外接凸包 ──────────────────────
  //
  // 五角星：外半径 1、内半径 0.4，10 个顶点。面积 = 10 × (1/2)·R·r·sin(36°)
  // ≈ 10 × 0.5 × 1 × 0.4 × 0.5878 ≈ 1.176
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy, bmesh, math
mesh = bpy.data.meshes.new('star')
bm = bmesh.new()
pts = []
for i in range(10):
    a = 6.283185307179586 * i / 10.0
    r = 1.0 if i % 2 == 0 else 0.4
    pts.append((math.cos(a) * r, math.sin(a) * r, 0.0))
verts = [bm.verts.new(p) for p in pts]
bm.faces.new(verts)
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('star', mesh)
result = obj
)PY");
    if (!scene.has_value()) {
      std::printf("  [FAIL] 星形抽取：%s\n", scene.error().message.c_str());
      return 1;
    }
    const double area = total_area(*scene);
    // 星形真实面积约 1.176；扇形三角化会得到接近外接圆（>2.5）
    char detail[192];
    std::snprintf(detail, sizeof(detail), "面积 %.3f（期望 ≈1.176；扇形会 >2.5）", area);
    expect(std::fabs(area - 1.176) < 0.05, "星形：凹角全部正确挖空", detail);
  }

  // ── ④ 退化输入：共线点 / 重复点不该崩，也不该产生负面积 ──────────────
  {
    const auto scene = st::raster::BpySceneExtractor::scene_from_script(R"PY(
import bpy, bmesh
mesh = bpy.data.meshes.new('degenerate')
bm = bmesh.new()
# 一个矩形，但边上插了两个共线点（常见于 CAD 导出的数据）
pts = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (2.0, 0.0, 0.0),
       (2.0, 1.0, 0.0), (0.0, 1.0, 0.0)]
verts = [bm.verts.new(p) for p in pts]
bm.faces.new(verts)
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new('deg', mesh)
result = obj
)PY");
    if (!scene.has_value()) {
      expect(false, "退化输入：不崩", scene.error().message);
    } else {
      const double area = total_area(*scene);
      char detail[160];
      std::snprintf(detail, sizeof(detail), "面积 %.3f（期望 2.000）", area);
      expect(std::fabs(area - 2.0) < 0.01, "退化输入（共线点）：面积正确", detail);
    }
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("凹多边形三角化全部通过\n");
  return 0;
}
