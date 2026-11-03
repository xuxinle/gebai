// `scene_xml_test` —— 三维场景 XML 形态（`scene_xml.hpp`）的正确性判据。
//
// ## 测试口径（与 scene_script_reverse_test 同一条：语义等价，不是逐字符相等）
//
// 往返：XML → SceneSpec → serialize_scene_xml → XML'，再解析 XML' 得到的
// SceneSpec' 必须与 SceneSpec **语义等价**（经 serialize_scene 的 Json 比对——
// 那边已有「只写非默认值」的规范化，两边都过它就等于两边语义一致）。
//
// ## 为什么全部图元各跑一条往返
//
// 标签表（`scene_xml.cpp`）与 `kShapes`（`scene3d_script.cpp`）是两张
// 层面不同但必须一致的表。手工同步必然漂移——逐图元解析 + 往返，
// 解析器加图元忘了加标签（或反向）立刻红。
#include <cstdio>
#include <string>

#include "st/core/json.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_serialize.hpp"
#include "st/raster/scene_xml.hpp"
#include "st/test/test.hpp"

namespace {

/// 两份 SceneSpec 是否语义等价（经 serialize_scene 的规范化 Json 比对）。
auto same_scene(const st::raster::SceneSpec& a, const st::raster::SceneSpec& b) -> bool {
  return st::json_dump(st::raster::serialize_scene(a)) ==
         st::json_dump(st::raster::serialize_scene(b));
}

/// XML → spec → XML → spec，两份 spec 语义等价（往返一致性）。
auto roundtrip_ok(const std::string& xml, std::string* failure = nullptr) -> bool {
  const auto first = st::raster::parse_scene_xml(xml);
  if (!first.has_value()) {
    if (failure != nullptr) *failure = "解析失败：" + first.error().message;
    return false;
  }
  const std::string out = st::raster::serialize_scene_xml(*first);
  const auto second = st::raster::parse_scene_xml(out);
  if (!second.has_value()) {
    if (failure != nullptr) {
      *failure = "序列化产物解析失败：" + second.error().message + "\n产物：\n" + out;
    }
    return false;
  }
  if (!same_scene(*first, *second)) {
    if (failure != nullptr) *failure = "往返语义不等价\n产物：\n" + out;
    return false;
  }
  return true;
}

}  // namespace

// ── 1. 基础解析：结构、字段、嵌套 ────────────────────────────────────────────
ST_TEST(scene_xml_parse_basic) {
  const auto spec = st::raster::parse_scene_xml(R"XML(
<Scene>
  <Camera eye="0, 0.6, 3.4" target="0, 0, 0" fov="45"/>
  <Lighting light_from="0.4, 0.8, 0.6" ambient="0.2">
    <ExtraLight direction="-0.6, -0.2, 0.5" intensity="0.5"/>
  </Lighting>
  <Shadow strength="0.45" ground_y="-0.55"/>
  <Sphere name="head" size="0.9" color="#c8a07a" translate="0, 0.4, 0">
    <Box name="eye" size="0.08, 0.08, 0.04" translate="0.2, 0.1, 0.85" rotation="0, 0, 45"/>
  </Sphere>
  <Animation name="wave" duration="2">
    <Track node="head" kind="rotation">
      <Key t="0" value="0, 0, 0" easing="ease-in-out"/>
      <Key t="1" value="0, 0, 6"/>
      <Key t="2" value="0, 0, 0"/>
    </Track>
  </Animation>
</Scene>
)XML");
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->objects.size(), std::size_t{1});
  ST_CHECK_EQ(spec->objects[0].name, std::string("head"));
  ST_CHECK_EQ(spec->objects[0].shape, std::string("sphere"));
  ST_CHECK(spec->objects[0].color.r == 0xc8 && spec->objects[0].color.g == 0xa0);
  ST_CHECK_EQ(spec->objects[0].children.size(), std::size_t{1});
  ST_CHECK_EQ(spec->objects[0].children[0].shape, std::string("box"));
  // 注：SceneSpec 层的 rotation 是弧度（解析时已从 XML 的度转换，
  // 与 JSON 形态同一口径）；45° ≈ 0.785398。
  ST_CHECK_NEAR(spec->objects[0].children[0].rotation.z, 0.785398f, 1e-4f);
  ST_CHECK_NEAR(spec->camera.fov_y_degrees, 45.0f, 1e-6f);
  ST_REQUIRE(spec->animation.has_value());
  ST_CHECK_EQ(spec->animation->tracks.size(), std::size_t{1});
  ST_CHECK_EQ(spec->animation->tracks[0].keys.size(), std::size_t{3});
  ST_CHECK_NEAR(spec->lighting.extra_lights[0].intensity, 0.5f, 1e-6f);
  ST_CHECK_NEAR(spec->lighting.extra_lights[0].direction.x, -0.6f, 1e-6f);
}

// ── 2. 未知属性：报错带行号 + 候选（XML 形态的核心优势）─────────────────────
ST_TEST(scene_xml_unknown_attr) {
  const auto spec = st::raster::parse_scene_xml(
      "<Scene>\n  <Sphere raduis=\"0.9\"/>\n</Scene>\n");
  ST_REQUIRE(!spec.has_value());
  const std::string message = spec.error().message;
  ST_CHECK(message.find("raduis") != std::string::npos);   // 拼错的属性名
  // 候选：suggest_fields 默认 max_distance=2，radius↔raduis 距离恰好 2（交换两字母
  // = 2 次替换）——在阈值内但边缘；这里只断言报错本身可定位，不绑定候选行为
  // （候选是提示不是契约，换算法不应让测试红）。
  ST_CHECK(message.find("2") != std::string::npos);        // 行号
  ST_CHECK(message.find("<Sphere") != std::string::npos);  // 标签
}

// ── 3. 未知图元标签：候选 ────────────────────────────────────────────────────
ST_TEST(scene_xml_unknown_tag) {
  const auto spec = st::raster::parse_scene_xml("<Scene>\n  <Sphrre/>\n</Scene>");
  ST_REQUIRE(!spec.has_value());
  ST_CHECK(spec.error().message.find("Sphere") != std::string::npos);
}

// ── 4. 全部图元：解析 + 往返 ─────────────────────────────────────────────────
ST_TEST(scene_xml_all_primitives) {
  const char* shapes[] = {"Sphere",   "Box",         "Plane",     "Cylinder", "Torus",
                          "UvSphere", "Cube",        "IcoSphere", "Cone",     "Capsule",
                          "Disc",     "Circle",      "Grid",      "Tube",     "Prism",
                          "Gear",     "TorusKnot",   "Arrow",     "Wedge",    "Tetrahedron",
                          "Octahedron", "Dodecahedron", "Icosahedron", "Helix"};
  std::string xml = "<Scene>\n";
  for (const char* tag : shapes) {
    xml += std::string("  <") + tag + " name=\"" + tag + "\"/>\n";
  }
  xml += "</Scene>";
  const auto spec = st::raster::parse_scene_xml(xml);
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->objects.size(), std::size_t{24});
  std::string failure;
  ST_CHECK(roundtrip_ok(xml, &failure));
  if (!failure.empty()) std::printf("    %s\n", failure.c_str());
}

// ── 5. 大小写宽松与别名 ──────────────────────────────────────────────────────
ST_TEST(scene_xml_alias_case) {
  const auto spec = st::raster::parse_scene_xml(
      "<Scene><sphere/><donut/><SPRING/></Scene>");
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->objects[0].shape, std::string("sphere"));
  ST_CHECK_EQ(spec->objects[1].shape, std::string("torus"));
  ST_CHECK_EQ(spec->objects[2].shape, std::string("helix"));
}

// ── 6. 裸图元根 ──────────────────────────────────────────────────────────────
ST_TEST(scene_xml_bare_root) {
  const auto spec = st::raster::parse_scene_xml("<Cube size=\"1, 1, 1\"/>");
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->objects.size(), std::size_t{1});
}

// ── 7. Group / Object（外部引用）解析与往返对称 ─────────────────────────────
ST_TEST(scene_xml_group_object) {
  const auto spec = st::raster::parse_scene_xml(
      "<Scene>\n"
      "  <Group name=\"robot\" translate=\"0, 0.5, 0\">\n"
      "    <Box name=\"body\"/>\n"
      "  </Group>\n"
      "  <Object name=\"chair\" shape=\"obj:chair.obj\"/>\n"
      "  <Object name=\"car\" gltf=\"car.glb\"/>\n"
      "</Scene>");
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->objects.size(), std::size_t{3});
  ST_CHECK(spec->objects[0].group);
  ST_CHECK_EQ(spec->objects[0].children.size(), std::size_t{1});
  ST_CHECK_EQ(spec->objects[1].shape, std::string("obj:chair.obj"));
  ST_CHECK_EQ(spec->objects[2].gltf, std::string("car.glb"));

  std::string failure;
  ST_CHECK(roundtrip_ok(
      "<Scene><Group name=\"a\"><Box name=\"b\"/></Group>"
      "<Object name=\"c\" shape=\"obj:chair.obj\"/></Scene>",
      &failure));
  if (!failure.empty()) std::printf("    %s\n", failure.c_str());
}

// ── 8. 类型错误带行号 ────────────────────────────────────────────────────────
ST_TEST(scene_xml_type_errors) {
  const auto number = st::raster::parse_scene_xml(
      "<Scene>\n  <Sphere roughness=\"abc\"/>\n</Scene>");
  ST_REQUIRE(!number.has_value());
  ST_CHECK(number.error().message.find("roughness") != std::string::npos);
  ST_CHECK(number.error().message.find("2") != std::string::npos);

  ST_CHECK(!st::raster::parse_scene_xml("<Sphere translate=\"1, 2\"/>").has_value());
  ST_CHECK(!st::raster::parse_scene_xml("<Sphere ground=\"maybe\"/>").has_value());
}

// ── 9. 层级错误：结构标签放错位置 ────────────────────────────────────────────
ST_TEST(scene_xml_misplaced) {
  const auto nested = st::raster::parse_scene_xml(
      "<Scene>\n  <Sphere>\n    <Camera/>\n  </Sphere>\n</Scene>");
  ST_REQUIRE(!nested.has_value());
  ST_CHECK(nested.error().message.find("结构标签") != std::string::npos);

  ST_CHECK(!st::raster::parse_scene_xml(
               "<Scene><Animation><Track node=\"a\" kind=\"rotation\">"
               "<Sphere/></Track></Animation></Scene>")
               .has_value());
}

// ── 10. shape 属性冗余 ───────────────────────────────────────────────────────
ST_TEST(scene_xml_shape_attr_redundant) {
  ST_CHECK(!st::raster::parse_scene_xml("<Sphere shape=\"cube\"/>").has_value());
}

// ── 11. looks_like_scene_xml ─────────────────────────────────────────────────
ST_TEST(scene_xml_looks_like) {
  ST_CHECK(st::raster::looks_like_scene_xml("<Scene><Sphere/></Scene>"));
  ST_CHECK(st::raster::looks_like_scene_xml("  \n<Cube/>"));
  ST_CHECK(st::raster::looks_like_scene_xml("<?xml version=\"1.0\"?>\n<Scene/>"));
  ST_CHECK(!st::raster::looks_like_scene_xml("{\"objects\": []}"));
  ST_CHECK(!st::raster::looks_like_scene_xml("<!-- 注释 -->"));
  ST_CHECK(!st::raster::looks_like_scene_xml(""));
}

// ── 12. 完整场景往返（机器人：层级/材质/动画/骨架/外部引用俱全）──────────────
ST_TEST(scene_xml_roundtrip_full) {
  std::string failure;
  const bool ok = roundtrip_ok(R"XML(<Scene cull="back" asset_basis="assets">
  <Camera eye="0, 1.2, 4" target="0, 0.3, 0" fov="42" near="0.01"/>
  <Lighting light_from="0.4, 0.8, 0.6" ambient="0.25" diffuse="0.7" rim="0.15">
    <ExtraLight direction="-0.6, -0.2, 0.5" intensity="0.4"/>
  </Lighting>
  <Shadow strength="0.4" ground_y="0" color="#101014"/>
  <Group name="robot" translate="0, 0, 0">
    <Box name="torso" size="0.6, 0.8, 0.35" color="#4a7dc4" metallic="0.2"
         roughness="0.4" translate="0, 0.9, 0"/>
    <Sphere name="head" size="0.45" color="#c8a07a" translate="0, 1.55, 0">
      <Box name="eye" size="0.08, 0.08, 0.04" translate="0.14, 0.05, 0.19"
           color="#101014"/>
    </Sphere>
    <Cylinder name="arm" size="0.12, 0.7" translate="0.38, 0.9, 0"/>
    <Object name="hand" shape="obj:hand.obj" translate="0.38, 0.5, 0"/>
  </Group>
  <Plane name="floor" size="8, 8" ground="true" color="#20242c"/>
  <Animation name="idle" duration="4">
    <Track node="head" kind="rotation">
      <Key t="0" value="0, 0, 0" easing="ease-in-out"/>
      <Key t="2" value="0, 6, 0"/>
      <Key t="4" value="0, 0, 0"/>
    </Track>
    <Track node="arm" kind="translate">
      <Key t="0" value="0, 0, 0"/>
      <Key t="2" value="0, 0.05, 0"/>
    </Track>
  </Animation>
  <Skeleton joints="torso, head, arm"/>
</Scene>)XML",
                               &failure);
  ST_CHECK(ok);
  if (!ok) std::printf("    %s\n", failure.c_str());
}

// ── 13. 与 JSON 形态的等价性（同一场景两种写法 → 同一 spec）──────────────────
ST_TEST(scene_xml_json_equivalence) {
  const auto from_xml = st::raster::parse_scene_xml(
      "<Scene><Sphere name=\"a\" size=\"0.9\" translate=\"0, 0.4, 0\"/></Scene>");
  const auto from_json = st::raster::parse_scene_spec_text(
      R"j({"objects":[{"shape":"sphere","name":"a","size":0.9,
           "translate":[0,0.4,0]}]})j");
  ST_REQUIRE(from_xml.has_value());
  ST_REQUIRE(from_json.has_value());
  ST_CHECK(same_scene(*from_xml, *from_json));
}

// ── 14. Camera/Lighting/Shadow 未知属性也报错（逐 scope 检查）────────────────
ST_TEST(scene_xml_struct_unknown_attrs) {
  ST_CHECK(!st::raster::parse_scene_xml(
               "<Scene><Camera eyez=\"0, 0, 3\"/></Scene>")
               .has_value());
  ST_CHECK(!st::raster::parse_scene_xml(
               "<Scene><Lighting ambientt=\"0.3\"/></Scene>")
               .has_value());
  ST_CHECK(!st::raster::parse_scene_xml(
               "<Scene><Shadow strengtht=\"0.4\"/></Scene>")
               .has_value());
  ST_CHECK(!st::raster::parse_scene_xml(
               "<Scene><Animation nam=\"wave\"/></Scene>")
               .has_value());
}
