// AgX 在关键亮度上的输出测量。
//
// ## 为什么需要它
//
// bpy 路径对齐真 Blender 时，背景出现系统性偏亮（本框架 rgb(129,135,147)
// vs 真 Blender rgb(71,76,83)），而清屏输入是同一个线性值 0.0706。
// 唯一可能出问题的环节就是色调映射算子。
//
// 本探针把 `ToneMap::Agx` 在若干标准亮度上的输出打印出来，好与
// 真 Blender 的 AgX 对照，判断是“实现有偏差”还是“本来就是这种观感”。
#include <cstdio>

#include "st/math/color.hpp"
#include "st/raster/color_pipeline.hpp"

auto main() -> int {
  using namespace st;
  const float values[] = {0.001f, 0.01f, 0.05f, 0.0706f, 0.18f, 0.5f, 1.0f, 2.0f, 4.0f};
  std::printf("%-10s %-24s %-24s\n", "线性", "AgX", "ACES");
  for (float v : values) {
    const auto agx =
        raster::encode_pixel(math::Vec3{v, v, v}, 1.0f, raster::ToneMap::Agx);
    const auto aces =
        raster::encode_pixel(math::Vec3{v, v, v}, 1.0f, raster::ToneMap::Aces);
    std::printf("%-10.4f rgb(%3d,%3d,%3d)         rgb(%3d,%3d,%3d)\n", double(v), int(agx.r),
                int(agx.g), int(agx.b), int(aces.r), int(aces.g), int(aces.b));
  }
  return 0;
}
