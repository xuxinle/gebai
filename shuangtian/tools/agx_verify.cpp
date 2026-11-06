// AgX 逐点对账：把工具/blender_agx_truth.py 的每一档真值与本框架比。
//
// ## 为什么需要它（而不是只看中性灰）
//
// 中性灰只能验「曲线」。**色相**必须用纯色验：本框架原来的解析拟合把
// 纯红 (1,0,0) 渲染成橙色，而那在灰阶上完全看不出来。
//
// 用法：agx_verify [LUT 路径]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "st/raster/agx_lut.hpp"
#include "st/raster/color_pipeline.hpp"

namespace {

struct Case {
  const char* name;
  float r;
  float g;
  float b;
  int truth_r;
  int truth_g;
  int truth_b;
};

// 真值来自 `tools/blender_agx_truth.py`（Blender 5.2.3，Standard/AgX 对照），
// 并由 `tools/agx_chain_dump.py` 用 OCIO 复核过**逐字节一致**。
constexpr Case kCases[] = {
    {"gray_0p05", 0.05f, 0.05f, 0.05f, 58, 58, 58},
    {"gray_0p18", 0.18f, 0.18f, 0.18f, 118, 118, 118},
    {"gray_0p5", 0.5f, 0.5f, 0.5f, 170, 170, 170},
    {"gray_1p0", 1.0f, 1.0f, 1.0f, 197, 197, 197},
    {"gray_2p0", 2.0f, 2.0f, 2.0f, 217, 217, 217},
    {"red_1p0", 1.0f, 0.0f, 0.0f, 219, 57, 33},
    {"green_1p0", 0.0f, 1.0f, 0.0f, 110, 197, 81},
    {"blue_1p0", 0.0f, 0.0f, 1.0f, 20, 86, 209},
    {"brass", 0.72f, 0.55f, 0.24f, 186, 171, 140},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    // 让用户能指定 LUT（不设也能从 Blender 安装目录找到）
#if defined(_WIN32)
    _putenv_s("ST_AGX_LUT", argv[1]);
#else
    setenv("ST_AGX_LUT", argv[1], 1);
#endif
  }
  st::raster::agx::invalidate_cache();

  if (!st::raster::agx::available()) {
    std::printf("AgX LUT 不可用：%s\n", st::raster::agx::unavailable_reason());
    std::printf("（会退回解析拟合，数值会偏亮几十级）\n\n");
  } else {
    std::printf("AgX LUT 已就绪，尺寸 %d³\n\n", st::raster::agx::lut_size());
  }

  std::printf("%-11s %-22s %-22s %s\n", "case", "本框架", "真 Blender", "差");
  int worst = 0;
  int total = 0;
  int count = 0;
  for (const auto& c : kCases) {
    const auto px = st::raster::encode_pixel(st::math::Vec3{c.r, c.g, c.b}, 1.0f,
                                             st::raster::ToneMap::Agx);
    const int dr = int(px.r) - c.truth_r;
    const int dg = int(px.g) - c.truth_g;
    const int db = int(px.b) - c.truth_b;
    const int d = std::max({std::abs(dr), std::abs(dg), std::abs(db)});
    worst = std::max(worst, d);
    total += d;
    ++count;
    std::printf("%-11s rgb(%3d,%3d,%3d)          rgb(%3d,%3d,%3d)          %+d,%+d,%+d%s\n",
                c.name, int(px.r), int(px.g), int(px.b), c.truth_r, c.truth_g, c.truth_b, dr,
                dg, db, d == 0 ? "  ← 一致" : "");
  }
  std::printf("\n最大差 %d 级，平均差 %.2f 级（%d 个样本）\n", worst, double(total) / count,
              count);
  std::printf("判据（`docs/bpy_render_parity.md §十四`）：最大差 ≤ 2\n");
  return worst <= 2 ? 0 : 1;
}
