// `scene_constraint_test` —— 装配约束（attach/connect/align）的正确性判据。
//
// ## 测试口径
//
// 数值断言为主：解算后的 translate/rotation/size 必须与手推的几何期望
// 逐项吻合（台面高度 → 立柱中心高度这类可精确推导的量）。
// 联动断言：改约束目标的一个数值，下游全部自动跟随。
// 错误断言：前向引用/互斥/auto 误用必须被拒且消息能指路。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_constraints.hpp"
#include "st/raster/scene_serialize.hpp"
#include "st/raster/scene_xml.hpp"
#include "st/test/test.hpp"

namespace {

auto find_object(const st::raster::SceneSpec& spec, const std::string& name)
    -> const st::raster::SceneObjectSpec* {
  for (const auto& object : spec.objects) {
    if (object.name == name) return &object;
  }
  return nullptr;
}

}  // namespace

// ── 1. attach 链：坐面逐级咬合 ───────────────────────────────────────────────
ST_TEST(scene_constraint_attach_chain) {
  const auto spec = st::raster::parse_scene_spec_text(R"j({
    "objects": [
      {"name":"table","shape":"box","size":[2,0.2,2],"translate":[0,0.5,0]},
      {"name":"gear","shape":"gear","size":[0.8,0.1],"teeth":10,
     "attach":"top_of:table+0,0.05,0"},
      {"name":"post","shape":"cylinder","size":[0.1,0.6],"attach":"top_of:gear"},
      {"name":"lamp","shape":"sphere","size":0.3,"attach":"top_of:post"}
    ]})j");
  ST_REQUIRE(spec.has_value());
  const auto* gear = find_object(*spec, "gear");
  const auto* post = find_object(*spec, "post");
  const auto* lamp = find_object(*spec, "lamp");
  ST_REQUIRE(gear != nullptr);
  ST_REQUIRE(post != nullptr);
  ST_REQUIRE(lamp != nullptr);
  // table top=0.6, +offset 0.05 → 0.65; gear 半高 0.05 → 中心 0.70
  ST_CHECK_NEAR(gear->translate.y, 0.70f, 1e-4f);
  // gear top=0.75; post 半高 0.3 → 1.05
  ST_CHECK_NEAR(post->translate.y, 1.05f, 1e-4f);
  // post top=1.35; lamp 半径 0.15 → 1.50
  ST_CHECK_NEAR(lamp->translate.y, 1.50f, 1e-4f);
  // XZ 对齐（默认贴目标锚点的 x/z）
  ST_CHECK_NEAR(gear->translate.x, 0.0f, 1e-4f);
}

// ── 2. attach 联动：改台面高度，全链跟随 ────────────────────────────────────
ST_TEST(scene_constraint_attach_follow) {
  const auto make = [](float table_y) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  R"j({"objects":[
                    {"name":"table","shape":"box","size":[2,0.2,2],"translate":[0,%f,0]},
                    {"name":"post","shape":"cylinder","size":[0.1,0.6],"attach":"top_of:table"}
                  ]})j",
                  double(table_y));
    return st::raster::parse_scene_spec_text(buffer);
  };
  const auto low = make(0.5f);
  const auto high = make(0.8f);
  ST_REQUIRE(low.has_value());
  ST_REQUIRE(high.has_value());
  const auto* post_low = find_object(*low, "post");
  const auto* post_high = find_object(*high, "post");
  ST_REQUIRE(post_low != nullptr);
  ST_REQUIRE(post_high != nullptr);
  // 台面 +0.3 → post 中心也 +0.3（约束的意义：改一处、全链自动跟随）。
  ST_CHECK_NEAR(post_high->translate.y - post_low->translate.y, 0.3f, 1e-4f);
  // 绝对值也对得上：top(0.5+0.1)=0.6 → post 中心 0.6+0.3=0.9。
  ST_CHECK_NEAR(post_low->translate.y, 0.9f, 1e-4f);
}

// ── 3. connect：中点/长度/旋转（+auto 长度）────────────────────────────────
ST_TEST(scene_constraint_connect) {
  const auto spec = st::raster::parse_scene_spec_text(R"j({
    "objects": [
      {"name":"a","shape":"box","size":[1,1,1],"translate":[0,0,0]},
      {"name":"b","shape":"box","size":[1,1,1],"translate":[3,1,0]},
      {"name":"rod","shape":"cylinder","size":[0.1,"auto"],
       "connect":"top_of:a -> top_of:b"}
    ]})j");
  ST_REQUIRE(spec.has_value());
  const auto* rod = find_object(*spec, "rod");
  ST_REQUIRE(rod != nullptr);
  // a top=(0,0.5,0), b top=(3,1.5,0)：距离=√(9+1)=3.1623；中点=(1.5,1.0,0)
  const float expected = std::sqrt(9.0f + 1.0f);
  ST_CHECK_NEAR(rod->size.y, expected, 1e-4f);
  ST_CHECK_NEAR(rod->translate.x, 1.5f, 1e-4f);
  ST_CHECK_NEAR(rod->translate.y, 1.0f, 1e-4f);
  // 方向 A→B = (3,1,0) 归一化。绕 Z 把 +Y 压向 +X：z = -atan2(3,1)（弧度）。
  const float expected_z_rad = -std::atan2(3.0f, 1.0f);
  ST_CHECK_NEAR(rod->rotation.z, expected_z_rad, 1e-3f);
  ST_CHECK_NEAR(rod->rotation.x, 0.0f, 1e-3f);
  ST_CHECK_NEAR(rod->rotation.y, 0.0f, 1e-3f);
}

// ── 4. 前向引用被拒（一遍拓扑序、有环必报）────────────────────────────────
ST_TEST(scene_constraint_forward_ref_rejected) {
  const auto bad = st::raster::parse_scene_spec_text(
      R"j({"objects":[{"name":"a","shape":"sphere","attach":"top_of:b"},
                      {"name":"b","shape":"box"}]})j");
  ST_REQUIRE(!bad.has_value());
  ST_CHECK(bad.error().message.find("b") != std::string::npos);
  ST_CHECK(bad.error().message.find("先出现") != std::string::npos);

  // 找不到名字
  const auto missing = st::raster::parse_scene_spec_text(
      R"j({"objects":[{"name":"a","shape":"sphere","attach":"top_of:ghost"}]})j");
  ST_REQUIRE(!missing.has_value());
  ST_CHECK(missing.error().message.find("ghost") != std::string::npos);
}

// ── 5. 互斥与 auto 误用 ─────────────────────────────────────────────────────
ST_TEST(scene_constraint_mutex_and_auto) {
  const auto both = st::raster::parse_scene_spec_text(
      R"j({"objects":[{"name":"t","shape":"box"},
                      {"name":"a","shape":"sphere","attach":"top_of:t","align":"axis:t"}]})j");
  ST_CHECK(!both.has_value());

  const auto auto_without_connect = st::raster::parse_scene_spec_text(
      R"j({"objects":[{"name":"t","shape":"box"},
                      {"name":"a","shape":"tube","size":[0.1,"auto"]}]})j");
  ST_REQUIRE(!auto_without_connect.has_value());
  ST_CHECK(auto_without_connect.error().message.find("auto") != std::string::npos);
}

// ── 6. align：轴线对齐（XZ 对准、Y 保留）───────────────────────────────────
ST_TEST(scene_constraint_align) {
  const auto spec = st::raster::parse_scene_spec_text(R"j({
    "objects": [
      {"name":"shaft","shape":"cylinder","size":[0.1,2],"translate":[0.5,1,0.5]},
      {"name":"cap","shape":"cylinder","size":[0.3,0.2],"translate":[2,3,2],
       "align":"axis:shaft"}
    ]})j");
  ST_REQUIRE(spec.has_value());
  const auto* cap = find_object(*spec, "cap");
  ST_REQUIRE(cap != nullptr);
  ST_CHECK_NEAR(cap->translate.x, 0.5f, 1e-4f);
  ST_CHECK_NEAR(cap->translate.z, 0.5f, 1e-4f);
  ST_CHECK_NEAR(cap->translate.y, 3.0f, 1e-4f);  // Y 保留
}

// ── 7. 锚点解析（词表/偏移/大小写）─────────────────────────────────────────
ST_TEST(scene_constraint_anchor_parse) {
  const auto full = st::raster::parse_anchor_ref("top_of:plinth+0,0.1,-0.2");
  ST_REQUIRE(full.has_value());
  ST_CHECK(full->anchor.value == st::raster::AnchorKind::Top);
  ST_CHECK_EQ(full->target, std::string("plinth"));
  ST_CHECK_NEAR(full->offset.y, 0.1f, 1e-6f);
  ST_CHECK_NEAR(full->offset.z, -0.2f, 1e-6f);

  const auto bare = st::raster::parse_anchor_ref("table");
  ST_REQUIRE(bare.has_value());
  ST_CHECK(bare->anchor.value == st::raster::AnchorKind::Bottom);  // 默认
  ST_CHECK_EQ(bare->target, std::string("table"));

  const auto bad = st::raster::parse_anchor_ref("elbow_of:pipe");
  ST_CHECK(!bad.has_value());

  const auto caseless = st::raster::parse_anchor_ref("TOP_OF:x");
  ST_REQUIRE(caseless.has_value());
  ST_CHECK(caseless->anchor.value == st::raster::AnchorKind::Top);
}

// ── 8. rotation_to_point_y 互逆性（引擎欧拉序）──────────────────────────────
ST_TEST(scene_constraint_rotation_inverse) {
  const st::math::Vec3 cases[] = {
      {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.7071f, 0.7071f, 0.0f},
      {-0.133f, -0.886f, -0.443f}, {0.0f, -1.0f, 0.0f},
  };
  for (const st::math::Vec3& direction : cases) {
    const st::math::Vec3 euler_deg = st::raster::rotation_to_point_y(direction);
    // from_euler 回旋 +Y 应指回 direction
    constexpr float kRad = 3.14159265358979f / 180.0f;
    const st::math::Quat q = st::math::Quat::from_euler(
        st::math::Vec3{euler_deg.x * kRad, euler_deg.y * kRad, euler_deg.z * kRad});
    const st::math::Vec3 u{q.x, q.y, q.z};
    const st::math::Vec3 v{0.0f, 1.0f, 0.0f};
    const st::math::Vec3 c1 = u.cross(v);
    const st::math::Vec3 c2 = u.cross(c1);
    const st::math::Vec3 moved = v + c1 * (2.0f * q.w) + c2 * 2.0f;
    const st::math::Vec3 want = direction.normalized();
    ST_CHECK((moved - want).length() < 1e-3f);
  }
}

// ── 9. XML 形态等价 + 往返 ──────────────────────────────────────────────────
ST_TEST(scene_constraint_xml_equivalence) {
  const auto from_xml = st::raster::parse_scene_xml(R"XML(<Scene>
  <Box name="table" size="2, 0.2, 2" translate="0, 0.5, 0"/>
  <Cylinder name="post" size="0.1, 0.6" attach="top_of:table"/>
  <Tube name="cable" connect="right_of:post -> front_of:table" size="0.05, auto"/>
</Scene>)XML");
  const auto from_json = st::raster::parse_scene_spec_text(R"j({
    "objects":[
      {"name":"table","shape":"box","size":[2,0.2,2],"translate":[0,0.5,0]},
      {"name":"post","shape":"cylinder","size":[0.1,0.6],"attach":"top_of:table"},
      {"name":"cable","shape":"tube","connect":"right_of:post -> front_of:table",
       "size":[0.05,"auto"]}
    ]})j");
  ST_REQUIRE(from_xml.has_value());
  ST_REQUIRE(from_json.has_value());
  const auto* post_x = find_object(*from_xml, "post");
  const auto* post_j = find_object(*from_json, "post");
  ST_REQUIRE(post_x != nullptr);
  ST_REQUIRE(post_j != nullptr);
  ST_CHECK_NEAR(post_x->translate.y, post_j->translate.y, 1e-4f);
  const auto* cable_x = find_object(*from_xml, "cable");
  const auto* cable_j = find_object(*from_json, "cable");
  ST_REQUIRE(cable_x != nullptr);
  ST_REQUIRE(cable_j != nullptr);
  ST_CHECK_NEAR(cable_x->size.y, cable_j->size.y, 1e-4f);

  // 解算结果 serialize 后可再读回（约束字段保留、数值不漂）
  const std::string text = st::raster::serialize_scene_text(*from_xml, true);
  const auto reparsed = st::raster::parse_scene_spec_text(text);
  ST_REQUIRE(reparsed.has_value());
  const auto* post2 = find_object(*reparsed, "post");
  ST_REQUIRE(post2 != nullptr);
  ST_CHECK_NEAR(post2->translate.y, post_x->translate.y, 1e-4f);
}
