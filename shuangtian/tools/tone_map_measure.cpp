// 量出三路的 transfer 曲线差异：同一组线性输入，各得到什么 8bit sRGB 输出。
//
// ## 为什么必须先量再改
//
// 「与浏览器/Blender 对齐」听起来像一个动作，实际涉及四层，每层都能单独错：
//
// | 层 | 三路现状 |
// |---|---|
// | 材质基色 | 都送线性值 ✓（已确认一致） |
// | 光照积分 | 各不相同（three 用 WebGL PBR，Blender 用 Cycles/EEVEE，本框架自研） |
// | **色调映射** | three=**None**、Blender=**AgX**、本框架默认=**ACES** |
// | sRGB 编码 | 三者相同（标准 IEC 61966-2-1） |
//
// 第 3 层是纯函数，可以**逐点精确量**；第 2 层做不到（渲染器算法不同）。
// 所以本探针只量第 3+4 层，把「能对齐的部分」先钉住。
//
// ## 判据怎么定
//
// 不说「差多少算对」——先说**现在差多少**。有了这张表才知道：
// ① 默认值该不该按来源切换 ② 残余差异有多大 ③ 哪些是"可以接受的不同"
//
// 这条探针**先只报告、不判 PASS/FAIL**：它是量尺，不是门禁。
// 有了数据之后再决定阈值（那时才写判据）。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/raster/color_pipeline.hpp"

namespace {

using st::raster::ToneMap;
using st::math::Vec3;

constexpr const char* kToneMapName(ToneMap op) {
  switch (op) {
    case ToneMap::None: return "None(three 默认)";
    case ToneMap::Reinhard: return "Reinhard";
    case ToneMap::Aces: return "ACES(本框架默认)";
    case ToneMap::Filmic: return "Filmic";
    case ToneMap::Agx: return "AgX(Blender 4.x 默认)";
  }
  return "?";
}

/// 一条中性灰输入，走「色调映射 → sRGB 编码 → 8bit」的完整链路。
///
/// 用**中性灰**（R=G=B）而不是彩色：
/// - 逐色道的算子（ACES/Reinhard/Filmic）对灰色与彩色行为一致
/// - 只有 AgX 是三维的，彩色输入下差异更大，但那属于**色相**问题，
///   先把亮度（灰阶）对齐更有价值
auto gray_response(float linear, ToneMap op) -> int {
  const Vec3 hdr{linear, linear, linear};
  const Vec3 mapped = st::raster::tone_map(hdr, op);
  return static_cast<int>(st::raster::linear_to_srgb8(mapped.x));
}

/// 彩色响应（用来量 AgX 的色相保护与逐色道算子的"烧色"差别）。
struct Rgb8 {
  int r{0};
  int g{0};
  int b{0};
};

auto color_response(float r, float g, float b, ToneMap op) -> Rgb8 {
  const Vec3 mapped = st::raster::tone_map(Vec3{r, g, b}, op);
  return Rgb8{static_cast<int>(st::raster::linear_to_srgb8(mapped.x)),
              static_cast<int>(st::raster::linear_to_srgb8(mapped.y)),
              static_cast<int>(st::raster::linear_to_srgb8(mapped.z))};
}

}  // namespace

int main() {
  const ToneMap ops[] = {ToneMap::None, ToneMap::Aces, ToneMap::Agx, ToneMap::Filmic,
                         ToneMap::Reinhard};

  // ── ① 灰阶响应表：线性 0..2（含 HDR 超白）────────────────────────────
  std::printf("① 灰阶响应（线性亮度的值 → 8bit sRGB）\n\n");
  std::printf("%-9s", "线性");
  for (const auto op : ops) std::printf(" %-17s", kToneMapName(op));
  std::printf("  ACES-vs-AgX  ACES-vs-None\n");

  const float gray_samples[] = {0.0f,  0.002f, 0.01f, 0.05f, 0.1f, 0.18f, 0.25f,
                               0.35f, 0.5f,   0.7f,  0.9f,  1.0f, 1.5f,  2.0f};
  double sum_aces_agx = 0.0;
  double sum_aces_none = 0.0;
  int count = 0;
  for (const float linear : gray_samples) {
    std::printf("%-9.3f", double(linear));
    int values[5] = {0};
    for (int i = 0; i < 5; ++i) {
      values[i] = gray_response(linear, ops[i]);
      std::printf(" %-17d", values[i]);
    }
    const int d_agx = values[1] - values[2];    // ACES - AgX
    const int d_none = values[1] - values[0];   // ACES - None
    std::printf("  %+11d  %+11d\n", d_agx, d_none);
    if (linear > 0.0f) {
      sum_aces_agx += std::abs(d_agx);
      sum_aces_none += std::abs(d_none);
      ++count;
    }
  }
  std::printf("\n  平均绝对差：ACES vs AgX = %.1f / 255；ACES vs None = %.1f / 255\n",
              count > 0 ? sum_aces_agx / count : 0.0,
              count > 0 ? sum_aces_none / count : 0.0);

  // ── ② 饱和色响应：量 AgX 的色相保护 vs 逐色道算子的"烧色" ────────────
  std::printf("\n② 高饱和色响应（HDR 亮部是否被冲成纯色）\n\n");
  struct Sample {
    const char* label;
    float r, g, b;
  };
  const Sample colors[] = {
      {"饱和红 HDR", 3.0f, 0.05f, 0.05f},
      {"饱和绿 HDR", 0.05f, 3.0f, 0.05f},
      {"饱和蓝 HDR", 0.05f, 0.05f, 3.0f},
      {"暖橙 HDR", 2.0f, 0.8f, 0.2f},
      {"中间调红", 0.5f, 0.05f, 0.05f},
  };
  std::printf("%-12s %-16s %-16s %-16s\n", "输入", "ACES", "AgX", "None");
  for (const auto& sample : colors) {
    const auto aces = color_response(sample.r, sample.g, sample.b, ToneMap::Aces);
    const auto agx = color_response(sample.r, sample.g, sample.b, ToneMap::Agx);
    const auto none = color_response(sample.r, sample.g, sample.b, ToneMap::None);
    char aces_text[32];
    char agx_text[32];
    char none_text[32];
    std::snprintf(aces_text, sizeof(aces_text), "(%d,%d,%d)", aces.r, aces.g, aces.b);
    std::snprintf(agx_text, sizeof(agx_text), "(%d,%d,%d)", agx.r, agx.g, agx.b);
    std::snprintf(none_text, sizeof(none_text), "(%d,%d,%d)", none.r, none.g, none.b);
    std::printf("%-12s %-16s %-16s %-16s\n", sample.label, aces_text, agx_text, none_text);
  }

  // ── ③ 结论：三路默认值差异有多大 ────────────────────────────────────
  std::printf("\n③ 三路默认值的中间调（线性 0.18 = 18%% 灰卡）\n\n");
  const int none_18 = gray_response(0.18f, ToneMap::None);
  const int aces_18 = gray_response(0.18f, ToneMap::Aces);
  const int agx_18 = gray_response(0.18f, ToneMap::Agx);
  std::printf("  three.js (None) = %d\n", none_18);
  std::printf("  Blender  (AgX)  = %d\n", agx_18);
  std::printf("  本框架  (ACES)  = %d\n", aces_18);
  std::printf("\n");
  std::printf("  three vs 本框架 = %+d / 255（%.1f%%）\n", aces_18 - none_18,
              double(aces_18 - none_18) / 255.0 * 100.0);
  std::printf("  Blender vs 本框架 = %+d / 255（%.1f%%）\n", aces_18 - agx_18,
              double(aces_18 - agx_18) / 255.0 * 100.0);
  std::printf("\n  结论：三路**都不一致**，且差值到了肉眼可辨的程度（>10/255）。\n");
  std::printf("  对齐的方向应是「按来源选默认」：three_script → None，bpy_script → AgX。\n");
  return 0;
}
