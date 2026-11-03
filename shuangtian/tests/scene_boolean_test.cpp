// `scene_boolean_test` —— 表达能力三件套的判据：布尔 / 草图挤出旋转 / 阵列。
//
// ## 测试口径
//
// 体积断言为主（散度定理，与解析值对比，容差按多边形逼近给 1~3%）：
// 布尔三运算、挤出封闭性、车削体积、阵列份数。再补确定性、错误报路、
// 与约束/Part 的组合。
#include <cmath>
#include <string>

#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_xml.hpp"
#include "st/test/test.hpp"

namespace {

constexpr st::math::Color kWhite{255, 255, 255, 255};

/// 有向体积的绝对值（散度定理）。
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

}  // namespace

// ── 1. 布尔三运算的体积语义（半搭立方：重叠 0.5）────────────────────────────
ST_TEST(scene_boolean_three_ops_volume) {
  const auto a = st::raster::Mesh::box(1, 1, 1);
  const auto b = st::raster::transform(
      a, st::math::Mat4::translation(st::math::Vec3{0.5f, 0.0f, 0.0f}));

  const auto unioned = st::raster::boolean(a, b, st::raster::BooleanOp::Union);
  ST_REQUIRE(unioned.has_value());
  ST_CHECK(near_ratio(volume_of(*unioned), 1.5, 0.02));

  const auto cut = st::raster::boolean(a, b, st::raster::BooleanOp::Subtract);
  ST_REQUIRE(cut.has_value());
  ST_CHECK(near_ratio(volume_of(*cut), 0.5, 0.02));

  const auto overlap = st::raster::boolean(a, b, st::raster::BooleanOp::Intersect);
  ST_REQUIRE(overlap.has_value());
  ST_CHECK(near_ratio(volume_of(*overlap), 0.5, 0.02));
}

// ── 2. 通孔：立方减贯通圆柱（多边形逼近 24 段，容差 2%）────────────────────
ST_TEST(scene_boolean_through_hole) {
  const auto box = st::raster::Mesh::box(2, 2, 2);
  const auto drill = st::raster::Mesh::cylinder(0.5f, 3.0f, 24, kWhite);
  const auto holed = st::raster::boolean(box, drill, st::raster::BooleanOp::Subtract);
  ST_REQUIRE(holed.has_value());
  const double expected = 8.0 - 3.141592653589793 * 0.25 * 2.0;
  ST_CHECK(near_ratio(volume_of(*holed), expected, 0.02));
  // 通孔意味着顶点数远多于原立方（有切割）
  ST_CHECK(holed->vertices.size() / 9U > 24U);
}

// ── 3. 布尔确定性（同输入两次逐字节相同）─────────────────────────────────
ST_TEST(scene_boolean_deterministic) {
  const auto box = st::raster::Mesh::box(2, 2, 2);
  const auto drill = st::raster::Mesh::cylinder(0.4f, 3.0f, 16, kWhite);
  const auto first = st::raster::boolean(box, drill, st::raster::BooleanOp::Subtract);
  const auto second = st::raster::boolean(box, drill, st::raster::BooleanOp::Subtract);
  ST_REQUIRE(first.has_value());
  ST_REQUIRE(second.has_value());
  ST_CHECK(first->vertices == second->vertices);
  ST_CHECK(first->indices == second->indices);
}

// ── 4. 开放面被拒（水密校验）─────────────────────────────────────────────
ST_TEST(scene_boolean_rejects_open_mesh) {
  const auto box = st::raster::Mesh::box(1, 1, 1);
  const auto plane = st::raster::Mesh::plane(2, 2, kWhite);
  const auto bad = st::raster::boolean(box, plane, st::raster::BooleanOp::Union);
  ST_REQUIRE(!bad.has_value());
  ST_CHECK(bad.error().message.find("闭合") != std::string::npos);
}

// ── 5. XML <Subtract> 标签 + 布尔对象真的有几何（渲染可见）────────────────
ST_TEST(scene_boolean_tag_geometry) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Subtract name="plate">
    <Box size="2, 0.3, 2"/>
    <Cylinder size="0.5, 1"/>
  </Subtract>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto* plate = find_object(*spec, "plate");
  ST_REQUIRE(plate != nullptr);
  ST_CHECK_EQ(plate->op, std::string("subtract"));
  // 布尔对象必须是「有几何」的实体（不能因为 group=true 被渲染跳过）：
  // 口径 = shape 是 group 但 op 非空（与 to_node/paint 同一判据）。
  ST_CHECK(plate->group);
  ST_CHECK(!plate->op.empty());
  // 且网格真的能建出来
  const auto mesh = st::raster::build_mesh(*plate);
  ST_REQUIRE(mesh.has_value());
  // 板 2×0.3×2 = 1.2。孔 `<Cylinder size="0.5, 1"/>`：size[0] 是**直径**
  //（与 schema 一致），半径 0.25；贯穿板厚 0.3 → 减 π·0.0625·0.3。
  const double expected = 1.2 - 3.141592653589793 * 0.0625 * 0.3;
  ST_CHECK(near_ratio(volume_of(*mesh), expected, 0.05));
}

// ── 6. 布尔操作数不足 / op 拼错 ──────────────────────────────────────────
ST_TEST(scene_boolean_errors) {
  const auto too_few = st::raster::parse_scene_xml(
      "<Scene><Subtract name=\"a\"><Box/></Subtract></Scene>");
  ST_REQUIRE(too_few.has_value());  // 解析过（报错在 build_mesh 阶段）
  const auto mesh = st::raster::build_mesh(*find_object(*too_few, "a"));
  ST_REQUIRE(!mesh.has_value());
  ST_CHECK(mesh.error().message.find("至少需要 2") != std::string::npos);

  const auto bad_op = st::raster::parse_scene_spec_text(
      R"j({"objects":[{"name":"a","shape":"box","op":"divide","children":[]}]})j");
  ST_REQUIRE(!bad_op.has_value());
  ST_CHECK(bad_op.error().message.find("divide") != std::string::npos);
}

// ── 7. Extrude：凹轮廓（L 形）体积 + 封闭 ────────────────────────────────
ST_TEST(scene_extrude_concave_profile) {
  // L 形轮廓（面积 = 1.0×0.15 + 0.15×0.65 = 0.2475），拉伸深度 size[1]=0.3
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Extrude name="bracket"
           profile="-0.5,-0.4 0.5,-0.4 0.5,-0.25 -0.35,-0.25 -0.35,0.4 -0.5,0.4"
           size="1, 0.3, 0.8"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto mesh = st::raster::build_mesh(*find_object(*spec, "bracket"));
  ST_REQUIRE(mesh.has_value());
  // 轮廓面积：宽 1.0 高 0.15 的横臂 + 宽 0.15 高 0.65 的立臂
  const double area = 1.0 * 0.15 + 0.15 * 0.65;
  const double expected = area * 0.3;
  ST_CHECK(near_ratio(volume_of(*mesh), expected, 0.05));
  // 封闭体（耳切封盖生效）：顶点数 > 轮廓点数 × 2
  ST_CHECK(mesh->vertices.size() / 9U >= 12U);
}

// ── 8. Revolve：旋转体体积（圆柱轮廓应还原成圆柱）──────────────────────────
ST_TEST(scene_revolve_cylinder) {
  // 轮廓 (r=0.5, y=0) → (0.5, 1)：绕 Y 转一圈 = 半径 0.5、高 1 的圆柱
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Revolve name="barrel" profile="0.5,0 0.5,1" segments="32"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto mesh = st::raster::build_mesh(*find_object(*spec, "barrel"));
  ST_REQUIRE(mesh.has_value());
  const double expected = 3.141592653589793 * 0.25 * 1.0;
  ST_CHECK(near_ratio(volume_of(*mesh), expected, 0.03));
}

// ── 9. 阵列：环 N 份 / 行 N 份（顶点数线性增长）───────────────────────────
ST_TEST(scene_pattern_counts) {
  const auto ring = st::raster::parse_scene_xml(R"XML(<Scene>
  <Ring name="fins" pattern_count="8" pattern_radius="1">
    <Box size="0.2, 0.4, 0.1"/>
  </Ring>
</Scene>)XML");
  ST_REQUIRE(ring.has_value());
  const auto ring_mesh = st::raster::build_mesh(*find_object(*ring, "fins"));
  ST_REQUIRE(ring_mesh.has_value());
  const auto single = st::raster::Mesh::box(0.2f, 0.4f, 0.1f);
  // 8 份 = 单个的 8 倍顶点（无焊接时精确 8×；merge 不去重）
  ST_CHECK(ring_mesh->vertices.size() >= single.vertices.size() * 8U);
  // 环铺开：包围盒 X 跨度应接近 2×radius
  const auto box = st::raster::bounds(*ring_mesh);
  ST_CHECK(box.size().x > 1.8f);

  const auto row = st::raster::parse_scene_xml(R"XML(<Scene>
  <Row name="steps" pattern_count="5" pattern_step="0.5, 0, 0">
    <Box size="0.3, 0.1, 0.3"/>
  </Row>
</Scene>)XML");
  ST_REQUIRE(row.has_value());
  const auto row_mesh = st::raster::build_mesh(*find_object(*row, "steps"));
  ST_REQUIRE(row_mesh.has_value());
  const auto row_box = st::raster::bounds(*row_mesh);
  // 5 份 × 0.5 间距 + 单件 0.3 → 跨度 2.3
  ST_CHECK_NEAR(row_box.size().x, 2.3f, 0.02f);
}

// ── 10. 阵列 + 布尔组合（环形孔阵列 = 一整块挖一圈孔）──────────────────────
ST_TEST(scene_pattern_inside_boolean) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Subtract name="perforated">
    <Cylinder size="2, 0.3"/>
    <Ring name="holes" pattern_count="6" pattern_radius="0.6">
      <Cylinder size="0.2, 1"/>
    </Ring>
  </Subtract>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto mesh = st::raster::build_mesh(*find_object(*spec, "perforated"));
  ST_REQUIRE(mesh.has_value());
  // 盘 π·1·0.3=0.9425，减 6 孔 π·0.01·0.3×6=0.0565
  const double expected = 3.141592653589793 * 1.0 * 0.3 -
                          3.141592653589793 * 0.01 * 0.3 * 6.0;
  ST_CHECK(near_ratio(volume_of(*mesh), expected, 0.06));
}

// ── 11. profile 解析错误报路 ────────────────────────────────────────────
ST_TEST(scene_profile_errors) {
  // 点太少（extrude 需 3 点）
  const auto short_profile = st::raster::parse_scene_xml(
      "<Scene><Extrude name=\"x\" profile=\"0,0 1,0\" size=\"0.2\"/></Scene>");
  ST_REQUIRE(short_profile.has_value());
  const auto mesh = st::raster::build_mesh(*find_object(*short_profile, "x"));
  ST_REQUIRE(!mesh.has_value());
  ST_CHECK(mesh.error().message.find("至少需要 3") != std::string::npos);

  // 缺逗号
  const auto bad_point = st::raster::parse_scene_xml(
      "<Scene><Extrude name=\"y\" profile=\"0 0 1,0 1,1\" size=\"0.2\"/></Scene>");
  ST_REQUIRE(bad_point.has_value());
  const auto mesh2 = st::raster::build_mesh(*find_object(*bad_point, "y"));
  ST_REQUIRE(!mesh2.has_value());
  ST_CHECK(mesh2.error().message.find("逗号") != std::string::npos);

  // 没有 profile
  const auto no_profile = st::raster::parse_scene_xml(
      "<Scene><Extrude name=\"z\" size=\"0.2\"/></Scene>");
  ST_REQUIRE(no_profile.has_value());
  const auto mesh3 = st::raster::build_mesh(*find_object(*no_profile, "z"));
  ST_REQUIRE(!mesh3.has_value());
  ST_CHECK(mesh3.error().message.find("profile") != std::string::npos);
}

// ── 12. 布尔/阵列的子元素**不得**进场景树（否则被重复绘制）───────────────
//
// 子元素（操作体/原型）的几何已经被 `build_mesh` 烤进父对象结果网格；
// 若还进场景树就会被当独立实体再画一次。实测的用户可见症状：
// Subtract 的挖孔圆柱变成一个纯白的鼓状物、阵列原型额外出现在原点
//——两者都叠在成品上，看着像“一堆白鼓”与“螺旋黑线”。
ST_TEST(scene_operand_children_not_in_tree) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Subtract name="plate">
    <Box name="base_part" size="2, 0.3, 2"/>
    <Cylinder name="hole_part" size="0.5, 1"/>
  </Subtract>
  <Ring name="ring" pattern_count="8" pattern_radius="1.5">
    <Box name="proto" size="0.2, 0.2, 0.2"/>
  </Ring>
  <Group name="real_group">
    <Box name="real_child" size="0.2, 0.2, 0.2"/>
  </Group>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());

  // 渲染入口把 spec 转成场景树：布尔/阵列的 children 应被排除，
  // 而普通 Group 的 children 必须保留（层级装配靠它）。
  const auto names = st::raster::collect_drawable_node_names(*spec);
  auto has = [&](const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
  };
  // 操作体 / 原型不得出现
  ST_CHECK(!has("base_part"));
  ST_CHECK(!has("hole_part"));
  ST_CHECK(!has("proto"));
  // 父对象本身（有实几何）与普通 group 的子元素必须出现
  ST_CHECK(has("plate"));
  ST_CHECK(has("ring"));
  ST_CHECK(has("real_child"));
}
