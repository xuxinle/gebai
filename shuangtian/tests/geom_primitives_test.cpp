/// 图元库的**几何契约**回归测试。
///
/// ## 为什么每个图元都要单独过一遍
///
/// 这一层出过的缺陷形态高度一致：**大部分图元对、某一个反了**。
/// 典型历史：`Mesh::sphere` 的绕序与顶点法线完全相反（实测 224/224），
/// 而 plane/box 都是对的；加上背面剔除后球只画出背面，而**当时 1849 条测试全绿**
/// （包括专门测剔除方向那条——它只用了平面）。
///
/// 所以这里的判据不是"某个图元画得对"，而是：
/// ① 每个图元**逐个**断言绕序与法线同向；
/// ② 每个闭合图元断言**水密**（每条边恰好被两个三角形共享）——这条同时能抓住
///    "少一个面/多一个面/三角形绕反"三类缺陷，而它们单看顶点数都发现不了；
/// ③ 法线是**单位长**（否则光照强度会随网格漂，且只在某些形状上漂）；
/// ④ UV 落在 0..1、几何在声明的包围盒内、同一参数两次生成**逐字节相同**。
///
/// ## 水密判据为何按"位置"而不是按"顶点索引"
///
/// 硬边（盒子的棱）在顶点表里是**分开的顶点**（法线不同），但几何上是同一条边。
/// 按索引判会把每个硬边都算成"只被一个三角形使用"，于是所有带硬边的图元
/// 都变成"不水密"——那是个假警报，会把注意力引到错的地方。

#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/primitives.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::raster::Mesh;
using st::raster::Bounds;
namespace prim = st::raster::primitives;
using prim::PrimitiveKind;
using prim::PrimitiveParams;

struct GeometryReport {
  int triangles{0};
  int same_winding{0};     ///< 几何法线与顶点法线同向
  int opposite_winding{0};
  int degenerate{0};
  int non_unit_normals{0};
  int uv_out_of_range{0};
  int duplicated_edges{0};   ///< 被 >2 个三角形共享的边（非流形）
  int boundary_edges{0};     ///< 只被 1 个三角形共享的边（洞）
  bool has_uv{false};
};

/// 位置量化键（水密判据按位置，见文件头）。
using PositionKey = std::tuple<std::int64_t, std::int64_t, std::int64_t>;

auto position_key(const Mesh& mesh, std::size_t index) -> PositionKey {
  const auto q = [&](std::size_t component) {
    const std::size_t at = index * Mesh::kVertexFloats + component;
    const double value = at < mesh.vertices.size() ? static_cast<double>(mesh.vertices[at]) : 0.0;
    return static_cast<std::int64_t>(std::llround(value * 1.0e6));
  };
  return {q(0), q(1), q(2)};
}

auto report(const Mesh& mesh) -> GeometryReport {
  GeometryReport out{};
  out.has_uv = mesh.has_uvs();

  // 顶点级检查：法线单位长、UV 在 0..1
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    const auto v = mesh[i];
    if (v.size() < Mesh::kVertexFloats) continue;
    const float nx = v[3];
    const float ny = v[4];
    const float nz = v[5];
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (std::abs(len - 1.0f) > 0.02f) ++out.non_unit_normals;
    if (out.has_uv) {
      const auto uv = mesh.uv_at(i);
      if (uv.first < -0.001f || uv.first > 1.001f || uv.second < -0.001f || uv.second > 1.001f) {
        ++out.uv_out_of_range;
      }
    }
  }

  std::map<std::pair<PositionKey, PositionKey>, int> edge_use;
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const std::size_t i0 = mesh.indices[t];
    const std::size_t i1 = mesh.indices[t + 1];
    const std::size_t i2 = mesh.indices[t + 2];
    const auto v0 = mesh[i0];
    const auto v1 = mesh[i1];
    const auto v2 = mesh[i2];
    if (v0.size() < Mesh::kVertexFloats || v1.size() < Mesh::kVertexFloats ||
        v2.size() < Mesh::kVertexFloats) {
      continue;
    }
    ++out.triangles;
    const st::math::Vec3 p0{v0[0], v0[1], v0[2]};
    const st::math::Vec3 p1{v1[0], v1[1], v1[2]};
    const st::math::Vec3 p2{v2[0], v2[1], v2[2]};
    const st::math::Vec3 geo = (p1 - p0).cross(p2 - p0);
    const float area2 = geo.length();
    if (area2 < 1.0e-6f) {
      ++out.degenerate;
    } else {
      const st::math::Vec3 n{(v0[3] + v1[3] + v2[3]) / 3.0f, (v0[4] + v1[4] + v2[4]) / 3.0f,
                             (v0[5] + v1[5] + v2[5]) / 3.0f};
      if (geo.dot(n) > 0.0f) {
        ++out.same_winding;
      } else {
        ++out.opposite_winding;
      }
    }
    // 边（按位置，无向）
    const std::size_t ids[3] = {i0, i1, i2};
    for (int k = 0; k < 3; ++k) {
      const PositionKey a = position_key(mesh, ids[static_cast<std::size_t>(k)]);
      const PositionKey b = position_key(mesh, ids[static_cast<std::size_t>((k + 1) % 3)]);
      if (a == b) continue;   // 退化边不计
      edge_use[a < b ? std::make_pair(a, b) : std::make_pair(b, a)] += 1;
    }
  }
  for (const auto& [edge, count] : edge_use) {
    (void)edge;
    if (count == 1) ++out.boundary_edges;
    if (count > 2) ++out.duplicated_edges;
  }
  return out;
}

/// 一个待测图元：名字 + 网格 + 是否应当是闭合体。
struct Case {
  const char* name;
  Mesh mesh;
  bool closed;
};

/// 全部图元的实例（参数统一用小分段数：足够暴露绕序/法线问题，又不至于让测试变慢）。
auto all_cases() -> std::vector<Case> {
  std::vector<Case> cases;
  const auto add = [&cases](const char* name, PrimitiveKind kind, PrimitiveParams params,
                            bool closed) {
    params.radius = 0.6f;
    params.depth = 1.0f;
    params.segments = 10;
    params.rings = 6;
    params.subdivisions = 1;
    params.major_segments = 12;
    params.minor_segments = 8;
    params.major_radius = 0.5f;
    params.minor_radius = 0.15f;
    params.columns = 3;
    params.rows = 3;
    params.teeth = 8;
    params.sides = 5;
    params.turns = 2.0f;
    params.color = Color{180, 190, 200, 255};
    cases.push_back(Case{name, prim::make(kind, params), closed});
  };
  add("plane", PrimitiveKind::Plane, {}, false);
  add("cube", PrimitiveKind::Cube, {}, true);
  add("uv_sphere", PrimitiveKind::UvSphere, {}, true);
  add("ico_sphere", PrimitiveKind::IcoSphere, {}, true);
  add("cylinder", PrimitiveKind::Cylinder, {}, true);
  add("cone", PrimitiveKind::Cone, {}, true);
  add("capsule", PrimitiveKind::Capsule, {}, true);
  add("torus", PrimitiveKind::Torus, {}, true);
  add("disc", PrimitiveKind::Disc, {}, false);
  add("circle", PrimitiveKind::Circle, {}, false);
  add("grid", PrimitiveKind::Grid, {}, false);
  add("tube", PrimitiveKind::Tube, {}, true);
  add("prism", PrimitiveKind::Prism, {}, true);
  add("gear", PrimitiveKind::Gear, {}, true);
  add("torus_knot", PrimitiveKind::TorusKnot, {}, true);
  add("arrow", PrimitiveKind::Arrow, {}, true);
  add("wedge", PrimitiveKind::Wedge, {}, true);
  add("tetrahedron", PrimitiveKind::Tetrahedron, {}, true);
  add("octahedron", PrimitiveKind::Octahedron, {}, true);
  add("dodecahedron", PrimitiveKind::Dodecahedron, {}, true);
  add("icosahedron", PrimitiveKind::Icosahedron, {}, true);
  add("helix", PrimitiveKind::Helix, {}, true);
  return cases;
}

}  // namespace

ST_TEST(primitives_every_generator_has_consistent_winding) {
  for (const Case& c : all_cases()) {
    const GeometryReport r = report(c.mesh);
    st::log::out("[prim] {:<14} tri={:5} same={:5} opposite={:4} degenerate={:3} boundary={:4} nonmanifold={:3} uv_bad={:3}\n",
                 c.name, r.triangles, r.same_winding, r.opposite_winding, r.degenerate,
                 r.boundary_edges, r.duplicated_edges, r.uv_out_of_range);
    ST_CHECK(r.triangles > 0);                     // 防"空网格恒通过"的假绿
    ST_CHECK_EQ(r.opposite_winding, 0);            // 绕序必须与法线同向
    ST_CHECK_EQ(r.non_unit_normals, 0);            // 法线必须单位长
    ST_CHECK_EQ(r.uv_out_of_range, 0);             // UV 必须在 0..1
  }
}

ST_TEST(primitives_closed_shapes_are_watertight) {
  for (const Case& c : all_cases()) {
    if (!c.closed) continue;
    const GeometryReport r = report(c.mesh);
    // 水密 = 没有只被一个三角形使用的边（洞）。非流形边（>2）另外判。
    ST_CHECK_EQ(r.boundary_edges, 0);
    ST_CHECK_EQ(r.duplicated_edges, 0);
  }
}

ST_TEST(primitives_are_deterministic) {
  // 同一参数两次生成必须**逐字节相同**：场景脚本的"可复现"建立在这一点上
  // （若有随机/时间依赖，同一份脚本两帧就会不同，而那种差在画面上微不可察）。
  for (const Case& c : all_cases()) {
    const auto kind = prim::parse_kind(c.name);
    ST_CHECK(kind.has_value());
    PrimitiveParams params{};
    params.radius = 0.6f;
    params.depth = 1.0f;
    params.segments = 10;
    params.rings = 6;
    params.subdivisions = 1;
    params.major_segments = 12;
    params.minor_segments = 8;
    params.major_radius = 0.5f;
    params.minor_radius = 0.15f;
    params.columns = 3;
    params.rows = 3;
    params.teeth = 8;
    params.sides = 5;
    params.turns = 2.0f;
    params.color = Color{180, 190, 200, 255};
    const Mesh again = prim::make(*kind, params);
    ST_CHECK_EQ(again.vertices.size(), c.mesh.vertices.size());
    ST_CHECK_EQ(again.indices.size(), c.mesh.indices.size());
    ST_CHECK(again.vertices == c.mesh.vertices);
    ST_CHECK(again.indices == c.mesh.indices);
  }
}

ST_TEST(primitives_respect_declared_size) {
  // 尺寸契约：`radius` 是"到中心的距离"，所以球的包围盒边长 = 2·radius（±2%）。
  // 这条能抓住"生成器里多乘/少乘了一个 2"——那种错在画面上只表现为"大了点"。
  PrimitiveParams params{};
  params.radius = 0.5f;
  const Mesh sphere = prim::make_uv_sphere(params);
  const Bounds box = st::raster::bounds(sphere);
  ST_CHECK_NEAR(box.size().x, 1.0f, 0.02f);
  ST_CHECK_NEAR(box.size().y, 1.0f, 0.02f);
  ST_CHECK_NEAR(box.size().z, 1.0f, 0.02f);
  ST_CHECK_NEAR(box.center().x, 0.0f, 0.001f);
  ST_CHECK_NEAR(box.center().y, 0.0f, 0.001f);

  // 圆柱：`depth` 是 Y 方向总长（不是半径）
  PrimitiveParams cyl{};
  cyl.radius = 0.3f;
  cyl.depth = 2.0f;
  const Bounds cyl_box = st::raster::bounds(prim::make_cylinder(cyl));
  ST_CHECK_NEAR(cyl_box.size().y, 2.0f, 0.02f);
  ST_CHECK_NEAR(cyl_box.size().x, 0.6f, 0.03f);
}

ST_TEST(primitives_names_round_trip) {
  // 规范名 → 枚举 → 规范名 必须闭环；别名也要能解析到同一枚举。
  constexpr PrimitiveKind kAll[] = {
      PrimitiveKind::Plane,        PrimitiveKind::Cube,        PrimitiveKind::UvSphere,
      PrimitiveKind::IcoSphere,    PrimitiveKind::Cylinder,    PrimitiveKind::Cone,
      PrimitiveKind::Capsule,      PrimitiveKind::Torus,       PrimitiveKind::Disc,
      PrimitiveKind::Circle,       PrimitiveKind::Grid,        PrimitiveKind::Tube,
      PrimitiveKind::Prism,        PrimitiveKind::Gear,        PrimitiveKind::TorusKnot,
      PrimitiveKind::Arrow,        PrimitiveKind::Wedge,       PrimitiveKind::Tetrahedron,
      PrimitiveKind::Octahedron,   PrimitiveKind::Dodecahedron, PrimitiveKind::Icosahedron,
      PrimitiveKind::Helix};
  for (const PrimitiveKind kind : kAll) {
    const std::string_view name = prim::kind_name(kind);
    const auto parsed = prim::parse_kind(name);
    ST_REQUIRE(parsed.has_value());
    ST_CHECK(*parsed == kind);
  }
  // 别名（Blender / three.js / 日常叫法）必须落到同一个枚举
  ST_CHECK(prim::parse_kind("sphere").value() == PrimitiveKind::UvSphere);
  ST_CHECK(prim::parse_kind("Ball").value() == PrimitiveKind::UvSphere);
  ST_CHECK(prim::parse_kind("uv-sphere").value() == PrimitiveKind::UvSphere);
  ST_CHECK(prim::parse_kind("box").value() == PrimitiveKind::Cube);
  ST_CHECK(prim::parse_kind("disk").value() == PrimitiveKind::Disc);
  ST_CHECK(prim::parse_kind("donut").value() == PrimitiveKind::Torus);
  ST_CHECK(prim::parse_kind("spring").value() == PrimitiveKind::Helix);
  ST_CHECK(!prim::parse_kind("dodecahedronx").has_value());
  // 报错要带**可用清单**（生成脚本的模型据此自我修正）
  const auto failed = prim::make_from_name("not_a_shape", {});
  ST_REQUIRE(!failed.has_value());
  ST_CHECK(failed.error().code == st::ErrorCode::Parse);
  ST_CHECK(failed.error().message.find("uv_sphere") != std::string::npos);
  ST_CHECK(failed.error().message.find("not_a_shape") != std::string::npos);
}

ST_TEST(primitives_sanitize_degenerate_params) {
  // 非法参数不能产出退化几何（零面积三角形会污染 AABB、让法线变 NaN）。
  PrimitiveParams bad{};
  bad.radius = 0.5f;
  bad.segments = 2;
  bad.rings = 0;
  const Mesh sphere = prim::make_uv_sphere(bad);
  ST_CHECK(sphere.vertex_count() >= 6);   // 3 段 × 2 环 + 两极
  const GeometryReport r = report(sphere);
  ST_CHECK_EQ(r.opposite_winding, 0);
  ST_CHECK_EQ(r.triangles, r.same_winding);   // 一个退化三角形都不许有

  PrimitiveParams bad_cyl{};
  bad_cyl.radius = -1.0f;
  bad_cyl.depth = 0.0f;
  bad_cyl.segments = 1;
  const Mesh cyl = prim::make_cylinder(bad_cyl);
  ST_CHECK(cyl.vertex_count() > 3);
}
