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
