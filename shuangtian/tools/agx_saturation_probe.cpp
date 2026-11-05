// 诊断：AgX 对「材质本色」的处理是否过度去饱和。
//
// ## 为什么需要
//
// 齿轮渲染出来黄铜色被冲成灰褐。可能是：
// ① AgX 在中间调去饱和过强（我的拟合实现的问题）
// ② 材质色本身送进去时就不是黄铜（色彩空间错）
// ③ 光照把值推到了高光区（AgX 该去饱和，但这是"正确"的）
//
// 三者都表现为"颜色变淡"，而处置方式完全不同。所以要把**每一步的具体
// 数值**打出来。
#include <cmath>
#include <cstdio>

#include "st/raster/color_pipeline.hpp"

namespace {

using st::math::Vec3;
using st::raster::ToneMap;

auto show(const char* label, Vec3 linear) -> void {
  const Vec3 aces = st::raster::tone_map(linear, ToneMap::Aces);
  const Vec3 agx = st::raster::tone_map(linear, ToneMap::Agx);
  const Vec3 none = st::raster::tone_map(linear, ToneMap::None);
  const auto to8 = [](float v) { return int(st::raster::linear_to_srgb8(v)); };
  std::printf("%-22s 输入(%.2f %.2f %.2f)\n", label, double(linear.x), double(linear.y),
              double(linear.z));
  std::printf("   ACES (%3d,%3d,%3d)   AgX (%3d,%3d,%3d)   None (%3d,%3d,%3d)\n",
              to8(aces.x), to8(aces.y), to8(aces.z), to8(agx.x), to8(agx.y), to8(agx.z),
              to8(none.x), to8(none.y), to8(none.z));
  // 饱和度（max-min）/max，用来量化"被冲淡了多少"
  const auto sat = [](Vec3 c) {
    const float hi = std::fmax(c.x, std::fmax(c.y, c.z));
    const float lo = std::fmin(c.x, std::fmin(c.y, c.z));
    return hi > 1e-6f ? (hi - lo) / hi : 0.0f;
  };
  std::printf("   饱和度：ACES %.2f   AgX %.2f   None %.2f\n", double(sat(aces)),
              double(sat(agx)), double(sat(none)));
}

}  // namespace

int main() {
  // 黄铜的本色（与 bpy_watch.py 里的 MAT_BRASS 一致）
  const Vec3 brass{0.72f, 0.55f, 0.24f};
  std::printf("=== 黄铜本色在三种色调映射下 ===\n\n");
  // 分别测「纯本色」与「受光后」（乘不同亮度）
  show("黄铜 × 0.3（暗）", Vec3{brass.x * 0.3f, brass.y * 0.3f, brass.z * 0.3f});
  show("黄铜 × 0.6（中）", Vec3{brass.x * 0.6f, brass.y * 0.6f, brass.z * 0.6f});
  show("黄铜 × 1.0（亮）", brass);
  show("黄铜 × 1.5（高光）", Vec3{brass.x * 1.5f, brass.y * 1.5f, brass.z * 1.5f});

  std::printf("\n=== 对照：高饱和纯色 ===\n\n");
  show("纯红 × 0.5", Vec3{0.5f, 0.0f, 0.0f});
  show("纯红 × 1.0", Vec3{1.0f, 0.0f, 0.0f});

  std::printf("\n=== 中性灰（基准：应当无饱和度）===\n\n");
  show("灰 × 0.18", Vec3{0.18f, 0.18f, 0.18f});
  show("灰 × 0.6", Vec3{0.6f, 0.6f, 0.6f});
  return 0;
}
