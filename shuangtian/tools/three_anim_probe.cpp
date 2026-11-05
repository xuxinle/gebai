// 检查 three 场景的动画字段是否真的被抽出来了。
//
// 为什么单独立一个探针：`userData` 动画的失败是**完全静默**的——
// 场景照常渲染、物体数照常正确，只是永远不动。靠肉眼看静态截图
// 根本区分不出来（"它本来就没在动" vs "动画坏了"）。
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/three_runtime.hpp"

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "examples/jianmu/scenes/04-watch.js";
  const auto source = st::fs::read_text(path);
  if (!source.has_value()) {
    std::printf("读不到 %s\n", path.c_str());
    return 1;
  }
  auto scene = st::raster::ThreeRuntime::instance().evaluate(*source, path);
  if (!scene.has_value()) {
    std::printf("求值失败: %s\n", scene.error().message.c_str());
    return 1;
  }
  std::printf("物体 %zu 个\n", scene->objects.size());
  std::printf("相机 eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f) fov=%.1f\n",
              static_cast<double>(scene->camera.eye.x),
              static_cast<double>(scene->camera.eye.y),
              static_cast<double>(scene->camera.eye.z),
              static_cast<double>(scene->camera.target.x),
              static_cast<double>(scene->camera.target.y),
              static_cast<double>(scene->camera.target.z),
              static_cast<double>(scene->camera.fov_y_degrees));
  int spinning = 0;
  int oscillating = 0;
  int no_frame = 0;
  for (const auto& object : scene->objects) {
    if (object.no_frame) {
      ++no_frame;
      std::printf("  noFrame   %-22s（取景时不计入包围盒）\n", object.name.c_str());
    }
    if (object.spin_speed != 0.0f) {
      ++spinning;
      std::printf("  spin      %-22s speed=%.4f axis=(%.1f,%.1f,%.1f)\n", object.name.c_str(),
                  static_cast<double>(object.spin_speed),
                  static_cast<double>(object.spin_axis.x),
                  static_cast<double>(object.spin_axis.y),
                  static_cast<double>(object.spin_axis.z));
    }
    if (object.oscillate_amplitude != 0.0f) {
      ++oscillating;
      std::printf("  oscillate %-22s amp=%.3f freq=%.3f phase=%.3f\n", object.name.c_str(),
                  static_cast<double>(object.oscillate_amplitude),
                  static_cast<double>(object.oscillate_frequency),
                  static_cast<double>(object.oscillate_phase));
    }
  }
  std::printf("合计：自转 %d 个 / 振荡 %d 个 / 取景排除 %d 个\n", spinning, oscillating,
              no_frame);
  return (spinning + oscillating) > 0 ? 0 : 2;
}
