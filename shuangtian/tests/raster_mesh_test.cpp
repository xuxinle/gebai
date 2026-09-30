/// 三维网格与 OBJ 加载：**平台中立**（不依赖任何图形 API）。
///
/// 这些用例原先住在 `gl_test.cpp` 里，于是"能不能加载模型"被绑死在"GL 是否可用"上——
/// 而几何解析与图形 API 毫无关系。抽出后：非 Windows 也能跑，换后端也不用搬。

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/mesh.hpp"

namespace {

using st::math::Color;
using st::raster::Mesh;

/// 手写一个立方体 OBJ（含法线），并**故意混入一个坏面**——
/// 真实导出文件里常有一两个退化/越界引用的面。
constexpr const char* kCubeObj = R"obj(
# 单位立方体，带法线
v -0.5 -0.5  0.5
v  0.5 -0.5  0.5
v  0.5  0.5  0.5
v -0.5  0.5  0.5
v -0.5 -0.5 -0.5
v  0.5 -0.5 -0.5
v  0.5  0.5 -0.5
v -0.5  0.5 -0.5
vn  0  0  1
vn  0  0 -1
vn  1  0  0
vn -1  0  0
vn  0  1  0
vn  0 -1  0
f 1//1 2//1 3//1 4//1
f 6//2 5//2 8//2 7//2
f 2//3 6//3 7//3 3//3
f 5//4 1//4 4//4 8//4
f 4//5 3//5 7//5 8//5
f 5//6 6//6 2//6 1//6
f 1 2 99
)obj";

}  // namespace

ST_TEST(mesh_generators_produce_closed_shapes) {
  const Mesh cube = Mesh::cube(1.0f);
  ST_CHECK_EQ(static_cast<int>(cube.vertex_count()), 24);   // 每面 4 个独立顶点（法线不同）
  ST_CHECK_EQ(static_cast<int>(cube.indices.size()), 36);   // 12 个三角形
  const Mesh sphere = Mesh::sphere(0.5f, 16);
  ST_CHECK(sphere.vertex_count() > 50);
  ST_CHECK_EQ(static_cast<int>(sphere.indices.size() % 3), 0);
  // 长方体三轴独立：非立方
  const Mesh box = Mesh::box(2.0f, 1.0f, 0.5f);
  ST_CHECK_EQ(static_cast<int>(box.vertex_count()), 24);
}

ST_TEST(mesh_obj_loads_cube_with_normals_and_skips_bad_faces) {
  // "坏面只跳过、不让整文件失败"是这条的关键断言：真实模型文件里常有越界/退化面。
  const auto mesh = Mesh::load_obj(kCubeObj, Color{0x60, 0xA5, 0xFA, 0xFF});
  ST_CHECK(mesh.has_value());
  if (!mesh.has_value()) return;
  ST_CHECK_EQ(static_cast<int>(mesh->indices.size()), 36);      // 6 个四边形 → 12 三角形
  ST_CHECK_EQ(static_cast<int>(mesh->vertex_count()), 36);
  // 法线必须归一化（否则光照会整体偏暗/偏亮——这是最容易被忽略的解析错误）
  for (std::size_t index = 0; index < mesh->vertex_count(); ++index) {
    const float nx = (*mesh)[index][3];
    const float ny = (*mesh)[index][4];
    const float nz = (*mesh)[index][5];
    ST_CHECK(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0f) < 0.01f);
  }
}

ST_TEST(mesh_obj_supports_negative_indices_and_missing_normals) {
  // 负索引（相对引用）与"没有 vn"都要能用：前者是 OBJ 规范的一部分，
  // 后者是大量导出器（尤其只导几何的流水线）的常态。
  constexpr const char* kTriangle = R"obj(
v 0 0 0
v 1 0 0
v 0 1 0
f -3 -2 -1
)obj";
  const auto mesh = Mesh::load_obj(kTriangle, Color{0xFF, 0xFF, 0xFF, 0xFF});
  ST_CHECK(mesh.has_value());
  if (!mesh.has_value()) return;
  ST_CHECK_EQ(static_cast<int>(mesh->indices.size()), 3);
  // 缺法线时按面计算 → 必须仍归一化（若是 (0,0,0)，光照会全黑）
  ST_CHECK(std::abs(std::abs((*mesh)[0][5]) - 1.0f) < 0.01f);
}

ST_TEST(mesh_obj_rejects_garbage_without_crashing) {
  // 空文件、只有注释、只有顶点没有面、完全不是 OBJ —— 都必须**返回错误**而不是崩。
  for (const char* text : {"", "# just a comment\n", "v 0 0 0\nv 1 0 0\n",
                           "not an obj at all\n{{{", "f 1 2 3\n"}) {
    ST_CHECK(!Mesh::load_obj(text, Color{0xFF, 0xFF, 0xFF, 0xFF}).has_value());
  }
  // 只有注释/顶点时也要给**可读的原因**
  const auto empty = Mesh::load_obj("v 0 0 0\n", Color{0xFF, 0xFF, 0xFF, 0xFF});
  ST_CHECK(!empty.has_value() && !empty.error().message.empty());
}
