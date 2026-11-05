// 验证 bpy 兼容层的 uv_sphere 几何：半径是否等于请求值（文档声称“被压扁”）。
//
// ## 为什么值得单独一条探针
//
// `examples/jianmu/scenes/quad.py` 的注释写着“本框架 bpy 兼容层的
// primitive_uv_sphere_add 有几何缺陷（顶点 x 超出半径两倍，球被压扁）”，
// 并据此**刻意改用立方体**去做对比场景。
//
// 而这类“文档里记着的缺陷”有两个风险：
//   ① 缺陷已被修好，文档没改 → 后来人继续绕过（并据此选错场景素材）；
//   ② 缺陷仍在，但描述不准 → 按描述去修会改错地方。
//
// 所以探针**逐轴量实际包围盒**，把“缺陷在不在、多大”变成数字。

#include <cmath>
#include <cstdio>
#include <string>

#include "st/raster/bpy_runtime.hpp"

namespace {

constexpr const char* kSphere = R"py(
import bpy
bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1.0)
result = bpy.context.active_object
)py";

}  // namespace

auto main() -> int {
  auto scene = st::raster::BpySceneExtractor::extract(kSphere);
  if (!scene.has_value()) {
    std::printf("抽取失败：%s\n", scene.error().message.c_str());
    return 2;
  }
  if (scene->objects.empty()) {
    std::printf("场景里没有物体\n");
    return 2;
  }
  const auto& mesh = scene->objects.front().mesh;
  if (mesh.vertices.empty()) {
    std::printf("网格没有顶点\n");
    return 2;
  }
  // 顶点布局：位置 3 + 法线 3 + 颜色 3（`Mesh::kVertexFloats`）
  constexpr std::size_t kStride = st::raster::Mesh::kVertexFloats;
  double lower[3] = {1e9, 1e9, 1e9};
  double upper[3] = {-1e9, -1e9, -1e9};
  double max_radius = 0.0;
  std::size_t worst_index = 0;
  std::size_t vertex_count = 0;
  for (std::size_t i = 0; i + kStride <= mesh.vertices.size(); i += kStride) {
    double r2 = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
      const double value = static_cast<double>(mesh.vertices[i + static_cast<std::size_t>(axis)]);
      lower[axis] = std::min(lower[axis], value);
      upper[axis] = std::max(upper[axis], value);
      r2 += value * value;
    }
    const double r = std::sqrt(r2);
    if (r > max_radius + 1e-9) {
      max_radius = r;
      worst_index = vertex_count;
    }
    ++vertex_count;
  }

  std::printf("顶点 %zu 个（请求 radius=1.0）\n", mesh.vertices.size() / kStride);
  std::printf("包围盒：x %.4f..%.4f  y %.4f..%.4f  z %.4f..%.4f\n",
              lower[0], upper[0], lower[1], upper[1], lower[2], upper[2]);
  std::printf("最大顶点半径：%.4f（第 %zu 个顶点）\n\n", max_radius, worst_index);
  // 前 3 个顶点也打出来（与 `tmp_sphere_dump` 对口径：同一份数据两处读数必须一致）
  for (std::size_t v = 0; v < 3 && v * kStride + 3 <= mesh.vertices.size(); ++v) {
    std::printf("  v%-3zu (%.4f, %.4f, %.4f)\n", v,
                static_cast<double>(mesh.vertices[v * kStride]),
                static_cast<double>(mesh.vertices[v * kStride + 1]),
                static_cast<double>(mesh.vertices[v * kStride + 2]));
  }
  std::printf("\n");

  int failures = 0;
  const auto check = [&](bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %s  %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
  };
  // 判据：三轴对称（球不是椭圆）、半径等于请求值。
  // 容差 1%：离散化的 UV 球在极点数小于等于环数时，极端顶点落不到精确 1.0 是正常的；
  // 但**超过 1.05 就说明真的出界了**（文档描述的缺陷是“两倍”）。
  for (int axis = 0; axis < 3; ++axis) {
    const double extent = std::max(std::fabs(lower[axis]), std::fabs(upper[axis]));
    check(std::fabs(extent - 1.0) < 0.05,
          axis == 0 ? "x 方向半径 ≈ 1.0" : (axis == 1 ? "y 方向半径 ≈ 1.0" : "z 方向半径 ≈ 1.0"),
          std::format("{:.4f}", extent));
  }
  check(max_radius < 1.05, "没有顶点超出半径（< 1.05）",
        std::format("最大 {:.4f}", max_radius));

  std::printf("\n%s（%d 项失败）\n", failures == 0 ? "uv_sphere 几何正确" : "uv_sphere 有几何问题",
              failures);
  return failures == 0 ? 0 : 1;
}
