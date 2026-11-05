// 涡轮发动机会用到的几何，逐个确认在 three_runtime 里能真出三角形。
//
// 为什么需要这个探针：`three_runtime` 是从 `geometry.attributes.position` 通用
// 抽取的，理论上所有几何都通。但"理论上通"和"实际有面"差得远——某个几何
// 若走索引缓冲、或顶点数在某个环节被丢掉，症状是**物体存在但画不出来**，
// 而物体数、网格数全都正常。涡轮这种"十几个同轴零件"的模型，少一层叶片
// 很难从截图上看出来。
#include <cstdio>
#include <string>
#include <vector>

#include "st/raster/three_runtime.hpp"

namespace {

/// 一个引肇零件会用到的一组几何。名字只用于诊断输出。
struct Case {
  const char* name;
  const char* source;
};

}  // namespace

int main() {
  const std::vector<Case> cases = {
      {"BoxGeometry", "new THREE.BoxGeometry(1, 2, 1)"},
      {"CylinderGeometry", "new THREE.CylinderGeometry(1, 1, 2, 16)"},
      {"ConeGeometry", "new THREE.ConeGeometry(1, 2, 16)"},
      {"SphereGeometry", "new THREE.SphereGeometry(1, 16, 12)"},
      {"TorusGeometry", "new THREE.TorusGeometry(1.5, 0.2, 8, 24)"},
      {"CircleGeometry", "new THREE.CircleGeometry(1, 24)"},
      {"RingGeometry", "new THREE.RingGeometry(0.6, 1.0, 24)"},
      {"PlaneGeometry", "new THREE.PlaneGeometry(2, 2)"},
      {"LatheGeometry", "new THREE.LatheGeometry([new THREE.Vector2(0.2, -1), "
                        "new THREE.Vector2(1.0, 0), new THREE.Vector2(0.2, 1)], 16)"},
      {"TubeGeometry", "new THREE.TubeGeometry(new THREE.CatmullRomCurve3(["
                       "new THREE.Vector3(0,0,0), new THREE.Vector3(1,1,0), "
                       "new THREE.Vector3(2,0,0)]), 12, 0.1, 6, false)"},
      {"SphereGeometry(open)", "new THREE.SphereGeometry(1, 16, 8, 0, Math.PI)"},
      {"CylinderGeometry(open)", "new THREE.CylinderGeometry(1, 1, 2, 16, 1, true)"},
  };

  for (const auto& item : cases) {
    const std::string script =
        std::string("const scene = new THREE.Scene();\n"
                    "const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);\n"
                    "camera.position.set(0, 0, 8);\n"
                    "scene.add(camera);\n"
                    "scene.add(new THREE.AmbientLight(0xffffff, 1.0));\n"
                    "const g = ") +
        item.source +
        ";\n"
        "const m = new THREE.Mesh(g, new THREE.MeshStandardMaterial({ color: 0xaaaaaa }));\n"
        "m.name = 'probe';\n"
        "scene.add(m);\n"
        "scene;\n";
    auto scene = st::raster::ThreeRuntime::instance().evaluate(script, "<geo>");
    if (!scene.has_value()) {
      std::printf("%-24s ✗ 求值失败：%s\n", item.name, scene.error().message.c_str());
      continue;
    }
    if (scene->objects.empty()) {
      std::printf("%-24s ✗ 物体数 0\n", item.name);
      continue;
    }
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    for (const auto& object : scene->objects) {
      vertices += object.mesh.vertices.size() / 9;
      triangles += object.mesh.indices.size() / 3;
    }
    std::printf("%-24s %s 顶点 %zu / 三角形 %zu\n", item.name,
                (vertices > 0 && triangles > 0) ? "✓" : "✗", vertices, triangles);
  }
  return 0;
}
