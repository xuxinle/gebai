// SIMD 边界：本文件是 CONVENTIONS §2 禁令（指针运算/R6 reinterpret_cast）的受控例外——
// 仅对 `std::uint32_t*` 行缓冲做定长向量访存，所有访存由调用方保证行内边界。
#include "st/raster/simd.hpp"

#include <algorithm>
#include <cstring>

#if defined(__SSE2__) || defined(__x86_64__)
#include <emmintrin.h>
#define ST_SIMD_X86 1
#else
#define ST_SIMD_X86 0
#endif

namespace st::raster::simd {
namespace {

#if ST_SIMD_X86
/// 每通道 (v * a + 127) / 255 的向量近似（v,a ≤ 255，不溢出 16 位）。
[[nodiscard]] inline auto scale_255(__m128i value, __m128i alpha) noexcept -> __m128i {
  const __m128i rounded = _mm_add_epi16(_mm_mullo_epi16(value, alpha), _mm_set1_epi16(127));
  return _mm_srli_epi16(_mm_add_epi16(rounded, _mm_srli_epi16(rounded, 8)), 8);
}

[[nodiscard]] inline auto low_lanes(std::uint32_t premultiplied) noexcept -> __m128i {
  return _mm_and_si128(_mm_set1_epi32(static_cast<int>(premultiplied)), _mm_set1_epi16(0x00FF));
}

[[nodiscard]] inline auto high_lanes(std::uint32_t premultiplied) noexcept -> __m128i {
  return _mm_and_si128(_mm_srli_epi16(_mm_set1_epi32(static_cast<int>(premultiplied)), 8),
                       _mm_set1_epi16(0x00FF));
}
#endif

[[nodiscard]] constexpr auto channel(std::uint32_t pixel, unsigned shift) noexcept
    -> std::uint32_t {
  return (pixel >> shift) & 0xFFU;
}

[[nodiscard]] constexpr auto pack(std::uint32_t red, std::uint32_t green, std::uint32_t blue,
                                  std::uint32_t alpha) noexcept -> std::uint32_t {
  return (red << 24U) | (green << 16U) | (blue << 8U) | alpha;
}

[[nodiscard]] auto blend_scalar(std::uint32_t dst, std::uint32_t src, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) noexcept -> std::uint32_t {
    return (value * alpha + 127U) / 255U;
  };
  const std::uint32_t sa = scale(channel(src, 0));
  if (sa == 0U) return dst;
  const std::uint32_t inverse = 255U - sa;
  const auto over = [inverse](std::uint32_t source, std::uint32_t destination) noexcept {
    return source + (destination * inverse + 127U) / 255U;
  };
  return pack(over(scale(channel(src, 24)), channel(dst, 24)),
              over(scale(channel(src, 16)), channel(dst, 16)),
              over(scale(channel(src, 8)), channel(dst, 8)), over(sa, channel(dst, 0)));
}

}  // namespace

auto backend() noexcept -> const char* {
#if ST_SIMD_X86
  return "sse2";
#elif defined(__ARM_NEON)
  return "neon";
#else
  return "scalar";
#endif
}

void fill_row(std::uint32_t* dst, std::size_t count, std::uint32_t premultiplied) noexcept {
  if (count == 0) return;
#if ST_SIMD_X86
  const __m128i value = _mm_set1_epi32(static_cast<int>(premultiplied));
  std::size_t index = 0;
  for (; index + 4 <= count; index += 4) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + index), value);
  }
  for (; index < count; ++index) dst[index] = premultiplied;
#else
  std::fill_n(dst, count, premultiplied);
#endif
}

void blend_row(std::uint32_t* dst, std::size_t count, std::uint32_t premultiplied,
               std::uint8_t alpha) noexcept {
  if (count == 0) return;
  if (alpha == 0) return;
  if (alpha == 255) {
    if ((premultiplied & 0xFFU) == 255U) {
      fill_row(dst, count, premultiplied);
      return;
    }
  }
#if ST_SIMD_X86
  const std::uint32_t source_alpha = ((premultiplied & 0xFFU) * alpha + 127U) / 255U;
  if (source_alpha == 0U) return;
  const __m128i alpha_vector = _mm_set1_epi16(static_cast<short>(alpha));
  const __m128i inverse_vector = _mm_set1_epi16(static_cast<short>(255U - source_alpha));
  const __m128i source_low = low_lanes(premultiplied);
  const __m128i source_high = high_lanes(premultiplied);
  const __m128i scaled_low = scale_255(source_low, alpha_vector);
  const __m128i scaled_high = scale_255(source_high, alpha_vector);
  std::size_t index = 0;
  for (; index + 4 <= count; index += 4) {
    const __m128i destination = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + index));
    const __m128i destination_low = _mm_and_si128(destination, _mm_set1_epi16(0x00FF));
    const __m128i destination_high =
        _mm_and_si128(_mm_srli_epi16(destination, 8), _mm_set1_epi16(0x00FF));
    const __m128i out_low = _mm_add_epi16(scaled_low, scale_255(destination_low, inverse_vector));
    const __m128i out_high = _mm_add_epi16(scaled_high, scale_255(destination_high, inverse_vector));
    const __m128i merged =
        _mm_or_si128(out_low, _mm_slli_epi16(_mm_and_si128(out_high, _mm_set1_epi16(0x00FF)), 8));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + index), merged);
  }
  for (; index < count; ++index) dst[index] = blend_scalar(dst[index], premultiplied, alpha);
#else
  for (std::size_t index = 0; index < count; ++index) {
    dst[index] = blend_scalar(dst[index], premultiplied, alpha);
  }
#endif
}

void erase_row(std::uint32_t* dst, std::size_t count, std::uint8_t alpha) noexcept {
  if (count == 0 || alpha == 0) return;
  const auto value = static_cast<std::uint32_t>(alpha);
  fill_row(dst, count, value);
}

}  // namespace st::raster::simd
