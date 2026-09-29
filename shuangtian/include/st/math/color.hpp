#pragma once

/// 颜色：sRGB 8 位直通 alpha（设计 token 与 API 层用）＋ 预乘转换（渲染缓冲用）＋ HSL 与对比度工具。
/// 约定：`Color` 是**直通 alpha**（straight alpha）；画布内部存**预乘**（premultiplied）以减少混合误差。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "st/math/geometry.hpp"

namespace st::math {

struct Color {
  std::uint8_t r{0};
  std::uint8_t g{0};
  std::uint8_t b{0};
  std::uint8_t a{255};

  friend constexpr auto operator==(Color lhs, Color rhs) noexcept -> bool {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
  }

  [[nodiscard]] static constexpr auto rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
      -> Color {
    return Color{red, green, blue, 255};
  }
  [[nodiscard]] static constexpr auto rgba(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                                           std::uint8_t alpha) -> Color {
    return Color{red, green, blue, alpha};
  }
  /// `0xRRGGBB` 或 `0xRRGGBBAA`。
  [[nodiscard]] static constexpr auto from_hex(std::uint32_t value) noexcept -> Color {
    if (value > 0xFFFFFFU) {
      return Color{static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
                   static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                   static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                   static_cast<std::uint8_t>(value & 0xFFU)};
    }
    return Color{static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                 static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                 static_cast<std::uint8_t>(value & 0xFFU), 255};
  }

  /// HSL（h ∈ [0,360)，s/l ∈ [0,1]）→ sRGB8。
  [[nodiscard]] static constexpr auto from_hsl(float hue, float saturation, float lightness,
                                               std::uint8_t alpha = 255) noexcept -> Color {
    const float h = std::fmod(hue < 0.0f ? hue + 360.0f : hue, 360.0f) / 360.0f;
    const float s = clamp01(saturation);
    const float l = clamp01(lightness);
    const auto channel = [](float p, float q, float t) constexpr noexcept -> float {
      if (t < 0.0f) t += 1.0f;
      if (t > 1.0f) t -= 1.0f;
      if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
      if (t < 1.0f / 2.0f) return q;
      if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
      return p;
    };
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    const auto quantize = [](float value) constexpr noexcept -> std::uint8_t {
      const float scaled = clamp01(value) * 255.0f + 0.5f;
      return static_cast<std::uint8_t>(scaled);
    };
    return Color{quantize(channel(p, q, h + 1.0f / 3.0f)), quantize(channel(p, q, h)),
                 quantize(channel(p, q, h - 1.0f / 3.0f)), alpha};
  }

  [[nodiscard]] constexpr auto with_alpha(std::uint8_t alpha) const noexcept -> Color {
    return Color{r, g, b, alpha};
  }
  [[nodiscard]] constexpr auto with_alpha_f(float alpha) const noexcept -> Color {
    const float scaled = clamp01(alpha) * 255.0f + 0.5f;
    return Color{r, g, b, static_cast<std::uint8_t>(scaled)};
  }
  [[nodiscard]] constexpr auto scale_alpha(float factor) const noexcept -> Color {
    const float scaled = clamp01(static_cast<float>(a) / 255.0f * factor) * 255.0f + 0.5f;
    return Color{r, g, b, static_cast<std::uint8_t>(scaled)};
  }
  [[nodiscard]] constexpr auto to_hex() const noexcept -> std::uint32_t {
    return (static_cast<std::uint32_t>(r) << 24U) | (static_cast<std::uint32_t>(g) << 16U) |
           (static_cast<std::uint32_t>(b) << 8U) | static_cast<std::uint32_t>(a);
  }
  [[nodiscard]] auto to_css() const -> std::string;
  /// 与白色/黑色按比例调亮调暗（交互态 hover/active 用）。
  [[nodiscard]] auto lighten(float amount) const noexcept -> Color;
  [[nodiscard]] auto darken(float amount) const noexcept -> Color;
  [[nodiscard]] auto mix(const Color& other, float t) const noexcept -> Color;
};

/// 线性插值（含 alpha）。
[[nodiscard]] auto lerp(const Color& from, const Color& to, float t) noexcept -> Color;

/// 预乘（渲染缓冲像素布局：R',G',B',A）。
[[nodiscard]] constexpr auto premultiply(const Color& color) noexcept -> std::uint32_t {
  const std::uint32_t alpha = color.a;
  const std::uint32_t red = (static_cast<std::uint32_t>(color.r) * alpha + 127U) / 255U;
  const std::uint32_t green = (static_cast<std::uint32_t>(color.g) * alpha + 127U) / 255U;
  const std::uint32_t blue = (static_cast<std::uint32_t>(color.b) * alpha + 127U) / 255U;
  return (red << 24U) | (green << 16U) | (blue << 8U) | alpha;
}

/// 预乘像素 → 直通颜色。
[[nodiscard]] constexpr auto unpremultiply(std::uint32_t pixel) noexcept -> Color {
  const std::uint32_t alpha = pixel & 0xFFU;
  if (alpha == 0U) return Color{0, 0, 0, 0};
  const auto unscale = [alpha](std::uint32_t channel) constexpr noexcept -> std::uint8_t {
    const std::uint32_t value = (channel * 255U + alpha / 2U) / alpha;
    return static_cast<std::uint8_t>(value > 255U ? 255U : value);
  };
  return Color{unscale((pixel >> 24U) & 0xFFU), unscale((pixel >> 16U) & 0xFFU),
               unscale((pixel >> 8U) & 0xFFU), static_cast<std::uint8_t>(alpha)};
}

/// 相对亮度（WCAG 2.1，用于对比度校验）。
[[nodiscard]] auto relative_luminance(const Color& color) noexcept -> float;

/// 对比度比（1.0 ~ 21.0）；WCAG AA 正文要求 ≥ 4.5。
[[nodiscard]] auto contrast_ratio(const Color& foreground, const Color& background) noexcept -> float;

/// 在前景/背景对比度不足时把前景调向黑或白，直到达到目标（设计系统自检用）。
[[nodiscard]] auto ensure_contrast(const Color& foreground, const Color& background,
                                   float target = 4.5f) noexcept -> Color;

}  // namespace st::math
