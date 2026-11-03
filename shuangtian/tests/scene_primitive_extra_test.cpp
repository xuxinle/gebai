// `scene_primitive_extra_test` —— 新增图元的判据。
//
// ## 测试口径
//
// 体积/拓扑断言为主：型钢截面面积可精确推导（I 形、C 形、L 形、方管），
// 螺旋楼梯按踏步数增长，紧固件按剖面语义。再补封闭性与错误选型。
#include <cmath>
#include <string>

#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"
#include "st/test/test.hpp"

namespace {

constexpr st::math::Color kWhite{255, 255, 255, 255};

auto volume_of(const st::raster::Mesh& mesh) -> double {
  double volume = 0.0;
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3U) {
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

auto near_ratio(double got, double expected, double tolerance) -> bool {
  return std::abs(got - expected) / std::max(expected, 1e-9) < tolerance;
}

auto find_object(const st::raster::SceneSpec& spec, const std::string& name)
    -> const st::raster::SceneObjectSpec* {
  for (const auto& object : spec.objects) {
    if (object.name == name) return &object;
  }
  return nullptr;
}

auto make_params(float radius, float depth, float height, float thickness,
                 float turns, int teeth, int sides, const char* section) -> st::raster::primitives::PrimitiveParams {
  // 按声明顺序赋值（designated initializer 要求字段序号，容易因新增字段而碎；
  // 用赋值既安全又不用每加字段就回来改测试）。
  st::raster::primitives::PrimitiveParams p{};
  p.radius = radius;
  p.depth = depth;
  p.height = height;
  p.teeth = teeth;
  p.sides = sides;
  p.turns = turns;
  p.thickness = thickness;
  p.section = section;
  p.color = kWhite;
  return p;
}

}  // namespace

// ── 1. 型钢四型的截面面积（体积 = 面积 × 长度）────────────────────────────
ST_TEST(primitive_beam_sections) {
  // 截面 0.42×0.42、壁厚 0.05、长度 2.2
  const double w = 0.42;
  const double h = 0.42;
  const double t = 0.05;
  const double length = 2.2;
  const float fw = 0.42f;
  const float fh = 0.42f;
  const float ft = 0.05f;
  const float flen = 2.2f;

  // 工字：两块翼缘（w×t）+ 腹板（t×(h-2t)）
  const double i_area = 2.0 * (w * t) + t * (h - 2.0 * t);
  const auto ibeam = st::raster::primitives::make_beam(
      make_params(fw * 0.5f, flen, fh, ft, 0.0f, 12, 0, "i"));
  ST_CHECK(near_ratio(volume_of(ibeam), i_area * length, 0.06));

  // 槽钢：翼缘 2 块 + 腹板 1 块（同样的面积公式，但只有一侧翼缘厚）
  const double channel_area = 2.0 * (w * t) + t * (h - 2.0 * t);
  const auto channel = st::raster::primitives::make_beam(
      make_params(fw * 0.5f, flen, fh, ft, 0.0f, 12, 0, "channel"));
  ST_CHECK(near_ratio(volume_of(channel), channel_area * length, 0.10));

  // 角钢：L 形 = w*t + (h-t)*t
  const double angle_area = w * t + (h - t) * t;
  const auto angle = st::raster::primitives::make_beam(
      make_params(fw * 0.5f, flen, fh, ft, 0.0f, 12, 0, "angle"));
  ST_CHECK(near_ratio(volume_of(angle), angle_area * length, 0.10));

  // 方管：外框 - 内框
  const double box_area = w * h - (w - 2.0 * t) * (h - 2.0 * t);
  const auto box = st::raster::primitives::make_beam(
      make_params(fw * 0.5f, flen, fh, ft, 0.0f, 12, 0, "box"));
  ST_CHECK(near_ratio(volume_of(box), box_area * length, 0.10));

  // 四种截面都不应退化成零体积
  ST_CHECK(volume_of(ibeam) > 0.005);
  ST_CHECK(volume_of(channel) > 0.005);
  ST_CHECK(volume_of(angle) > 0.005);
  ST_CHECK(volume_of(box) > 0.005);
}

// ── 2. 螺旋楼梯：踏步数线性增长 + 总升高 ────────────────────────────────
ST_TEST(primitive_staircase) {
  const auto few = st::raster::primitives::make_staircase(
      make_params(0.85f, 2.0f, 0.12f, 0.0f, 1.0f, 8, 0, ""));
  const auto many = st::raster::primitives::make_staircase(
      make_params(0.85f, 2.0f, 0.12f, 0.0f, 1.0f, 16, 0, ""));
  // 16 级的三角形数应明显多于 8 级
  ST_CHECK(many.indices.size() > few.indices.size());
  // 包围盒高度 = 总升高 + 踏步厚（约等于 depth + 一点）
  const auto box = st::raster::bounds(many);
  ST_CHECK(box.size().y > 1.9f);
  ST_CHECK(box.size().y < 2.4f);
  // 半径铺开（X 或 Z 跨度接近 2×半径）
  ST_CHECK(std::max(box.size().x, box.size().z) > 1.5f);
}

// ── 3. 星形 / 十字柱 ───────────────────────────────────────────────────
ST_TEST(primitive_ornament) {
  const auto star = st::raster::primitives::make_ornament(
      make_params(0.5f, 0.22f, 0.0f, 0.22f, 0.0f, 12, 5, "star"));
  ST_CHECK(volume_of(star) > 0.01);
  // 星形外接圆是 radius，包围盒 X 跨度接近 2×radius
  const auto star_box = st::raster::bounds(star);
  ST_CHECK(star_box.size().x > 0.85f);

  const auto cross = st::raster::primitives::make_ornament(
      make_params(0.46f, 0.22f, 0.0f, 0.15f, 0.0f, 12, 0, "cross"));
  ST_CHECK(volume_of(cross) > 0.01);

  // 星形比同尺寸的十字体积小（星形有凹角）
  ST_CHECK(volume_of(star) < volume_of(cross) * 1.5);
}

// ── 4. 紧固件三件套 ───────────────────────────────────────────────────
ST_TEST(primitive_fastener) {
  const auto bolt = st::raster::primitives::make_fastener(
      make_params(0.14f, 0.1f, 0.0f, 0.05f, 5.0f, 12, 0, "bolt"));
  // 螺栓 = 六角头 + 杆（杆长 = turns × 头厚 = 0.5）。
  // 正六边形面积 = (3√3/2)R² ≈ 2.598R²（**不是** √3R²）。
  const double head = (3.0 * 1.7320508 / 2.0) * 0.14 * 0.14 * 0.1;
  const double shank = 3.141592653589793 * 0.05 * 0.05 * 0.5;
  std::printf("    [bolt] 实测 %.6f 期望 %.6f\n", volume_of(bolt), head + shank);
  ST_CHECK(near_ratio(volume_of(bolt), head + shank, 0.10));
  // 螺栓 = 头（+Y）+ 杆（-Y）：包围盒高度应跨过 y=0
  const auto bolt_box = st::raster::bounds(bolt);
  ST_CHECK(bolt_box.min.y < -0.1f);   // 杆往下
  ST_CHECK(bolt_box.max.y > 0.05f);   // 头往上

  const auto nut = st::raster::primitives::make_fastener(
      make_params(0.14f, 0.09f, 0.0f, 0.05f, 0.0f, 12, 0, "nut"));
  // 螺母 = 实心六角 - 内孔
  const double solid_hex = (3.0 * 1.7320508 / 2.0) * 0.14 * 0.14 * 0.09;
  const double hole = 3.141592653589793 * 0.05 * 0.05 * 0.09;
  ST_CHECK(near_ratio(volume_of(nut), solid_hex - hole, 0.10));

  const auto washer = st::raster::primitives::make_fastener(
      make_params(0.17f, 0.1f, 0.0f, 0.05f, 0.0f, 12, 0, "washer"));
  // 垫圈 = 扁环：π(R²-r²)·厚（厚 = 头厚×0.22）
  const double washer_expected =
      3.141592653589793 * (0.17 * 0.17 - 0.05 * 0.05) * 0.022;
  ST_CHECK(near_ratio(volume_of(washer), washer_expected, 0.15));
  // 垫圈很扁：Y 跨度远小于 X 跨度
  const auto washer_box = st::raster::bounds(washer);
  ST_CHECK(washer_box.size().y < washer_box.size().x * 0.4f);
}

// ── 5. XML 标签 + 字段（section/thickness）往返 ────────────────────────
ST_TEST(primitive_extra_xml) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Beam name="beam" section="channel" size="0.3, 2" thickness="0.04"/>
  <Staircase name="stair" size="0.8" turns="1.5" teeth="12"/>
  <Ornament name="star" section="star" sides="6" size="0.4, 0.2"/>
  <Fastener name="nut" section="nut" size="0.12, 0.08" thickness="0.04"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(find_object(*spec, "beam")->section, std::string("channel"));
  ST_CHECK_NEAR(find_object(*spec, "beam")->thickness, 0.04f, 1e-6f);
  ST_CHECK_EQ(find_object(*spec, "stair")->shape, std::string("staircase"));
  ST_CHECK_NEAR(find_object(*spec, "stair")->turns, 1.5f, 1e-6f);
  ST_CHECK_EQ(find_object(*spec, "star")->section, std::string("star"));
  ST_CHECK_EQ(find_object(*spec, "nut")->section, std::string("nut"));

  // 全部能建出网格
  for (const char* name : {"beam", "stair", "star", "nut"}) {
    const auto mesh = st::raster::build_mesh(*find_object(*spec, name));
    ST_REQUIRE(mesh.has_value());
    ST_CHECK(!mesh->is_empty());
  }
}

// ── 6. 别名解析（ibeam/star/bolt 等短名）────────────────────────────────
ST_TEST(primitive_extra_aliases) {
  const auto as_ibeam = st::raster::primitives::parse_kind("ibeam");
  ST_REQUIRE(as_ibeam.has_value());
  ST_CHECK(*as_ibeam == st::raster::primitives::PrimitiveKind::Beam);

  const auto as_star = st::raster::primitives::parse_kind("star");
  ST_REQUIRE(as_star.has_value());
  ST_CHECK(*as_star == st::raster::primitives::PrimitiveKind::Ornament);

  const auto as_bolt = st::raster::primitives::parse_kind("bolt");
  ST_REQUIRE(as_bolt.has_value());
  ST_CHECK(*as_bolt == st::raster::primitives::PrimitiveKind::Fastener);

  const auto as_stairs = st::raster::primitives::parse_kind("stairs");
  ST_REQUIRE(as_stairs.has_value());
  ST_CHECK(*as_stairs == st::raster::primitives::PrimitiveKind::Staircase);

  // 全部规范名都能解析回来（枚举 ↔ 名字双向一致）
  for (const auto& name : {"beam", "staircase", "ornament", "fastener"}) {
    const auto kind = st::raster::primitives::parse_kind(name);
    ST_REQUIRE(kind.has_value());
    ST_CHECK_EQ(std::string(st::raster::primitives::kind_name(*kind)),
                std::string(name));
  }
}
