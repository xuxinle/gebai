// 验证 `parse_scene_spec_text` 能同时吃 JSON 与 three.js 源码。
//
// ## 为什么这组判据重要
//
// 它是**裁云联动的唯一接口**：裁云的 `model_script` 字段、三维预览、
// 自检三条路都走这个函数。如果它只认 JSON，那 jianmu 里写的场景
// 就进不了片子——而那正是"联动"的全部意义。
#include <cstdio>
#include <string>

#include "st/raster/scene3d_script.hpp"

namespace {

auto describe(const char* label, const char* text) -> void {
  const auto spec = st::raster::parse_scene_spec_text(text);
  std::printf("%s\n", label);
  if (!spec.has_value()) {
    std::printf("  ✗ 失败：%s\n", spec.error().message.c_str());
    return;
  }
  std::printf("  ✓ 解析成功：objects=%zu three_script=%zu 字节 fov=%.1f\n",
              spec->objects.size(), spec->three_script.size(),
              static_cast<double>(spec->camera.fov_y_degrees));
}

}  // namespace

int main() {
  // ① 声明式 JSON（旧路，必须继续能用）
  describe("① JSON 声明式脚本", R"json({
  "camera": {"eye": [0, 1, 4], "target": [0, 0, 0], "fov": 45},
  "objects": [
    {"name": "box", "shape": "box", "size": [1, 1, 1],
     "translate": [0, 0.5, 0], "color": [180, 140, 80]}
  ]
})json");

  // ② three.js 源码（新路，jianmu 写的就是这种）
  describe("② three.js 源码", R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(38, 16 / 9, 0.1, 100);
camera.position.set(3, 2, 5);
camera.lookAt(0, 0, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0x8899bb, 0.8));
const key = new THREE.DirectionalLight(0xffffff, 2.0);
key.position.set(3, 4, 3);
scene.add(key);
const ball = new THREE.Mesh(new THREE.SphereGeometry(1, 32, 24),
  new THREE.MeshStandardMaterial({ color: 0xc79a4e, metalness: 0.9, roughness: 0.25 }));
ball.name = 'ball';
scene.add(ball);
scene;
)js");

  // ③ 空文本：必须给出明确错误（不能静默当成空场景）
  describe("③ 空文本（应当失败并说明原因）", "");

  // ④ 坏的 JSON：不能静默回退成 three.js（那会给一个莫名其妙的 JS 语法错）
  describe("④ 残缺 JSON（应当失败）", R"json({"camera": {"eye": [0,1,4]})json");
  return 0;
}
