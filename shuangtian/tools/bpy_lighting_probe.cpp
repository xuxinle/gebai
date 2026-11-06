// 诊断：多材质场景的灯光/材质到底抽成什么。
//
// ## 为什么需要
//
// 立方体渲染成**全黑**——可能是：光照方向反了、强度为 0、材质基色没设上、
// 或法线朝向不对。四者都表现为"黑的"，而 R/G/B 都是 0 时分不清是哪个。
//
// 所以把抽出来的**具体数值**打出来：有数字才有判断依据。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "st/raster/bpy_runtime.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法：bpy_lighting_probe <脚本.py>\n");
    return 2;
  }
  const auto script = read_file(argv[1]);
  if (script.empty()) {
    std::printf("读不到：%s\n", argv[1]);
    return 1;
  }
  if (!st::raster::BpySceneExtractor::available()) {
    std::printf("bpy 不可用\n");
    return 1;
  }
  const auto scene = st::raster::BpySceneExtractor::scene_from_script(script);
  if (!scene.has_value()) {
    std::printf("抽取失败：%s\n", scene.error().message.c_str());
    return 1;
  }

  std::printf("物体 %zu 个\n", scene->objects.size());
  for (const auto& object : scene->objects) {
    std::printf("\n[%s]\n", object.name.c_str());
    std::printf("  三角形 %zu，材质槽 %zu，分组 %zu\n",
                object.mesh.indices.size() / 3, object.materials.size(),
                object.mesh.material_group_count());
    const auto& m = object.material;
    std::printf("  material[0]: base=(%.3f %.3f %.3f) set=%d metal=%.2f rough=%.2f\n",
                double(m.base_color.x), double(m.base_color.y), double(m.base_color.z),
                m.base_color_set ? 1 : 0, double(m.metallic), double(m.roughness));
    for (std::size_t i = 0; i < object.materials.size(); ++i) {
      const auto& mi = object.materials[i];
      std::printf("  slot[%zu]:    base=(%.3f %.3f %.3f) set=%d\n", i,
                  double(mi.base_color.x), double(mi.base_color.y),
                  double(mi.base_color.z), mi.base_color_set ? 1 : 0);
    }
    for (std::size_t g = 0; g < object.mesh.material_group_count(); ++g) {
      const auto grp = object.mesh.material_group_at(g);
      std::printf("  group[%zu]: start=%u count=%u slot=%u\n", g, grp[0], grp[1], grp[2]);
    }
  }

  const auto& l = scene->lighting;
  std::printf("\nlighting:\n");
  std::printf("  light_from = (%.3f %.3f %.3f)\n", double(l.light_from.x),
              double(l.light_from.y), double(l.light_from.z));
  std::printf("  ambient=%.3f diffuse=%.3f exposure=%.3f ibl_strength=%.3f\n",
              double(l.ambient), double(l.diffuse), double(l.exposure),
              double(l.ibl_strength));
  std::printf("  tone_map=%d（0=None 1=Reinhard 2=ACES 3=Filmic）\n",
              static_cast<int>(l.tone_map));

  std::printf("\ncamera: fov=%.2f° eye=(%.2f %.2f %.2f)\n",
              double(scene->camera.fov_y_degrees), double(scene->camera.eye.x),
              double(scene->camera.eye.y), double(scene->camera.eye.z));
  for (const auto& w : scene->warnings) {
    std::printf("warning: %s\n", w.c_str());
  }
  return 0;
}
