/// 相机朝向抽取的**准确**回归。
///
/// ## 为什么这组测试必须存在
///
/// 这里曾经发生过一次「假缺陷」：把 `camera.target`（一个**点**）当成方向读，
/// 于是从 `target.z = 9.29` 推出「方向 z 只有 -0.29」，断定
/// "`lookAt` 在 QuickJS 里角度减半"，还据此在场景脚本里写了一条规避方案、
/// 在文档里记了一条缺陷。
///
/// 实际 `eye.z = 10`、方向 `(0.71, 0, -0.71)` → `target.z = 10 - 0.71 = 9.29`，
/// 完全正确。
///
/// 所以这组用例的作用不是"发现缺陷"，而是**把正确的期望值钉死**：
/// 以后任何人再怀疑 `lookAt`，跑一遍就知道是真坏还是算错了。

#include <cmath>
#include <string>

#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"

namespace {

constexpr double kPi = 3.14159265358979;

/// 求出相机朝向的**单位方向**（`target - eye` 归一化）。
///
/// 刻意单独开一个函数：混淆"点"与"方向"正是那次误判的根源，
/// 把它封装成有名字的函数能让误用变得显眼。
auto camera_direction(const st::raster::Camera& camera, double& nx, double& ny, double& nz)
    -> void {
  const double dx = static_cast<double>(camera.target.x) - static_cast<double>(camera.eye.x);
  const double dy = static_cast<double>(camera.target.y) - static_cast<double>(camera.eye.y);
  const double dz = static_cast<double>(camera.target.z) - static_cast<double>(camera.eye.z);
  const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
  nx = len > 0.0 ? dx / len : 0.0;
  ny = len > 0.0 ? dy / len : 0.0;
  nz = len > 0.0 ? dz / len : 0.0;
}

auto evaluate(const char* body) -> st::Result<st::raster::ThreeScene> {
  return st::raster::ThreeRuntime::instance().evaluate(body, "<camera-test>");
}

/// 场景外壳：一个相机 + 一个哨兵盒（避免"空场景"被当成错误）。
constexpr const char* kWrapperHead = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
const probe = new THREE.Mesh(new THREE.BoxGeometry(0.01, 0.01, 0.01),
  new THREE.MeshStandardMaterial({ color: 0xffffff }));
probe.name = 'probe';
scene.add(probe);
)js";
constexpr const char* kWrapperTail = "\nscene;\n";

auto build(const std::string& middle) -> st::Result<st::raster::ThreeScene> {
  const std::string source = std::string(kWrapperHead) + middle + kWrapperTail;
  return evaluate(source.c_str());
}

}  // namespace

ST_TEST(three_camera_look_at_axis_aligned) {
  // 看向原点：方向必须是纯 -Z
  auto scene = build(R"js(
camera.position.set(0, 0, 10);
camera.lookAt(0, 0, 0);
scene.add(camera);
)js");
  ST_REQUIRE(scene.has_value());
  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  camera_direction(scene->camera, nx, ny, nz);
  ST_CHECK_NEAR(nx, 0.0, 1e-4);
  ST_CHECK_NEAR(ny, 0.0, 1e-4);
  ST_CHECK_NEAR(nz, -1.0, 1e-4);
}

ST_TEST(three_camera_look_at_diagonal_is_45_degrees) {
  // ## 这一条就是那次误判的现场
  //
  // eye=(0,0,10) → target=(10,0,0)：方向是 (0.7071, 0, -0.7071)，
  // 即**45°**。当时从 `target.z = 9.29` 读成"z 只有 -0.29"从而算出 22°，
  // 于是误判 "角度减半"。
  //
  // 这里直接断言**方向**（而不是 target），把口径钉死。
  auto scene = build(R"js(
camera.position.set(0, 0, 10);
camera.lookAt(10, 0, 0);
scene.add(camera);
)js");
  ST_REQUIRE(scene.has_value());
  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  camera_direction(scene->camera, nx, ny, nz);
  const double kHalfSqrt2 = 0.70710678;
  ST_CHECK_NEAR(nx, kHalfSqrt2, 1e-4);
  ST_CHECK_NEAR(ny, 0.0, 1e-4);
  ST_CHECK_NEAR(nz, -kHalfSqrt2, 1e-4);

  // 顺手把 target 也断言上：它是"点"（含 eye 偏移），不是方向。
  // 写出来是为了让后来者一眼看出两者不同。
  ST_CHECK_NEAR(static_cast<double>(scene->camera.target.z), 10.0 - kHalfSqrt2, 1e-3);
}

ST_TEST(three_camera_look_at_pitch_works) {
  // 俯视：eye 在上方看向原点，方向 y 分量必须为负且量级正确
  auto scene = build(R"js(
camera.position.set(0, 10, 10);
camera.lookAt(0, 0, 0);
scene.add(camera);
)js");
  ST_REQUIRE(scene.has_value());
  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  camera_direction(scene->camera, nx, ny, nz);
  const double kHalfSqrt2 = 0.70710678;
  ST_CHECK_NEAR(nx, 0.0, 1e-4);
  ST_CHECK_NEAR(ny, -kHalfSqrt2, 1e-4);
  ST_CHECK_NEAR(nz, -kHalfSqrt2, 1e-4);
}

ST_TEST(three_camera_look_at_matches_manual_quaternion) {
  // `lookAt` 与手写 quaternion 必须得到同一个朝向（差一个符号即说明理解错了）。
  //
  // 手写路径：绕 Y 转 yaw 后，方向是 `(-sin(yaw), 0, -cos(yaw))`。
  // 所以要朝向 `(dx, 0, dz)` 归一化后的方向，yaw = atan2(-dx, -dz)。
  auto look_at_scene = build(R"js(
camera.position.set(0, 0, 10);
camera.lookAt(6, 0, 8);
scene.add(camera);
)js");
  auto manual_scene = build(R"js(
camera.position.set(0, 0, 10);
const yaw = Math.atan2(-6, -(-2));
camera.quaternion.setFromAxisAngle(new THREE.Vector3(0, 1, 0), yaw);
scene.add(camera);
)js");
  ST_REQUIRE(look_at_scene.has_value());
  ST_REQUIRE(manual_scene.has_value());

  double ax = 0.0;
  double ay = 0.0;
  double az = 0.0;
  double bx = 0.0;
  double by = 0.0;
  double bz = 0.0;
  camera_direction(look_at_scene->camera, ax, ay, az);
  camera_direction(manual_scene->camera, bx, by, bz);
  ST_CHECK_NEAR(ax, bx, 1e-3);
  ST_CHECK_NEAR(ay, by, 1e-3);
  ST_CHECK_NEAR(az, bz, 1e-3);
}

ST_TEST(three_camera_look_at_reaches_backend_camera) {
  // 抽取出来的相机要能被 `Camera` 结构直接用于渲染（不是只有 `target` 对）。
  auto scene = build(R"js(
camera.position.set(3, 4, 5);
camera.lookAt(0, 0, 0);
scene.add(camera);
)js");
  ST_REQUIRE(scene.has_value());
  const auto& camera = scene->camera;
  // eye 与脚本写的一致
  ST_CHECK_NEAR(static_cast<double>(camera.eye.x), 3.0, 1e-4);
  ST_CHECK_NEAR(static_cast<double>(camera.eye.y), 4.0, 1e-4);
  ST_CHECK_NEAR(static_cast<double>(camera.eye.z), 5.0, 1e-4);
  // 方向指向原点：与 (-3,-4,-5) 同向
  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  camera_direction(camera, nx, ny, nz);
  const double len = std::sqrt(9.0 + 16.0 + 25.0);
  ST_CHECK_NEAR(nx, -3.0 / len, 1e-3);
  ST_CHECK_NEAR(ny, -4.0 / len, 1e-3);
  ST_CHECK_NEAR(nz, -5.0 / len, 1e-3);
  // near/far/fov 也要带出来（缺了会让"物体被裁掉"变成谜题）
  ST_CHECK(camera.near_z > 0.0f);
  ST_CHECK(camera.far_z > camera.near_z);
  ST_CHECK(camera.fov_y_degrees > 1.0f);
}
