/// bpy × three.js **混合工作流**的回归测试。
///
/// ## 这条缝为什么值得单独测
///
/// 混合模式（`bpy_script` + `three_script` 同时给）有三个都**不会报错**的失败形态：
///
/// | 失败形态 | 现象 |
/// |---|---|
/// | 资产没挂进 three 脚本 | 组装脚本拿不到几何 → 要么抛错、要么画个空场景 |
/// | 资产跨求值残留（单例运行时） | 换了场景，画面还是上一场景的模型 |
/// | bpy 的相机/灯光压过 three | 镜头不对，但"看着像画了东西" |
///
/// 三条里第 2 条最阴：它让"应当失败的用例"通过（本仓库实测踩到——反证用例恒绿，
/// 因为它读到了上一轮残留的 `THREE.__assets`）。
///
/// 所以这里的判据必须包含**反证**（不带资产必失败），而不只是"混合能出图"。

#include <string>

#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"

using st::raster::BpyAsset;
using st::raster::ThreeAssets;
using st::raster::ThreeRuntime;

namespace {

/// 一个最小立方体资产（手工顶点表：与 bpy 抽取出来的形态一致——每顶点 9 float）。
auto make_cube_asset(const std::string& name) -> BpyAsset {
  BpyAsset asset;
  asset.name = name;
  // 8 顶点 × 9 float（位置 3 + 法线 3 + 颜色 3），12 三角形。
  const float half = 0.5f;
  const float positions[8][3] = {{-half, -half, -half}, {half, -half, -half},
                                 {half, half, -half},   {-half, half, -half},
                                 {-half, -half, half},  {half, -half, half},
                                 {half, half, half},    {-half, half, half}};
  const float normals[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                               {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
  for (int i = 0; i < 8; ++i) {
    asset.mesh.vertices.push_back(positions[i][0]);
    asset.mesh.vertices.push_back(positions[i][1]);
    asset.mesh.vertices.push_back(positions[i][2]);
    asset.mesh.vertices.push_back(normals[i][0]);
    asset.mesh.vertices.push_back(normals[i][1]);
    asset.mesh.vertices.push_back(normals[i][2]);
    asset.mesh.vertices.push_back(1.0f);
    asset.mesh.vertices.push_back(1.0f);
    asset.mesh.vertices.push_back(1.0f);
  }
  asset.mesh.indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                        1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0};
  return asset;
}

/// 用资产的 three 脚本：拿 `THREE.__assets.<name>` 建一个 Mesh。
constexpr const char* kUsesAsset = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(45, 4/3, 0.1, 100);
camera.position.set(2.6, 2.2, 3.4);
camera.lookAt(0, 0, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 0.8));
const geometry = THREE.__assets['demo_cube'];
if (!geometry) throw new Error('资产缺失：demo_cube');
const mesh = new THREE.Mesh(geometry, new THREE.MeshStandardMaterial({ color: 0x88aaff }));
mesh.name = 'from_bpy';
scene.add(mesh);
scene;
)js";

}  // namespace

ST_TEST(mixed_assets_reach_the_three_script) {
  if (!ThreeRuntime::available()) return;   // three.js 不可用的构建不算失败
  ThreeAssets assets;
  assets.items.push_back(make_cube_asset("demo_cube"));

  auto scene = ThreeRuntime::instance().evaluate_with_assets(kUsesAsset, assets, "mixed.js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(!scene->objects.empty());
  // 物体来自 three 脚本（名字是脚本里设的）——说明它确实用资产建了 Mesh。
  bool found = false;
  for (const auto& object : scene->objects) {
    if (object.name == "from_bpy") found = true;
  }
  ST_CHECK(found);
}

ST_TEST(mixed_assets_do_not_leak_into_the_next_evaluation) {
  // ⚠ 这条是这个文件里**最重要**的一条。
  //
  // `ThreeRuntime` 是线程内单例（QuickJS 运行时不能重建），所以挂在 `THREE`
  // 上的东西会跨求值残留。实测踩到：上一个带资产的场景跑完，下一个**不带资产**
  // 的脚本仍能拿到 `THREE.__assets` —— 于是"应当失败"的反证用例恒绿。
  //
  /* 先把资产灌进去（这一步会成功）。 */
  ThreeAssets assets;
  assets.items.push_back(make_cube_asset("demo_cube"));
  auto first = ThreeRuntime::instance().evaluate_with_assets(kUsesAsset, assets, "mixed.js");
  ST_REQUIRE(first.has_value());

  /* 再**不带任何资产**求值同一份脚本：必须失败（脚本开头就断言资产存在）。 */
  ThreeAssets empty;
  auto second = ThreeRuntime::instance().evaluate_with_assets(kUsesAsset, empty, "mixed.js");
  ST_CHECK(!second.has_value());

  /* 走普通 `evaluate`（很多调用方用的那条路）也一样：不该拿到残留。 */
  auto third = ThreeRuntime::instance().evaluate(kUsesAsset, "mixed.js");
  ST_CHECK(!third.has_value());
}

ST_TEST(mixed_assets_empty_keeps_plain_scenes_working) {
  // 零资产不该影响普通场景（回归：这条路径每天被所有三维用法走到）。
  if (!ThreeRuntime::available()) return;
  constexpr const char* kPlain = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(45, 4/3, 0.1, 100);
camera.position.set(0, 0, 3);
scene.add(camera);
scene.add(new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
                         new THREE.MeshStandardMaterial({ color: 0xffffff })));
scene;
)js";
  ThreeAssets empty;
  auto scene = ThreeRuntime::instance().evaluate_with_assets(kPlain, empty, "plain.js");
  ST_REQUIRE(scene.has_value());
  ST_CHECK(!scene->objects.empty());
}
