/// AgX 与真 Blender 的像素级基线。
///
/// ## 为什么这条测试必须存在
///
/// 本框架原先的 AgX 是 Troy Sobotka 的**解析拟合**，实测在 0.18 灰处
/// 比真 Blender 亮 **70 级**，而且把纯红渲成橙色（色相都不对）。
/// 换成 Blender 的 OCIO LUT 之后逐点命中——但那需要 2.9 MB 的外部数据，
/// 极容易被“简化”回去。这条测试把结果钉住。
///
/// ## 期望值从哪来（**不是手推的**）
///
/// 全部来自 `tools/blender_agx_truth.py`（自发光平面给精确线性值）与
/// `tools/agx_chain_dump.py`（让 OCIO 自己算，复核逐字节一致）。
///
/// ## LUT 缺席时怎么办
///
/// 本仓不嵌入那 2.9 MB 的 `.cube`（换版本要能替换、且不该让仓库多一个
/// 二进制源文件）。所以测试在 LUT 不可用时**跳过**并说明原因，而不是红：
///
/// · 有 LUT  → 逐点对账（最大差 ≤2）
/// · 无 LUT  → 只验证「降级路径仍然自洽」（不 crash、输出在 [0,1]）
///
/// 这样 CI 不依赖 Blender 安装，而本地有 Blender 时能验真值。
#include <cmath>
#include <cstdlib>

#include "st/raster/agx_lut.hpp"
#include "st/raster/color_pipeline.hpp"
#include "st/test/test.hpp"

namespace {

struct AgxCase {
  const char* name;
  float r;
  float g;
  float b;
  int truth_r;
  int truth_g;
  int truth_b;
};

// 真值来源：`tools/blender_agx_truth.py` + `tools/agx_chain_dump.py`
// （Blender 5.2.3，display=sRGB，view=AgX，look=None，exposure=0）
constexpr AgxCase kCases[] = {
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

/// AgX 的 `tone_map` 输出**已经是显示编码值**，`encode_pixel` 不能再编码一次。
///
/// ## 这条守的是一个实测撞到的差
///
/// 第一版让 `encode_pixel` 对 `tone_map` 的返回值照常做 `linear_to_srgb8`，
/// 结果 0.18 灰从真值 118 抬到 **186**（+68 级）——而画面看上去只是“更亮”，
/// 极难定位。
///
/// 所以本测试直接断言契约标志，再断言像素：只改一个地方（把标志去掉）就能
/// 让两条同时红。
ST_TEST(agx_output_is_already_encoded) {
  ST_CHECK(st::raster::tone_map_outputs_encoded(st::raster::ToneMap::Agx));
  // 其余算子都是「输出线性值」，不能被当成已编码
  ST_CHECK(!st::raster::tone_map_outputs_encoded(st::raster::ToneMap::None));
  ST_CHECK(!st::raster::tone_map_outputs_encoded(st::raster::ToneMap::Aces));
  ST_CHECK(!st::raster::tone_map_outputs_encoded(st::raster::ToneMap::AcesThree));
  ST_CHECK(!st::raster::tone_map_outputs_encoded(st::raster::ToneMap::Filmic));
  ST_CHECK(!st::raster::tone_map_outputs_encoded(st::raster::ToneMap::Reinhard));
}

/// 降级路径自洽：LUT 不可用时 AgX 仍能返回 [0,1] 内的值（不崩、不为 NaN）。
///
/// 这一条**故意不验数值**——解析拟合与真值差几十级是已知的，
/// 把它的数值当基线会把错误固化。只验“有值且单调”。
ST_TEST(agx_degraded_path_is_self_consistent) {
  // 无论 LUT 在不在，这条都应成立（在的时候走 LUT，也是单调的）
  const auto c0 = st::raster::encode_pixel(st::math::Vec3{0.0f, 0.0f, 0.0f}, 1.0f,
                                           st::raster::ToneMap::Agx);
  const auto c1 = st::raster::encode_pixel(st::math::Vec3{0.18f, 0.18f, 0.18f}, 1.0f,
                                           st::raster::ToneMap::Agx);
  const auto c2 = st::raster::encode_pixel(st::math::Vec3{1.0f, 1.0f, 1.0f}, 1.0f,
                                           st::raster::ToneMap::Agx);
  // 单调递增（AgX 是压缩曲线，但不是非单调的）
  ST_CHECK(int(c1.r) > int(c0.r));
  ST_CHECK(int(c2.r) > int(c1.r));
  // 不溢出
  ST_CHECK(int(c2.r) <= 255);
}

/// 有 LUT 时逐点对账真 Blender AgX。
///
/// ## 判据：最大差 ≤2
///
/// 那 2 级来自 LUT 的 tetrahedral 插值与格子密度（57³），**不是实现错**。
/// 实测 9 个样本里 7 个逐字节一致，`red_1p0` 的 B 通道差 2 是唯一位差。
///
/// ## 为什么必须包含纯色样本
///
/// 实测踩过：把 LUT 索引写成「b 变化最快」时，**中性灰 5 档全绿**
/// （三坐标相等，两种索引等价），而纯红渲成蓝色、纯蓝渲成红色。
/// 只测灰阶会一路通过。
ST_TEST(agx_matches_blender_agx_when_lut_available) {
  if (!st::raster::agx::available()) {
    // 没有 LUT：这条用例不适用（本仓不嵌入 .cube，见文件头说明）。
    // 不判红——但要留下**可观测**的痕迹，而不是静默跳过。
    ST_CHECK(st::raster::agx::unavailable_reason() != nullptr);
    ST_CHECK(st::raster::agx::unavailable_reason()[0] != '\0');
    return;
  }
  ST_CHECK(st::raster::agx::lut_size() == 57);

  int worst = 0;
  for (const auto& c : kCases) {
    const auto px = st::raster::encode_pixel(st::math::Vec3{c.r, c.g, c.b}, 1.0f,
                                             st::raster::ToneMap::Agx);
    worst = std::max({worst, std::abs(int(px.r) - c.truth_r),
                      std::abs(int(px.g) - c.truth_g), std::abs(int(px.b) - c.truth_b)});
  }
  ST_CHECK(worst <= 2);
}

/// 纯色不得串道：红的 R 必须显著大于 G/B，蓝的 B 必须显著大于 R/G。
///
/// ## 这条守什么
///
/// LUT 索引次序写错时（b 最快 vs r 最快），**红会渲成蓝**。上面那条
/// `matches_blender` 能抓到，但它的失败信息是「差 219 级」——不够直接。
/// 这条用**语义判据**（红通道关系）定位，一眼看出是串道而不是曲线偏。
ST_TEST(agx_primary_colors_do_not_cross_channels) {
  const auto red = st::raster::encode_pixel(st::math::Vec3{1.0f, 0.0f, 0.0f}, 1.0f,
                                            st::raster::ToneMap::Agx);
  // 红必须是最强通道（AgX 会去饱和，但主色相不变）
  ST_CHECK(int(red.r) > int(red.g));
  ST_CHECK(int(red.r) > int(red.b));

  const auto blue = st::raster::encode_pixel(st::math::Vec3{0.0f, 0.0f, 1.0f}, 1.0f,
                                             st::raster::ToneMap::Agx);
  ST_CHECK(int(blue.b) > int(blue.r));
  ST_CHECK(int(blue.b) > int(blue.g));

  const auto green = st::raster::encode_pixel(st::math::Vec3{0.0f, 1.0f, 0.0f}, 1.0f,
                                              st::raster::ToneMap::Agx);
  ST_CHECK(int(green.g) > int(green.r));
  ST_CHECK(int(green.g) > int(green.b));
}
