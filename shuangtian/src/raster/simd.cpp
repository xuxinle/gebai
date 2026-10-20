// SIMD 边界：本文件是 CONVENTIONS §2 禁令（指针运算/R6 reinterpret_cast）的受控例外——
// 仅对 `std::uint32_t*` 行缓冲做定长向量访存，所有访存由调用方保证行内边界。
#include "st/raster/simd.hpp"

#include <algorithm>
#include <array>
#include <cstring>

// x86-64 判定：MSVC 不定义 `__SSE2__`/`__x86_64__`（实测 cl 的预定义宏表）——
// 漏掉 `_M_X64` 会让整个 MSVC 构建**静默退化到 scalar**（基线上就这么发了好久，
// 由 backend() 能力自报测试抓出）。x64 平台基线即保证 SSE2。
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define ST_SIMD_X86 1
#else
#define ST_SIMD_X86 0
#endif

// AVX2：运行时检测（__cpuid），编译期只在指令集可用时启用（MSVC /arch 不强制全局开）。
// 语义与 SSE2/标量路径逐像素一致（tests/raster_simd_test.cpp 三路对齐断言）。
#if ST_SIMD_X86 && (defined(__AVX2__) || defined(_MSC_VER))
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define ST_SIMD_AVX2 1
#else
#define ST_SIMD_AVX2 0
#endif

namespace st::raster::simd {
namespace {

#if ST_SIMD_X86
/// 每通道 (v * a + 127) / 255 的向量近似（v,a ≤ 255，不溢出 16 位）。
[[nodiscard]] inline auto scale_255(__m128i value, __m128i alpha) noexcept -> __m128i {
  // 精确除 255：(x * 0x8081) >> 23——对全部 16 位 x 零误差（穷举验证）。
  // 旧实现 (t + (t>>8)) >> 8 在 65536 种输入里有 128 个 off-by-one，与标量参考
  // 不一致；由 tests/raster_simd_test.cpp 的「SIMD 逐像素等于标量」断言抓出。
  const __m128i rounded = _mm_add_epi16(_mm_mullo_epi16(value, alpha), _mm_set1_epi16(127));
  return _mm_srli_epi16(_mm_mulhi_epu16(rounded, _mm_set1_epi16(static_cast<short>(0x8081))), 7);
}

[[nodiscard]] inline auto low_lanes(std::uint32_t premultiplied) noexcept -> __m128i {
  return _mm_and_si128(_mm_set1_epi32(static_cast<int>(premultiplied)), _mm_set1_epi16(0x00FF));
}

[[nodiscard]] inline auto high_lanes(std::uint32_t premultiplied) noexcept -> __m128i {
  return _mm_and_si128(_mm_srli_epi16(_mm_set1_epi32(static_cast<int>(premultiplied)), 8),
                       _mm_set1_epi16(0x00FF));
}
#endif

#if ST_SIMD_AVX2
/// AVX2 版 16 通道并行 (v * a + 127) / 255（v,a ≤ 255，16 位 lane 不溢出）。
/// 精确除 255 公式同 SSE2 版：mulhi(t, 0x8081) >> 7。
[[nodiscard]] inline auto scale_255_avx(__m256i value, __m256i alpha) noexcept -> __m256i {
  const __m256i rounded = _mm256_add_epi16(_mm256_mullo_epi16(value, alpha), _mm256_set1_epi16(127));
  return _mm256_srli_epi16(_mm256_mulhi_epu16(rounded, _mm256_set1_epi16(static_cast<short>(0x8081))), 7);
}

[[nodiscard]] inline auto low_lanes_avx(std::uint32_t premultiplied) noexcept -> __m256i {
  return _mm256_and_si256(_mm256_set1_epi32(static_cast<int>(premultiplied)),
                          _mm256_set1_epi16(0x00FF));
}

[[nodiscard]] inline auto high_lanes_avx(std::uint32_t premultiplied) noexcept -> __m256i {
  return _mm256_and_si256(_mm256_srli_epi16(_mm256_set1_epi32(static_cast<int>(premultiplied)), 8),
                          _mm256_set1_epi16(0x00FF));
}

/// CPU 是否支持 AVX2（首次调用缓存，进程生命周期内不变）。
[[nodiscard]] auto cpu_has_avx2() noexcept -> bool {
#if defined(_MSC_VER)
  static const bool supported = [] {
    std::array<int, 4> cpuinfo1{};
    std::array<int, 4> cpuinfo7{};
    __cpuid(cpuinfo1.data(), 1);
    const bool os_xsave = (cpuinfo1[2] & (1 << 27)) != 0;
    const bool avx = (cpuinfo1[2] & (1 << 28)) != 0;
    if (!os_xsave || !avx) return false;
    // OSXSAVE 后还要确认操作系统真的在上下文切换里保存 YMM（否则用 AVX 即崩）。
    std::uint64_t xcr0 = 0;
    xcr0 = static_cast<std::uint64_t>(_xgetbv(0));
    if ((xcr0 & 0x6) != 0x6) return false;
    __cpuidex(cpuinfo7.data(), 7, 0);
    return (cpuinfo7[1] & (1 << 5)) != 0;  // EBX.AVX2
  }();
  return supported;
#else
  // GCC/Clang：内建已含 OS YMM 状态检查（等价于上方的 CPUID+XGETBV 序列）。
  return __builtin_cpu_supports("avx2") != 0;
#endif
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

/// 逐像素预乘 `src-over` 的标量参考实现（`over_row` 的三路基准，语义即它的契约）。
///
/// **用无除法等价式**：`(x * n + 127) / 255` → `((x * n + 128) * 257) >> 16`。
///
/// 为何重要：本函数每像素本来要做 **4 次整数除法**，而 x86 整数除法是 20~40 周期/次。
/// 在长行上这笔成本被带宽掩盖，但**短行摊不开**：实测长 1~6 时是长 512 的
/// **11~20 倍 ns/px**（`tools/simd_short_row_probe.cpp`），而圆角控件每行只有几像素、
/// 卡片类小形状（96×32 r6）整体就在这一档——那是界面里最常见的形状。
///
/// 等价性：`(x*n+127)/255 == ((x*n+128)*257)>>16` 对 `x∈[0,255]、n∈[0,255]`
/// **穷举验证一致**（65536 组，`tools/mul255_equiv_probe.cpp`）。
///
/// 为何不是 `x*n/255` 后再补：那会把“向上取整”丢掉，半透明像素系统性偏暗。
[[nodiscard]] auto over_scalar(std::uint32_t dst, std::uint32_t src) noexcept -> std::uint32_t {
  const std::uint32_t source_alpha = src & 0xFFU;
  if (source_alpha == 0U) return dst;
  const std::uint32_t inverse = 255U - source_alpha;
  const auto over = [inverse](std::uint32_t source, std::uint32_t destination) noexcept {
    return source + (((destination * inverse + 128U) * 257U) >> 16U);
  };
  return pack(over(channel(src, 24), channel(dst, 24)),
              over(channel(src, 16), channel(dst, 16)),
              over(channel(src, 8), channel(dst, 8)),
              over(source_alpha, channel(dst, 0)));
}

}  // namespace

auto backend() noexcept -> const char* {
#if ST_SIMD_AVX2
  if (cpu_has_avx2()) return "avx2";
#endif
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
#if ST_SIMD_AVX2
  if (cpu_has_avx2()) {
    const __m256i value = _mm256_set1_epi32(static_cast<int>(premultiplied));
    std::size_t index = 0;
    for (; index + 8 <= count; index += 8) {
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + index), value);
    }
    if (index < count) {
      const std::size_t remain = count - index;
      if (remain >= 4) {
        const __m128i tail = _mm_set1_epi32(static_cast<int>(premultiplied));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + index), tail);
        index += 4;
      }
      for (; index < count; ++index) dst[index] = premultiplied;
    }
    return;
  }
#endif
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
  const std::uint32_t source_alpha = ((premultiplied & 0xFFU) * alpha + 127U) / 255U;
  if (source_alpha == 0U) return;
#if ST_SIMD_AVX2
  if (cpu_has_avx2()) {
    const __m256i alpha_vector = _mm256_set1_epi16(static_cast<short>(alpha));
    const __m256i inverse_vector = _mm256_set1_epi16(static_cast<short>(255U - source_alpha));
    const __m256i source_low = low_lanes_avx(premultiplied);
    const __m256i source_high = high_lanes_avx(premultiplied);
    const __m256i scaled_low = scale_255_avx(source_low, alpha_vector);
    const __m256i scaled_high = scale_255_avx(source_high, alpha_vector);
    std::size_t index = 0;
    for (; index + 8 <= count; index += 8) {
      const __m256i destination = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + index));
      const __m256i destination_low = _mm256_and_si256(destination, _mm256_set1_epi16(0x00FF));
      const __m256i destination_high =
          _mm256_and_si256(_mm256_srli_epi16(destination, 8), _mm256_set1_epi16(0x00FF));
      const __m256i out_low =
          _mm256_add_epi16(scaled_low, scale_255_avx(destination_low, inverse_vector));
      const __m256i out_high =
          _mm256_add_epi16(scaled_high, scale_255_avx(destination_high, inverse_vector));
      const __m256i merged = _mm256_or_si256(
          out_low, _mm256_slli_epi16(_mm256_and_si256(out_high, _mm256_set1_epi16(0x00FF)), 8));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + index), merged);
    }
    for (; index < count; ++index) dst[index] = blend_scalar(dst[index], premultiplied, alpha);
    return;
  }
#endif
#if ST_SIMD_X86
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

void over_row(std::uint32_t* dst, const std::uint32_t* src, std::size_t count) noexcept {
  if (count == 0) return;
  // **短行直接走标量，不碰 SIMD 常量**。
  //
  // 为何：下面每条向量路径开头都要构造 3 个广播常量（`_mm256_set1_*`），
  // 而 `count < 8`（AVX2）/ `< 4`（SSE2）时**一个向量迭代都不执行**——
  // 纯白花。实测代价（`tools/simd_short_row_probe.cpp`，ns/px）：
  //   长 1~6 → **3.79~5.98**（是长 512 的 **9~14 倍**）；长 8 → 0.89；长 ≥16 → 0.46。
  //
  // 为何短行很常见：贴图合成（`blit_geometry_sprite`）**逐行**调本函数，
  // 而圆角控件的上下圆角带每行只有几个像素；卡片/按钮类小形状（
  // 96×32 r6）整体就落在这一档——实测每像素成本是小面板的 13 倍。
  //
  // 阈值取 8：AVX2 宽度。`count < 8` 时向量主循环本来就一次都不跑，
  // 只剩下“构造常量 + 进入标量尾巴”这两笔开销，直接跳过它们。
  if (count < 8) {
    for (std::size_t index = 0; index < count; ++index) {
      dst[index] = over_scalar(dst[index], src[index]);
    }
    return;
  }
#if ST_SIMD_AVX2
  if (cpu_has_avx2()) {
    const __m256i low_mask = _mm256_set1_epi16(0x00FF);
    const __m256i alpha_mask = _mm256_set1_epi32(0x000000FF);
    const __m256i full = _mm256_set1_epi32(255);
    std::size_t index = 0;
    for (; index + 8 <= count; index += 8) {
      const __m256i source = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + index));
      const __m256i destination = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + index));
      // 每像素的 `255 - a` 复制到两个 16 位 lane（低字节/高字节各一组通道）。
      const __m256i inverse32 = _mm256_sub_epi32(
          full, _mm256_and_si256(source, alpha_mask));
      const __m256i inverse = _mm256_and_si256(
          _mm256_or_si256(inverse32, _mm256_slli_epi32(inverse32, 16)), low_mask);
      const __m256i source_low = _mm256_and_si256(source, low_mask);
      const __m256i source_high = _mm256_and_si256(_mm256_srli_epi16(source, 8), low_mask);
      const __m256i destination_low = _mm256_and_si256(destination, low_mask);
      const __m256i destination_high =
          _mm256_and_si256(_mm256_srli_epi16(destination, 8), low_mask);
      const __m256i out_low =
          _mm256_add_epi16(source_low, scale_255_avx(destination_low, inverse));
      const __m256i out_high =
          _mm256_add_epi16(source_high, scale_255_avx(destination_high, inverse));
      const __m256i merged = _mm256_or_si256(
          out_low, _mm256_slli_epi16(_mm256_and_si256(out_high, low_mask), 8));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + index), merged);
    }
    for (; index < count; ++index) dst[index] = over_scalar(dst[index], src[index]);
    return;
  }
#endif
#if ST_SIMD_X86
  const __m128i low_mask = _mm_set1_epi16(0x00FF);
  const __m128i alpha_mask = _mm_set1_epi32(0x000000FF);
  const __m128i full = _mm_set1_epi32(255);
  std::size_t index = 0;
  for (; index + 4 <= count; index += 4) {
    const __m128i source = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + index));
    const __m128i destination = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + index));
    const __m128i inverse32 = _mm_sub_epi32(full, _mm_and_si128(source, alpha_mask));
    const __m128i inverse =
        _mm_and_si128(_mm_or_si128(inverse32, _mm_slli_epi32(inverse32, 16)), low_mask);
    const __m128i source_low = _mm_and_si128(source, low_mask);
    const __m128i source_high = _mm_and_si128(_mm_srli_epi16(source, 8), low_mask);
    const __m128i destination_low = _mm_and_si128(destination, low_mask);
    const __m128i destination_high = _mm_and_si128(_mm_srli_epi16(destination, 8), low_mask);
    const __m128i out_low = _mm_add_epi16(source_low, scale_255(destination_low, inverse));
    const __m128i out_high = _mm_add_epi16(source_high, scale_255(destination_high, inverse));
    const __m128i merged =
        _mm_or_si128(out_low, _mm_slli_epi16(_mm_and_si128(out_high, low_mask), 8));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + index), merged);
  }
  for (; index < count; ++index) dst[index] = over_scalar(dst[index], src[index]);
#else
  for (std::size_t index = 0; index < count; ++index) dst[index] = over_scalar(dst[index], src[index]);
#endif
}

void convert_rgba_to_xrgb_row(std::uint32_t* dst, const std::uint32_t* src,
                              std::size_t count) noexcept {
  if (count == 0) return;
#if ST_SIMD_AVX2
  if (cpu_has_avx2()) {
    const __m256i alpha_bits = _mm256_set1_epi32(static_cast<int>(0xFF000000U));
    std::size_t index = 0;
    for (; index + 8 <= count; index += 8) {
      const __m256i value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + index));
      // `srli_epi32(…, 8)` 得 `0x00RRGGBB`，再置高字节为 `0xFF`（不透明标记，
      // 与标量实现及旧版后端逐字节一致）。
      const __m256i shifted = _mm256_srli_epi32(value, 8);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + index),
                          _mm256_or_si256(shifted, alpha_bits));
    }
    for (; index < count; ++index) dst[index] = ((src[index] >> 8U) & 0x00FFFFFFU) | 0xFF000000U;
    return;
  }
#endif
#if ST_SIMD_X86
  const __m128i alpha_bits = _mm_set1_epi32(static_cast<int>(0xFF000000U));
  std::size_t index = 0;
  for (; index + 4 <= count; index += 4) {
    const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + index));
    const __m128i shifted = _mm_srli_epi32(value, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + index), _mm_or_si128(shifted, alpha_bits));
  }
  for (; index < count; ++index) dst[index] = ((src[index] >> 8U) & 0x00FFFFFFU) | 0xFF000000U;
#else
  for (std::size_t index = 0; index < count; ++index) {
    dst[index] = ((src[index] >> 8U) & 0x00FFFFFFU) | 0xFF000000U;
  }
#endif
}

}  // namespace st::raster::simd
