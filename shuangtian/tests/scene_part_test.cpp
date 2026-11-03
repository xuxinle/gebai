// `scene_part_test` —— Part/Use 部件库与 snap 网格吸附的正确性判据。
//
// ## 测试口径
//
// 展开数值断言：Use 展开后的命名（实例_零件）、镜像（X 取反）、模板内
// 引用改写（attach/connect 指向同实例的零件）必须与手写等价场景一致。
// snap：手写 translate 吸附到网格、约束解算值不被破坏。
#include <cmath>
#include <string>

#include "st/raster/scene3d_script.hpp"
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

// ── 1. 基本展开：命名前缀 + 外壳平移叠加 ────────────────────────────────────
ST_TEST(scene_part_expand_basic) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Part name="column">
    <Cylinder name="body" size="0.2, 1.8"/>
    <Cone name="cap" size="0.28, 0.16" attach="top_of:body"/>
  </Part>
  <Use part="column" name="left" translate="-1.2, 0, 0"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  ST_CHECK(find_object(*spec, "left_body") != nullptr);   // 前缀命名
  ST_CHECK(find_object(*spec, "left_cap") != nullptr);
  ST_CHECK(find_object(*spec, "body") == nullptr);        // 模板本身不进场景
  const auto* body = find_object(*spec, "left_body");
  ST_REQUIRE(body != nullptr);
  ST_CHECK_NEAR(body->translate.x, -1.2f, 1e-5f);          // 外壳平移叠加
  // 内部约束已解算：cap 贴 body 顶（引用改写后 attach 指向 left_body）
  const auto* cap = find_object(*spec, "left_cap");
  ST_REQUIRE(cap != nullptr);
  ST_CHECK_NEAR(cap->translate.y, 0.9f + 0.08f, 1e-4f);    // body 顶 + cap 半高
}

// ── 2. 镜像：X 取反、双实例对称 ─────────────────────────────────────────────
ST_TEST(scene_part_mirror) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Part name="arm">
    <Box name="seg" size="0.1, 0.5, 0.1" translate="0.3, 0, 0"/>
  </Part>
  <Use part="arm" name="L" translate="-0.5, 0, 0"/>
  <Use part="arm" name="R" translate="0.5, 0, 0" mirror="x"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto* left = find_object(*spec, "L_seg");
  const auto* right = find_object(*spec, "R_seg");
  ST_REQUIRE(left != nullptr);
  ST_REQUIRE(right != nullptr);
  ST_CHECK_NEAR(left->translate.x, -0.5f + 0.3f, 1e-5f);   // -0.2
  ST_CHECK_NEAR(right->translate.x, 0.5f - 0.3f, 1e-5f);   // +0.2（镜像翻转局部 X）
  ST_CHECK_NEAR(left->translate.x + right->translate.x, 0.0f, 1e-5f);  // 关于原点对称
}

// ── 3. 模板内 connect 的引用改写 ────────────────────────────────────────────
ST_TEST(scene_part_connect_rewrite) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Part name="tower">
    <Cylinder name="mast" size="0.1, 2"/>
    <Sphere name="knob" size="0.2"/>
    <Tube name="tie" connect="top_of:mast -> center_of:knob+0.5, 0, 0" size="0.03, auto"/>
  </Part>
  <Use part="tower" name="A" translate="0, 0, 0"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto* tie = find_object(*spec, "A_tie");
  ST_REQUIRE(tie != nullptr);
  // connect 两端都被改写并解算：tie 的长度 = mast 顶到 knob 偏移点的距离
  // mast top=(0,1,0)（外壳在 0），knob center=(0,0,0)+offset(0.5,0,0)=(0.5,0,0)
  const float expected = std::sqrt(1.0f + 0.25f);
  ST_CHECK_NEAR(tie->size.y, expected, 1e-4f);
}

// ── 4. 引用外部对象不改写（跨部件装配合法）────────────────────────────────
ST_TEST(scene_part_external_ref_kept) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene>
  <Box name="base" size="2, 0.2, 2" translate="0, 0.5, 0"/>
  <Part name="post">
    <Cylinder name="rod" size="0.1, 1" attach="top_of:base"/>
  </Part>
  <Use part="post" name="P" translate="0.5, 0, 0"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto* rod = find_object(*spec, "P_rod");
  ST_REQUIRE(rod != nullptr);
  // attach 引用模板外的 base：不改写，解算到 base 顶面。
  // 注：attach 覆盖整个 translate（含 Use 外壳的平移）——「要么手摆、
  // 要么贴」二选一，这是约束的既定语义（要偏移用锚点偏移 +0.5,0,0）。
  ST_CHECK_NEAR(rod->translate.y, 0.6f + 0.5f, 1e-4f);   // base 顶 + rod 半高
  ST_CHECK_NEAR(rod->translate.x, 0.0f, 1e-4f);           // 对齐 base 锚点 XZ
}

// ── 5. 错误：未定义的 Part / 重复定义 ───────────────────────────────────────
ST_TEST(scene_part_errors) {
  const auto missing = st::raster::parse_scene_xml(
      "<Scene><Use part=\"ghost\"/></Scene>");
  ST_REQUIRE(!missing.has_value());
  ST_CHECK(missing.error().message.find("ghost") != std::string::npos);

  const auto duplicated = st::raster::parse_scene_xml(
      "<Scene><Part name=\"p\"><Box/></Part><Part name=\"p\"><Box/></Part></Scene>");
  ST_REQUIRE(!duplicated.has_value());
  ST_CHECK(duplicated.error().message.find("重复") != std::string::npos);

  const auto bad_mirror = st::raster::parse_scene_xml(
      "<Scene><Part name=\"p\"><Box/></Part><Use part=\"p\" mirror=\"y\"/></Scene>");
  ST_REQUIRE(!bad_mirror.has_value());
  ST_CHECK(bad_mirror.error().message.find("mirror") != std::string::npos);
}

// ── 6. snap：手写吸附、约束值不破坏 ────────────────────────────────────────
ST_TEST(scene_snap) {
  const auto spec = st::raster::parse_scene_xml(R"XML(<Scene snap="0.1">
  <Box name="table" size="2, 0.2, 2" translate="0.03, 0.51, -0.04"/>
  <Cylinder name="post" size="0.1, 0.6" attach="top_of:table"/>
</Scene>)XML");
  ST_REQUIRE(spec.has_value());
  const auto* table = find_object(*spec, "table");
  ST_REQUIRE(table != nullptr);
  // 手写 translate 吸附到 0.1 网格
  ST_CHECK_NEAR(table->translate.x, 0.0f, 1e-5f);
  ST_CHECK_NEAR(table->translate.y, 0.5f, 1e-5f);
  ST_CHECK_NEAR(table->translate.z, 0.0f, 1e-5f);
  // 约束解算值精确（吸附后的台面顶 0.6 + post 半高 0.3 = 0.9）
  const auto* post = find_object(*spec, "post");
  ST_REQUIRE(post != nullptr);
  ST_CHECK_NEAR(post->translate.y, 0.9f, 1e-4f);

  // snap=0（默认）不吸附
  const auto raw = st::raster::parse_scene_xml(
      "<Scene><Box name=\"t\" translate=\"0.03, 0, 0\"/></Scene>");
  ST_REQUIRE(raw.has_value());
  ST_CHECK_NEAR(raw->objects[0].translate.x, 0.03f, 1e-6f);
}

// ── 7. snap 非法值 ─────────────────────────────────────────────────────────
ST_TEST(scene_snap_invalid) {
  const auto bad = st::raster::parse_scene_xml(
      "<Scene snap=\"-0.5\"><Box name=\"t\"/></Scene>");
  ST_CHECK(!bad.has_value());
}
