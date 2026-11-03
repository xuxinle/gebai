// connect 单场景数值直读：定位测试 3 的期望偏差。
#include <cstdio>
#include <cmath>

#include "st/raster/scene3d_script.hpp"

auto main() -> int {
  const auto spec = st::raster::parse_scene_spec_text(R"j({
    "objects": [
      {"name":"a","shape":"box","size":[1,1,1],"translate":[0,0,0]},
      {"name":"b","shape":"box","size":[1,1,1],"translate":[3,1,0]},
      {"name":"rod","shape":"cylinder","size":[0.1,"auto"],
       "connect":"top_of:a -> top_of:b"}
    ]})j");
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 1;
  }
  for (const auto& object : spec->objects) {
    std::printf("%-4s t=(%.4f, %.4f, %.4f) r=(%.4f, %.4f, %.4f) size_y=%.4f\n",
                object.name.c_str(), double(object.translate.x),
                double(object.translate.y), double(object.translate.z),
                double(object.rotation.x), double(object.rotation.y),
                double(object.rotation.z), double(object.size.y));
  }
  const float expected = std::sqrt(9.0f + 1.0f);
  std::printf("期望：midpoint=(1.5, 1.0, 0) length=%.4f\n", double(expected));
  return 0;
}
