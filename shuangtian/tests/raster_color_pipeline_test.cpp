/// 颜色管线（sRGB 编解码 + 色调映射）回归测试。
///
/// ## 这组用例钉的是什么
///
/// 颜色管线的错误**不会崩溃、不会报错**，它只会让画面看起来"不太对"。
/// 所以判据只能是数值，而且要覆盖三类容易写错的地方：
///
/// 1. **sRGB 分段点不连续**：两段（线性段 / 幂函数段）在 0.0031308 处必须接上。
///    接不上就有一条肉眼可见的暗部"台阶"——且只出现在某些底色上。
/// 2. **往返不一致**：`srgb_to_linear(linear_to_srgb(x)) == x`。不一致说明
///    有一侧常数抄错了（0.04045 与 0.0031308 是最常见的混淆）。
/// 3. **色调映射把纯白压灰**：这是 ACES/Filmic 的**正常行为**（高光留余量），
///    但 `None` 绝不能压——它是默认档，压了老场景就变了。
///
/// ## 为什么必须有一条"默认不改变画面"
///
/// 渐进采用的底线：老脚本没碰新参数时，输出必须与改动前一致。
/// 这条断了，所有已出的片子都会变样，而且没人会立刻发现。

#include <cmath>
#include <cstdint>

#include "st/raster/color_pipeline.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::math::Vec3;
using st::raster::ToneMap;

namespace {

/// 浮点近似比较（sRGB 曲线在小值处斜率 12.92，容差按绝对值给）。
auto near(float a, float b, float tol) -> bool { return std::abs(a - b) <= tol; }

constexpr float kCutLinear = 0.0031308f;   // 线性侧分段点
constexpr float kCutSrgb = 0.04045f;       // 解码侧分段点（= 上线 × 12.92）

}  // namespace

ST_TEST(color_srgb_roundtrip_is_exact) {
  // 往返一致性：解码(编码(x)) == x。
  // 同时钉住两件事：分段点两侧常数配对正确、两段在接缝处连续。
  for (int i = 0; i <= 1000; ++i) {
    const float linear = static_cast<float>(i) / 1000.0f;
    const float back = st::raster::srgb_to_linear(st::raster::linear_to_srgb(linear));
    ST_CHECK(near(back, linear, 1e-4f));
  }
}

ST_TEST(color_srgb_is_continuous_at_both_boundaries) {
  // 编码侧接缝
  const float left = st::raster::linear_to_srgb(kCutLinear - 1e-7f);
  const float right = st::raster::linear_to_srgb(kCutLinear + 1e-7f);
  ST_CHECK(near(left, right, 1e-4f));
  // 解码侧接缝
  const float dl = st::raster::srgb_to_linear(kCutSrgb - 1e-7f);
  const float dr = st::raster::srgb_to_linear(kCutSrgb + 1e-7f);
  ST_CHECK(near(dl, dr, 1e-5f));
  (void)dl;
}

ST_TEST(color_srgb_dark_end_follows_the_linear_branch) {
  // ## 为何单立这条（逆向验证抓出的漏洞）
  //
  // “连续性”测不出“把分段函数退化成纯幂函数”——因为 **幂函数本身处处连续**。
  // 实测：把编码改成 `pow(x, 1/2.4)` 后，接缝测试仍然绿。
  //
  // 真正区分两者的是**暗部**：分段函数的暗部走线性段 `x*12.92`，
  // 而纯幂函数在暗部会抬高约 0.04~0.05（实测 x=0.003 时差 0.0501）。
  // 这个偏差在画面上是“暗处发灰”，绝对不能忍。
  ST_CHECK(near(st::raster::linear_to_srgb(0.003f), 0.0388f, 0.0015f));
  ST_CHECK(near(st::raster::linear_to_srgb(0.001f), 0.0129f, 0.0015f));
  // 暗部斜率必须严格是 12.92（线性段）。纯幂函数在这一段斜率约 3~5。
  const float slope = (st::raster::linear_to_srgb(0.002f) -
                       st::raster::linear_to_srgb(0.0005f)) / 0.0015f;
  ST_CHECK(near(slope, 12.92f, 0.05f));
}

ST_TEST(color_srgb_mid_grey_matches_documented_value) {
  // 已知锚点：线性 0.5 → sRGB ≈ 0.7354。这是整条曲线最容易被记住的数。
  ST_CHECK(near(st::raster::linear_to_srgb(0.5f), 0.7354f, 0.0005f));
  ST_CHECK(st::raster::linear_to_srgb(0.0f) == 0.0f);
  ST_CHECK(near(st::raster::linear_to_srgb(1.0f), 1.0f, 1e-6f));
}

ST_TEST(color_srgb8_rounds_instead_of_truncating) {
  // 截断会让整幅系统性偏暗半级。0.5 线性 → 187.5 → 四舍五入到 188。
  ST_CHECK(static_cast<int>(st::raster::linear_to_srgb8(0.5f)) == 188);
  ST_CHECK(static_cast<int>(st::raster::linear_to_srgb8(0.0f)) == 0);
  ST_CHECK(static_cast<int>(st::raster::linear_to_srgb8(1.0f)) == 255);
  // 超范围输入必须夹住，不能绕回
  ST_CHECK(static_cast<int>(st::raster::linear_to_srgb8(-1.0f)) == 0);
  ST_CHECK(static_cast<int>(st::raster::linear_to_srgb8(10.0f)) == 255);
}

ST_TEST(color_tonemap_none_is_identity_clamp) {
  // **渐进采用底线**：None 档时输出等于"线性→sRGB"，不再压灰。
  ST_CHECK(near(st::raster::tone_map_channel(0.3f, ToneMap::None), 0.3f, 1e-6f));
  ST_CHECK(near(st::raster::tone_map_channel(1.0f, ToneMap::None), 1.0f, 1e-6f));
  // 超过 1 的 HDR 夹到 1（不做滚降）
  ST_CHECK(near(st::raster::tone_map_channel(8.0f, ToneMap::None), 1.0f, 1e-6f));
  ST_CHECK(near(st::raster::tone_map_channel(-2.0f, ToneMap::None), 0.0f, 1e-6f));
}

ST_TEST(color_tonemap_operators_roll_off_highlights) {
  // 三个算子的共同特征：HDR 越大越接近 1，但**不会等于 1**（留余量），且单调。
  //
  // 注：ACES 近似在 x≈7.5 处会饱和到 1.0（该曲线本身的性质），
  // 所以这里用 2.0 / 5.0 这对点，而不是 2.0 / 8.0。
  for (const ToneMap op : {ToneMap::Reinhard, ToneMap::Aces, ToneMap::Filmic}) {
    const float a = st::raster::tone_map_channel(2.0f, op);
    const float b = st::raster::tone_map_channel(5.0f, op);
    ST_CHECK(a < 1.0f);
    ST_CHECK(b >= a);
    ST_CHECK(b <= 1.0f);
  }
  // 零处三者都必须为 0（无光就黑，任何算子都不该抬黑位）
  for (const ToneMap op : {ToneMap::Reinhard, ToneMap::Aces, ToneMap::Filmic}) {
    ST_CHECK(near(st::raster::tone_map_channel(0.0f, op), 0.0f, 1e-6f));
  }
}

ST_TEST(color_tonemap_operators_are_actually_different) {
  // 防"拷贝粘贴后三个算子一样"：中调必须分得开。
  const float r = st::raster::tone_map_channel(0.5f, ToneMap::Reinhard);
  const float a = st::raster::tone_map_channel(0.5f, ToneMap::Aces);
  const float f = st::raster::tone_map_channel(0.5f, ToneMap::Filmic);
  ST_CHECK(near(r, 1.0f / 3.0f, 1e-4f));   // Reinhard 的闭式解
  ST_CHECK(std::abs(r - a) > 0.05f);
  ST_CHECK(std::abs(a - f) > 0.01f);
}

ST_TEST(color_aces_matches_its_reference_values) {
  // ## 为何要钉具体值（逆向验证抓出的漏洞）
  //
  // 上面那条只要求“ACES 与 Filmic 差 >0.01”。实测把 ACES 的系数
  // `2.51` 改成 `2.15` 后，**两条都仍然绿**（那条|Δ|阈值刚好跨过）。
  //
  // 这里直接钉 Narkowicz 曲线的参考值（由公式本身算出，
  // 可用任何可信实现复核；容差 0.004 远小于错误实现的偏差 0.086）。
  ST_CHECK(near(st::raster::tone_map_channel(0.18f, ToneMap::Aces), 0.2669f, 0.004f));
  ST_CHECK(near(st::raster::tone_map_channel(0.25f, ToneMap::Aces), 0.3741f, 0.004f));
  ST_CHECK(near(st::raster::tone_map_channel(0.5f, ToneMap::Aces), 0.6163f, 0.004f));
  ST_CHECK(near(st::raster::tone_map_channel(1.0f, ToneMap::Aces), 0.8038f, 0.004f));
  ST_CHECK(near(st::raster::tone_map_channel(2.0f, ToneMap::Aces), 0.9149f, 0.004f));
}

ST_TEST(color_encode_pixel_applies_exposure_before_mapping) {
  // ## 为何必须钉具体值（逆向验证抓了三轮）
  //
  // 先用 `None` 档：None 就是 `clamp(x,0,1)`，**与乘法可交换**（不溢出时），
  // 错误实现结果完全相同。改用 ACES 后又只断言 `b>a && b<2a`——两种实现都满足。
  //
  // 真正分得开的是 **HDR 输入 + 曝光>1**：
  //   正确：aces(0.25×2) = aces(0.5) = 0.6163
  //   错误：min(1, aces(0.25)×2) = min(1, 0.7482) = 0.7482
  // 差 0.13，远超 8bit 量化步长。
  const Color a = st::raster::encode_pixel(Vec3{0.25f, 0.25f, 0.25f}, 1.0f, ToneMap::Aces);
  const Color b = st::raster::encode_pixel(Vec3{0.25f, 0.25f, 0.25f}, 2.0f, ToneMap::Aces);
  ST_CHECK(b.r > a.r);
  // 关键：输出字节必须等于**正确顺序**算出的值
  const float correct_mapped = st::raster::tone_map_channel(0.25f * 2.0f, ToneMap::Aces);
  const int expect_byte = static_cast<int>(st::raster::linear_to_srgb8(correct_mapped));
  ST_CHECK(static_cast<int>(b.r) == expect_byte);
  // 并且不能与错误顺序的结果相同（分辨力）
  const float wrong_order = st::raster::tone_map_channel(0.25f, ToneMap::Aces) * 2.0f;
  ST_CHECK(std::abs(correct_mapped - wrong_order) > 0.01f);
  // 曝光 0 必须全黑（而不是"忽略曝光"）
  const Color z = st::raster::encode_pixel(Vec3{0.9f, 0.9f, 0.9f}, 0.0f, ToneMap::None);
  ST_CHECK(z.r == 0);
  // 超亮输入 + ACES：有滚降，不会全白糊死。用 5.0（在 x≈7.5 处才饱和）
  const Color hot = st::raster::encode_pixel(Vec3{5.0f, 5.0f, 5.0f}, 1.0f, ToneMap::Aces);
  ST_CHECK(static_cast<int>(hot.r) < 255);
}
