// SIMD 边界：本文件是 CONVENTIONS §2 禁令（指针运算/R6 reinterpret_cast）的受控例外——
// 仅对 `const std::uint8_t*` 字节缓冲做定长向量访存，所有访存由调用方保证区间边界。
#include "st/search/simd_scan.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>

// x86-64 判定：与 `src/raster/simd.cpp` 同一口径（MSVC 不定义 `__SSE2__`/`__x86_64__`，
// 漏掉 `_M_X64` 会让整个构建**静默退化到 scalar**）。x64 平台基线即保证 SSE2。
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define ST_SCAN_SIMD_X86 1
#else
#define ST_SCAN_SIMD_X86 0
#endif

// AVX2：运行时检测（与 raster/simd.cpp 同一套判据），编译期只在指令集可用时启用。
#if ST_SCAN_SIMD_X86 && (defined(__AVX2__) || defined(_MSC_VER))
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define ST_SCAN_SIMD_AVX2 1
#else
#define ST_SCAN_SIMD_AVX2 0
#endif

namespace st::search::simd {
namespace {

#if ST_SCAN_SIMD_AVX2
/// CPU 是否支持 AVX2（首次调用缓存；判据与 raster/simd.cpp 的 cpu_has_avx2 同源）。
[[nodiscard]] auto cpu_has_avx2() noexcept -> bool {
#if defined(_MSC_VER)
  static const bool supported = [] {
    std::array<int, 4> cpuinfo1{};
    std::array<int, 4> cpuinfo7{};
    __cpuid(cpuinfo1.data(), 1);
    const bool os_xsave = (cpuinfo1[2] & (1 << 27)) != 0;
    const bool avx = (cpuinfo1[2] & (1 << 28)) != 0;
    if (!os_xsave || !avx) return false;
    const std::uint64_t xcr0 = static_cast<std::uint64_t>(_xgetbv(0));
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

/// 标量基准：`memchr`（libc 自带向量化，作为「标量语义」的权威参照）。
[[nodiscard]] auto find_byte_scalar(const std::uint8_t* data, std::size_t size, std::size_t from,
                                    std::uint8_t needle) noexcept -> std::size_t {
  if (from >= size) return size;
  const void* hit = std::memchr(data + from, static_cast<int>(needle), size - from);
  if (hit == nullptr) return size;
  return static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - data);
}

}  // namespace

auto find_byte_from(const std::uint8_t* data, std::size_t size, std::size_t from,
                    std::uint8_t needle) noexcept -> std::size_t {
  if (data == nullptr || from >= size) return size;
#if ST_SCAN_SIMD_AVX2
  if (cpu_has_avx2()) {
    const __m256i wanted = _mm256_set1_epi8(static_cast<char>(needle));
    std::size_t index = from;
    // 32 字节一组：一次比较 + movemask 得到「哪些 lane 命中」的位图，
    // 命中就用 ctz 取出最低位（= 最靠前的命中）——这是 memchr 的标准向量形态。
    for (; index + 32 <= size; index += 32) {
      const __m256i block =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
      const unsigned mask = static_cast<unsigned>(
          _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, wanted)));
      if (mask != 0U) {
        // 位运算取最低置位（不引 <bit> 的 ctz 依赖：MSVC/gcc 都支持 __builtin/`_tzcnt`
        // 之外的可移植写法，这里是 32 位无符号的标准技巧）。
        unsigned lowest = mask;
        std::size_t offset = 0;
        while ((lowest & 1U) == 0U) {
          lowest >>= 1U;
          ++offset;
        }
        return index + offset;
      }
    }
    // 尾部不足 32 字节：退回标量（libc memchr 本身已向量化）。
    return find_byte_scalar(data, size, index, needle);
  }
#endif
#if ST_SCAN_SIMD_X86
  const __m128i wanted = _mm_set1_epi8(static_cast<char>(needle));
  std::size_t index = from;
  for (; index + 16 <= size; index += 16) {
    const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + index));
    const unsigned mask =
        static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(block, wanted)));
    if (mask != 0U) {
      unsigned lowest = mask;
      std::size_t offset = 0;
      while ((lowest & 1U) == 0U) {
        lowest >>= 1U;
        ++offset;
      }
      return index + offset;
    }
  }
  return find_byte_scalar(data, size, index, needle);
#else
  return find_byte_scalar(data, size, from, needle);
#endif
}

auto find_byte(const std::uint8_t* data, std::size_t size, std::uint8_t needle) noexcept
    -> std::size_t {
  return find_byte_from(data, size, 0, needle);
}

auto find_substring(const std::uint8_t* data, std::size_t size, const std::uint8_t* needle,
                    std::size_t needle_size) noexcept -> std::size_t {
  if (needle_size == 0) return 0;
  if (data == nullptr || needle == nullptr || size < needle_size) return size;
  if (needle_size == 1) return find_byte(data, size, needle[0]);
  const std::uint8_t first = needle[0];
  const std::size_t last_start = size - needle_size;
  std::size_t from = 0;
  while (from <= last_start) {
    const std::size_t candidate = find_byte_from(data, size, from, first);
    if (candidate > last_start) return size;
    if (std::memcmp(data + candidate, needle, needle_size) == 0) return candidate;
    from = candidate + 1;
  }
  return size;
}

auto find_byte_case_insensitive_from(const std::uint8_t* data, std::size_t size,
                                    std::size_t from, std::uint8_t letter) noexcept -> std::size_t {
  if (data == nullptr || from >= size) return size;
  // 参与比较的两个字节：原字母 + ASCII 折叠后的另一种形态（非字母则两者相同）。
  const std::uint8_t folded =
      (letter >= 'a' && letter <= 'z')   ? static_cast<std::uint8_t>(letter - 'a' + 'A')
      : (letter >= 'A' && letter <= 'Z') ? static_cast<std::uint8_t>(letter - 'A' + 'a')
                                         : letter;
  const auto matches = [letter, folded](std::uint8_t value) noexcept {
    return value == letter || value == folded;
  };
#if ST_SCAN_SIMD_AVX2
  if (cpu_has_avx2()) {
    const __m256i wanted_a = _mm256_set1_epi8(static_cast<char>(letter));
    const __m256i wanted_b = _mm256_set1_epi8(static_cast<char>(folded));
    std::size_t index = from;
    for (; index + 32 <= size; index += 32) {
      const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + index));
      const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(_mm256_or_si256(
          _mm256_cmpeq_epi8(block, wanted_a), _mm256_cmpeq_epi8(block, wanted_b))));
      if (mask != 0U) {
        unsigned lowest = mask;
        std::size_t offset = 0;
        while ((lowest & 1U) == 0U) {
          lowest >>= 1U;
          ++offset;
        }
        return index + offset;
      }
    }
    for (; index < size; ++index) {
      if (matches(data[index])) return index;
    }
    return size;
  }
#endif
#if ST_SCAN_SIMD_X86
  const __m128i wanted_a = _mm_set1_epi8(static_cast<char>(letter));
  const __m128i wanted_b = _mm_set1_epi8(static_cast<char>(folded));
  std::size_t index = from;
  for (; index + 16 <= size; index += 16) {
    const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + index));
    const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(
        _mm_or_si128(_mm_cmpeq_epi8(block, wanted_a), _mm_cmpeq_epi8(block, wanted_b))));
    if (mask != 0U) {
      unsigned lowest = mask;
      std::size_t offset = 0;
      while ((lowest & 1U) == 0U) {
        lowest >>= 1U;
        ++offset;
      }
      return index + offset;
    }
  }
  for (; index < size; ++index) {
    if (matches(data[index])) return index;
  }
  return size;
#else
  for (std::size_t index = from; index < size; ++index) {
    if (matches(data[index])) return index;
  }
  return size;
#endif
}

auto backend() noexcept -> const char* {
#if ST_SCAN_SIMD_AVX2
  if (cpu_has_avx2()) return "avx2";
#endif
#if ST_SCAN_SIMD_X86
  return "sse2";
#elif defined(__ARM_NEON)
  return "neon";
#else
  return "scalar";
#endif
}

auto as_bytes(std::string_view text) noexcept -> std::span<const std::uint8_t> {
  // 特例：空视图不能 `data()` 强转为非空指针后再取 0 长度以外的东西——
  // `std::span(p, 0)` 对空 `string_view` 的 `nullptr` 是合法的，但直接传 nullptr
  // 给 `_mm_loadu_si128` 一类访存不安全，所以保持 len=0（调用方循环不进入）。
  if (text.empty()) return {};
  return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                       text.size());
}

auto as_text(std::span<const std::uint8_t> bytes) noexcept -> std::string_view {
  if (bytes.empty()) return {};
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace st::search::simd