/// three.js 运行时与抽取器的回归测试。
///
/// ## 这些判据要抓的是什么
///
/// 这一层把「模型写的 three.js 代码」翻译成「本框架能画的东西」。翻译错的方式
/// 很多且**都不报错**——画面照样出来，只是不对（颜色偏了、物体歪了、灯不亮、
/// 位置错位）。所以每条判据都必须对准一个**具体的翻译风险**：
///
/// | 判据 | 抓的缺陷 |
/// |---|---|
/// | 能否加载 | 内嵌资源没进产物 / QuickJS 环境桩缺失 |
/// | 几何不丢 | 只读 position 丢了 index（画出来是一堆碎片） |
/// | 矩阵不转置 | three 与霜天都列主序；若谁改成行主序，物体位置会跑到镜像处 |
/// | 灯方向取反 | three 给"射出"、霜天要"指向光源"。漏了负号 = 灯照不到东西，画面全黑 |
/// | 颜色口径 | three 的 `Color.toArray()` 已是线性；再二次解码会让画面发暗 |
/// | DoubleSide | 不映射成 `CullMode::None` 时，薄片背面会消失 |
/// | 相机朝向 | 不取 `quaternion` 算 target 时，相机永远看向 -Z（转镜头无效） |
///
/// ## 为什么像素判据也在这里
///
/// 抽取正确 ≠ 画出来对。上面每一条都可能"数据对、渲染错"。所以最后两条
/// 走完整渲染路径（`Scene3D` + `Canvas`）再量像素——这才是"闭环"。
/// 量法统一走 `to_rgba8()`：`pixels()` 是 0xAARRGGBB 打包，拿错格式会把
/// alpha 当蓝色通道（本仓实测踩过，四个颜色判据因此全错）。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"

using st::raster::Canvas;
using st::raster::ThreeObject;
using st::raster::ThreeRuntime;
using st::raster::ThreeScene;

namespace {

constexpr int kW = 240;
constexpr int kH = 240;

/// 一份"模型会写"的最小场景：一个暖色球 + 一个冷色盒 + 相机 + 灯。
constexpr const char* kBasicScene = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 1, 0.1, 100);
camera.position.set(0, 1.0, 4.0);
camera.lookAt(0, 0.4, 0);
scene.add(camera);

scene.add(new THREE.AmbientLight(0xffffff, 0.5));
const key = new THREE.DirectionalLight(0xffffff, 2.0);
key.position.set(2.5, 4.0, 3.0);
key.target.position.set(0, 0, 0);
scene.add(key);
scene.add(key.target);

const ball = new THREE.Mesh(
  new THREE.SphereGeometry(0.55, 32, 24),
  new THREE.MeshStandardMaterial({ color: 0xd08a50, metalness: 0.2, roughness: 0.5 }));
ball.name = 'ball';
ball.position.set(-0.7, 0.55, 0);
scene.add(ball);

const box = new THREE.Mesh(
  new THREE.BoxGeometry(0.8, 0.8, 0.8),
  new THREE.MeshStandardMaterial({ color: 0x4a7fd0, metalness: 0.0, roughness: 0.6 }));
box.name = 'box';
box.position.set(0.75, 0.4, 0);
scene.add(box);

scene;
)js";

/// 渲染一遍并取直通 RGBA 字节（**不要**用 `pixels()`，那是 0xAARRGGBB）。
auto render_to_rgba(const ThreeScene& scene) -> std::vector<std::uint8_t> {
  auto backend = st::raster::Scene3D::create(kW, kH);
  if (!backend.has_value()) return {};
  Canvas canvas(kW, kH);

  backend.value()->begin_frame(st::math::Color{12, 16, 24, 255});
  backend.value()->set_camera(scene.camera);
  backend.value()->set_lighting(scene.lighting);
  for (const auto& object : scene.objects) {
    backend.value()->set_cull_mode(object.cull);
    backend.value()->set_material(object.material);
    backend.value()->draw_mesh(object.mesh, object.world);
  }
  backend.value()->end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                                    static_cast<float>(kH)});
  return canvas.to_rgba8();
}

struct PixelStats {
  int non_background{0};
  int warm{0};
  int cool{0};
};

/// 以左上角像素为背景采样（角落极少被主体占据），统计覆盖率与冷暖分布。
auto analyze(const std::vector<std::uint8_t>& rgba) -> PixelStats {
  PixelStats stats;
  if (rgba.size() < 4) return stats;
  const int bg_r = rgba[0];
  const int bg_g = rgba[1];
  const int bg_b = rgba[2];
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const int r = rgba[i];
    const int g = rgba[i + 1];
    const int b = rgba[i + 2];
    const int distance = std::abs(r - bg_r) + std::abs(g - bg_g) + std::abs(b - bg_b);
    if (distance > 12) {
      ++stats.non_background;
      if (r > b + 8) ++stats.warm;
      if (b > r + 8) ++stats.cool;
    }
  }
  return stats;
}

/// 按名字找物体（找不到返回空指针）。
auto find_object(const ThreeScene& scene, const std::string& name) -> const ThreeObject* {
  for (const auto& object : scene.objects) {
    if (object.name == name) return &object;
  }
  return nullptr;
}

}  // namespace

ST_TEST(three_runtime_is_available) {
  // 内嵌资源缺失、QuickJS 环境桩不全、three.cjs 与引擎版本不兼容——都会在这里现形。
  if (!ThreeRuntime::available()) {
    ST_FAIL(std::format("three 不可用: {}", ThreeRuntime::unavailable_reason()));
    return;
  }
  ST_CHECK(ThreeRuntime::unavailable_reason().empty());
}

ST_TEST(three_runtime_extracts_geometry_without_loss) {
  auto& runtime = ThreeRuntime::instance();
  auto scene = runtime.evaluate(kBasicScene, "basic.js");
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(scene->objects.size() == 2U);

  for (const auto& object : scene->objects) {
    // 顶点表布局：每顶点 9 个 float，顶点数必须与索引能对上
    ST_CHECK(object.mesh.vertex_count() > 0U);
    ST_CHECK(!object.mesh.indices.empty());
    // 索引不得越界——这是"读了 index 但没算对元素宽度"的样子
    bool all_in_range = true;
    for (std::uint32_t index : object.mesh.indices) {
      if (index >= object.mesh.vertex_count()) all_in_range = false;
    }
    ST_CHECK(all_in_range);
    // 三角形数必须是 3 的倍数
    ST_CHECK(object.mesh.indices.size() % 3U == 0U);
  }

  // 球的三角形数应显著多于盒（SphereGeometry(0.55, 32, 24) ≈ 1400+，盒只有 12）
  const auto* ball = find_object(*scene, "ball");
  const auto* box = find_object(*scene, "box");
  ST_REQUIRE(ball != nullptr);
  ST_REQUIRE(box != nullptr);
  ST_CHECK(ball->mesh.indices.size() > box->mesh.indices.size() * 10U);
  // UV 与法线都要在（缺法线会让着色全黑；缺 UV 贴不了图）
  ST_CHECK(ball->mesh.has_uvs());
}

ST_TEST(three_runtime_maps_world_matrix_without_transpose) {
  auto& runtime = ThreeRuntime::instance();
  auto scene = runtime.evaluate(kBasicScene, "basic.js");
  ST_REQUIRE(scene.has_value());

  // three 与霜天都是列主序 `m[column * 4 + row]`：平移在 m[12..14]。
  // 若谁把其中一边改成行主序，这里的值会跑到 m[3]/m[7]/m[11]，
  // 而**画面只是位置错**、不报任何错——所以必须直接钉数值。
  const auto* ball = find_object(*scene, "ball");
  ST_REQUIRE(ball != nullptr);
  ST_CHECK_NEAR(ball->world.m[12], -0.7f, 1e-4);
  ST_CHECK_NEAR(ball->world.m[13], 0.55f, 1e-4);
  ST_CHECK_NEAR(ball->world.m[14], 0.0f, 1e-4);

  const auto* box = find_object(*scene, "box");
  ST_REQUIRE(box != nullptr);
  ST_CHECK_NEAR(box->world.m[12], 0.75f, 1e-4);
}

ST_TEST(three_runtime_flips_light_direction) {
  auto& runtime = ThreeRuntime::instance();
  auto scene = runtime.evaluate(kBasicScene, "basic.js");
  ST_REQUIRE(scene.has_value());

  // three 的平行光是"从 position 射向 target"；霜天的 `light_from` 是"指向光源"。
  // 本场景：position(2.5, 4, 3) → target(0,0,0)，射出方向 ≈ (-0.44, -0.71, -0.53)
  // 于是霜天该收到 ≈ (0.44, 0.71, 0.53)。
  //
  // 漏了取反的后果是"灯照不到东西、画面全黑"——看起来像渲染器坏了，
  // 而数据层面完全正确。本仓的聚光曾踩过同一个坑，所以这条必须有。
  const auto d = scene->lighting.light_from;
  ST_CHECK(d.y > 0.0f);
  ST_CHECK(d.x > 0.0f);
  ST_CHECK(d.z > 0.0f);

  // 环境光累加进 ambient（three 的 AmbientLight 强度 0.5、白色 → 环境项约 0.5）
  ST_CHECK(scene->lighting.ambient > 0.3f);
  ST_CHECK(scene->lighting.ambient < 0.8f);
  ST_CHECK(scene->lighting.diffuse > 1.5f);
}

ST_TEST(three_runtime_keeps_linear_color) {
  auto& runtime = ThreeRuntime::instance();
  auto scene = runtime.evaluate(kBasicScene, "basic.js");
  ST_REQUIRE(scene.has_value());

  // 0xd08a50 在 three 里已被解码成线性 [0.63, 0.25, 0.08]。若这一层又做了一次
  // sRGB→线性（重复解码），值得掉到 ≈ [0.36, 0.05, 0.007]——画面会明显发暗，
  // 但"看起来只是配色不对"，很难想到是色彩空间。
  const auto* ball = find_object(*scene, "ball");
  ST_REQUIRE(ball != nullptr);
  ST_CHECK_NEAR(ball->material.base_color.x, 0.6308f, 0.02);
  ST_CHECK_NEAR(ball->material.base_color.y, 0.2542f, 0.02);
  ST_CHECK_NEAR(ball->material.base_color.z, 0.0802f, 0.02);
  ST_CHECK(ball->material.base_color_set);
}

ST_TEST(three_runtime_maps_side_to_cull_mode) {
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kSides = R"js(
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
  camera.position.set(0, 0, 4);
  scene.add(camera);

  const front = new THREE.Mesh(new THREE.PlaneGeometry(1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff, side: THREE.FrontSide }));
  front.name = 'front';
  scene.add(front);

  const panel = new THREE.Mesh(new THREE.PlaneGeometry(1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff, side: THREE.DoubleSide }));
  panel.name = 'double';
  panel.position.x = 1.5;
  scene.add(panel);

  const back = new THREE.Mesh(new THREE.PlaneGeometry(1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff, side: THREE.BackSide }));
  back.name = 'back';
  back.position.x = -1.5;
  scene.add(back);

  scene;
  )js";
  auto scene = runtime.evaluate(kSides, "sides.js");
  ST_REQUIRE(scene.has_value());

  const auto* front = find_object(*scene, "front");
  const auto* double_sided = find_object(*scene, "double");
  const auto* back = find_object(*scene, "back");
  ST_REQUIRE(front != nullptr);
  ST_REQUIRE(double_sided != nullptr);
  ST_REQUIRE(back != nullptr);
  ST_CHECK(front->cull == st::raster::CullMode::Back);
  ST_CHECK(double_sided->cull == st::raster::CullMode::None);
  ST_CHECK(back->cull == st::raster::CullMode::Front);
}

ST_TEST(three_runtime_derives_camera_target_from_quaternion) {
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kCamera = R"js(
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(50, 1.5, 0.2, 200);
  camera.position.set(3, 2, 0);
  camera.lookAt(0, 0, 0);     // 看向原点 ⇒ 视线方向 (-3, -2, 0) 归一化
  scene.add(camera);
  scene.add(new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
    new THREE.MeshStandardMaterial({ color: 0xffffff })));
  scene;
  )js";
  auto scene = runtime.evaluate(kCamera, "camera.js");
  ST_REQUIRE(scene.has_value());

  ST_CHECK_NEAR(scene->camera.eye.x, 3.0f, 1e-4);
  ST_CHECK_NEAR(scene->camera.eye.y, 2.0f, 1e-4);
  ST_CHECK_NEAR(scene->camera.fov_y_degrees, 50.0f, 1e-4);
  ST_CHECK_NEAR(scene->camera.near_z, 0.2f, 1e-4);
  ST_CHECK_NEAR(scene->camera.far_z, 200.0f, 1e-3);

  // target 由四元数推出：应落在原点方向（-Y 分量必须存在，说明真的读了朝向）
  ST_CHECK(scene->camera.target.y < scene->camera.eye.y);
  ST_CHECK(scene->camera.target.x < scene->camera.eye.x);
}

ST_TEST(three_runtime_renders_pixels) {
  auto& runtime = ThreeRuntime::instance();
  auto scene = runtime.evaluate(kBasicScene, "basic.js");
  ST_REQUIRE(scene.has_value());

  const auto rgba = render_to_rgba(*scene);
  ST_REQUIRE(rgba.size() == static_cast<std::size_t>(kW) * kH * 4U);

  const auto stats = analyze(rgba);
  const double coverage =
      100.0 * static_cast<double>(stats.non_background) / static_cast<double>(kW * kH);

  // 画面非空（东西真画出来了）
  ST_CHECK(coverage > 3.0);
  // 也没糊满（相机在物体内 / 物体太大）
  ST_CHECK(coverage < 70.0);
  // 两个物体都在：暖色球 + 冷色盒
  ST_CHECK(stats.warm > 300);
  ST_CHECK(stats.cool > 300);
}

ST_TEST(three_runtime_camera_change_changes_pixels) {
  auto& runtime = ThreeRuntime::instance();
  auto scene = runtime.evaluate(kBasicScene, "basic.js");
  ST_REQUIRE(scene.has_value());

  const auto before = render_to_rgba(*scene);
  ST_REQUIRE(!before.empty());

  // 换相机：从侧面看。若画面不变，说明相机没真的接到渲染器
  //（这正是"抽取对了但没接上"的典型样子，而它不会报错）。
  scene->camera.eye = st::math::Vec3{3.4f, 1.6f, 2.2f};
  scene->camera.target = st::math::Vec3{0.0f, 0.4f, 0.0f};
  const auto after = render_to_rgba(*scene);
  ST_REQUIRE(after.size() == before.size());

  std::size_t changed = 0;
  for (std::size_t i = 0; i + 3 < before.size() && i + 3 < after.size(); i += 4) {
    if (before[i] != after[i] || before[i + 1] != after[i + 1] || before[i + 2] != after[i + 2]) {
      ++changed;
    }
  }
  const double changed_percent =
      100.0 * static_cast<double>(changed) / static_cast<double>(kW * kH);
  ST_CHECK(changed_percent > 5.0);
}

ST_TEST(three_runtime_reports_clear_errors) {
  auto& runtime = ThreeRuntime::instance();

  // 语法错误：消息里必须带 three.js 侧的文件名/行号（否则模型无法自我修正）
  auto broken = runtime.evaluate("const scene = new THREE.Scene(;", "broken.js");
  ST_CHECK(!broken.has_value());
  if (!broken.has_value()) {
    ST_CHECK(broken.error().message.find("broken.js") != std::string::npos);
  }

  // 没有场景：要说清"该怎么改"，而不是只说"失败"
  auto no_scene = runtime.evaluate("const x = 1 + 1; x;", "noscene.js");
  ST_CHECK(!no_scene.has_value());
  if (!no_scene.has_value()) {
    const auto& message = no_scene.error().message;
    const bool mentions_scene = message.find("Scene") != std::string::npos ||
                                message.find("scene") != std::string::npos;
    ST_CHECK(mentions_scene);
  }

  // 运行时异常（调用不存在的属性）也要带定位
  auto throws =
      runtime.evaluate("const scene = new THREE.Scene(); scene.noSuchMethod();", "throws.js");
  ST_CHECK(!throws.has_value());
  if (!throws.has_value()) ST_CHECK(!throws.error().message.empty());
}

ST_TEST(three_runtime_supports_generative_geometry) {
  // 模型最爱写的三件事：循环生成、ExtrudeGeometry（轮廓）、LatheGeometry（旋转体）。
  // 这三个若能跑，"完整支持 three.js"在数据侧就站得住。
  auto& runtime = ThreeRuntime::instance();
  constexpr const char* kGenerative = R"js(
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
  camera.position.set(0, 1, 5);
  camera.lookAt(0, 0, 0);
  scene.add(camera);
  scene.add(new THREE.AmbientLight(0xffffff, 1.0));

  // ① 循环生成 8 个球（JSON 表达不了，这是 three.js 的核心优势）
  const material = new THREE.MeshStandardMaterial({ color: 0x8899ff });
  for (let i = 0; i < 8; i++) {
    const mesh = new THREE.Mesh(new THREE.SphereGeometry(0.15, 12, 8), material);
    mesh.name = 's' + i;
    mesh.position.set((i - 3.5) * 0.4, 0, 0);
    scene.add(mesh);
  }

  // ② ExtrudeGeometry + Shape（轮廓建模）
  const shape = new THREE.Shape();
  shape.moveTo(0, 0); shape.lineTo(1, 0); shape.lineTo(1, 1); shape.lineTo(0, 1);
  shape.closePath();
  const extruded = new THREE.Mesh(
    new THREE.ExtrudeGeometry(shape, { depth: 0.3, bevelEnabled: false }),
    new THREE.MeshStandardMaterial({ color: 0xffaa00 }));
  extruded.name = 'extruded';
  extruded.position.set(-2, -1, 0);
  scene.add(extruded);

  // ③ LatheGeometry（旋转体）
  const points = [];
  for (let i = 0; i <= 8; i++) {
    points.push(new THREE.Vector2(0.2 + 0.1 * Math.sin(i / 8 * Math.PI), i / 8));
  }
  const lathed = new THREE.Mesh(
    new THREE.LatheGeometry(points, 20),
    new THREE.MeshStandardMaterial({ color: 0x00cc88 }));
  lathed.name = 'lathed';
  lathed.position.set(2, -1, 0);
  scene.add(lathed);

  scene;
  )js";
  auto scene = runtime.evaluate(kGenerative, "generative.js");
  ST_REQUIRE(scene.has_value());

  // 8 球 + 2 个程序化几何 = 10
  ST_CHECK(scene->objects.size() == 10U);

  const auto* extruded = find_object(*scene, "extruded");
  const auto* lathed = find_object(*scene, "lathed");
  ST_REQUIRE(extruded != nullptr);
  ST_REQUIRE(lathed != nullptr);
  ST_CHECK(extruded->mesh.vertex_count() >= 8U);
  ST_CHECK(!extruded->mesh.indices.empty());
  ST_CHECK(lathed->mesh.vertex_count() >= 20U);

  // 循环生成的 8 个球
  int sphere_count = 0;
  for (const auto& object : scene->objects) {
    if (object.name.size() == 2 && object.name[0] == 's') ++sphere_count;
  }
  ST_CHECK(sphere_count == 8);
}
