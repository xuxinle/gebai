/// 颜色管线的实现：sRGB 编解码 + 色调映射。纯函数，无状态。
///
/// 各算子的常数出处（都可核对）：
/// - sRGB 分段：IEC 61966-2-1；与 Blender `gpu_shader_colorspace.bsl.hh`
///   的 `scene_linear_to_rec709_srgb` 同一组（0.04045 / 12.92 / 1.055 / 2.4）。
/// - ACES 近似：Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" (2015)。
/// - Filmic：John Hable, "Uncharted 2: HDR Lighting" (2010) 的
///   `Uncharted2Tonemap`，按 Hable 原文的常数（A/B/C/D/E/F）。

#include "st/raster/color_pipeline.hpp"

#include <algorithm>

namespace st::raster {
namespace {

constexpr float kSrgbCutoff = 0.0031308f;   // 线性侧分段点
constexpr float kSrgbSlope = 12.92f;
constexpr float kSrgbAlpha = 1.055f;
constexpr float kSrgbOffset = 0.055f;
constexpr float kSrgbGamma = 2.4f;

/// 线性 → sRGB 的**未截断**版本（内部用：先映射再 clamp）。
[[nodiscard]] auto encode_unclamped(float linear) noexcept -> float {
  if (linear <= kSrgbCutoff) return linear * kSrgbSlope;
  return kSrgbAlpha * std::pow(linear, 1.0f / kSrgbGamma) - kSrgbOffset;
}

/// Reinhard：x/(1+x)。定义域 x≥0 时值域 [0,1)。
[[nodiscard]] auto reinhard(float x) noexcept -> float {
  const float v = std::max(x, 0.0f);
  return v / (1.0f + v);
}

/// Narkowicz 的 ACES 近似。
///
/// 注意它**定义域是 [0, ∞)**，但对负值会给出奇怪结果（分子分母都变小），
/// 所以入口先 clamp。这条曲线在 1.0 处约 0.8，即"纯白会略灰"——
/// 这是电影感的来源（高光有余量滚降），不是 bug。
[[nodiscard]] auto aces(float x) noexcept -> float {
  const float v = std::max(x, 0.0f);
  constexpr float a = 2.51f;
  constexpr float b = 0.03f;
  constexpr float c = 2.43f;
  constexpr float d = 0.59f;
  constexpr float e = 0.14f;
  const float num = v * (a * v + b);
  const float den = v * (c * v + d) + e;
  return std::clamp(num / den, 0.0f, 1.0f);
}

/// Hable 的 Uncharted2 曲线（先抬曝光再映射，与原文一致）。
[[nodiscard]] auto hable_curve(float x) noexcept -> float {
  constexpr float A = 0.15f;
  constexpr float B = 0.50f;
  constexpr float C = 0.10f;
  constexpr float D = 0.20f;
  constexpr float E = 0.02f;
  constexpr float F = 0.30f;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

/// Filmic：Hable 曲线按白点归一化。
///
/// 白点取 11.2 是原文的 `W = 11.2`（"日光直射"对应值）。归一化后
/// 输入 11.2 映射到 1.0，否则整幅偏灰。
[[nodiscard]] auto filmic(float x) noexcept -> float {
  constexpr float kWhitePoint = 11.2f;
  const float v = std::max(x, 0.0f);
  const float white = hable_curve(kWhitePoint);
  if (white <= 0.0f) return 0.0f;
  return std::clamp(hable_curve(v) / white, 0.0f, 1.0f);
}

}  // namespace

auto linear_to_srgb(float linear) noexcept -> float {
  return std::clamp(encode_unclamped(std::clamp(linear, 0.0f, 1.0f)), 0.0f, 1.0f);
}

auto srgb_to_linear(float srgb) noexcept -> float {
  const float v = std::clamp(srgb, 0.0f, 1.0f);
  if (v <= kSrgbCutoff * kSrgbSlope) return v / kSrgbSlope;
  return std::pow((v + kSrgbOffset) / kSrgbAlpha, kSrgbGamma);
}

auto linear_to_srgb(math::Vec3 linear) noexcept -> math::Vec3 {
  return math::Vec3{linear_to_srgb(linear.x), linear_to_srgb(linear.y),
                    linear_to_srgb(linear.z)};
}

auto srgb_to_linear(math::Vec3 srgb) noexcept -> math::Vec3 {
  return math::Vec3{srgb_to_linear(srgb.x), srgb_to_linear(srgb.y),
                    srgb_to_linear(srgb.z)};
}

auto linear_to_srgb8(float linear) noexcept -> std::uint8_t {
  // 四舍五入而不是截断：截断会让整幅系统性偏暗半级（0.5 的偏差在 8bit 上是可见的）。
  const float encoded = linear_to_srgb(linear);
  const int value = static_cast<int>(std::lround(encoded * 255.0f));
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

auto tone_map_channel(float x, ToneMap op) noexcept -> float {
  switch (op) {
    case ToneMap::None:
      return std::clamp(x, 0.0f, 1.0f);
    case ToneMap::Reinhard:
      return reinhard(x);
    case ToneMap::Aces:
      return aces(x);
    case ToneMap::Filmic:
      return filmic(x);
  }
  return std::clamp(x, 0.0f, 1.0f);
}

auto tone_map(math::Vec3 hdr, ToneMap op) noexcept -> math::Vec3 {
  if (op == ToneMap::None) {
    return math::Vec3{std::clamp(hdr.x, 0.0f, 1.0f), std::clamp(hdr.y, 0.0f, 1.0f),
                      std::clamp(hdr.z, 0.0f, 1.0f)};
  }
  return math::Vec3{tone_map_channel(hdr.x, op), tone_map_channel(hdr.y, op),
                    tone_map_channel(hdr.z, op)};
}

auto encode_pixel(math::Vec3 hdr_linear, float exposure, ToneMap op) noexcept -> math::Color {
  const float gain = exposure > 0.0f ? exposure : 0.0f;
  const math::Vec3 exposed{hdr_linear.x * gain, hdr_linear.y * gain, hdr_linear.z * gain};
  const math::Vec3 mapped = tone_map(exposed, op);
  return math::Color{linear_to_srgb8(mapped.x), linear_to_srgb8(mapped.y),
                     linear_to_srgb8(mapped.z), 255};
}

}  // namespace st::raster
