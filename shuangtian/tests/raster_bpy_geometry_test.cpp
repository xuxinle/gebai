/// bpy 兼容层**几何正确性**回归：图元必须落在请求的尺寸内。
///
/// ## 这条为什么必须存在
///
/// bpy 侧的几何错误**不会报任何错**：脚本跑通、物体数对、面数对，只是形状不对。
/// 而形状不对的表现又很像“渲染问题”——排查会从光照一路查到相机。
///
/// 实测这次抓到的缺陷（2026-11-05）：
///
/// | 层 | 缺陷 | 症状 |
/// |---|---|---|
/// | `prelude.py` 的 `_sin`/`_cos` | 泰勒展开只对 \|a\| ≤ π 有效，调用方给的却是 0..2π | 顶点跑到半径 3.64 处，球被拉成多瓣花 |
/// | `bpy_ops.py` 的 `_make_uv_sphere` | 环数多算一个 → 末两环读越界顶点 | 同上（叠加） |
///
/// 两层都修后，`radius=1.0` 的球包围盒是 1.0018 / 1.0000 / 1.0000。
/// （1.0018 是 11 阶级数在 π 附近的残差，**不是越界**——所以判据用 5% 容差而不是
/// 1e-3：这条测试守的是“形状对”，不是“浮点精确”。）
///
/// ## 为什么判据是包围盒而不是顶点数
///
/// 顶点数只说明“生成了多少点”，不说明它们在哪儿。而**包围盒**是用户
/// 一眼就能看出不对的量（“球是扁的 / 花形的”），也是最难在错实现下碰巧通过的量。

#include <cmath>
#include <string>

#include "st/raster/bpy_runtime.hpp"
#include "st/test/test.hpp"

namespace {

struct Bounds {
  double lower[3]{1e9, 1e9, 1e9};
  double upper[3]{-1e9, -1e9, -1e9};
  double max_radius{0.0};
  std::size_t vertex_count{0};
};

auto measure(const st::raster::Mesh& mesh) -> Bounds {
  Bounds bounds;
  constexpr std::size_t kStride = st::raster::Mesh::kVertexFloats;
  for (std::size_t i = 0; i + kStride <= mesh.vertices.size(); i += kStride) {
    double radius_squared = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
      const double value = static_cast<double>(mesh.vertices[i + static_cast<std::size_t>(axis)]);
      bounds.lower[axis] = std::min(bounds.lower[axis], value);
      bounds.upper[axis] = std::max(bounds.upper[axis], value);
      radius_squared += value * value;
    }
    bounds.max_radius = std::max(bounds.max_radius, std::sqrt(radius_squared));
    ++bounds.vertex_count;
  }
  return bounds;
}

/// 跑一段 bpy 脚本，返回第一个物体的网格（失败时返回空网格）。
auto first_mesh(const char* script) -> st::raster::Mesh {
  auto scene = st::raster::BpySceneExtractor::extract(script);
  if (!scene.has_value() || scene->objects.empty()) return {};
  return scene->objects.front().mesh;
}

}  // namespace

ST_TEST(bpy_uv_sphere_fits_requested_radius) {
  if (!st::raster::BpySceneExtractor::available()) return;   // 无 MicroPython 的构建跳过
  // 24×12 是"能看出花形"的密度：旧实现在这个密度上最大半径 3.64。
  const auto mesh = first_mesh(R"py(
import bpy
bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1.0)
result = bpy.context.active_object
)py");
  ST_REQUIRE(!mesh.vertices.empty());
  const auto bounds = measure(mesh);
  // 三轴对称到 1.0（容差 5%：级数残差 + 离散化的极环）
  for (int axis = 0; axis < 3; ++axis) {
    const double extent = std::max(std::fabs(bounds.lower[axis]), std::fabs(bounds.upper[axis]));
    ST_CHECK(std::fabs(extent - 1.0) < 0.05);
  }
  // 真正的"越界"判据：没有任何顶点超出半径（旧实现 3.64）
  ST_CHECK(bounds.max_radius < 1.05);
}

ST_TEST(bpy_uv_sphere_vertex_count_matches_grid) {
  if (!st::raster::BpySceneExtractor::available()) return;
  // **顶点数是另一条正交判据**：环数算错会让顶点数偏离 (ring_count-1)*segments+2。
  // 两个缺陷（三角函数发散 / 环数多算）恰好都改这个数，所以单独钉住它。
  const auto mesh = first_mesh(R"py(
import bpy
bpy.ops.mesh.primitive_uv_sphere_add(segments=8, ring_count=4, radius=1.0)
result = bpy.context.active_object
)py");
  ST_REQUIRE(!mesh.vertices.empty());
  // 环数 = ring_count-1 = 3（不含两极）
  const auto bounds = measure(mesh);
  (void)bounds;
  // 顶点表是"每三角形独立三顶点"，所以不能直接数顶点表；
  // 用索引数反推三角形数：top(8) + 中间带 2×(3-1)×8=32 + bottom(8) = 48 三角形
  // ⇒ 索引 144。旧实现（环数多算）会得到更多。
  ST_CHECK(mesh.indices.size() == 144);
}

ST_TEST(bpy_cylinder_and_torus_fit_requested_size) {
  if (!st::raster::BpySceneExtractor::available()) return;
  // 柱体：radius=0.5, depth=2.0 → x/y 半径 0.5、z 跨度 2.0
  const auto cylinder = first_mesh(R"py(
import bpy
bpy.ops.mesh.primitive_cylinder_add(vertices=16, radius=0.5, depth=2.0)
result = bpy.context.active_object
)py");
  ST_REQUIRE(!cylinder.vertices.empty());
  const auto c = measure(cylinder);
  ST_CHECK(std::fabs(std::max(std::fabs(c.lower[0]), std::fabs(c.upper[0])) - 0.5) < 0.05);
  ST_CHECK(std::fabs(c.upper[2] - c.lower[2] - 2.0) < 0.05);

  // 环面：major=1.0, minor=0.25 → 外径 1.25
  const auto torus = first_mesh(R"py(
import bpy
bpy.ops.mesh.primitive_torus_add(major_radius=1.0, minor_radius=0.25,
                                 major_segments=24, minor_segments=8)
result = bpy.context.active_object
)py");
  ST_REQUIRE(!torus.vertices.empty());
  const auto t = measure(torus);
  const double outer = std::max(std::fabs(t.lower[0]), std::fabs(t.upper[0]));
  ST_CHECK(std::fabs(outer - 1.25) < 0.06);
  ST_CHECK(t.max_radius < 1.31);
}
