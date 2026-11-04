// 相机朝向抽取得对不对。
//
// ## 这个探针存在的原因（一条被自己误导的经历）
//
// 它原来只打印 `eye` 与 `target`。而 `target = eye + dir`（不是方向本身），
// 我一度把 `target.z = 9.29` 当成“方向 z 只有 -0.29”，于是断定
// “`lookAt` 在 QuickJS 里角度减半”，还据此在文档里写了一条规避方案。
//
// 实际上 `eye.z = 10`、方向 `(0.71, 0, -0.71)` → `target.z = 10 - 0.71 = 9.29`，
// 完全正确。
//
// 修法：**同时打印方向**（`target - eye` 归一化），让读数者不必心算。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

auto run_case(const char* label, const char* source) -> void {
  auto scene = st::raster::ThreeRuntime::instance().evaluate(source, "<probe>");
  std::printf("%s\n", label);
  if (!scene.has_value()) {
    std::printf("  求值失败：%s\n", scene.error().message.c_str());
    return;
  }
  const auto& camera = scene->camera;
  const double dx = static_cast<double>(camera.target.x) - static_cast<double>(camera.eye.x);
  const double dy = static_cast<double>(camera.target.y) - static_cast<double>(camera.eye.y);
  const double dz = static_cast<double>(camera.target.z) - static_cast<double>(camera.eye.z);
  const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
  const double nx = len > 0.0 ? dx / len : 0.0;
  const double ny = len > 0.0 ? dy / len : 0.0;
  const double nz = len > 0.0 ? dz / len : 0.0;
  std::printf("  eye    = (%.2f, %.2f, %.2f)\n", static_cast<double>(camera.eye.x),
              static_cast<double>(camera.eye.y), static_cast<double>(camera.eye.z));
  std::printf("  target = (%.2f, %.2f, %.2f)\n", static_cast<double>(camera.target.x),
              static_cast<double>(camera.target.y), static_cast<double>(camera.target.z));
  // **方向**才是要判读的量（target 是点，含 eye 的偏移）
  std::printf("  方向   = (%.4f, %.4f, %.4f)\n", nx, ny, nz);
  // 与 X 轴/水平面的夹角，方便与手算期望直接对照
  std::printf("  水平偏角 = %.1f°  俯仰 = %.1f°\n",
              std::atan2(nx, nz) * 180.0 / 3.14159265358979,
              std::atan2(ny, std::sqrt(nx * nx + nz * nz)) * 180.0 / 3.14159265358979);
}

}  // namespace

int main() {
  // 基准：eye 在 +Z、看向原点 → 方向应是 (0,0,-1)
  run_case("① 看向原点（期望方向 (0,0,-1)）", R"js(
const s = new THREE.Scene();
const c = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
c.position.set(0, 0, 10);
c.lookAt(0, 0, 0);
s.add(c);
s;
)js");

  // 斜向：eye 在 +Z、看向 +X → 期望方向 (0.7071, 0, -0.7071)
  run_case("② 看向 +X（期望方向 (0.7071,0,-0.7071)）", R"js(
const s = new THREE.Scene();
const c = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
c.position.set(0, 0, 10);
c.lookAt(10, 0, 0);
s.add(c);
s;
)js");

  // 俯视：eye 在 +Z、看向下方 → 期望方向 y 分量为负
  run_case("③ 看向下方（期望方向 y < 0）", R"js(
const s = new THREE.Scene();
const c = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
c.position.set(0, 10, 10);
c.lookAt(0, 0, 0);
s.add(c);
s;
)js");

  // 手写 quaternion（不调 lookAt）——与 ② 应完全一致
  run_case("④ 手写 quaternion（应与 ② 一致）", R"js(
const s = new THREE.Scene();
const c = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
c.position.set(0, 0, 10);
const yaw = Math.atan2(10, 10);
const pitch = 0;
c.quaternion.setFromAxisAngle(new THREE.Vector3(0, 1, 0), yaw);
c.quaternion.multiply(
  new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(1, 0, 0), -pitch));
s.add(c);
s;
)js");
  return 0;
}
