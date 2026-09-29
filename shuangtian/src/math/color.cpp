#include "st/math/color.hpp"

#include <format>

namespace st::math {
namespace {

[[nodiscard]] constexpr auto quantize(float value) noexcept -> std::uint8_t {
  const float scaled = clamp01(value) * 255.0f + 0.5f;
  return static_cast<std::uint8_t>(scaled);
}

/// sRGB 分量 → 线性光（WCAG 亮度计算）。
[[nodiscard]] auto to_linear(std::uint8_t channel) noexcept -> float {
  const float value = static_cast<float>(channel) / 255.0f;
  return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

auto Color::to_css() const -> std::string {
  if (a == 255U) return std::format("#{:02x}{:02x}{:02x}", r, g, b);
  return std::format("rgba({},{},{},{:.3f})", static_cast<unsigned>(r), static_cast<unsigned>(g),
                     static_cast<unsigned>(b), static_cast<double>(a) / 255.0);
}

auto Color::lighten(float amount) const noexcept -> Color {
  const float ratio = clamp01(amount);
  return Color{quantize(static_cast<float>(r) / 255.0f + (1.0f - static_cast<float>(r) / 255.0f) * ratio),
               quantize(static_cast<float>(g) / 255.0f + (1.0f - static_cast<float>(g) / 255.0f) * ratio),
               quantize(static_cast<float>(b) / 255.0f + (1.0f - static_cast<float>(b) / 255.0f) * ratio),
               a};
}

auto Color::darken(float amount) const noexcept -> Color {
  const float ratio = 1.0f - clamp01(amount);
  return Color{quantize(static_cast<float>(r) / 255.0f * ratio),
               quantize(static_cast<float>(g) / 255.0f * ratio),
               quantize(static_cast<float>(b) / 255.0f * ratio), a};
}

auto Color::mix(const Color& other, float t) const noexcept -> Color {
  const float ratio = clamp01(t);
  return st::math::lerp(*this, other, ratio);
}

auto lerp(const Color& from, const Color& to, float t) noexcept -> Color {
  const float ratio = clamp01(t);
  const auto blend_channel = [ratio](std::uint8_t a, std::uint8_t b) noexcept -> std::uint8_t {
    const float value = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * ratio;
    return static_cast<std::uint8_t>(value + 0.5f);
  };
  return Color{blend_channel(from.r, to.r), blend_channel(from.g, to.g),
               blend_channel(from.b, to.b), blend_channel(from.a, to.a)};
}

auto relative_luminance(const Color& color) noexcept -> float {
  return 0.2126f * to_linear(color.r) + 0.7152f * to_linear(color.g) + 0.0722f * to_linear(color.b);
}

auto contrast_ratio(const Color& foreground, const Color& background) noexcept -> float {
  const float light = relative_luminance(foreground);
  const float dark = relative_luminance(background);
  const float lighter = light > dark ? light : dark;
  const float darker = light > dark ? dark : light;
  return (lighter + 0.05f) / (darker + 0.05f);
}

auto ensure_contrast(const Color& foreground, const Color& background, float target) noexcept
    -> Color {
  if (contrast_ratio(foreground, background) >= target) return foreground;
  const Color black{0, 0, 0, foreground.a};
  const Color white{255, 255, 255, foreground.a};
  Color best = foreground;
  float best_ratio = contrast_ratio(foreground, background);
  for (int step = 1; step <= 20; ++step) {
    const float ratio = static_cast<float>(step) / 20.0f;
    for (const Color& anchor : {black, white}) {
      const Color candidate = lerp(foreground, anchor, ratio);
      const float candidate_ratio = contrast_ratio(candidate, background);
      if (candidate_ratio > best_ratio) {
        best_ratio = candidate_ratio;
        best = candidate;
      }
    }
  }
  return best;
}

}  // namespace st::math
