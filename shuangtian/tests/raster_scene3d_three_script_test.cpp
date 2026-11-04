/// `SceneSpec.three_script`（three.js 源码场景）的接入测试。
///
/// ## 为什么这一层要有独立的用例
///
/// `three_runtime` 的用例只证明"抽得对"；本文件证明"**接上了**"。
/// 两者之间的缝是有真实风险的：
///
/// - `render_impl` 的 three 分支是否真的被走到（而不是静默画了空场景）；
/// - three 分支是否**只走自己那条**（不再去遍历空的 `objects`，也不去跑约束解算）；
/// - `objects` 缺省时会不会被误报成"必填字段缺失"；
/// - 传统 JSON 路径（没有 `three_script`）是否**逐像素不受影响**。
///
/// 最后一条最重要：这是"渐进采用"的底线。旧场景必须一位都不变。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

using st::raster::Canvas;
using st::raster::SceneSpec;

namespace {

constexpr int kW = 200;
constexpr int kH = 200;

/// 三个暖色方块横排 —— 用循环生成（JSON 要写三遍，这是 three.js 的核心优势）。
constexpr const char* kThreeScene = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
camera.position.set(0, 0, 5);
camera.lookAt(0, 0, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 1.2));

const material = new THREE.MeshStandardMaterial({ color: 0xd08040 });
for (let i = 0; i < 3; i++) {
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(0.6, 0.6, 0.6), material);
  mesh.name = 'block' + i;
  mesh.position.set((i - 1) * 1.0, 0, 0);
  scene.add(mesh);
}
scene;
)js";

/// 等价的传统 JSON 场景（同样三个暖色方块），用于"旧路径不变"的对照。
constexpr const char* kJsonScene = R"json({
  "camera": { "eye": [0, 0, 5], "target": [0, 0, 0], "fov": 45 },
  "lighting": { "light_from": [0, 0, 1], "ambient": 1.2, "diffuse": 0.0, "rim": 0.0 },
  "objects": [
    { "shape": "cube", "size": [0.6, 0.6, 0.6], "color": "#d08040",
      "translate": [-1, 0, 0] },
    { "shape": "cube", "size": [0.6, 0.6, 0.6], "color": "#d08040",
      "translate": [0, 0, 0] },
    { "shape": "cube", "size": [0.6, 0.6, 0.6], "color": "#d08040",
      "translate": [1, 0, 0] }
  ]
})json";

auto render_spec(const SceneSpec& spec) -> std::vector<std::uint8_t> {
  Canvas canvas(kW, kH);
  const auto status = st::raster::render_scene(spec, canvas, st::math::Color{10, 10, 14, 255});
  if (!status.has_value()) {
    // 把失败原因写进日志：否则下面只会看到“rbga 为空”，而不知道是为什么
    ST_LOG_ERROR("render_scene 失败: {}", status.error().message);
    return {};
  }
  return canvas.to_rgba8();
}

/// 非背景像素数（以左上角像素为背景）。
auto coverage(const std::vector<std::uint8_t>& rgba) -> int {
  if (rgba.size() < 4) return 0;
  const int bg_r = rgba[0];
  const int bg_g = rgba[1];
  const int bg_b = rgba[2];
  int count = 0;
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const int d = std::abs(rgba[i] - bg_r) + std::abs(rgba[i + 1] - bg_g) +
                  std::abs(rgba[i + 2] - bg_b);
    if (d > 12) ++count;
  }
  return count;
}

}  // namespace

ST_TEST(scene_spec_three_script_renders) {
  auto spec = st::raster::scene_spec_from_three(kThreeScene, "blocks.js");
  ST_REQUIRE(spec.has_value());
  ST_CHECK(!spec->three_script.empty());

  const auto rgba = render_spec(*spec);
  ST_REQUIRE(rgba.size() == static_cast<std::size_t>(kW) * kH * 4U);

  // 三个方块应占相当一块面积（每个 0.6 见方、间距 1.0、fov 45 @ z=5）
  const int painted = coverage(rgba);
  if (painted <= 1500) {
    // 失败时把关键事实打出来（否则“覆盖率低”无法区分“画小了”与“没画”）
    const auto three = st::raster::ThreeRuntime::instance().evaluate(kThreeScene, "blocks.js");
    if (three.has_value()) {
      ST_LOG_ERROR("诊断：抽出 {} 个物体，相机 eye=({:.2f},{:.2f},{:.2f})",
                   three->objects.size(), three->camera.eye.x, three->camera.eye.y,
                   three->camera.eye.z);
      for (const auto& object : three->objects) {
        ST_LOG_ERROR("  {} v={} tri={} world=({:.2f},{:.2f},{:.2f})", object.name,
                     object.mesh.vertex_count(), object.mesh.indices.size() / 3,
                     object.world.m[12], object.world.m[13], object.world.m[14]);
      }
    }
    ST_LOG_ERROR("诊断：非背景像素 {}（期望 > 1500）", painted);
  }
  ST_CHECK(painted > 1500);
  // 但也不该糊满（相机若钻进去就满了）
  ST_CHECK(painted < kW * kH * 3 / 4);
}

ST_TEST(scene_spec_three_script_takes_precedence_over_objects) {
  // 同时给 `objects` 与 `three_script`：按文档，three 优先。
  // 若这条错了，症状是"改了脚本画面不变"——极难自查。
  auto parsed = st::raster::parse_scene_spec_text(R"json({
    "three_script": "const scene = new THREE.Scene(); scene.add(new THREE.PerspectiveCamera(45,1,0.1,100)); scene;",
    "objects": [ { "shape": "cube", "size": [3, 3, 3], "color": "#ff0000" } ]
  })json");
  ST_REQUIRE(parsed.has_value());
  ST_CHECK(!parsed->three_script.empty());
  // three 模式下不解析 objects（避免无意义地建树/解算，也避免掩盖脚本错误）
  ST_CHECK(parsed->objects.empty());
}

ST_TEST(scene_spec_objects_is_optional_with_three_script) {
  // 只写 three_script、不写 objects：不该报"objects 缺失"
  auto with_three = st::raster::parse_scene_spec_text(R"json({
    "three_script": "const scene = new THREE.Scene(); scene;"
  })json");
  ST_CHECK(with_three.has_value());

  // 但两者都不写时，仍应明确报什么缺了（不能静默给空场景）
  auto neither = st::raster::parse_scene_spec_text("{}");
  ST_CHECK(!neither.has_value());
  if (!neither.has_value()) {
    ST_CHECK(neither.error().message.find("objects") != std::string::npos);
  }
}

ST_TEST(scene_spec_three_script_errors_propagate_with_location) {
  // 语法错误必须带 `three_script_name` 里的文件名，否则模型无法定位
  auto spec = st::raster::scene_spec_from_three(
      "const scene = new THREE.Scene(;", "hero-scene.js");
  ST_CHECK(!spec.has_value());
  if (!spec.has_value()) {
    const auto& message = spec.error().message;
    ST_CHECK(message.find("hero-scene.js") != std::string::npos);
  }

  // 空脚本要给出明确说明，而不是"加载失败"
  auto empty = st::raster::scene_spec_from_three("", "empty.js");
  ST_CHECK(!empty.has_value());
  if (!empty.has_value()) {
    ST_CHECK(empty.error().message.find("three") != std::string::npos);
  }
}

ST_TEST(scene_spec_json_path_unaffected_by_three_support) {
  // 渐进采用的底线：没有 `three_script` 的旧场景必须完全走老路径。
  // 判据不是"能出图"，而是**两次渲染逐像素相同**——这才是"没受影响"。
  auto parsed = st::raster::parse_scene_spec_text(kJsonScene);
  ST_REQUIRE(parsed.has_value());
  ST_CHECK(parsed->three_script.empty());
  ST_CHECK(parsed->objects.size() == 3U);

  const auto first = render_spec(*parsed);
  const auto second = render_spec(*parsed);
  ST_REQUIRE(!first.empty());
  ST_REQUIRE(first.size() == second.size());
  ST_CHECK(first == second);   // 确定性：同一份 spec 两次逐字节相同

  // 而且确实画了东西
  ST_CHECK(coverage(first) > 1500);
}

ST_TEST(scene_spec_three_and_json_both_produce_similar_coverage) {
  // 两条路径画的是同一个"三个方块"的构图。覆盖率相近说明 three 的世界矩阵
  // 与相机都被**真的用上了**（若矩阵转置或相机没接，覆盖率会差很远）。
  auto json_spec = st::raster::parse_scene_spec_text(kJsonScene);
  ST_REQUIRE(json_spec.has_value());
  auto three_spec = st::raster::scene_spec_from_three(kThreeScene, "blocks.js");
  ST_REQUIRE(three_spec.has_value());

  const auto json_pixels = coverage(render_spec(*json_spec));
  const auto three_pixels = coverage(render_spec(*three_spec));
  ST_REQUIRE(json_pixels > 0);
  ST_REQUIRE(three_pixels > 0);

  // 两者构图相同，覆盖率应在同一量级（允许 2.5x 差异：材质/光照细节不同）
  const double ratio = static_cast<double>(three_pixels) / static_cast<double>(json_pixels);
  ST_CHECK(ratio > 0.4);
  ST_CHECK(ratio < 2.5);
}

ST_TEST(scene_spec_text_accepts_three_source_directly) {
  // ## 裁云联动的接缝：`parse_scene_spec_text` 必须同时吃 JSON 与 three 源码
  //
  // 它是裁云 `model_script`、三维预览、自检三条路的**共同入口**。
  // 若它只认 JSON，threelab 里写的场景就进不了片子——而那正是联动的全部意义。
  //
  // 判据不用“看起来像不像 JS”，而是：先试 JSON，是对象才算声明式，
  // 其余一律当 three 源码（这样 `{` 开头的 three 脚本也能走对分支）。
  constexpr const char* kThree = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(35, 1, 0.1, 100);
camera.position.set(0, 0, 8);
camera.lookAt(0, 0, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 1.0));
const ball = new THREE.Mesh(new THREE.SphereGeometry(1.5, 24, 16),
  new THREE.MeshStandardMaterial({ color: 0x4488ff }));
ball.name = 'ball';
scene.add(ball);
scene;
)js";
  const auto from_three = st::raster::parse_scene_spec_text(kThree);
  ST_REQUIRE(from_three.has_value());
  ST_CHECK(!from_three->three_script.empty());
  ST_CHECK_NEAR(static_cast<double>(from_three->camera.fov_y_degrees), 35.0, 0.5);

  // JSON 形态必须继续可用（旧剧本不能坏）
  constexpr const char* kJson = R"json({
  "camera": {"eye": [0, 1, 4], "target": [0, 0, 0], "fov": 45},
  "objects": [{"name": "box", "shape": "box", "size": [1, 1, 1],
               "translate": [0, 0.5, 0], "color": [180, 140, 80]}]
})json";
  const auto from_json = st::raster::parse_scene_spec_text(kJson);
  ST_REQUIRE(from_json.has_value());
  ST_CHECK(from_json->three_script.empty());
  ST_CHECK(from_json->objects.size() == 1U);
}

ST_TEST(scene_spec_overlay_windows_are_applied) {
  // ## 三扇叠加窗（裁云的 `model_*` 全靠它们接进场景）
  //
  // 任一扇没生效时**画面根本不变也不报错**——实测在 `camera_override`
  // 上卡了一轮：探针显示"差异 0.0%"，真实原因是 schema 拒了字段、
  // 渲染根本没发生。所以这里直接验渲染结果的像素差异。
  constexpr const char* kScript = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(30, 1, 0.1, 500);
camera.position.set(0, 0, 60);
camera.lookAt(0, 0, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0x8899bb, 0.8));
const key = new THREE.DirectionalLight(0xffffff, 2.2);
key.position.set(3, 4, 5);
scene.add(key);
const box = new THREE.Mesh(new THREE.BoxGeometry(20, 20, 20),
  new THREE.MeshStandardMaterial({ color: 0xc79a4e, metalness: 0.9, roughness: 0.25 }));
box.name = 'box';
box.position.set(4, 0, 0);
scene.add(box);
scene;
)js";
  auto base = st::raster::scene_spec_from_three(kScript, "<overlay>");
  ST_REQUIRE(base.has_value());

  auto render_once = [](const st::raster::SceneSpec& spec) {
    st::raster::Canvas canvas{240, 240, 1.0f};
    auto created = st::raster::Scene3D::create(240, 240);
    if (!created.has_value()) return std::vector<std::uint32_t>{};
    const auto status = st::raster::render_scene_into(spec, **created, canvas,
                                                      st::math::Color{10, 12, 16, 255});
    if (!status.has_value()) return std::vector<std::uint32_t>{};
    const auto view = canvas.pixels();
    return std::vector<std::uint32_t>(view.begin(), view.end());
  };
  auto differ_ratio = [](const std::vector<std::uint32_t>& a,
                         const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    std::size_t different = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i] != b[i]) ++different;
    }
    return static_cast<double>(different) / static_cast<double>(a.size());
  };

  const auto baseline = render_once(*base);
  ST_REQUIRE(!baseline.empty());

  // ① 整体姿态
  {
    st::raster::SceneSpec spec = *base;
    spec.extra_transform = st::math::Mat4::rotation(st::math::Vec3{0.0f, 1.0f, 0.0f}, 0.7f);
    ST_CHECK(differ_ratio(baseline, render_once(spec)) > 0.01);
  }
  // ② 相机覆盖
  {
    st::raster::SceneSpec spec = *base;
    st::raster::Camera camera{};
    camera.eye = st::math::Vec3{60.0f, 40.0f, 0.0f};
    camera.target = st::math::Vec3{0.0f, 0.0f, 0.0f};
    camera.up = st::math::Vec3{0.0f, 1.0f, 0.0f};
    camera.fov_y_degrees = 45.0f;
    camera.near_z = 0.1f;
    camera.far_z = 500.0f;
    spec.camera_override = camera;
    ST_CHECK(differ_ratio(baseline, render_once(spec)) > 0.01);
  }
  // ③ 材质覆盖
  {
    st::raster::SceneSpec spec = *base;
    st::raster::Material material{};
    material.base_color = st::math::Vec3{1.0f, 0.0f, 0.0f};
    material.base_color_set = true;
    material.metallic = 0.0f;
    material.roughness = 0.9f;
    spec.material_override = material;
    ST_CHECK(differ_ratio(baseline, render_once(spec)) > 0.01);
  }
}
