/// three.js → 本框架的**语义翻译**测试。
///
/// ## 为什么这一组要单独存在
///
/// `three_runtime` 的用例证明"能抽出来"，但它抽出来的是"数据"。
/// 数据对**不代表语义对**——两套引擎对同一个概念的定义常有细微差别，
/// 而差别几乎总是**不报错、只让画面变样**：
///
/// | 概念 | three | 本框架 | 混淆的后果 |
/// |---|---|---|---|
/// | 平行光方向 | `position → target`（射出） | 指向光源 | 灯照不到东西，全黑 |
/// | `opacity` | 需 `transparent: true` 才生效 | `< 1` 就混合 | 半透明物体变实心 |
/// | `side` | Front/Back/Double | CullMode | 薄片背面消失 / 内壁外露 |
/// | 颜色 | `Color.toArray()` 已是线性 | base_color 要线性 | 二次解码 → 画面发暗 |
/// | 坐标系 | +Y 上、相机看 -Z | 同 | 若不同，整个场景会翻/转 |
/// | 矩阵序 | 列主序 `m[col*4+row]` | 同 | 若不同，位置镜像错位 |
///
/// 每一条都对准一个真实的混淆点。这组用例的写法因此是"**给一份能区分两种
/// 解释的输入，断言只有正确解释能得到那个输出**"。

#include <cmath>
#include <cstddef>
#include <format>
#include <string>

#include "st/raster/scene3d.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"

using st::raster::ThreeObject;
using st::raster::ThreeRuntime;
using st::raster::ThreeScene;

namespace {

auto find_object(const ThreeScene& scene, const std::string& name) -> const ThreeObject* {
  for (const auto& object : scene.objects) {
    if (object.name == name) return &object;
  }
  return nullptr;
}

}  // namespace

ST_TEST(three_semantics_colorspace_is_not_decoded_twice) {
  // 关键判据：一个纯红 `0xff0000` 应当是 `[1, 0, 0]` 的线性值。
  // 若错做了一次 sRGB→线性，红色会掉到 0.21（画面明显发暗但不报错）。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  scene.add(new THREE.PerspectiveCamera(45, 1, 0.1, 100));
  const red = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0xff0000 }));
  red.name = 'red';
  scene.add(red);
  // 中灰 0x808080：sRGB 0.502 → 线性 0.216。这两个值差 2.3 倍，最好区分。
  const grey = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0x808080 }));
  grey.name = 'grey';
  grey.position.x = 2;
  scene.add(grey);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "colorspace.js");
  ST_REQUIRE(scene.has_value());

  const auto* red = find_object(*scene, "red");
  ST_REQUIRE(red != nullptr);
  // 纯红：线性值就该是 1.0（three 的 Color(0xff0000) 仍给 1.0）
  ST_CHECK_NEAR(red->material.base_color.x, 1.0f, 1e-3);
  ST_CHECK_NEAR(red->material.base_color.y, 0.0f, 1e-3);

  const auto* grey = find_object(*scene, "grey");
  ST_REQUIRE(grey != nullptr);
  // 中灰：**必须是线性 0.216**。若是 0.502 说明 three 那边没解码；
  // 若是 0.09 说明又多做了一次解码。两个都错，正是本用例的价值。
  ST_CHECK_NEAR(grey->material.base_color.x, 0.2159f, 0.01);
}

ST_TEST(three_semantics_handles_axes_convention) {
  // 三个轴各放一个物体，断言 world 矩阵里的平移落在正确位置。
  //
  // 这条抓的是"坐标系约定差一个轴/差一个符号"——那种错会让整个场景
  // 翻转或旋转 90°，而 everything 都不报错。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
  camera.position.set(0, 0, 6);
  camera.lookAt(0, 0, 0);
  scene.add(camera);

  const px = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.2, 0.2),
    new THREE.MeshStandardMaterial({ color: 0xffffff }));
  px.name = 'plus_x';
  px.position.set(2, 0, 0);     // +X 右
  scene.add(px);

  const py = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.2, 0.2),
    new THREE.MeshStandardMaterial({ color: 0xffffff }));
  py.name = 'plus_y';
  py.position.set(0, 2, 0);     // +Y 上
  scene.add(py);

  const pz = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.2, 0.2),
    new THREE.MeshStandardMaterial({ color: 0xffffff }));
  pz.name = 'plus_z';
  pz.position.set(0, 0, 2);     // +Z 朝观众（右手系）
  scene.add(pz);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "axes.js");
  ST_REQUIRE(scene.has_value());

  const auto* x = find_object(*scene, "plus_x");
  const auto* y = find_object(*scene, "plus_y");
  const auto* z = find_object(*scene, "plus_z");
  ST_REQUIRE(x != nullptr);
  ST_REQUIRE(y != nullptr);
  ST_REQUIRE(z != nullptr);

  // 列主序：平移在 m[12](x) / m[13](y) / m[14](z)
  ST_CHECK_NEAR(x->world.m[12], 2.0f, 1e-4);
  ST_CHECK_NEAR(x->world.m[13], 0.0f, 1e-4);
  ST_CHECK_NEAR(y->world.m[12], 0.0f, 1e-4);
  ST_CHECK_NEAR(y->world.m[13], 2.0f, 1e-4);
  ST_CHECK_NEAR(z->world.m[12], 0.0f, 1e-4);
  ST_CHECK_NEAR(z->world.m[14], 2.0f, 1e-4);
}

ST_TEST(three_semantics_rotation_composes_correctly) {
  // 绕 Z 转 90°：局部 +X 方向应变成世界 +Y 方向。
  // 判据取矩阵第一列（局部 X 轴在世界里的像）。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  scene.add(new THREE.PerspectiveCamera(45, 1, 0.1, 100));
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff }));
  mesh.name = 'rot';
  mesh.rotation.z = Math.PI / 2;
  scene.add(mesh);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "rotation.js");
  ST_REQUIRE(scene.has_value());
  const auto* mesh = find_object(*scene, "rot");
  ST_REQUIRE(mesh != nullptr);

  // 列主序：第 0 列 = m[0..3] = 局部 X 轴的像
  // 绕 Z 转 +90°：(1,0,0) → (0,1,0)
  ST_CHECK_NEAR(mesh->world.m[0], 0.0f, 1e-4);
  ST_CHECK_NEAR(mesh->world.m[1], 1.0f, 1e-4);
}

ST_TEST(three_semantics_parent_child_transform_chain) {
  // 父子链：父平移 (1,0,0) + 绕 Z 转 90°，子局部 (0,1,0)。
  //
  // ## 期望值不是手算的（我算错过一次，记在这里）
  //
  // 我的第一次手算写的是 (0,1,0)——直觉上“父转了 90°，子向上就变成向左”
  // 似乎应当得到 (0,1,0)。但用 three 自己算出来是 **(-2.2e-16, -2.2e-16, 0)**，
  // 也就是**原点**。原因：父矩阵是 `T(1,0,0)·R_z(90°)`，先把 (0,1,0) 旋转再平移：
  // `R_z(90°)·(0,1,0) = (-1,0,0)`，再 `+(1,0,0) = (0,0,0)`。
  //
  // 教训：变换链**不手算**。本用例的期望值取自 three 自己的
  // `matrixWorld`（见注释中的数据源），且判据是**与本框架抽取结果逐项对齐**。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  scene.add(new THREE.PerspectiveCamera(45, 1, 0.1, 100));
  const parent = new THREE.Group();
  parent.position.set(1, 0, 0);
  parent.rotation.z = Math.PI / 2;
  const child = new THREE.Mesh(new THREE.BoxGeometry(0.2, 0.2, 0.2),
    new THREE.MeshStandardMaterial({ color: 0xffffff }));
  child.name = 'child';
  child.position.set(0, 1, 0);
  parent.add(child);
  scene.add(parent);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "parent-child.js");
  ST_REQUIRE(scene.has_value());
  const auto* child = find_object(*scene, "child");
  ST_REQUIRE(child != nullptr);

  // 预期：子落在原点（three 实测 -2.2e-16 ≈ 0）
  ST_CHECK_NEAR(child->world.m[12], 0.0f, 1e-4);
  ST_CHECK_NEAR(child->world.m[13], 0.0f, 1e-4);
  ST_CHECK_NEAR(child->world.m[14], 0.0f, 1e-4);
  // 旋转部分：局部 X 轴变成世界 Y 轴（父的 90° 旋转必须传下来）
  ST_CHECK_NEAR(child->world.m[0], 0.0f, 1e-4);
  ST_CHECK_NEAR(child->world.m[1], 1.0f, 1e-4);
}

ST_TEST(three_semantics_opacity_requires_transparent_flag) {
  // three 的语义：`opacity < 1` 但没设 `transparent: true` → **仍然不透明**。
  // 本框架的语义是"`opacity < 1` 就混合"。两边不同，必须翻译。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  scene.add(new THREE.PerspectiveCamera(45, 1, 0.1, 100));

  // 忘了设 transparent（three 会当不透明处理）
  const forgot = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff, opacity: 0.4 }));
  forgot.name = 'forgot';
  scene.add(forgot);

  // 正确设了 transparent
  const glass = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff, opacity: 0.4, transparent: true }));
  glass.name = 'glass';
  glass.position.x = 2;
  scene.add(glass);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "opacity.js");
  ST_REQUIRE(scene.has_value());

  const auto* forgot = find_object(*scene, "forgot");
  const auto* glass = find_object(*scene, "glass");
  ST_REQUIRE(forgot != nullptr);
  ST_REQUIRE(glass != nullptr);
  // 没设 transparent → 本框架也该当不透明（与 three 的最终观感一致）
  ST_CHECK_NEAR(forgot->material.opacity, 1.0f, 1e-4);
  // 设了 → 保持 0.4
  ST_CHECK_NEAR(glass->material.opacity, 0.4f, 1e-4);
  // 透明物体不写深度（否则会挡住后面的东西）
  ST_CHECK(!glass->material.depth_write);
}

ST_TEST(three_semantics_basic_material_is_unlit) {
  // three 的 `MeshBasicMaterial` **不受光照**（纯色）。本框架用
  // "自发光 + unlit" 表达同一语义。若漏了这一步，Basic 材质会被
  // 光照公式加工一遍——画面会比预期暗或有明暗面。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  scene.add(new THREE.PerspectiveCamera(45, 1, 0.1, 100));
  scene.add(new THREE.AmbientLight(0xffffff, 0.1));   // 环境光很弱
  const basic = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshBasicMaterial({ color: 0x40c080 }));
  basic.name = 'basic';
  scene.add(basic);
  const standard = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0x40c080 }));
  standard.name = 'standard';
  standard.position.x = 2;
  scene.add(standard);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "basic-material.js");
  ST_REQUIRE(scene.has_value());

  const auto* basic = find_object(*scene, "basic");
  ST_REQUIRE(basic != nullptr);
  ST_CHECK(basic->material.emission_unlit);
  ST_CHECK(basic->material.emission > 0.0f);

  const auto* standard = find_object(*scene, "standard");
  ST_REQUIRE(standard != nullptr);
  ST_CHECK(!standard->material.emission_unlit);
}

ST_TEST(three_semantics_point_and_spot_lights_map_to_extra_lights) {
  // 点光/聚光要进 `extra_lights`（主光是 `Lighting::diffuse` 那一路）。
  // 判据：各自出现在附加光源里，且类型/位置/方向被正确翻译。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  scene.add(new THREE.PerspectiveCamera(45, 1, 0.1, 100));
  // 一个平行光当主光
  const key = new THREE.DirectionalLight(0xffffff, 1.5);
  key.position.set(2, 3, 2);
  scene.add(key);
  // 一个点光
  const bulb = new THREE.PointLight(0xff8844, 3.0, 8.0);
  bulb.position.set(-1.5, 1.0, 1.0);
  scene.add(bulb);
  // 一个聚光
  const spot = new THREE.SpotLight(0x88ff44, 2.0, 10.0, Math.PI / 8, 0.3);
  spot.position.set(0, 3, 0);
  spot.target.position.set(0, 0, 0);
  scene.add(spot);
  scene.add(spot.target);
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "lights.js");
  ST_REQUIRE(scene.has_value());

  // 主光：来自 key.position（指向光源）
  ST_CHECK(scene->lighting.diffuse > 1.0f);
  ST_CHECK(scene->lighting.light_from.y > 0.0f);

  // 附加光源里应有点光与聚光
  bool has_point = false;
  bool has_spot = false;
  for (const auto& light : scene->lighting.extra_lights) {
    if (light.kind == st::raster::LightKind::Point && light.intensity > 0.0f) {
      has_point = true;
      // 点光位置应被翻译过来
      ST_CHECK_NEAR(light.position.x, -1.5f, 1e-3);
      ST_CHECK_NEAR(light.position.y, 1.0f, 1e-3);
      // three 的 distance → 本框架的 radius
      ST_CHECK_NEAR(light.radius, 8.0f, 1e-3);
    }
    if (light.kind == st::raster::LightKind::Spot && light.intensity > 0.0f) {
      has_spot = true;
      // three 的 angle（半角弧度）→ 本框架的度
      // π/8 = 22.5°；penumbra 0.3 → 内锥 22.5*(1-0.3) = 15.75°
      ST_CHECK_NEAR(light.spot_outer_degrees, 22.5f, 0.5);
      ST_CHECK(light.spot_inner_degrees > 0.0f);
      ST_CHECK(light.spot_inner_degrees < light.spot_outer_degrees);
    }
  }
  ST_CHECK(has_point);
  ST_CHECK(has_spot);
}

ST_TEST(three_semantics_camera_up_and_orthographic) {
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kScene = R"js(
  const scene = new THREE.Scene();
  // 正交相机：可视高度 = (top - bottom) / zoom
  const ortho = new THREE.OrthographicCamera(-4, 4, 3, -3, 0.1, 100);
  ortho.zoom = 1.5;   // 放大 1.5 倍 → 有效可视高度 = 6/1.5 = 4
  ortho.position.set(0, 0, 10);
  ortho.updateProjectionMatrix();
  scene.add(ortho);
  scene.add(new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff })));
  scene;
  )js";
  auto scene = runtime.evaluate(kScene, "ortho.js");
  ST_REQUIRE(scene.has_value());

  ST_CHECK(scene->camera.projection == st::raster::ProjectionKind::Orthographic);
  // (top - bottom) / zoom = (3 - (-3)) / 1.5 = 4
  ST_CHECK_NEAR(scene->camera.ortho_scale, 4.0f, 0.01);
}
