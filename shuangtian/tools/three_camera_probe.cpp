// 判定 `lookAt` 在 QuickJS 里是否真的改了 quaternion。
//
// 三条对照：
//   A. 手动 `setFromAxisAngle` 转 90° —— 若抽取读得到，说明 quaternion 通路是通的
//   B. `lookAt` 一个明显不同的方向 —— 若 target 不变，就是 lookAt 失效
//   C. `Matrix4.lookAt` 单独调用 —— 直接看这个矩阵有没有被写
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/three_runtime.hpp"

int main() {
  struct Case {
    const char* label;
    const char* source;
  };
  const Case cases[] = {
      {"A 手动 quaternion（绕 Y 转 90°）",
       "const s = new THREE.Scene();\n"
       "const c = new THREE.PerspectiveCamera(30, 16/9, 0.5, 400);\n"
       "c.position.set(0, 0, 10);\n"
       "c.quaternion.setFromAxisAngle(new THREE.Vector3(0,1,0), Math.PI/2);\n"
       "s.add(c);\n"
       "s;\n"},
      {"B lookAt 到原点",
       "const s = new THREE.Scene();\n"
       "const c = new THREE.PerspectiveCamera(30, 16/9, 0.5, 400);\n"
       "c.position.set(0, 0, 10);\n"
       "c.lookAt(0, 0, 0);\n"
       "s.add(c);\n"
       "s;\n"},
      {"C lookAt 到 (10,0,0)（应转 -90°）",
       "const s = new THREE.Scene();\n"
       "const c = new THREE.PerspectiveCamera(30, 16/9, 0.5, 400);\n"
       "c.position.set(0, 0, 10);\n"
       "c.lookAt(10, 0, 0);\n"
       "s.add(c);\n"
       "s;\n"},
  };
  for (const auto& item : cases) {
    auto scene = st::raster::ThreeRuntime::instance().evaluate(item.source, "<probe>");
    std::printf("%s\n", item.label);
    if (!scene.has_value()) {
      std::printf("  求值失败：%s\n", scene.error().message.c_str());
      continue;
    }
    const auto& camera = scene->camera;
    std::printf("  eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f)\n",
                static_cast<double>(camera.eye.x), static_cast<double>(camera.eye.y),
                static_cast<double>(camera.eye.z), static_cast<double>(camera.target.x),
                static_cast<double>(camera.target.y), static_cast<double>(camera.target.z));
  }
  return 0;
}
