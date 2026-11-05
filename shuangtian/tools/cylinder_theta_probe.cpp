// 确定 `CylinderGeometry(..., thetaStart, thetaLength)` 转到 X 轴后，
// 究竟覆盖哪个方位。
//
// ## 为什么必须实测
//
// 剖切视图的方向全靠它：画错半圈 = 敞开的变成盖住的，而画面**不会报错**，
// 只是"里面什么都看不见"。而 Cylinder 的 θ 起点在 +Z、绕 Y 逆时针，
// 再叠加 `rotation.z = PI/2` 的坐标变换——推导极易出错。
//
// 判据：生成半圈筒，读回它的世界包围盒。
// 若包围盒的 y 只有正半边（或只有负半边），就知道覆盖的是上还是下。
#include <cstdio>
#include <string>

#include "st/raster/pick.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

auto probe(const char* label, const char* theta_args) -> void {
  const std::string script =
      std::string("const scene = new THREE.Scene();\n"
                  "const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);\n"
                  "camera.position.set(0, 0, 10);\n"
                  "scene.add(camera);\n"
                  "scene.add(new THREE.AmbientLight(0xffffff, 1.0));\n"
                  "// 半径 4 的半圈筒，长 2，绕 Y 生成后转到 X 轴\n"
                  "const geo = new THREE.CylinderGeometry(4, 4, 2, 32, 1, true, ") +
      theta_args +
      ");\n"
      "const m = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({ color: 0xaaaaaa }));\n"
      "m.name = 'half';\n"
      "m.rotation.z = Math.PI / 2;\n"
      "scene.add(m);\n"
      "scene;\n";
  auto scene = st::raster::ThreeRuntime::instance().evaluate(script, "<theta>");
  std::printf("%-28s", label);
  if (!scene.has_value()) {
    std::printf(" ✗ %s\n", scene.error().message.c_str());
    return;
  }
  if (scene->objects.empty()) {
    std::printf(" ✗ 无物体\n");
    return;
  }
  const auto& object = scene->objects.front();
  const auto pick = st::raster::make_pick_object(object.name, object.mesh, object.world);
  const auto& b = pick.world_bounds;
  std::printf(" y:[%.2f, %.2f]  z:[%.2f, %.2f]  x:[%.2f, %.2f]\n",
              static_cast<double>(b.min.y), static_cast<double>(b.max.y),
              static_cast<double>(b.min.z), static_cast<double>(b.max.z),
              static_cast<double>(b.min.x), static_cast<double>(b.max.x));
}

}  // namespace

int main() {
  std::printf("（半径 4；轴向转到 X。看 y 的范围判断覆盖上半还是下半）\n\n");
  probe("thetaStart=0  len=PI", "0, Math.PI");
  probe("thetaStart=PI len=PI", "Math.PI, Math.PI");
  probe("thetaStart=0  len=2PI", "0, Math.PI * 2");
  std::printf("\n判读：y 全为正 = 上半圈；y 全为负 = 下半圈；两者都有 = 整圈\n");
  return 0;
}
