// 定位 `camera.lookAt()` 在 QuickJS 里的数值缺陷。
//
// ## 手段
//
// 没有直接的“读回任意 JS 值”接口，但物体名会被抽进 `ThreeObject::name`。
// 所以把诊断值拼进一个哨兵 Mesh 的名字里，C++ 侧读出来即可——
// 这是最轻的做法，不改任何生产接口。
//
// ## 要区分的三件事
//
// 1. `Matrix4.lookAt` 本身写对了没有（读矩阵元素）
// 2. `Quaternion.setFromRotationMatrix` 转对没有（读四元数）
// 3. `Vector3.applyQuaternion` 转对没有（读最终方向）
//
// 三者任一出错都会让“脚本里写的视角不起作用”，但修法完全不同。
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

/// 把一段诊断 JS 包成场景，取回哨兵物体的名字。
auto diagnose(const char* label, const char* body) -> void {
  const std::string source = std::string(R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
camera.position.set(0, 0, 10);
const parts = [];
)js") + body + R"js(
// 哨兵：名字里带着诊断结果
const probe = new THREE.Mesh(new THREE.BoxGeometry(0.01, 0.01, 0.01),
  new THREE.MeshStandardMaterial({ color: 0xffffff }));
probe.name = parts.join(' | ');
scene.add(probe);
scene;
)js";
  auto scene = st::raster::ThreeRuntime::instance().evaluate(source, "<diag>");
  std::printf("%s\n", label);
  if (!scene.has_value()) {
    std::printf("  求值失败：%s\n", scene.error().message.c_str());
    return;
  }
  for (const auto& object : scene->objects) {
    if (object.name == "BoxGeometry") continue;
    std::printf("  %s\n", object.name.c_str());
  }
}

}  // namespace

int main() {
  // ① 单独测 Matrix4.lookAt：不给 up，用默认 (0,1,0)
  //    期望：eye=(0,0,10)、target=(10,0,0) → z 轴 = normalize(eye-target) = (-0.707,0,0.707)
  //    则 te[8..10] 应约为 (-0.707, 0, 0.707)
  diagnose("① Matrix4.lookAt（eye 在 +Z，看向 +X）", R"js(
const m = new THREE.Matrix4();
m.lookAt(new THREE.Vector3(0, 0, 10), new THREE.Vector3(10, 0, 0), new THREE.Vector3(0, 1, 0));
parts.push('m8=' + m.elements[8].toFixed(4));
parts.push('m9=' + m.elements[9].toFixed(4));
parts.push('m10=' + m.elements[10].toFixed(4));
)js");

  // ② 把那个矩阵喂给 quaternion，看转出来什么
  //    若矩阵对而四元数错，问题在 setFromRotationMatrix
  diagnose("② setFromRotationMatrix（同一个矩阵）", R"js(
const m = new THREE.Matrix4();
m.lookAt(new THREE.Vector3(0, 0, 10), new THREE.Vector3(10, 0, 0), new THREE.Vector3(0, 1, 0));
const q = new THREE.Quaternion();
q.setFromRotationMatrix(m);
parts.push('q=(' + q.x.toFixed(4) + ',' + q.y.toFixed(4) + ',' + q.z.toFixed(4) + ',' + q.w.toFixed(4) + ')');
const d = new THREE.Vector3(0, 0, -1).applyQuaternion(q);
parts.push('dir=(' + d.x.toFixed(4) + ',' + d.y.toFixed(4) + ',' + d.z.toFixed(4) + ')');
// 期望 dir ≈ (0.707, 0, -0.707)
)js");

  // ③ 对照：手写 quaternion（绕 Y 转 -45°），看 applyQuaternion 本身准不准
  diagnose("③ 对照：手写 quaternion → applyQuaternion", R"js(
const q2 = new THREE.Quaternion();
q2.setFromAxisAngle(new THREE.Vector3(0, 1, 0), -Math.PI / 4);
const d2 = new THREE.Vector3(0, 0, -1).applyQuaternion(q2);
parts.push('dir=(' + d2.x.toFixed(4) + ',' + d2.y.toFixed(4) + ',' + d2.z.toFixed(4) + ')');
// 期望 dir ≈ (-0.707, 0, -0.707)
)js");

  // ④ 真实路径：camera.lookAt 之后读 quaternion
  diagnose("④ camera.lookAt（真实路径）", R"js(
camera.lookAt(10, 0, 0);
parts.push('q=(' + camera.quaternion.x.toFixed(4) + ',' + camera.quaternion.y.toFixed(4) + ','
           + camera.quaternion.z.toFixed(4) + ',' + camera.quaternion.w.toFixed(4) + ')');
const d3 = new THREE.Vector3(0, 0, -1).applyQuaternion(camera.quaternion);
parts.push('dir=(' + d3.x.toFixed(4) + ',' + d3.y.toFixed(4) + ',' + d3.z.toFixed(4) + ')');
)js");
  return 0;
}
