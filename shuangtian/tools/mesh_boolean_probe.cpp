// 布尔运算隔离验证：数学正确性（体积/拓扑）+ 确定性（逐字节）。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/raster/mesh.hpp"
#include "st/raster/mesh_ops.hpp"

namespace {

constexpr st::math::Color kWhite{255, 255, 255, 255};

/// 散度定理体积。
auto volume_of(const st::raster::Mesh& mesh) -> double {
  double volume = 0.0;
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const auto at = [&](std::size_t index) {
      const std::size_t base = static_cast<std::size_t>(mesh.indices[index]) * 9U;
      return st::math::Vec3{mesh.vertices[base], mesh.vertices[base + 1],
                            mesh.vertices[base + 2]};
    };
    const st::math::Vec3 a = at(i);
    const st::math::Vec3 b = at(i + 1);
    const st::math::Vec3 c = at(i + 2);
    volume += double(a.dot(b.cross(c))) / 6.0;
  }
  return std::abs(volume);
}

}  // namespace

auto main() -> int {
  int failures = 0;
  // 事实核验：Mesh::box 首三角形的平面法线（用绕序叉积）与体积符号。
  {
    const auto box = st::raster::Mesh::box(2, 2, 2);
    const auto at = [&](std::size_t index) {
      const std::size_t base = static_cast<std::size_t>(box.indices[index]) * 9U;
      return st::math::Vec3{box.vertices[base], box.vertices[base + 1],
                            box.vertices[base + 2]};
    };
    const st::math::Vec3 a = at(0);
    const st::math::Vec3 b = at(1);
    const st::math::Vec3 c = at(2);
    const st::math::Vec3 n = (b - a).cross(c - a).normalized();
    const double signed_volume = volume_of(box) * 1.0;
    // 若法线朝外（CCW 外看），绕序体积应为正。
    std::printf("box 首三角形：v0=(%.2f,%.2f,%.2f) 法线=(%.2f,%.2f,%.2f) |体积|=%.3f\n",
                double(a.x), double(a.y), double(a.z), double(n.x), double(n.y),
                double(n.z), signed_volume);
  }
  // 圆柱绕序核验：有向体积符号（正 = 法线朝外的闭合实体）。
  {
    const auto cyl = st::raster::Mesh::cylinder(0.5f, 3.0f, 16, kWhite);
    double signed_volume = 0.0;
    for (std::size_t i = 0; i + 2 < cyl.indices.size(); i += 3) {
      const auto at = [&](std::size_t index) {
        const std::size_t base = static_cast<std::size_t>(cyl.indices[index]) * 9U;
        return st::math::Vec3{cyl.vertices[base], cyl.vertices[base + 1],
                              cyl.vertices[base + 2]};
      };
      const st::math::Vec3 va = at(i);
      const st::math::Vec3 vb = at(i + 1);
      const st::math::Vec3 vc = at(i + 2);
      signed_volume += double(va.dot(vb.cross(vc))) / 6.0;
    }
    std::printf("cylinder 有向体积 = %.4f（正 = 法线朝外）\n", signed_volume);
  }

  const auto check = [&](const char* label, bool ok, const std::string& detail) {
    std::printf("%-34s %s %s\n", label, ok ? "[OK]" : "[FAIL]", detail.c_str());
    if (!ok) ++failures;
  };

  // ── 1. Subtract：立方体打贯通孔 ──
  // 立方 2³=8；圆柱 r=0.5 h=3（贯通）体积 π·0.25·2=π/2 ≈ 1.5708
  // 期望 ≈ 8 - 1.5708 = 6.4292（圆柱在立方面内的部分 h=2）。
  {
    const auto box = st::raster::Mesh::box(2, 2, 2);
    const auto drill = st::raster::Mesh::cylinder(0.5f, 3.0f, 24, kWhite);
    const auto holed = st::raster::boolean(box, drill, st::raster::BooleanOp::Subtract);
    if (!holed) {
      check("Subtract 立方-圆柱", false, holed.error().message);
    } else {
      const double got = volume_of(*holed);
      const double expected = 8.0 - 3.14159265358979 * 0.25 * 2.0;
      char detail[96];
      std::snprintf(detail, sizeof(detail), "体积 %.4f 期望 %.4f（差 %.2f%%）三角形 %zu",
                    got, expected, std::abs(got - expected) / expected * 100.0,
                    holed->indices.size() / 3U);
      check("Subtract 立方-圆柱（通孔）",
            std::abs(got - expected) / expected < 0.02, detail);
    }
  }
  // ── 1b. Subtract 半搭立方：A 去掉重叠半块 = 0.5 ──
  {
    const auto a = st::raster::Mesh::box(1, 1, 1);
    const auto b = st::raster::transform(a, st::math::Mat4::translation(st::math::Vec3{0.5f, 0.0f, 0.0f}));
    const auto cut = st::raster::boolean(a, b, st::raster::BooleanOp::Subtract);
    if (!cut) {
      check("Subtract 半搭立方", false, cut.error().message);
    } else {
      const double got = volume_of(*cut);
      const double expected = 0.5;  // A 去掉重叠半块
      char detail[64];
      std::snprintf(detail, sizeof(detail), "体积 %.4f 期望 %.4f", got, expected);
      check("Subtract 半搭立方", std::abs(got - expected) / expected < 0.02, detail);
    }
  }

  // ── 2b. Union 半搭立方 ──
  {
    const auto a = st::raster::Mesh::box(1, 1, 1);
    const auto b = st::raster::transform(a, st::math::Mat4::translation(st::math::Vec3{0.5f, 0.0f, 0.0f}));
    const auto merged = st::raster::boolean(a, b, st::raster::BooleanOp::Union);
    if (!merged) {
      check("Union 半搭立方", false, merged.error().message);
    } else {
      const double got = volume_of(*merged);
      const double expected = 1.5;  // 1 + 1 - 0.5（重叠半块）
      char detail[64];
      std::snprintf(detail, sizeof(detail), "体积 %.4f 期望 %.4f", got, expected);
      check("Union 半搭立方", std::abs(got - expected) / expected < 0.02, detail);
    }
  }

  // ── 3. Intersect：交叉立方 ──
  {
    const auto a = st::raster::Mesh::box(1, 1, 1);
    const auto b = st::raster::transform(a, st::math::Mat4::translation(st::math::Vec3{0.5f, 0.0f, 0.0f}));
    const auto inter = st::raster::boolean(a, b, st::raster::BooleanOp::Intersect);
    if (!inter) {
      check("Intersect 交叉立方", false, inter.error().message);
    } else {
      const double got = volume_of(*inter);
      const double expected = 0.5;
      char detail[64];
      std::snprintf(detail, sizeof(detail), "体积 %.4f 期望 %.4f", got, expected);
      check("Intersect 交叉立方", std::abs(got - expected) / expected < 0.02, detail);
    }
  }

  // ── 4. 确定性：同一输入两次运算逐字节相同 ──
  {
    const auto box = st::raster::Mesh::box(2, 2, 2);
    const auto drill = st::raster::Mesh::cylinder(0.4f, 3.0f, 16, kWhite);
    const auto first = st::raster::boolean(box, drill, st::raster::BooleanOp::Subtract);
    const auto second = st::raster::boolean(box, drill, st::raster::BooleanOp::Subtract);
    if (!first || !second) {
      check("确定性", false, "运算失败");
    } else {
      const bool same = first->vertices == second->vertices &&
                        first->indices == second->indices;
      check("确定性（逐字节）", same, same ? "两次输出完全一致" : "输出漂移！");
    }
  }

  // ── 5. 不相交快速路：Union 走包围盒捷径 ──
  {
    const auto a = st::raster::Mesh::box(1, 1, 1);
    const auto b = st::raster::transform(a, st::math::Mat4::translation(st::math::Vec3{10.0f, 0.0f, 0.0f}));
    const auto merged = st::raster::boolean(a, b, st::raster::BooleanOp::Union);
    if (!merged) {
      check("不相交 Union 快速路", false, merged.error().message);
    } else {
      const double got = volume_of(*merged);
      check("不相交 Union 快速路", std::abs(got - 2.0) < 1e-6,
            "体积 2.0（两块独立）");
    }
    const auto inter = st::raster::boolean(a, b, st::raster::BooleanOp::Intersect);
    check("不相交 Intersect 报错", !inter.has_value(), "空集被拒（指路）");
  }

  // ── 6. 开放面拒绝 ──
  {
    const auto box = st::raster::Mesh::box(1, 1, 1);
    // Plane 是无厚度开放面
    const auto plane = st::raster::Mesh::plane(2, 2, kWhite);
    const auto bad = st::raster::boolean(box, plane, st::raster::BooleanOp::Union);
    check("开放面（Plane）被拒", !bad.has_value(), "水密校验生效");
  }

  // ── 7. 链式：一基体多工具（法兰四孔）──
  {
    const auto plate = st::raster::Mesh::box(2, 0.3f, 2);
    std::vector<st::raster::Mesh> operands;
    operands.push_back(plate);
    for (const auto& [dx, dz] : {std::pair{0.7f, 0.7f}, {-0.7f, 0.7f},
                                {0.7f, -0.7f}, {-0.7f, -0.7f}}) {
      auto drill = st::raster::Mesh::cylinder(0.12f, 1.0f, 12, kWhite);
      operands.push_back(st::raster::transform(
          drill, st::math::Mat4::translation(st::math::Vec3{dx, 0.0f, dz})));
    }
    const auto flange = st::raster::boolean_chain(operands,
                                                  st::raster::BooleanOp::Subtract);
    if (!flange) {
      check("链式四孔（法兰）", false, flange.error().message);
    } else {
      const double got = volume_of(*flange);
      const double hole = 3.14159265358979 * 0.12 * 0.12 * 0.3;
      const double expected = 2.0 * 0.3 * 2.0 - 4 * hole;
      char detail[80];
      std::snprintf(detail, sizeof(detail), "体积 %.4f 期望 %.4f（4 孔）", got,
                    expected);
      check("链式四孔（法兰）", std::abs(got - expected) / expected < 0.03, detail);
    }
  }

  std::printf("\n%s\n", failures == 0 ? "全部通过" : "有失败项");
  return failures == 0 ? 0 : 1;
}
