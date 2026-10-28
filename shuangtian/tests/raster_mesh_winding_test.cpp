/// 网格生成器的**绕序一致性**回归测试。
///
/// ## 为何必须有这条（本轮最贵的缺陷）
///
/// `Mesh::sphere` 早先的绕序与顶点法线**完全相反**（实测 224/224 反向），
/// 而其余生成器（plane / box）是对的。这个不一致在引入背面剔除之前
/// **完全看不出来**——两面都画，深度测试自己挑出对的那一面，画面是对的。
///
/// 加上 `CullMode::Back` 后它立刻现形：球只画出**背面**
/// （实测画面里 `n.z>0.5` 的像素 0 个、`n.z<-0.5` 的 25 万个），
/// 内外翻转、完全不对。而当时**全部 1849 条测试都是绿的**——
/// 包括专门测剔除方向那条（它只用平面，平面的绕序恰好是对的）。
///
/// 所以判据必须是“**每个生成器**的绕序都与法线同向”，而不是只测一个。

#include <cmath>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/mesh.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::raster::Mesh;

namespace {

/// 统计绕序与法线同向/反向的三角形数。
///
/// 判据：三角形 `(v0,v1,v2)` 的**几何法线** `(p1-p0)×(p2-p0)` 与该三角形的
/// 平均顶点法线点积为正 ⇒ 绕序与法线同向（逆时针 = 正面，与 D3D/OpenGL 约定一致）。
struct Winding {
  int same{0};
  int opposite{0};
  int degenerate{0};
};

auto analyze(const Mesh& mesh) -> Winding {
  Winding w{};
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const auto i0 = mesh.indices[i];
    const auto i1 = mesh.indices[i + 1];
    const auto i2 = mesh.indices[i + 2];
    const auto v0 = mesh[i0];
    const auto v1 = mesh[i1];
    const auto v2 = mesh[i2];
    if (v0.size() < Mesh::kVertexFloats || v1.size() < Mesh::kVertexFloats ||
        v2.size() < Mesh::kVertexFloats) {
      continue;
    }
    const st::math::Vec3 p0{v0[0], v0[1], v0[2]};
    const st::math::Vec3 p1{v1[0], v1[1], v1[2]};
    const st::math::Vec3 p2{v2[0], v2[1], v2[2]};
    const st::math::Vec3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    const st::math::Vec3 e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
    const st::math::Vec3 geo{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                             e1.x * e2.y - e1.y * e2.x};
    const float len = std::sqrt(geo.x * geo.x + geo.y * geo.y + geo.z * geo.z);
    // ## 极点退化三角形要跳过（否则会误判成“绕序反向”）
    //
    // 球体生成器在南北极处，一圈顶点的**位置相同**（只有法线不同），
    // 构成几何面积为零的三角形。浮点误差使 `len` 不等于 0，它们会落进
    // “反向”桶里（实测 16 个 = `slices`）。
    //
    // 剔除阈值取相对量：`len` 远小于典型边长即可（球半径 1 时边长约 0.1）。
    if (len < 1e-4f) {
      ++w.degenerate;
      continue;
    }
    const st::math::Vec3 n{(v0[3] + v1[3] + v2[3]) / 3.0f, (v0[4] + v1[4] + v2[4]) / 3.0f,
                           (v0[5] + v1[5] + v2[5]) / 3.0f};
    const float d = geo.x * n.x + geo.y * n.y + geo.z * n.z;
    if (d > 0.0f) {
      ++w.same;
    } else {
      ++w.opposite;
    }
  }
  return w;
}

}  // namespace

ST_TEST(mesh_generators_have_consistent_winding) {
  // 每个生成器都单独断言：一个坏了不该被另一个的对冲掉。
  struct Case {
    const char* name;
    Mesh mesh;
  };
  const Case cases[] = {
      {"plane", Mesh::plane(2.0f, 2.0f, Color{200, 200, 200, 255})},
      {"box", Mesh::box(1.0f, 1.0f, 1.0f)},
      {"cube", Mesh::cube(1.0f)},
      {"sphere", Mesh::sphere(1.0f, 16)},
      {"colored_sphere", Mesh::colored_sphere(1.0f, 16, Color{120, 140, 160, 255})},
      {"colored_box", Mesh::colored_box(1.0f, 1.0f, 1.0f, Color{120, 140, 160, 255})},
      {"textured_plane", Mesh::textured_plane(2.0f, 2.0f, Color{200, 200, 200, 255})},
  };
  for (const Case& c : cases) {
    const Winding w = analyze(c.mesh);
    st::log::out("[winding] {:<16} same={:4} opposite={:4} degenerate={:3}\n", c.name, w.same,
              w.opposite, w.degenerate);
    // 反向三角形一个都不允许（这就是 `CullMode::Back` 会把物体剔成内壁的原因）
    ST_CHECK(w.opposite == 0);
    // 且必须真的检查到了三角形（防止“空网格恒通过”——那是最典型的假绿）
    ST_CHECK(w.same > 0);
  }
}

ST_TEST(mesh_sphere_winding_matches_normals) {
  // 球体单独再测一遍，因为它正是曾经出错的那个。
  // 断言细化到“同向数 == 总三角形数”，不给“大部分对”留口子。
  const Mesh sphere = Mesh::sphere(1.0f, 12);
  const Winding w = analyze(sphere);
  ST_CHECK(w.opposite == 0);
  ST_CHECK(w.same > 100);   // 12 段球至少上百个三角形
}
